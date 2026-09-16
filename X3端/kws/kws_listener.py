#!/usr/bin/env python3
"""kws_listener.py — X3 端 KWS 常驻进程 (2026-09-16 重构)

职责:
  - 独占麦克风 (arecord stdin)
  - 永远调 oww.predict() (不被业务阻塞,根除 stateful buffer 残留)
  - 检测唤醒 → 发 wake_detected → VAD 录音 → 发 recording_done
  - 通过 Unix domain socket 与 kws_worker.py 通信

不做:
  - 不碰 /dev/ttyS3 (STM32 串口归 worker)
  - 不连 WS brain (业务归 worker)
  - 不发 STM32 心跳 (worker 用 dog_uart.DogLink)

用法(由 start_kws_dual.sh 启动):
  arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 -t raw 2>/dev/null | python3 /root/kws/kws_listener.py

依赖: openwakeword / onnxruntime / numpy (X3 上已装,见 kws_protocol.py)
"""
import argparse
import json
import os
import socket
import sys
import time
from collections import deque

import numpy as np

import kws_protocol as P


# === oww 懒加载 ===
_oww = None


def get_oww(model_path: str):
    global _oww
    if _oww is None:
        from openwakeword.model import Model
        print(f"[listener] 加载 oww model: {model_path}", flush=True)
        _oww = Model(wakeword_models=[model_path], inference_framework="onnx")
        print(f"[listener] oww 加载完成", flush=True)
    return _oww


def compute_rms(pcm_bytes: bytes) -> int:
    """帧 RMS,用于静音检测 + KWS RMS 预筛"""
    samples = np.frombuffer(pcm_bytes, dtype=np.int16)
    if len(samples) == 0:
        return 0
    return int(np.sqrt(np.mean(samples.astype(float) ** 2)))


def make_socket_server(path: str) -> socket.socket:
    """清残留 + bind + listen(1)"""
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    # 1MB buffer,确保 8s recording_done (256KB raw) 一次 send 成功
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1_048_576)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1_048_576)
    s.bind(path)
    os.chmod(path, 0o660)  # 防同主机其他用户嗅探
    s.listen(P.SOCKET_BACKLOG)
    return s


def send_json(sock: socket.socket, obj: dict):
    """newline-delimited JSON"""
    line = (json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8")
    sock.sendall(line)


class KWSListener:
    """常驻 KWS + ring buffer(预录唤醒前 PCM)+ VAD 录音"""

    def __init__(self, model_path: str, threshold: float, cooldown: float):
        self.threshold = threshold
        self.cooldown = cooldown
        self.oww = get_oww(model_path)

        # ring buffer:最近 RING_BUFFER_SEC 秒的 int16 sample
        # (关键 — 让唤醒事件可以附带唤醒前音频,但 oww buffer 永远滚动不被冻住)
        ring_samples = int(P.RING_BUFFER_SEC * P.SAMPLE_RATE)
        self.ring: deque = deque(maxlen=ring_samples)

        # 状态机 (两态: kws / recording)
        self.state = "kws"
        self.last_wake_ts = 0.0      # listener 内部 cooldown
        self.record_buf: list[bytes] = []
        self.silence_chunks = 0

    def _on_wake(self, conn, ts, score, rms, current_chunk):
        """触发唤醒:快照 ring buffer → 发 wake_detected → 进 recording"""
        pre_pcm_bytes = np.array(self.ring, dtype=np.int16).tobytes()
        msg = {
            "type": "wake_detected",
            "ts": ts,
            "score": score,
            "rms": rms,
            "pre_pcm_b64": P.encode_pcm_b64(pre_pcm_bytes),
        }
        try:
            send_json(conn, msg)
        except (BrokenPipeError, ConnectionResetError, OSError) as e:
            # worker 死了 — 跳过本次 wake, 继续 KWS
            print(f"[listener] ERR send wake_detected: {e}, 跳回 kws", flush=True)
            self.state = "kws"
            return
        print(f"[listener] 🌟 wake score={score:.3f} rms={rms}", flush=True)

        # 把当前 chunk 也加入录音 buffer
        self.record_buf = [current_chunk]
        self.silence_chunks = 0
        self.last_wake_ts = ts

    def _on_recording_done(self, conn, ts):
        """VAD 终止:拼完整录音 → 发 recording_done → 切回 kws"""
        pcm_bytes = b"".join(self.record_buf)
        duration_s = len(pcm_bytes) / (P.SAMPLE_RATE * P.SAMPLE_WIDTH)
        msg = {
            "type": "recording_done",
            "ts": ts,
            "pcm_b64": P.encode_pcm_b64(pcm_bytes),
            "duration_s": duration_s,
            "silence_chunks": self.silence_chunks,
        }
        self.record_buf = []
        try:
            send_json(conn, msg)
        except (BrokenPipeError, ConnectionResetError, OSError) as e:
            print(f"[listener] ERR send recording_done: {e}", flush=True)
        print(
            f"[listener] rec done {len(pcm_bytes)} bytes ({duration_s:.2f}s "
            f"silence_chunks={self.silence_chunks})",
            flush=True,
        )
        self.state = "kws"
        self.silence_chunks = 0

    def run(self, conn: socket.socket):
        """主循环:从 stdin 读 chunk, KWS 评分 + VAD 录音"""
        try:
            while True:
                chunk = sys.stdin.buffer.read(P.CHUNK_BYTES)
                if not chunk:
                    # arecord 退出 / pipe 断
                    print("[listener] stdin EOF (arecord 退出?)", flush=True)
                    break
                pcm = np.frombuffer(chunk, dtype=np.int16)

                if self.state == "kws":
                    # 1) ring buffer 永远滚动 ←—— 这是核心!
                    #    oww audio buffer 由 predict() 持续接收新 PCM 维持滑动,
                    #    不再被"业务长任务"冻结(业务在 worker 进程,不在这里)
                    self.ring.extend(pcm.tolist())

                    # 2) listener 内部 cooldown (防同 utterance 多帧连环高分)
                    now = time.time()
                    if now - self.last_wake_ts < self.cooldown:
                        continue

                    # 3) RMS 预筛:静音(<100)和疑似机械声(>3500)直接跳过
                    rms = compute_rms(chunk)
                    if rms < P.RMS_KWS_LOW or rms > P.RMS_KWS_HIGH:
                        continue

                    # 4) oww 评分 (predict 基于最近 N 帧, 不再被冻结)
                    preds = self.oww.predict(pcm)
                    if not preds:
                        continue
                    score = float(next(iter(preds.values()))[-1])
                    if score < self.threshold:
                        continue

                    # 5) ★ 触发唤醒 ★
                    self.state = "recording"
                    self._on_wake(conn, now, score, rms, chunk)

                else:  # recording
                    # ring 不再写 (避免覆盖预录;反正 state 已切到 recording)
                    self.record_buf.append(chunk)
                    rms = compute_rms(chunk)
                    if rms < P.SILENCE_RMS_THRESHOLD:
                        self.silence_chunks += 1
                    else:
                        self.silence_chunks = 0

                    # VAD 终止:静音 1.44s 或录音 8s 上限
                    if self.silence_chunks >= P.MAX_SILENCE_CHUNKS:
                        self._on_recording_done(conn, time.time())
                    elif len(self.record_buf) >= P.MAX_RECORD_CHUNKS:
                        self._on_recording_done(conn, time.time())
        except KeyboardInterrupt:
            print("\n[listener] Bye", flush=True)
        finally:
            try:
                conn.close()
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser(description="X3 端 KWS 常驻 listener (Unix socket)")
    ap.add_argument("--model",    default=P.DEFAULT_MODEL)
    ap.add_argument("--threshold", type=float, default=P.KWS_THRESHOLD)
    ap.add_argument("--cooldown",  type=float, default=P.LISTENER_COOLDOWN_SEC)
    ap.add_argument("--socket",    default=P.SOCKET_PATH)
    args = ap.parse_args()

    print(
        f"[listener] 启动 model={args.model} threshold={args.threshold} "
        f"cooldown={args.cooldown}s socket={args.socket}",
        flush=True,
    )

    sock = make_socket_server(args.socket)
    print(f"[listener] 等待 worker 连接 {args.socket} ...", flush=True)
    conn, _ = sock.accept()  # 阻塞到 worker 上线
    print(f"[listener] worker 已连", flush=True)

    try:
        listener = KWSListener(args.model, args.threshold, args.cooldown)
        listener.run(conn)
    finally:
        sock.close()


if __name__ == "__main__":
    main()