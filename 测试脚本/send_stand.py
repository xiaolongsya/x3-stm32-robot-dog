#!/usr/bin/env python3
"""PC 端 STM32 UART 测试脚本(替代 tabby 输入)
 * 用法: python send_stand.py <COM口>  例如 COM7
 * 功能: 通过 CP2102 发送 'stand\n' 给 STM32, 打印 STM32 回包
"""
import serial
import sys
import time

if len(sys.argv) < 2:
    print("用法: python send_stand.py COM7")
    sys.exit(1)

com = sys.argv[1]
print(f"打开 {com} 115200 8N1 ...")
s = serial.Serial(com, 115200, timeout=2)

# 清空接收缓冲
s.reset_input_buffer()

# 发送 stand 命令
print("发送: stand")
s.write(b"stand\n")
time.sleep(0.5)

# 读回包
resp = s.read(64)
print(f"回包: {resp}")
print(f"回包 hex: {resp.hex(' ')}")
s.close()