#!/usr/bin/env python3
"""step_debug.py — 8 路舵机交互式分步调试 (2026-09-12)

设计目的:
  step trot 不动,通讯 OK(sit/stand 已验证)。
  分步调试:把抬腿动作拆到单舵机单关节,逐步看机器狗反应,定位问题。

用法:
  python3 step_debug.py

交互:
  [enter]         执行当前预命令 + 自动跳到下一步
  [数字 n]        跳到第 n 步
  [任意文本]      当作 STM32 命令发送(不跳步,可重复测同一动作)
  [q]            退出(自动回 STAND)

预设步骤按"从小到大"递进:
  - 0~1:   起始 / STAND 确认
  - 2~13:  单关节测试(每对角线的大腿/小腿各 +20/-20)
  - 14~25: 单腿全收测试(每条腿的肩+小腿一起动)
  - 26~29: step trot 实测

环境变量:
  KWS_UART   串口设备(默认 /dev/ttyS1)
  KWS_BAUD   波特率(默认 115200)

已知:
  - 不依赖 STM32 printf 回执(no-reply bug)
  - 用户眼睛看机器狗判断动作对不对
  - 任何时候按 Ctrl+C 都会进 finally 回 STAND
"""
import os
import sys
import time
import serial

UART_DEV = os.environ.get("KWS_UART", "/dev/ttyS1")
UART_BAUD = int(os.environ.get("KWS_BAUD", "115200"))

# === 8 路舵机表 (2026-09-12 实测) ===
# (id, name, stand, is_right, safe_min, safe_max)
SERVOS = [
    (0, "BR 小腿", 1600, True,  1400, 1600),
    (1, "BR 肩",   1150, True,  1000, 2000),
    (2, "FR 小腿", 1500, True,  1400, 1600),
    (3, "FR 肩",   1200, True,  1000, 2000),
    (4, "FL 肩",   1820, False, 1000, 2000),
    (5, "FL 小腿", 1500, False, 1400, 1600),
    (6, "BL 肩",   1850, False, 1000, 2000),
    (7, "BL 小腿", 1400, False, 1400, 1600),
]


def lift_pwm(id):
    """收腿 PWM(收 20µs):右腿 -,左腿 +"""
    s = SERVOS[id]
    return s[2] - 20 if s[3] else s[2] + 20


# === 预设步骤 ===
# 每步: (描述, 命令或 None, sleep 或 None)
# 命令 None = 只 sleep 不发命令;sleep None = 发完命令立即返回
STEPS = [
    # 0-1: 起始
    ("0. all 1500 (8 路全中位,确认基本通讯)",          'all 1500',  None),
    ("1. stand (8 路 → STAND 实测 PWM)",               'stand',     None),

    # 2-4: 1,3 脚大腿(对角线 1)
    ("2. FR 肩 → 1180 (1200-20,右腿收腿方向)",        '3 1180',    None),
    ("3. BL 肩 → 1870 (1850+20,左腿收腿方向)",        '6 1870',    None),
    ("4. 回 STAND (验证 1,3 脚肩收腿方向)",           'stand',     None),

    # 5-7: 1,3 脚小腿
    ("5. FR 小腿 → 1480 (1500-20,右腿收腿方向)",      '2 1480',    None),
    ("6. BL 小腿 → 1420 (1400+20,左腿收腿方向)",      '7 1420',    None),
    ("7. 回 STAND (验证 1,3 脚小腿收腿方向)",         'stand',     None),

    # 8-10: 2,4 脚大腿(对角线 2)
    ("8. BR 肩 → 1130 (1150-20,右腿收腿方向)",        '1 1130',    None),
    ("9. FL 肩 → 1840 (1820+20,左腿收腿方向)",        '4 1840',    None),
    ("10. 回 STAND (验证 2,4 脚肩收腿方向)",          'stand',     None),

    # 11-13: 2,4 脚小腿
    ("11. BR 小腿 → 1580 (1600-20,右腿收腿方向)",     '0 1580',    None),
    ("12. FL 小腿 → 1520 (1500+20,左腿收腿方向)",     '5 1520',    None),
    ("13. 回 STAND (验证 2,4 脚小腿收腿方向)",        'stand',     None),

    # 14-16: 单腿 1 (FR) 全收
    ("14. FR 肩 收 20",                                '3 1180',    None),
    ("15. FR 小腿 收 20",                              '2 1480',    None),
    ("16. 回 STAND (FR 全收测试完)",                   'stand',     None),

    # 17-19: 单腿 3 (BL) 全收
    ("17. BL 肩 收 20",                                '6 1870',    None),
    ("18. BL 小腿 收 20",                              '7 1420',    None),
    ("19. 回 STAND (BL 全收测试完)",                   'stand',     None),

    # 20-22: 单腿 2 (FL) 全收
    ("20. FL 肩 收 20",                                '4 1840',    None),
    ("21. FL 小腿 收 20",                              '5 1520',    None),
    ("22. 回 STAND (FL 全收测试完)",                   'stand',     None),

    # 23-25: 单腿 4 (BR) 全收
    ("23. BR 肩 收 20",                                '1 1130',    None),
    ("24. BR 小腿 收 20",                              '0 1580',    None),
    ("25. 回 STAND (BR 全收测试完)",                   'stand',     None),

    # 26-29: 完整踏步测试
    ("26. step trot (启动踏步)",                       'step trot', None),
    ("27. 等 5 秒 (看机器狗动作)",                     None,        5),
    ("28. step stop",                                  'step stop', None),
    ("29. 最终回 STAND",                               'stand',     None),
]


def print_header():
    print("=" * 64, flush=True)
    print("8 路舵机交互式分步调试  (2026-09-12)", flush=True)
    print("=" * 64, flush=True)
    print(f"UART: {UART_DEV} @ {UART_BAUD}", flush=True)
    print(f"背景:step trot 不动 / sit/stand 通讯 OK / 分步定位问题", flush=True)
    print("", flush=True)

    # 打印 SERVO 表
    print("8 路舵机表:", flush=True)
    print(f"  {'id':<3} {'名称':<8} {'STAND':<6} {'方向':<8} {'收腿 PWM':<10} {'limit':<12}", flush=True)
    for s in SERVOS:
        id, name, stand, is_right, smin, smax = s
        dir_str = "右(PWM-)" if is_right else "左(PWM+)"
        lp = lift_pwm(id)
        print(f"  {id:<3} {name:<8} {stand:<6} {dir_str:<8} {lp:<10} ({smin},{smax})", flush=True)
    print("", flush=True)

    # 打印步骤列表
    print(f"预设步骤 ({len(STEPS)} 步):", flush=True)
    for i, (desc, cmd, sleep_s) in enumerate(STEPS):
        extra = f" sleep={sleep_s}s" if sleep_s else ""
        cmd_str = f"'{cmd}'" if cmd else "(仅 sleep)"
        print(f"  [{i:>2}] {desc}  → {cmd_str}{extra}", flush=True)
    print("", flush=True)


def main():
    print_header()

    # 开串口
    try:
        ser = serial.Serial(UART_DEV, UART_BAUD, timeout=0.3)
    except Exception as e:
        print(f"[ERR] 串口打开失败: {e}", flush=True)
        sys.exit(1)
    time.sleep(0.3)
    ser.reset_input_buffer()
    print(f"[test] ✅ 串口已开\n", flush=True)

    cur = 0
    try:
        while True:
            if cur >= len(STEPS):
                print("\n[test] 所有步骤走完,退出", flush=True)
                break

            desc, cmd, sleep_s = STEPS[cur]
            prompt = (
                f"\n[{cur:>2}/{len(STEPS)-1}] {desc}\n"
                f"预命令: {cmd if cmd else '(无)'}, sleep: {sleep_s if sleep_s else 0}s\n"
                f"> [enter] 执行+跳步 | [数字] 跳步 | [文本] 自定义命令(不跳) | [q] 退出: "
            )
            try:
                inp = input(prompt).strip()
            except EOFError:
                print("\n[test] EOF,退出", flush=True)
                break

            if inp.lower() == 'q':
                print("[test] 用户退出", flush=True)
                break

            # 跳步
            if inp.isdigit():
                n = int(inp)
                if 0 <= n < len(STEPS):
                    cur = n
                    continue
                else:
                    print(f"  [warn] 步骤超出范围 0..{len(STEPS)-1}", flush=True)
                    continue

            # 自定义命令(覆盖预命令)
            if inp:
                cmd = inp
                sleep_s = None
                tag = "[自定义]"
            else:
                tag = "[预命令]"

            # 执行
            if cmd:
                try:
                    ser.write((cmd + "\n").encode())
                    ser.flush()
                    print(f"  {tag} → UART: '{cmd}'", flush=True)
                except Exception as e:
                    print(f"  [ERR] 串口写失败: {e}", flush=True)
                    break

            if sleep_s:
                print(f"  → sleep {sleep_s}s ...", flush=True)
                time.sleep(sleep_s)

            # enter(空)才跳步,自定义命令不跳步(可重复测)
            if not inp:
                cur += 1

    except KeyboardInterrupt:
        print("\n[test] Ctrl+C 中断", flush=True)

    finally:
        # 回 STAND
        print("\n[test] 回 STAND 收尾...", flush=True)
        try:
            ser.write(b"stand\n")
            ser.flush()
            time.sleep(0.3)
        except Exception:
            pass
        ser.close()
        print("[test] 串口已关, done", flush=True)


if __name__ == "__main__":
    main()
