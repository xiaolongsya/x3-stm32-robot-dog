#!/usr/bin/env python3
"""PC 端 STM32 UART 命令脚本 (兼容文本模式 + 二进制协议)
 * 用法: python send_command.py <COM口> <text_cmd|binary> [args...]
 *   text: python send_command.py COM7 stand
 *         python send_command.py COM7 center
 *         python send_command.py COM7 bob
 *         python send_command.py COM7 0 1500    (set servo0=1500)
 *   bin:  python send_command.py COM7 bin AA550501030100000008
"""
import serial
import sys
import time

def send_text(s, cmd):
    """发文本命令 (回显 + OK/ERR)"""
    s.reset_input_buffer()
    s.write((cmd + "\n").encode())
    time.sleep(0.3)
    return s.read(128)

def send_binary(s, hex_frame):
    """发二进制协议帧"""
    s.reset_input_buffer()
    s.write(bytes.fromhex(hex_frame))
    time.sleep(0.3)
    return s.read(64)

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("用法:")
        print("  python send_command.py COM7 stand        (文本模式)")
        print("  python send_command.py COM7 center        (8路=1500)")
        print("  python send_command.py COM7 bob          (站↔蹲循环)")
        print("  python send_command.py COM7 0 1500      (servo0=1500)")
        print("  python send_command.py COM7 bin AA550... (二进制帧)")
        sys.exit(1)

    com = sys.argv[1]
    mode = sys.argv[2]

    s = serial.Serial(com, 115200, timeout=2)
    print(f"打开 {com} @ 115200")

    if mode == "bin":
        resp = send_binary(s, sys.argv[3])
    else:
        # 文本模式: sys.argv[2..] 拼成命令
        cmd = " ".join(sys.argv[2:])
        resp = send_text(s, cmd)

    print(f"回包 hex: {resp.hex(' ') if resp else '(none)'}")
    print(f"回包: {resp}")
    s.close()