#!/usr/bin/env python3
"""PC 端 STM32 二进制协议命令脚本
 * 用法: python send_binary.py <COM口> <command> [args]
 *   python send_binary.py COM7 stand         (MOTION_PLAY id=1 STAND, duration=1ms)
 *   python send_binary.py COM7 bob           (MOTION_PLAY id=3 BOB)
 *   python send_binary.py COM7 trot          (MOTION_PLAY id=2 TROT)
 *   python send_binary.py COM7 stop          (EMERGENCY_STOP)
 *   python send_binary.py COM7 heart         (HEARTBEAT)
 *   python send_binary.py COM7 pwm 0 1500    (SET_PWM servo0=1500)
 *   python send_binary.py COM7 raw <hex>     (raw hex frame)
"""
import serial
import sys
import time

def csum8(data):
    c = 0
    for b in data:
        c ^= b
    return c

def frame(cmd, data):
    """生成协议帧: AA 55 <len> <cmd> <data...> <csum8>"""
    payload = bytes([len(data), cmd]) + data
    cs = csum8(payload)
    return bytes([0xAA, 0x55]) + payload + bytes([cs])

def send(s, fr):
    s.reset_input_buffer()
    s.write(fr)
    time.sleep(0.3)
    return s.read(64)

MOTIONS = {
    "stand": 1,    # MOTION_STAND
    "trot": 2,     # MOTION_TROT
    "bob": 3,      # MOTION_BOB
    "shin": 4,     # MOTION_SHIN_TEST
    "stop": -1,    # EMERGENCY_STOP (特殊处理)
    "heart": -1,   # HEARTBEAT (特殊处理)
}

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    com, cmd = sys.argv[1], sys.argv[2]
    s = serial.Serial(com, 115200, timeout=2)
    print(f"打开 {com} @ 115200")

    if cmd == "raw":
        fr = bytes.fromhex(sys.argv[3])
    elif cmd == "pwm":
        sid = int(sys.argv[3])
        pulse = int(sys.argv[4])
        # SET_PWM (0x03): data = [(id u8, pulse u16 LE) * N]
        fr = frame(0x03, bytes([sid, pulse & 0xFF, (pulse >> 8) & 0xFF]))
    elif cmd == "stop":
        # EMERGENCY_STOP (0x06): data=[]
        fr = frame(0x06, b"")
    elif cmd == "heart":
        # HEARTBEAT (0x05): data=[]
        fr = frame(0x05, b"")
    elif cmd in MOTIONS and MOTIONS[cmd] > 0:
        mid = MOTIONS[cmd]
        # MOTION_PLAY (0x01): data=[id u8, duration_ms u32 LE]
        # duration = 1ms (短促触发,stand 应该保持)
        fr = frame(0x01, bytes([mid, 0x01, 0x00, 0x00, 0x00]))
    else:
        print(f"未知命令: {cmd}")
        sys.exit(1)

    print(f"发送: {fr.hex(' ')}")
    resp = send(s, fr)
    print(f"回包: {resp.hex(' ') if resp else '(none)'}")
    print(f"回包原文: {resp}")
    s.close()