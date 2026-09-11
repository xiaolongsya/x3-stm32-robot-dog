#!/usr/bin/env python3
"""kws_voice_control.py — 机器狗声控(唤醒词开关坐/站)

流程(开关模式,无需命令词训练):
    狗站着 → 喊 "小龙" → 日志 "在呢 → 坐下" → UART 发 "sit"   → 狗子坐下
    狗坐着 → 喊 "小龙" → 日志 "在呢 → 立正" → UART 发 "stand" → 狗子站起来
    (每次喊都切换一次)

用法:
    arecord -D plughw:1,0 -f S16_LE -r 16000 -c 1 -t raw 2>/dev/null \\
        | python3 /usr/local/bin/kws_voice_control.py [阈值]

环境变量:
    KWS_UART      串口设备(默认 /dev/ttyS1)
    KWS_BAUD      波特率(默认 115200)
    KWS_MODEL     wake 模型路径(默认 /tmp/xiaolong.onnx)
    KWS_COOLDOWN  唤醒冷却秒数(默认 1.5)
"""
import os
import sys
import time
import numpy as np

# ── 配置 ──────────────────────────────────────────────
THRESHOLD = float(sys.argv[1]) if len(sys.argv) > 1 else 0.7
UART_DEV = os.environ.get("KWS_UART", "/dev/ttyS1")
UART_BAUD = int(os.environ.get("KWS_BAUD", "115200"))
MODEL_PATH = os.environ.get("KWS_MODEL", "/tmp/xiaolong.onnx")
COOLDOWN = float(os.environ.get("KWS_COOLDOWN", "1.5"))

CHUNK = 1280  # 80ms @ 16kHz,openWakeWord 标准 chunk


# ── UART ──────────────────────────────────────────────
class UartLink:
    """Pi → STM32 串口下发"""

    def __init__(self, dev: str, baud: int):
        import serial  # 延迟 import,没接线也能报错清楚
        self.ser = serial.Serial(dev, baud, timeout=0.5)
        time.sleep(0.5)  # 等串口稳定
        self.ser.reset_input_buffer()

    def send(self, cmd: str, wait: float = 0.3) -> str:
        """发命令,返回 STM32 回执(空字符串 = 没回,不阻塞)"""
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()
        time.sleep(wait)
        try:
            if self.ser.in_waiting:
                return self.ser.readline().decode(errors="ignore").strip()
        except Exception:
            pass
        return ""

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


# ── 主流程 ────────────────────────────────────────────

def main():
    from openwakeword.model import Model

    if not os.path.isfile(MODEL_PATH):
        print(f"[ERR] 唤醒模型不存在: {MODEL_PATH}", flush=True)
        sys.exit(1)

    oww = Model(wakeword_model_paths=[MODEL_PATH])

    # 串口(连不上也不阻塞 —— 只影响下发,不影响识别调试)
    uart = None
    try:
        uart = UartLink(UART_DEV, UART_BAUD)
        print(f"[ctl] ✅ UART OK: {UART_DEV} @ {UART_BAUD}", flush=True)
    except Exception as e:
        print(f"[ctl] ⚠️  UART 打不开({e})— 只识别不下发", flush=True)

    print(f"[ctl] ✅ 唤醒模型: {MODEL_PATH}", flush=True)
    print(f"[ctl] 阈值: {THRESHOLD}  冷却: {COOLDOWN}s", flush=True)
    print(f"[ctl] 模式: 开关(站着→坐 / 坐着→站)", flush=True)
    print("=" * 46, flush=True)
    print("[ctl] 待机中 —— 喊 '小龙' 切换坐/站", flush=True)
    print("=" * 46, flush=True)

    is_sitting = False   # 上电默认是站立姿态(STM32 固件开机写 STAND)
    last_wake_ts = 0.0
    tick = 0

    stdin = sys.stdin.buffer
    chunk_bytes = CHUNK * 2

    while True:
        chunk = stdin.read(chunk_bytes)
        if not chunk or len(chunk) < chunk_bytes:
            break

        pcm = np.frombuffer(chunk, dtype=np.int16)
        preds = oww.predict(pcm)
        ts = time.strftime("%H:%M:%S")
        now = time.time()

        # 取 wake score
        s = preds.get("xiaolong")
        try:
            if isinstance(s, (np.ndarray, list)):
                score = float(s[-1])
            else:
                score = float(s) if s is not None else 0.0
        except (TypeError, IndexError):
            score = 0.0

        if score >= THRESHOLD and (now - last_wake_ts) > COOLDOWN:
            # 切换状态
            if is_sitting:
                uart_cmd = "stand"
                action = "立正"
            else:
                uart_cmd = "sit"
                action = "坐下"

            reply = ""
            if uart:
                try:
                    reply = uart.send(uart_cmd)
                except Exception as e:
                    reply = f"(uart err: {e})"

            print(f"[{ts}] 🌟 在呢 → {action} (score={score:.3f}) "
                  f"→ UART '{uart_cmd}' {reply}", flush=True)

            is_sitting = not is_sitting
            last_wake_ts = now
            oww.reset()
        else:
            tick += 1
            if tick % 25 == 0:  # 每 ≈2 秒一行
                bar = "█" * int(score * 20) + "░" * (20 - int(score * 20))
                print(f"[{ts}] 待机 score={score:.3f} [{bar}]", flush=True)

    if uart:
        uart.close()


if __name__ == "__main__":
    main()
