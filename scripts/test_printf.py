#!/usr/bin/env python3
"""test_printf.py — 触发 STM32 printf,方便逻辑分析仪抓 PA9 波形 (2026-09-12)

用法:
    python3 test_printf.py [DEVICE] [CMD1 CMD2 ...]

默认:
    DEVICE = /dev/ttyS1
    CMD    = sit stand center all 1500 step trot step stop

每个命令间隔 1s,留时间给逻辑分析仪抓 PA9 波形。

示例:
    python3 test_printf.py                          # 默认设备 + 默认命令
    python3 test_printf.py /dev/ttyS0 sit          # 用 ttyS0,只发 sit
    python3 test_printf.py /dev/ttyS1 sit stand    # ttyS1,两个命令
"""
import sys
import time
import serial

DEVICE = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyS1"
COMMANDS = sys.argv[2:] if len(sys.argv) > 2 else [
    "sit", "stand", "center", "all 1500", "step trot", "step stop"
]


def main():
    print(f"[test] DEVICE: {DEVICE} @ 115200", flush=True)
    print(f"[test] COMMANDS: {COMMANDS}", flush=True)
    print(f"[test] ⚠ 同步:在 Pi 上跑本脚本,逻辑分析仪抓 STM32 PA9 波形", flush=True)
    print("", flush=True)

    try:
        s = serial.Serial(DEVICE, 115200, timeout=0.5)
    except Exception as e:
        print(f"[test] ERR 串口打开失败: {e}", flush=True)
        sys.exit(1)

    time.sleep(0.3)
    s.reset_input_buffer()

    try:
        for cmd in COMMANDS:
            print(f"[test] t={time.time():.1f} → '{cmd}'", flush=True)
            s.write((cmd + "\n").encode())
            s.flush()
            time.sleep(1.0)  # 给逻辑分析仪抓波形的时间

        print("", flush=True)
        print(f"[test] done — 检查逻辑分析仪 STM32 PA9 通道", flush=True)

    except KeyboardInterrupt:
        print("[test] Ctrl+C 中断", flush=True)

    finally:
        s.close()


if __name__ == "__main__":
    main()
