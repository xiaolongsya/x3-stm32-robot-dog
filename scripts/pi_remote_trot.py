#!/usr/bin/env python3
"""pi_remote_trot.py — Pi 端远程控制 8 路舵机,模拟 step trot (2026-09-12)

目的:不让 STM32 跑 ISR(避免烧固件)。Pi 端按 100Hz 发 8 路 PWM 命令,
     模拟 STM32 stepping.c 的 sin² 抬腿曲线。

用法:
    python3 pi_remote_trot.py [H_LIFT_mm] [SECS]

默认:H_LIFT=10mm, SECS=5s。

示例:
    python3 pi_remote_trot.py 10 5      # 10mm 抬腿,跑 5 秒
    python3 pi_remote_trot.py 5 10      # 5mm 抬腿,跑 10 秒
    python3 pi_remote_trot.py 15 3      # 15mm 抬腿(逼近 SERVO_LIMIT),跑 3 秒

设计:
- 8 路舵机 STAND PWM 和收腿方向在 SERVOS 表(2026-09-12 实测)
- 每帧算 sin² 抬腿曲线 + 收腿公式(右腿 PWM 减 / 左腿 PWM 增)
- 100Hz 发 8 路命令(每路 "id pulse\n")
- STM32 main.c 已支持 "<id> <pulse>" 命令,无需烧录新固件
- H_LIFT 可在 Pi 端任意调整(改参数即可)

已知限制:
- Python 不是硬实时,100Hz 可能掉到 80-90Hz(lag 监控可见)
- 8 路命令每路 7 字节 = 56 字节/帧,@100Hz = 5600 字节/秒,
  115200 baud 上限 11520 字节/秒,占用 49%
- STM32 处理 8 条命令约 5ms,余量 5ms

如果步态不稳/抖动,说明 Python 实时性不够,建议烧 STM32 跑 ISR。
"""
import os
import sys
import time
import math
import serial

UART_DEV = os.environ.get("KWS_UART", "/dev/ttyS1")
UART_BAUD = int(os.environ.get("KWS_BAUD", "115200"))

# === 参数 ===
H_LIFT = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
PERIOD = 2.0  # 周期(秒)
PWM_PER_MM = 4.0  # 远场近似 1mm ≈ 4µs

# === 8 路舵机表 (2026-09-12 实测,与 stepping.c SERVO_STEP 对齐) ===
SERVOS = [
    # (id, name, STAND, is_right, safe_min, safe_max)
    (0, "BR 小腿", 1600, True,  1400, 1600),
    (1, "BR 肩",   1150, True,  1000, 2000),
    (2, "FR 小腿", 1500, True,  1400, 1600),
    (3, "FR 肩",   1200, True,  1000, 2000),
    (4, "FL 肩",   1820, False, 1000, 2000),
    (5, "FL 小腿", 1500, False, 1400, 1600),
    (6, "BL 肩",   1850, False, 1000, 2000),
    (7, "BL 小腿", 1400, False, 1400, 1600),
]


def calc_pwm(h_lift, phase):
    """计算 8 路 PWM(与 stepping.c stepping_trot_step 算法一致)

    phase ∈ [0, 1)
    """
    if phase < 0.5:
        phase_in_swing = phase * 2.0
        swing_ids = {2, 3, 6, 7}  # 腿 1+3 swing: FR + BL
    else:
        phase_in_swing = (phase - 0.5) * 2.0
        swing_ids = {0, 1, 4, 5}  # 腿 2+4 swing: BR + FL

    sin_val = math.sin(math.pi * phase_in_swing)
    h_mm = h_lift * sin_val * sin_val
    ds_pwm = h_mm * PWM_PER_MM

    pwms = []
    for id, name, stand, is_right, smin, smax in SERVOS:
        if id in swing_ids:
            pwm = stand - ds_pwm if is_right else stand + ds_pwm
        else:
            pwm = stand
        # clamp 到 SERVO_LIMIT
        if pwm < smin: pwm = smin
        if pwm > smax: pwm = smax
        pwms.append(int(pwm + 0.5))
    return pwms


def write_stand(ser):
    """写 8 路 STAND"""
    for s in SERVOS:
        ser.write(f"{s[0]} {s[2]}\n".encode())
    ser.flush()


def main():
    print(f"[pi-trot] UART: {UART_DEV} @ {UART_BAUD}", flush=True)
    print(f"[pi-trot] H_LIFT: {H_LIFT} mm, PERIOD: {PERIOD}s, 时长: {SECS}s", flush=True)

    try:
        ser = serial.Serial(UART_DEV, UART_BAUD, timeout=0.1)
    except Exception as e:
        print(f"[pi-trot] ERR 串口开失败: {e}", flush=True)
        sys.exit(1)

    time.sleep(0.3)
    ser.reset_input_buffer()

    # 起始 STAND
    write_stand(ser)
    print(f"[pi-trot] 起始 STAND 已写", flush=True)
    time.sleep(0.5)

    tick_period = PERIOD / 100.0  # 100Hz
    n_ticks = int(SECS / tick_period)
    print(f"[pi-trot] 总 tick: {n_ticks}, 周期: {tick_period*1000:.1f}ms", flush=True)
    print(f"[pi-trot] 按 Ctrl+C 可随时中断(自动回 STAND)", flush=True)
    print("", flush=True)

    t_start = time.time()
    tick = 0
    max_lag = 0.0

    try:
        while tick < n_ticks:
            phase = (tick * tick_period / PERIOD) % 1.0
            pwms = calc_pwm(H_LIFT, phase)

            t_now = time.time() - t_start
            t_target = tick * tick_period
            lag = t_target - t_now  # 正数 = 落后
            if lag > max_lag:
                max_lag = lag

            # 发 8 路 PWM(每路 7 字节)
            for id, pwm in enumerate(pwms):
                ser.write(f"{id} {pwm}\n".encode())
            ser.flush()

            # 进度(每秒打印一次 = 每 100 tick)
            if tick % 100 == 0:
                phase_in_swing = phase * 2 if phase < 0.5 else (phase - 0.5) * 2
                sin_val = math.sin(math.pi * phase_in_swing)
                h_mm = H_LIFT * sin_val * sin_val
                pair = "1+3" if phase < 0.5 else "2+4"
                print(f"[pi-trot] tick={tick:>4} t={t_now:.2f}s phase={phase:.3f} "
                      f"pair={pair} h={h_mm:.1f}mm lag={lag*1000:+.1f}ms",
                      flush=True)

            tick += 1

            # 等待下一 tick
            next_t = t_start + tick * tick_period
            sleep_s = next_t - time.time()
            if sleep_s > 0:
                time.sleep(sleep_s)
            # 如果 sleep_s < 0,落后了,立即继续(不补)

    except KeyboardInterrupt:
        print(f"\n[pi-trot] Ctrl+C 中断", flush=True)

    finally:
        # 回 STAND
        try:
            write_stand(ser)
        except Exception:
            pass
        time.sleep(0.3)
        ser.close()
        print(f"[pi-trot] 回 STAND, 串口关", flush=True)
        print(f"[pi-trot] max lag: {max_lag*1000:.1f}ms "
              f"(>5ms 说明 Python 实时性不够,考虑烧 STM32 跑 ISR)", flush=True)


if __name__ == "__main__":
    main()
