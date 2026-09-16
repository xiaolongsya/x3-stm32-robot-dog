#!/usr/bin/env python3
"""kws_record_send.py — X3 端 KWS 唤醒 → VAD 录音 → WS 上传 PC brain → STM32 执行

2026-09-16:生产链路打通版本。

链路:
    arecord (16k mono S16_LE)
       ↓
    KWS 推理 (xiaolong.onnx, 80ms 一段)
       ↓ 唤醒(score > threshold)
    VAD 录音 (累计静音 1.5s 或最大 8s)
       ↓
    base64 PCM → WS 长连 PC brain (ws://<本机IP>:8765)
       ↓
    等 {type: action, actions: [...]} 响应
       ↓
    翻译成 STM32 UART 帧 + 发 /dev/ttyS3@115200

后台:
- STM32 心跳 100ms 一发(防 watchdog 200ms 超时回 STAND)
- WS 断线指数退避 1s/2s/4s/8s/最大 8s

用法:
    /root/kws/kws_record_send.py --pc-url  ws://192.168.160.91:8765

依赖:openwakeword / onnxruntime / pyserial / websockets / numpy
"""
import argparse
import asyncio
import base64
import json
import os
import struct
import sys
import threading
import time

import numpy as np
import serial

DEFAULT_MODEL  = "/root/kws/xiaolong.onnx"
DEFAULT_PORT   = "/dev/ttyS3"
DEFAULT_BAUD   = 115200
DEFAULT_PC_URL = "ws://192.168.160.91:8765"
DEFAULT_THRESH = 0.85
CHUNK = 1280           # 80ms @ 16kHz
MAX_SILENCE_CHUNKS = 18    # 80ms × 18 = 1.44s 静音
MAX_RECORD_CHUNKS  = 100   # 80ms × 100 = 8s 最大


def csum8(data: bytes) -> int:
    c = 0
    for b in data:
        c ^= b
    return c & 0xFF


def send_frame(ser: serial.Serial, cmd: int, payload: bytes = b"") -> None:
    body = bytes([len(payload), cmd]) + payload
    ser.write(b"\xaa\x55" + body + bytes([csum8(body)]))
    ser.flush()


def action_to_stm32_frame(action: dict) -> bytes | None:
    """LLM 输出 action → STM32 UART 帧"""
    cmd_name = action.get("cmd")
    if cmd_name == "ACTION_PLAY":
        action_id = action.get("id")
        if action_id not in (5, 6, 7, 8):
            return None
        return send_frame_bytes(0x09, bytes([action_id, 1]))
    elif cmd_name == "MOTION_PLAY":
        motion_id = action.get("id")
        dur_ms = int(action.get("duration_ms", 5000))
        if motion_id not in (1, 2, 3, 4):
            return None
        payload = bytes([motion_id]) + struct.pack("<I", dur_ms)
        return send_frame_bytes(0x01, payload)
    elif cmd_name == "EMERGENCY_STOP":
        return send_frame_bytes(0x06, b"")
    return None


def send_frame_bytes(cmd: int, payload: bytes) -> bytes:
    body = bytes([len(payload), cmd]) + payload
    return b"\xaa\x55" + body + bytes([csum8(body)])


def compute_rms(pcm_bytes: bytes) -> int:
    """计算帧 RMS,用于静音检测"""
    samples = np.frombuffer(pcm_bytes, dtype=np.int16)
    if len(samples) == 0:
        return 0
    return int(np.sqrt(np.mean(samples.astype(float) ** 2)))


SILENCE_RMS_THRESHOLD = 500   # 帧 RMS < 500 视为静音(VAD 终止条件)
RMS_KWS_LOW = 100               # KWS 评分前 RMS 下限(< 此值 = 没人说话,跳过)
RMS_KWS_HIGH = 3500             # KWS 评分前 RMS 上限(> 此值 = 狗机械声/拍桌/噪声,跳过)
MAX_COOLDOWN = 60.0             # cooldown 累加上限(秒)


class X3KWSBrainLink:
    """X3 端:KWS + VAD 录音 + WS 上传 + STM32 翻译"""

    # 状态机:
    # "kws"       - KWS 评分监听中
    # "recording" - VAD 录音中(刚唤醒)
    # "busy"      - WS 上传 + STM32 执行中(屏蔽 KWS,防止回声触发)
    # cooldown 默认 10s(KWS 唤醒冷却,防回声短窗触发)

    def __init__(self, model_path, port, baud, pc_url, threshold, cooldown):
        self.threshold = threshold
        self.cooldown = cooldown
        self.pc_url = pc_url
        self.last_wake_ts = 0.0
        self.mode = "kws"
        self.consecutive_false_wakes = 0  # 连续误唤醒计数(用于 cooldown 累加)

        # STM32 串口
        self.ser = serial.Serial(port, baud, timeout=0.5)
        # 心跳守护
        self.hb_stop = threading.Event()
        threading.Thread(target=self._heartbeat, daemon=True).start()

        # KWS 模型
        from openwakeword.model import Model
        self.oww = Model(wakeword_models=[model_path], inference_framework="onnx")
        print(f"[kws] 模型: {model_path}, 阈值: {threshold}")

        # 状态
        self.mode = "kws"            # "kws" / "recording"
        self.record_buffer = []       # 录音缓冲(list of bytes)
        self.silence_chunks = 0

    def _heartbeat(self):
        while not self.hb_stop.is_set():
            send_frame(self.ser, 0x05)
            self.hb_stop.wait(0.1)

    def process_chunk(self, chunk: bytes) -> bytes | None:
        """处理一帧 80ms PCM。返回非 None 表示录音完成。"""
        # busy 状态:录音 + WS + STM32 期间完全不评分
        if self.mode == "busy":
            return None

        if self.mode == "kws":
            # 唤醒冷却:距离上次唤醒 < cooldown,不评分
            now = time.time()
            if now - self.last_wake_ts < self.cooldown:
                return None
            # RMS 预筛:狗机械声 RMS 很高(>3500),人说话 RMS 中等(100-3500)
            # 跳过纯静音(<100)和疑似机械噪声(>3500),只让中间区间评分
            rms = compute_rms(chunk)
            if rms < RMS_KWS_LOW or rms > RMS_KWS_HIGH:
                return None
            pcm = np.frombuffer(chunk, dtype=np.int16)
            preds = self.oww.predict(pcm)
            if not preds:
                return None
            key = next(iter(preds.keys()))
            s = preds[key]
            try:
                score = float(s[-1] if isinstance(s, (np.ndarray, list)) else s)
            except (TypeError, IndexError):
                return None
            if score >= self.threshold:
                print(f"[kws] 🌟 唤醒 score={score:.3f} rms={rms} → 开始录音")
                self.mode = "recording"
                self.record_buffer = [chunk]
                self.silence_chunks = 0
                self.last_wake_ts = now
            return None
        else:
            # recording 模式
            self.record_buffer.append(chunk)
            rms = compute_rms(chunk)
            if rms < SILENCE_RMS_THRESHOLD:
                self.silence_chunks += 1
            else:
                self.silence_chunks = 0
            if self.silence_chunks >= MAX_SILENCE_CHUNKS:
                pcm = self._finish_recording()
                return pcm
            if len(self.record_buffer) >= MAX_RECORD_CHUNKS:
                pcm = self._finish_recording()
                return pcm
            return None

    def set_busy(self, busy: bool):
        """录音完成 → WS 上传 → STM32 执行期间设为 busy,屏蔽 KWS"""
        self.mode = "busy" if busy else "kws"
        if busy:
            self.last_wake_ts = time.time()  # busy 期内也算冷却起点
        print(f"[kws] 模式: {self.mode}")

    def _finish_recording(self):
        pcm = b"".join(self.record_buffer)
        print(f"[rec] 录音完成 {len(pcm)} bytes ({len(pcm)/32000:.2f}s)")
        self.record_buffer = []
        # 关键:这里设 "busy" 而不是 "kws",防止主循环拿到 pcm 之前的窗口期
        # KWS 误判录音期间的 chunk(set_busy 在主循环里调用,有 race condition)
        self.mode = "busy"
        self.silence_chunks = 0
        return pcm

    async def send_to_brain(self, pcm: bytes) -> dict | None:
        """WS 上传 PC brain,等 action 响应。返回 actions 列表或 None"""
        import websockets
        ws_id = "x3_local"
        try:
            async with websockets.connect(self.pc_url) as ws:
                print(f"[ws] 已连接 {self.pc_url}")
                # 发 utterance
                await ws.send(json.dumps({
                    "type": "utterance",
                    "pcm": base64.b64encode(pcm).decode(),
                    "session_id": ws_id,
                }))
                print(f"[ws] → utterance ({len(pcm)} bytes)")
                # 等响应(可能 action 或 error)
                while True:
                    raw = await ws.recv()
                    msg = json.loads(raw)
                    print(f"[ws] ← {msg}")
                    if msg["type"] in ("action", "error"):
                        return msg
        except Exception as e:
            print(f"[ws] 错误: {e}")
            return None

    def translate_and_send_actions(self, actions: list):
        """翻译 actions 为 STM32 帧并发送"""
        for a in actions:
            frame = action_to_stm32_frame(a)
            if frame:
                self.ser.write(frame)
                self.ser.flush()
                print(f"[stm32] → {a['cmd']} #{a.get('id','')}"
                      + (f" dur={a.get('duration_ms')}ms" if 'duration_ms' in a else ""))
            else:
                print(f"[stm32] 跳过非法: {a}")

    def close(self):
        self.hb_stop.set()
        self.ser.close()


async def websockets_connect(url):
    import websockets
    return websockets.connect(url)


async def stdin_reader(queue: asyncio.Queue):
    """后台线程:读 arecord stdin(80ms 一段)→ 放入 asyncio.Queue"""
    loop = asyncio.get_event_loop()

    def _read():
        while True:
            chunk = sys.stdin.buffer.read(CHUNK * 2)
            if not chunk:
                break
            asyncio.run_coroutine_threadsafe(queue.put(chunk), loop)
    t = threading.Thread(target=_read, daemon=True)
    t.start()


async def run(args):
    link = X3KWSBrainLink(
        model_path=args.model,
        port=args.port,
        baud=args.baud,
        pc_url=args.pc_url,
        threshold=args.threshold,
        cooldown=args.cooldown,
    )

    queue: asyncio.Queue = asyncio.Queue()
    await stdin_reader(queue)

    print(f"[kws] 启动,PC brain @ {args.pc_url}")
    print(f"[kws] 唤醒冷却 {args.cooldown}s(冷却期内不评分,避免回声触发)")
    print("[kws] 监听中 —— 说 '小龙' 触发整句录音 → WS 上传 → STM32 执行")
    print("=" * 70, flush=True)

    try:
        while True:
            chunk = await queue.get()
            pcm = await asyncio.to_thread(link.process_chunk, chunk)
            if pcm:
                # 录音完毕 → busy 状态(屏蔽 KWS,防回声)
                await asyncio.to_thread(link.set_busy, True)
                # 误唤醒信号:大概率是回声/噪声 → 等会延长 cooldown
                false_wake = False
                try:
                    resp = await link.send_to_brain(pcm)
                    if resp and resp.get("type") == "action":
                        actions = resp.get("actions", [])
                        reply = resp.get("reply", "")
                        print(f"[brain] reply: {reply}")
                        if actions:
                            # 真有动作:发 STM32
                            await asyncio.to_thread(link.translate_and_send_actions, actions)
                        else:
                            # actions=空:LLM 判定"不在能力范围"或"无动作"
                            # 但用户**真的说了话**,只是不能做 → 当作正常唤醒
                            print("[brain] actions 空(可能超出能力),不当误唤醒")
                    elif resp and resp.get("type") == "error":
                        print(f"[brain] error: {resp.get('code')} {resp.get('reply')}")
                        if resp.get("code") == "ASR_FAIL":
                            false_wake = True  # ASR 失败 = 录音里没人说话 = 误唤醒
                    else:
                        print("[brain] 无响应")
                finally:
                    # cooldown 累加:误唤醒计数 → effective cooldown
                    # 1 次: base
                    # 2 次: base × 3
                    # 3+ 次: base × 5,封顶 MAX_COOLDOWN
                    base_cooldown = args.cooldown
                    if false_wake:
                        link.consecutive_false_wakes += 1
                        if link.consecutive_false_wakes <= 1:
                            multiplier = 1.0
                        elif link.consecutive_false_wakes == 2:
                            multiplier = 3.0
                        else:
                            multiplier = 5.0
                    else:
                        link.consecutive_false_wakes = 0
                        multiplier = 1.0
                    effective_cooldown = min(base_cooldown * multiplier, MAX_COOLDOWN)
                    elapsed = time.time() - link.last_wake_ts
                    remaining = max(0, effective_cooldown - elapsed)
                    if remaining > 0:
                        if false_wake:
                            print(f"[kws] ⚠️ 误唤醒×{link.consecutive_false_wakes},"
                                  f"cooldown={effective_cooldown:.1f}s,剩余 {remaining:.1f}s")
                        else:
                            print(f"[kws] 冷却剩余 {remaining:.1f}s,继续屏蔽 KWS")
                        await asyncio.sleep(remaining)
                    # 清空 queue 里累积的"陈旧"chunk(录音期间 + cooldown 期内)
                    # 这些 chunk 可能含 dog 动机械声 / 房间回声
                    # 直接丢弃,从 fresh audio 开始
                    drained = 0
                    while not queue.empty():
                        try:
                            queue.get_nowait()
                            drained += 1
                        except asyncio.QueueEmpty:
                            break
                    if drained > 0:
                        print(f"[kws] 清空 queue {drained} 个陈旧 chunk")
                    await asyncio.to_thread(link.set_busy, False)
    except KeyboardInterrupt:
        print("\n[kws] Bye")
    finally:
        link.close()


def main():
    ap = argparse.ArgumentParser(description="X3 端 KWS + VAD 录音 + WS 上传 PC brain")
    ap.add_argument("--model",    default=DEFAULT_MODEL)
    ap.add_argument("--port",     default=DEFAULT_PORT)
    ap.add_argument("--baud",     type=int, default=DEFAULT_BAUD)
    ap.add_argument("--pc-url",   default=DEFAULT_PC_URL)
    ap.add_argument("--threshold", type=float, default=DEFAULT_THRESH)
    # cooldown 默认 0:busy + queue drain 已经保证唤醒间隔,cooldown 冗余
    ap.add_argument("--cooldown",  type=float, default=0.0)
    args = ap.parse_args()

    asyncio.run(run(args))


if __name__ == "__main__":
    main()