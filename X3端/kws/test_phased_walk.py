#!/usr/bin/env python3
"""手扶机身测试一轮两段式 WALK，打印预计动作阶段。"""

import argparse
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from dog_uart import DEFAULT_PORT, DogLink


LEGS = ("FR 右前", "FL 左前", "BL 左后", "BR 右后")


def wait_until(deadline: float):
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        time.sleep(min(remaining, 0.05))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="仅适用于已烧录提交 7979449 或更新固件的 STM32。")
    parser.add_argument("--direction", choices=("forward", "backward"),
                        default="forward", help="默认前进")
    parser.add_argument("--supported", action="store_true",
                        help="确认机身已托住，不会因抬腿而倾倒")
    args = parser.parse_args()
    if not args.supported:
        parser.error("先托住机身，再加 --supported")
    try:
        occupied = subprocess.run(["fuser", DEFAULT_PORT],
                                  capture_output=True, check=False)
        if occupied.returncode == 0:
            parser.error(f"{DEFAULT_PORT} 正被占用，先停止 KWS worker")
    except FileNotFoundError:
        pass

    direction = 1 if args.direction == "forward" else -1
    link = DogLink()
    try:
        if not link.emergency_stop():
            raise RuntimeError("STM32 未确认 STAND，测试取消")
        print("请确认 STM32 已烧录 7979449 或更新固件，机身保持托住。", flush=True)
        print("3 秒后开始一轮 8 秒步态；Ctrl+C 可中断。", flush=True)
        for second in (3, 2, 1):
            print(f"  {second}...", flush=True)
            time.sleep(1)
        if not link.walk(direction, 8):
            raise RuntimeError("STM32 未确认 WALK，测试取消")

        start = time.monotonic()
        for leg_index, leg_name in enumerate(LEGS):
            phase = leg_index * 2
            print(f"{phase:>2}s  预计 {leg_name} 收腿、送脚；其他三腿保持", flush=True)
            wait_until(start + phase + 1)
            print(f"{phase + 1:>2}s  预计 {leg_name} 落脚；四脚一起缓慢推地", flush=True)
            wait_until(start + phase + 2)
        print(" 8s  一轮结束，回 STAND", flush=True)
    except KeyboardInterrupt:
        print("\n已中断，发送回站姿命令", flush=True)
    finally:
        try:
            link.emergency_stop()
        finally:
            link.close()


if __name__ == "__main__":
    main()
