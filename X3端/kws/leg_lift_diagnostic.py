#!/usr/bin/env python3
"""单腿小腿方向诊断。仅在机身被托住、四脚不承重时运行。"""

import argparse
from pathlib import Path
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from dog_uart import CMD_SET_PWM, DEFAULT_PORT, DogLink


# 腿名: (小腿舵机编号, STAND PWM, 向狗头方向收腿时的 PWM 符号)
LEGS = {
    "FR": (2, 1600, -1),
    "FL": (5, 1400, +1),
    "BL": (7, 1480, +1),
    "BR": (0, 1600, -1),
}


def pulses(start: int, end: int, step_us: int):
    direction = 1 if end > start else -1
    current = start
    while current != end:
        current += direction * min(step_us, abs(end - current))
        yield current


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("leg", choices=LEGS, help="要检查的腿")
    parser.add_argument("--amplitude", type=int, default=300,
                        help="收腿幅度，默认 300us，范围 50..500us")
    parser.add_argument("--supported", action="store_true",
                        help="确认机身已被托住，四脚不承重")
    args = parser.parse_args()
    if not args.supported:
        parser.error("先托住机身，再加 --supported")
    if not 50 <= args.amplitude <= 500:
        parser.error("--amplitude 必须在 50..500us")
    try:
        occupied = subprocess.run(["fuser", DEFAULT_PORT],
                                  capture_output=True, check=False)
        if occupied.returncode == 0:
            parser.error(f"{DEFAULT_PORT} 正被占用，先停止 KWS worker")
    except FileNotFoundError:
        pass

    servo_id, stand, sign = LEGS[args.leg]
    target = stand + sign * args.amplitude
    link = DogLink()
    try:
        if not link.emergency_stop():
            raise RuntimeError("STM32 未确认 STAND，测试取消")
        print(f"{args.leg} 小腿 servo{servo_id}: {stand} → {target} → {stand}us",
              flush=True)
        for pulse in pulses(stand, target, 50):
            if not link.cmd(CMD_SET_PWM, struct.pack("<BH", servo_id, pulse)):
                raise RuntimeError(f"设定 {pulse}us 失败")
            time.sleep(0.15)
        print("到达目标，保持 1 秒，请观察脚是否向上离地", flush=True)
        time.sleep(1.0)
        for pulse in pulses(target, stand, 50):
            if not link.cmd(CMD_SET_PWM, struct.pack("<BH", servo_id, pulse)):
                raise RuntimeError(f"返回 {pulse}us 失败")
            time.sleep(0.15)
    finally:
        try:
            link.emergency_stop()
        finally:
            link.close()


if __name__ == "__main__":
    main()
