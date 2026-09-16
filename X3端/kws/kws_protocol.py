# kws_protocol.py — X3 端双进程 KWS 架构 IPC 协议
"""2026-09-16 重构:kws_listener ↔ kws_worker 通过 Unix domain socket 通信

进程边界(原 kws_record_send.py 单进程全包拆成两个):
  kws_listener.py  — 独占麦克风 (arecord),永远调 oww.predict()
                     检测到唤醒 → 发 wake event 给 worker → 自己 VAD 录音
                     → 录完发 recording_done + 完整 PCM
  kws_worker.py    — 独占 /dev/ttyS3 (STM32 串口),听 wake event
                     收到 recording_done → 上送 PC brain → 翻译 STM32 帧
                     100ms heartbeat (复用 dog_uart.DogLink)
  kws_protocol.py  — 本文件,定义 socket path + JSON 消息 schema + PCM 编解码

消息格式(JSON 文本,每条以 \\n 结尾,newline-delimited JSON):

  listener → worker (唤醒触发后立即发):
    {"type":"wake","ts":<float>,"score":<float>,"rms":<int>,"pre_pcm_b64":<str>}
      pre_pcm_b64 = 唤醒前 1.5s PCM (int16 LE mono 16kHz) 的 base64
      worker 用来对 ASR 文本做对齐(可选,目前未用)

  listener → worker (VAD 录音结束后发):
    {"type":"recording_done","ts":<float>,"pcm_b64":<str>,"duration_s":<float>}
      pcm_b64 = 完整录音 (从唤醒时刻起算) PCM 的 base64
      duration_s = 录音时长,worker 用 < 2s 判 false_wake

  双向错误:
    {"type":"error","code":<str>,"msg":<str>}

  worker → listener (可选心跳,目前未实现,留位):
    {"type":"heartbeat","ts":<float>}

为什么拆:
  openWakeWord 是 stateful 模型,单进程里被 13s 长任务(录音+WS+LLM+STM32)
  冻结 → audio_buffer 残留 → 二次唤醒。拆双进程后 listener 永远调 predict,
  audio_buffer 永远滚动,根因被消除。
"""
import base64
import json
import os
import socket

# === Unix domain socket 路径 ===
SOCKET_PATH = "/tmp/kws_listener.sock"
SOCKET_BACKLOG = 1  # 只接 1 个 worker;新连接来时踢掉旧的

# === PCM 参数(与 arecord -f S16_LE -r 16000 -c 1 一致)===
SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH = 2  # int16
CHUNK_BYTES = 1280 * 2  # 80ms @ 16kHz mono S16_LE = 2560 bytes

# === Listener 内部 ring buffer(预录唤醒前 PCM)===
RING_BUFFER_SEC = 1.5
RING_BUFFER_BYTES = int(RING_BUFFER_SEC * SAMPLE_RATE * SAMPLE_WIDTH)  # 48000

# === KWS 评分阈值 ===
RMS_KWS_LOW = 100       # < 此值 = 静音,跳过
RMS_KWS_HIGH = 3500     # > 此值 = 狗机械声,跳过
KWS_THRESHOLD = 0.85    # score >= 此值触发唤醒
LISTENER_COOLDOWN_SEC = 1.0  # listener 内部冷却,防短窗连环触发(1s 已够,因为 oww 永远滚动)

# === VAD 参数(在 listener 内做,worker 不碰麦克风)===
SILENCE_RMS_THRESHOLD = 500  # 帧 RMS < 此值视为静音
MAX_SILENCE_CHUNKS = 18      # 80ms × 18 = 1.44s 静音结束
MAX_RECORD_CHUNKS = 100      # 80ms × 100 = 8s 上限

# === 默认模型 / 端口 ===
DEFAULT_MODEL = "/root/kws/xiaolong.onnx"
DEFAULT_PORT = "/dev/ttyS3"
DEFAULT_BAUD = 115200
DEFAULT_PC_URL = "ws://192.168.160.91:8765"


# === 编码 / 解码 ===
def encode_pcm(pcm_bytes: bytes) -> str:
    """int16 LE PCM bytes → base64 str(JSON 安全)"""
    return base64.b64encode(pcm_bytes).decode()


def decode_pcm(b64_str: str) -> bytes:
    """base64 str → int16 LE PCM bytes"""
    return base64.b64decode(b64_str)


# === JSON 消息构造 ===
def msg_wake(ts: float, score: float, rms: int, pre_pcm: bytes) -> str:
    return json.dumps({
        "type": "wake",
        "ts": ts,
        "score": score,
        "rms": rms,
        "pre_pcm_b64": encode_pcm(pre_pcm),
    })


def msg_recording_done(ts: float, pcm: bytes, duration_s: float) -> str:
    return json.dumps({
        "type": "recording_done",
        "ts": ts,
        "pcm_b64": encode_pcm(pcm),
        "duration_s": duration_s,
    })


def msg_error(code: str, msg: str) -> str:
    return json.dumps({
        "type": "error",
        "code": code,
        "msg": msg,
    })


# === Socket 工具 ===
def cleanup_socket():
    """listener 启动前清掉残留 socket 文件(上一次崩溃可能留下)"""
    try:
        os.unlink(SOCKET_PATH)
    except FileNotFoundError:
        pass


def send_json_line(sock: socket.socket, line: str):
    """发一条 newline-delimited JSON(末尾自动加 \\n)"""
    sock.sendall((line + "\n").encode())


def recv_json_line(sock: socket.socket) -> dict | None:
    """收一条 newline-delimited JSON(读到 \\n 为止)"""
    buf = b""
    while True:
        b = sock.recv(1)
        if not b:  # 连接关闭
            return None
        if b == b"\n":
            break
        buf += b
        if len(buf) > 1_000_000:  # 1MB 上限,防恶意大消息
            raise ValueError("JSON message too large")
    try:
        return json.loads(buf.decode())
    except json.JSONDecodeError as e:
        raise ValueError(f"bad JSON: {buf[:80]!r}") from e