#!/usr/bin/env python3
"""kws_with_action.py — X3 端 KWS 唤醒 → 直接发 STM32 动作(蹲下起立)

2026-09-16 简化方案(用户拍板):不接 PC brain、不做 ASR/LLM,
每次唤醒发一次 ACTION_PLAY 0x09 [action_id=7, repeat=1] (=蹲下起立一次)。

依赖: openwakeword / onnxruntime / pyserial / numpy
X3 上都已具备,不需要装新包。

用法:
    arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 -t raw 2>/dev/null \\
        | python3 /root/kws/kws_with_action.py

参数:
    --threshold 0.85  唤醒阈值(2026-09-16 实测:0.85 无误识别,说"小龙" score 0.97+)
    --cooldown  1.5   唤醒冷却秒数
    --model     PATH  模型路径(默认 /root/kws/xiaolong.onnx)
    --port      /dev/ttyS3
    --baud      115200
    --action-id 7     ACTION_PLAY id(7=SIT_TO_STAND 蹲下起立; 5=坐下 6=起立 8=立→坐)
    --quiet           不打 score 进度条(只打唤醒)

中止: Ctrl+C
"""
import argparse
import os
import sys
import threading
import time

import numpy as np
import serial

DEFAULT_MODEL  = "/root/kws/xiaolong.onnx"
DEFAULT_PORT   = "/dev/ttyS3"
DEFAULT_BAUD   = 115200
DEFAULT_THRESH = 0.85     # 2026-09-16 实测最佳值
DEFAULT_ACTION = 7        # SIT_TO_STAND = 蹲下起立(实际是发两帧:5+6)
CHUNK = 1280              # 80ms @ 16kHz
SIT_MS    = 1200           # SIT_RAMP_MS(800) + 缓冲
STAND_MS  = 1600           # STAND_RAMP_MS(1200) + 缓冲
COOLDOWN_S = 3.5           # 蹲下(1.2) + 起立(1.6) + 缓冲


def csum8(data: bytes) -> int:
    c = 0
    for b in data:
        c ^= b
    return c & 0xFF


def send_frame(ser: serial.Serial, cmd: int, payload: bytes = b"") -> bytes | None:
    """发一帧,短暂读 50ms STM32 ACK 用于诊断。返回 ACK 字节或 None"""
    body = bytes([len(payload), cmd]) + payload
    frame = b"\xaa\x55" + body + bytes([csum8(body)])
    try:
        ser.write(frame)
        ser.flush()
    except Exception as e:
        print(f"[serial] write err: {e}", file=sys.stderr, flush=True)
        return None
    # 短暂读 50ms 找 STM32 响应
    t0 = time.time()
    buf = b""
    while time.time() - t0 < 0.05:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
        time.sleep(0.005)
    return buf


def do_sit_stand(ser: serial.Serial) -> None:
    """蹲下起立:STM32 没这个单一动作,拆成发两帧 + 等待 ramp
    1) ACTION_PLAY id=5 (SIT_DOWN, SIT_RAMP_MS=800ms)
    2) 等 SIT_MS(1200ms,ramp + 缓冲)
    3) ACTION_PLAY id=6 (STAND_UP, STAND_RAMP_MS=1200ms)
    """
    # 1) 蹲下
    ack1 = send_frame(ser, 0x09, bytes([5, 1]))
    if ack1 and b"\xaa\x55\x01\x89" in ack1:
        print(f"   [1/2] ✅ 蹲下(id=5)ACK")
    else:
        print(f"   [1/2] ⚠️ 蹲下: {ack1.hex()[:40] if ack1 else '无响应'}")
        return
    time.sleep(SIT_MS / 1000.0)
    # 2) 起立
    ack2 = send_frame(ser, 0x09, bytes([6, 1]))
    if ack2 and b"\xaa\x55\x01\x89" in ack2:
        print(f"   [2/2] ✅ 起立(id=6)ACK")
    else:
        print(f"   [2/2] ⚠️ 起立: {ack2.hex()[:40] if ack2 else '无响应'}")


class HeartbeatThread:
    """100ms 一发 CMD_HEARTBEAT 0x05,防 STM32 watchdog 200ms 超时回 STAND。
    STM32 watchdog 由 commands.c + watchdog.c 实现,200ms 无心跳就回 STAND。
    任何"动作"都会被立刻打断,所以必须持续发心跳。
    """
    def __init__(self, ser: serial.Serial):
        self.ser = ser
        self.stop = threading.Event()
        self.t = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.t.start()

    def stop_(self):
        self.stop.set()
        self.t.join(timeout=1.0)

    def _run(self):
        while not self.stop.is_set():
            send_frame(self.ser, 0x05)  # CMD_HEARTBEAT, len=0
            self.stop.wait(0.1)


def parse_args():
    ap = argparse.ArgumentParser(description="X3 端 KWS 唤醒 → STM32 动作")
    ap.add_argument("--threshold", type=float, default=DEFAULT_THRESH)
    ap.add_argument("--cooldown",  type=float, default=COOLDOWN_S)
    ap.add_argument("--model",     type=str,
                    default=os.environ.get("KWS_MODEL", DEFAULT_MODEL))
    ap.add_argument("--port",      type=str, default=DEFAULT_PORT)
    ap.add_argument("--baud",      type=int, default=DEFAULT_BAUD)
    ap.add_argument("--action-id", type=int, default=DEFAULT_ACTION,
                    help="ACTION_PLAY id(默认 7=蹲下起立)")
    ap.add_argument("--quiet",     action="store_true")
    return ap.parse_args()


def main():
    args = parse_args()

    if not os.path.isfile(args.model):
        print(f"[ERR] 模型不存在: {args.model}", file=sys.stderr)
        sys.exit(1)

    # 1) 串口
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.5)
    except Exception as e:
        print(f"[ERR] 串口 {args.port}@{args.baud} 打不开: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"[kws] ✅ 串口: {args.port}@{args.baud}")

    # 2) 心跳守护(STM32 watchdog 200ms 超时 → 必须持续发心跳)
    hb = HeartbeatThread(ser)
    hb.start()
    print(f"[kws] ✅ 心跳守护: 100ms 周期")

    # 3) KWS 模型
    try:
        from openwakeword.model import Model
    except ImportError:
        print("[ERR] openwakeword 未装", file=sys.stderr)
        sys.exit(1)
    oww = Model(wakeword_models=[args.model], inference_framework="onnx")
    print(f"[kws] ✅ 模型: {args.model}")
    print(f"[kws] 阈值: {args.threshold}  冷却: {args.cooldown}s")
    print(f"[kws] 动作: ACTION_PLAY id={args.action_id}(= 蹲下起立)")
    print(f"[kws] 监听中 —— 说 '小龙' 触发狗子蹲下起立")
    print("=" * 64, flush=True)

    last_wake_ts = 0.0
    peak_recent = 0.0
    tick = 0

    try:
        while True:
            chunk = sys.stdin.buffer.read(CHUNK * 2)
            if not chunk:
                break

            pcm = np.frombuffer(chunk, dtype=np.int16)
            preds = oww.predict(pcm)
            if not preds:
                continue
            key = next(iter(preds.keys()))
            s = preds[key]
            try:
                score = float(s[-1]) if isinstance(s, (np.ndarray, list)) else float(s)
            except (TypeError, IndexError):
                continue

            if score > peak_recent:
                peak_recent = score

            now = time.time()
            ts = time.strftime("%H:%M:%S")

            if score >= args.threshold and (now - last_wake_ts) > args.cooldown:
                # 唤醒 → 蹲下起立(发两帧:5 蹲下 + 6 起立)
                print(f"[{ts}] 🌟 唤醒 → 蹲下起立 (score={score:.3f})", flush=True)
                do_sit_stand(ser)
                last_wake_ts = now
                peak_recent = 0.0
            elif not args.quiet:
                tick += 1
                if tick % 25 == 0:
                    bar = "█" * int(min(score, 1.0) * 20) + "░" * (20 - int(min(score, 1.0) * 20))
                    print(f"[{ts}] score={score:.3f} (peak={peak_recent:.3f}) [{bar}]",
                          flush=True)
                    peak_recent = 0.0
    except KeyboardInterrupt:
        print("\n[kws] Bye")
    finally:
        hb.stop_()
        ser.close()


if __name__ == "__main__":
    main()