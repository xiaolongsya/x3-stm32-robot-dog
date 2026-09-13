#!/usr/bin/env python3
"""test_trot_5s.py — 跑 N 秒踏步 + 自动停止(2026-09-12)

用法:
    python3 test_trot_5s.py [seconds]   # 默认 5 秒

⚠️ 测试前确认:
1. STM32 已烧录最新固件(stepping.c @ 753e686)
2. STM32 默认 STAND 状态(上电后)
3. 机器狗放在平稳地面,首次跑手扶住
4. Pi↔STM32 UART 接线 OK(Pi GPIO14/15 ↔ STM32 PA10/PA9)

已知:
- STM32 printf 不回(no-reply bug),脚本不依赖 STM32 回执
- ISR 100Hz 跑步态,main loop 只发命令,即使 printf 卡死也阻断不了 ISR
"""
import os
import sys
import time
import serial

UART_DEV = os.environ.get("KWS_UART", "/dev/ttyS1")
UART_BAUD = int(os.environ.get("KWS_BAUD", "115200"))
SEC = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0


def main():
    print(f"[test] UART: {UART_DEV} @ {UART_BAUD}", flush=True)
    print(f"[test] trot 持续: {SEC}s (周期 2s = {SEC/2:.1f} 个完整步态)", flush=True)

    # 开串口
    try:
        ser = serial.Serial(UART_DEV, UART_BAUD, timeout=0.3)
    except Exception as e:
        print(f"[test] ERR 串口打开失败: {e}", flush=True)
        sys.exit(1)
    print(f"[test] ✅ 串口已开", flush=True)

    time.sleep(0.3)  # 等串口稳定
    ser.reset_input_buffer()

    try:
        # 1. 启动踏步
        print(f"[test] → UART 'step trot'", flush=True)
        ser.write(b"step trot\n")
        ser.flush()

        # 2. 跑 N 秒(进度条)
        t0 = time.time()
        last_print = t0
        while time.time() - t0 < SEC:
            e = time.time() - t0
            if time.time() - last_print >= 0.5:
                bar = "█" * int(e / SEC * 20) + "░" * (20 - int(e / SEC * 20))
                print(f"\r[test] trot {e:5.2f}s / {SEC:.0f}s [{bar}]", end="", flush=True)
                last_print = time.time()
            time.sleep(0.05)
        print(f"\r[test] trot {SEC:5.2f}s / {SEC:.0f}s [{'█'*20}]", flush=True)

        # 3. 停止
        print(f"[test] → UART 'step stop'", flush=True)
        ser.write(b"step stop\n")
        ser.flush()
        time.sleep(0.3)

        # 4. 看 STM32 有没有回(no-reply bug 时无)
        if ser.in_waiting:
            reply = ser.read(ser.in_waiting).decode(errors="ignore").strip()
            print(f"[test] STM32 回: {reply!r}", flush=True)
        else:
            print(f"[test] (无 STM32 回执 — no-reply bug 待修)", flush=True)

    finally:
        ser.close()
        print(f"[test] 串口已关,done", flush=True)


if __name__ == "__main__":
    main()
