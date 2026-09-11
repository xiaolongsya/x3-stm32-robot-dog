#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
机器狗舵机串口控制脚本
=====================
通过 Pi Zero3 26-pin 排针的 UART5 (PH2/PH3) = /dev/ttyS1 @ 115200 8N1
与 STM32G431 USART1 (PA9=TX / PA10=RX) 通信。

STM32 命令协议 (main.c parse_uart_command):
  <id 0-7> <pulse 500-2500>    设单路舵机  -> 回 "OK s<N>=<P>"
  all <pulse>                   设全部 8 路  -> 回 "OK all=<P>"
  center                        全部居中(1500) -> 回 "OK center"
  非法格式                       -> 回 "ERR fmt"
  id > 7                        -> 回 "ERR id>7"

舵机参数 (py-apple-dynamics 借鉴):
  freq=50Hz, min_us=500 (0°), max_us=2500 (180°), 中位=1500µs=90°
  Pulse = 500 + (angle/180) * 2000
  合法范围 500-2500, 超出 STM32 自动忽略

舵机 ID 映射表 (STM32G431 main.c set_servo_pulse):
  0: PA2  TIM2_CH3  servo0
  1: PA3  TIM2_CH4  servo1
  2: PA4  TIM3_CH2  servo2
  3: PA5  TIM2_CH1  servo3
  4: PA6  TIM3_CH1  servo4
  5: PA7  TIM17_CH1 servo5
  6: PB0  TIM3_CH3  servo6
  7: PA8  TIM1_CH1  servo7

接线:
  Pi Zero3 26-pin pin 8  (TXD, PH2) ---- STM32 PA10 (USART1_RX)
  Pi Zero3 26-pin pin 10 (RXD, PH3) ---- STM32 PA9  (USART1_TX)
  Pi Zero3 26-pin pin 6  (GND)    ---- STM32 GND
"""

import argparse
import sys
import time

import serial

# ==================== 配置 ====================
SERIAL_PORT = '/dev/ttyS1'
BAUDRATE = 115200
TIMEOUT = 1.0  # 秒, 舵机命令响应 <10ms, 留余量

PULSE_MIN = 500
PULSE_MAX = 2500
PULSE_CENTER = 1500  # 90° 几何中位 (站立姿态: 大腿水平 + 小腿垂直)

SERVO_MAP = {
    0: 'PA2  TIM2_CH3  servo0',
    1: 'PA3  TIM2_CH4  servo1',
    2: 'PA4  TIM3_CH2  servo2',
    3: 'PA5  TIM2_CH1  servo3',
    4: 'PA6  TIM3_CH1  servo4',
    5: 'PA7  TIM17_CH1 servo5',
    6: 'PB0  TIM3_CH3  servo6',
    7: 'PA8  TIM1_CH1  servo7',
}


# ==================== 串口 ====================
def open_serial(port=SERIAL_PORT, baudrate=BAUDRATE, timeout=TIMEOUT):
    """打开 Pi UART5 串口, 115200 8N1 无流控"""
    return serial.Serial(
        port=port,
        baudrate=baudrate,
        timeout=timeout,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        rtscts=False,  # 无硬件流控
        xonxoff=False,  # 无软件流控
        dsrdtr=False,
    )


# ==================== 底层命令 ====================
def send_cmd(ser, cmd):
    """发送一条命令(自动补 \\n), 读取一行回包"""
    if not cmd.endswith('\n'):
        cmd += '\n'
    ser.write(cmd.encode('ascii'))
    ser.flush()
    line = ser.readline().decode('ascii', errors='replace').strip()
    return line


def set_servo(ser, servo_id, pulse):
    """设单路舵机. servo_id=0..7, pulse=500..2500"""
    if not (0 <= servo_id <= 7):
        return f'ERR local: id {servo_id} out of range [0,7]'
    if not (PULSE_MIN <= pulse <= PULSE_MAX):
        return f'ERR local: pulse {pulse} out of range [{PULSE_MIN},{PULSE_MAX}]'
    return send_cmd(ser, f'{servo_id} {pulse}')


def set_all(ser, pulse):
    """设全部 8 路. pulse=500..2500"""
    if not (PULSE_MIN <= pulse <= PULSE_MAX):
        return f'ERR local: pulse {pulse} out of range [{PULSE_MIN},{PULSE_MAX}]'
    return send_cmd(ser, f'all {pulse}')


def center(ser):
    """全部舵机居中 (Pulse=1500, 站立姿态)"""
    return send_cmd(ser, 'center')


# ==================== 自检 ====================
def self_test():
    """8 步自检: center → 单路最小/中位/最大 → 全路摆动 → 回到中心"""
    print('=== 机器狗舵机串口自检 ===')
    print(f'端口: {SERIAL_PORT} @ {BAUDRATE} 8N1 (无流控)')
    print(f'接线: Pi 26-pin pin 8/10 <-> STM32 PA10/PA9, GND <-> GND')
    print()
    print('打开串口 ...', end=' ', flush=True)
    try:
        ser = open_serial()
    except Exception as e:
        print(f'FAIL: {e}')
        return 1
    print(f'OK ({ser.name})')
    time.sleep(0.1)

    cases = [
        ('center       ', lambda: center(ser),                       'OK center'),
        ('set 0 1500   ', lambda: set_servo(ser, 0, 1500),           'OK s0=1500'),
        ('set 7 2500   ', lambda: set_servo(ser, 7, 2500),           'OK s7=2500'),
        ('set 3 500    ', lambda: set_servo(ser, 3, 500),            'OK s3=500'),
        ('set 5 2000   ', lambda: set_servo(ser, 5, 2000),           'OK s5=2000'),
        ('all 1500     ', lambda: set_all(ser, 1500),                'OK all=1500'),
        ('all 2000     ', lambda: set_all(ser, 2000),                'OK all=2000'),
        ('all 1000     ', lambda: set_all(ser, 1000),                'OK all=1000'),
        ('center       ', lambda: center(ser),                       'OK center'),
    ]

    print()
    print(f'{"步骤":<14} {"命令":<14} {"期望":<14} {"实际":<14} 结果')
    print('-' * 70)
    passed = 0
    for label, fn, expected in cases:
        actual = fn()
        ok = actual == expected
        passed += int(ok)
        mark = '✓' if ok else '✗'
        print(f'{label:<14} {expected:<14} {expected:<14} {actual:<14} {mark}')

    print()
    print(f'=== 结果: {passed}/{len(cases)} 通过 ===')
    ser.close()
    return 0 if passed == len(cases) else 2


# ==================== CLI ====================
def main_cli():
    parser = argparse.ArgumentParser(
        description='机器狗舵机串口控制 (Pi /dev/ttyS1 <-> STM32 USART1)',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
例子:
  %(prog)s set 0 1500        # 设 0 号舵机到 1500µs (90° 中位)
  %(prog)s all 2000          # 全部 8 路设到 2000µs
  %(prog)s center            # 全部回到 1500µs 中位
  %(prog)s list              # 列出舵机 ID 映射
  %(prog)s                   # (无参数) 跑 9 步自检
""",
    )
    sub = parser.add_subparsers(dest='cmd')

    p_set = sub.add_parser('set', help='设单路舵机')
    p_set.add_argument('id', type=int, choices=range(8), metavar='ID', help='舵机 ID (0-7)')
    p_set.add_argument('pulse', type=int, help='脉宽 (500-2500)')

    p_all = sub.add_parser('all', help='设全部 8 路舵机')
    p_all.add_argument('pulse', type=int, help='脉宽 (500-2500)')

    sub.add_parser('center', help='全部居中 (1500µs = 90°)')

    sub.add_parser('list', help='列出舵机 ID <-> 引脚映射表')

    args = parser.parse_args()

    if args.cmd is None:
        # 无子命令: 跑自检
        sys.exit(self_test())

    if args.cmd == 'list':
        print('=== 舵机 ID 映射表 (STM32G431 main.c set_servo_pulse) ===')
        for i, m in SERVO_MAP.items():
            print(f'  {i}: {m}')
        print()
        print('=== Pi UART 接线 ===')
        print('  Pi Zero3 26-pin pin 8  (TXD, PH2 = UART5_TX) ---> STM32 PA10 (USART1_RX)')
        print('  Pi Zero3 26-pin pin 10 (RXD, PH3 = UART5_RX) <--- STM32 PA9  (USART1_TX)')
        print('  Pi Zero3 26-pin pin 6  (GND)                  ---  STM32 GND')
        print('  Linux 设备: /dev/ttyS1 (H616 serial@5001400, UART5)')
        return

    with open_serial() as ser:
        time.sleep(0.05)
        if args.cmd == 'set':
            print(set_servo(ser, args.id, args.pulse))
        elif args.cmd == 'all':
            print(set_all(ser, args.pulse))
        elif args.cmd == 'center':
            print(center(ser))


if __name__ == '__main__':
    main_cli()
