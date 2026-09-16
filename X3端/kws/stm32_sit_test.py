#!/usr/bin/env python3
"""stm32_sit_test.py — 单独测试 STM32 蹲下动作(诊断 KWS 链路)

2026-09-16:写来验证 X3 ↔ STM32 UART 物理链路 + STM32 watchdog 心跳

用法(SX3 上):
    python3 /root/kws/stm32_sit_test.py

输出:
    ✅ 打开 /dev/ttyS3@115200
    ✅ 心跳守护: 100ms
    发送 ACTION_PLAY id=5 (= 蹲下)
    等待 ACK (2 秒)...
    收到字节: aaaa55028900xx  ← ACK 帧
    ✅ 收到 ACK, status=0 (OK)
    狗子应该正在蹲下...

排查表(没收到 ACK 时):
    - 狗子上电了吗(LED 亮)?
    - STM32 固件烧录了吗(bootloader 是否能进)?
    - X3 ↔ STM32 UART 线序对吗(ttyS3 TX → STM32 PA10 RX, ttyS3 RX → STM32 PA9 TX)?
    - 波特率对吗(STM32 USART1 = 115200)?
    - X3 的 ttyS3 真接的 USART1 吗(不是别的串口)?

中止: Ctrl+C
"""
import sys
import threading
import time

import serial

PORT = "/dev/ttyS3"
BAUD = 115200


def csum8(data: bytes) -> int:
    c = 0
    for b in data:
        c ^= b
    return c & 0xFF


def send(ser: serial.Serial, cmd: int, payload: bytes = b"") -> None:
    body = bytes([len(payload), cmd]) + payload
    ser.write(b"\xaa\x55" + body + bytes([csum8(body)]))
    ser.flush()


def heartbeat_loop(ser: serial.Serial, stop: threading.Event):
    while not stop.is_set():
        send(ser, 0x05)  # CMD_HEARTBEAT
        stop.wait(0.1)


def main():
    try:
        ser = serial.Serial(PORT, BAUD, timeout=1.0)
        print(f"[test] ✅ 打开 {PORT}@{BAUD}")
    except Exception as e:
        print(f"[test] ❌ 串口打不开: {e}")
        sys.exit(1)

    stop = threading.Event()
    threading.Thread(target=heartbeat_loop, args=(ser, stop), daemon=True).start()
    print(f"[test] ✅ 心跳守护: 100ms(防 STM32 watchdog 200ms 超时)")

    # 1) 发 ACTION_PLAY id=5(蹲下)
    print(f"[test] ▶ 发送 ACTION_PLAY id=5 (= 蹲下, 渐进 800ms)")
    send(ser, 0x09, bytes([5, 1]))  # repeat=1

    # 2) 读 ACK(2 秒窗口)
    print(f"[test] ⏳ 等待 ACK (2 秒)...")
    buf = b""
    deadline = time.time() + 2.0
    while time.time() < deadline:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
        time.sleep(0.05)
    print(f"[test] 收到 {len(buf)} 字节: {buf.hex() if buf else '(空)'}")

    # 3) 解析 ACK(cmd=0x09 → ack_cmd=0x89)
    # ACK 帧格式: AA 55 [data_len] [cmd|0x80] [status] [data...] [csum]
    # status 占 1 字节,无额外 data 时 data_len=1(不是 2)
    ACK_CMD = 0x89
    head = bytes([0xAA, 0x55, 0x01, ACK_CMD])  # 修正: 0x01 不是 0x02
    if head in buf:
        idx = buf.find(head)
        if len(buf) >= idx + 5:
            status = buf[idx + 4]
            print(f"[test] ✅ 收到 ACK, status={status} ({'OK' if status == 0 else f'ERR {status}'})")
            if status == 0:
                # 4) 等蹲下完成(ramp 800ms)
                print(f"[test] ⏳ 蹲下中(渐进 800ms)...")
                time.sleep(1.5)
                print(f"[test] ▶ 发送 ACTION_PLAY id=6 (= 起立, 渐进 800ms)")
                send(ser, 0x09, bytes([6, 1]))
                time.sleep(1.5)
                print(f"[test] ✅ 完整流程跑完(sit + stand)")
                print(f"[test] 按 Ctrl+C 退出(否则心跳会一直跑)")
            else:
                print(f"[test] ❌ ACK status={status}(STM32 commands.h 看状态码含义)")
        else:
            print(f"[test] ⚠️ ACK 帧不完整")
    else:
        print(f"[test] ❌ 没收到 ACK 帧头 AA 55 02 89")
        print(f"[test] 排查:")
        print(f"   1) 狗子上电了吗(STM32 板载 LED 亮)?")
        print(f"   2) STM32 固件烧录了吗(看 .bin 文件时间戳 / 能不能进 bootloader)?")
        print(f"   3) X3 ↔ STM32 UART 线序对吗(ttyS3 ↔ PA9/PA10)?")
        print(f"   4) X3 的 ttyS3 真接的是 STM32 吗(不是别的串口)?")

    stop.set()
    ser.close()


if __name__ == "__main__":
    main()