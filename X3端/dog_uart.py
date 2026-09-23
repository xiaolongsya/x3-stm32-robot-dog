#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dog_uart.py — X3 端机器狗控制 CLI (2026-09-16)

用法(在 X3 上):
    python3 dog_uart.py sit            # 蹲下(渐进 800ms),永久保持
    python3 dog_uart.py sit 3          # 蹲下,保持 3 秒后回 STAND
    python3 dog_uart.py stand          # 站立(渐进)
    python3 dog_uart.py squat 5        # 蹲下起立循环 5 次
    python3 dog_uart.py trot 10        # 踏步 10 秒
    python3 dog_uart.py forward 8      # 前进一轮约 8 秒(需新版 STM32 固件)
    python3 dog_uart.py backward 8     # 后退一轮约 8 秒(需新版 STM32 固件)
    python3 dog_uart.py bob 15         # 蹲起循环动作 15 秒
    python3 dog_uart.py seq "sit 2; stand 1; squat 3"   # 动作组合
    python3 dog_uart.py stop           # 急停(立即回 STAND)
    python3 dog_uart.py pwm 0 1500     # 单路舵机
    python3 dog_uart.py raw AA5505...  # 原始帧

协议(与 STM32 commands.c 一致):
    帧:   AA 55 len cmd data... csum8   (csum8 = XOR(len,cmd,data))
    ACK:  AA 55 len (cmd|0x80) status data csum8
          status: 0=OK 1=CRC_ERR 2=BAD_CMD 3=BAD_PARAM 4=BAD_LEN

安全设计:
    - 后台心跳线程 100ms 一发 → watchdog 不超时,姿态保持
    - 脚本退出(Ctrl+C / 结束)= 心跳停 → STM32 200ms 后自动回 STAND
"""

import argparse
import serial
import struct
import sys
import threading
import time

DEFAULT_PORT = "/dev/ttyS3"
DEFAULT_BAUD = 115200

# === 命令码(STM32 commands.c) ===
CMD_MOTION_PLAY     = 0x01
CMD_SET_PWM         = 0x03
CMD_HEARTBEAT       = 0x05
CMD_EMERGENCY_STOP  = 0x06
CMD_ACTION_PLAY     = 0x09

# === ACTION_PLAY 动作 id(motions.c 2026-09-16) ===
ACTION_SIT_DOWN     = 5   # 任意 → 真实蹲姿(渐进 800ms)
ACTION_STAND_UP     = 6   # 任意 → STAND(渐进 1200ms)
ACTION_SIT_TO_STAND = 7   # = STAND_UP 别名
ACTION_STAND_TO_SIT = 8   # = SIT_DOWN 别名

# STM32 motions.c 的 ramp 固定时长(SIT_RAMP_MS / STAND_RAMP_MS)
# 发完动作后至少等这么久,才能发下一个(否则 STM32 会 ramp_cancel 掉上一个)
ACTION_RAMP_MS = {5: 800, 6: 1200, 7: 1200, 8: 800}

STATUS_NAMES = {0: "OK", 1: "CRC_ERR", 2: "BAD_CMD", 3: "BAD_PARAM", 4: "BAD_LEN"}
RAMP_MS = 800   # 兼容旧代码(SIT 默认);新代码用 ACTION_RAMP_MS[aid]

ACK  = 0x80
HEAD = b"\xaa\x55"


def csum8(data: bytes) -> int:
    c = 0
    for b in data:
        c ^= b
    return c & 0xFF


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    body = bytes([len(payload), cmd]) + payload
    return HEAD + body + bytes([csum8(body)])


class DogLink:
    def __init__(self, port=DEFAULT_PORT, baud=DEFAULT_BAUD, heartbeat=True):
        self.ser = serial.Serial(port, baud, timeout=0.5)
        # 2026-09-17 修 M1:心跳线程与主线程共用 self.ser,write 不是线程安全的。
        # 两帧几乎同时写会导致字节交错 → STM32 看到错位帧头 → CRC_ERR。
        # 所有 ser.write 走这把锁(pyserial 的 write 内部不保证原子)。
        self._tx_lock = threading.Lock()
        self._hb_stop = threading.Event()
        self._hb_thread = None
        self.last_cmd_error = None   # 最近一次 cmd 失败原因(None=成功)2026-09-17 加
        if heartbeat:
            self._hb_thread = threading.Thread(target=self._heartbeat_loop, daemon=True)
            self._hb_thread.start()

    # === 心跳:保持 watchdog 不超时,姿态得以维持 ===
    def _heartbeat_loop(self):
        frame = build_frame(CMD_HEARTBEAT)
        while not self._hb_stop.is_set():
            try:
                with self._tx_lock:
                    self.ser.write(frame)
            except Exception:
                pass
            self._hb_stop.wait(0.1)

    def close(self):
        self._hb_stop.set()
        if self._hb_thread:
            self._hb_thread.join(timeout=1.0)
        self.ser.close()

    # === 底层收发 ===
    def send_raw(self, data: bytes):
        with self._tx_lock:
            self.ser.write(data)
            self.ser.flush()

    def read_ack(self, cmd: int, timeout=0.5):
        """读 ACK(容忍 STM32 夹发的 'OK\\n' 诊断文本)。返回 (status, data) 或 None"""
        deadline = time.time() + timeout
        buf = b""
        while time.time() < deadline:
            n = self.ser.in_waiting
            if n:
                buf += self.ser.read(n)
            # 找帧头
            idx = buf.find(HEAD)
            if idx >= 0 and len(buf) >= idx + 4:
                length = buf[idx + 2]
                total = 4 + length + 1
                if len(buf) >= idx + total:
                    frame = buf[idx: idx + total]
                    cs = csum8(frame[2: 4 + length])
                    if cs != frame[4 + length]:
                        buf = buf[idx + 1:]
                        continue
                    ack_cmd = frame[3]
                    if ack_cmd != (cmd | ACK):
                        buf = buf[idx + 1:]
                        continue
                    status = frame[4]
                    data = frame[5: 4 + length]
                    return status, data
                buf = buf[idx:]
            time.sleep(0.01)
        return None

    def cmd(self, cmd_id: int, payload: bytes = b"", timeout=0.5, retries=1):
        """发一帧 + 等 ACK。返回 True/False

        2026-09-17 修 M2:原先无 ACK 就静默丢,现在默认重试 1 次。
        重试对现有命令是安全的 —— 都是"设到某个姿态"的幂等操作
        (ACTION_PLAY 重发 = 再 ramp 到同一目标;EMERGENCY_STOP 重发无副作用)。
        最坏代价:真断线时多花 timeout×retries 秒。
        """
        for attempt in range(retries + 1):
            self.send_raw(build_frame(cmd_id, payload))
            r = self.read_ack(cmd_id, timeout)
            if r is not None:
                status, _ = r
                name = STATUS_NAMES.get(status, f"?{status}")
                print(f"  {'✓' if status == 0 else '✗'} cmd 0x{cmd_id:02X} → {name}")
                self.last_cmd_error = None if status == 0 else name
                return status == 0
            if attempt < retries:
                print(f"  ↻ cmd 0x{cmd_id:02X} 无 ACK,重试 {attempt + 1}/{retries}")
        print(f"  ✗ cmd 0x{cmd_id:02X} 无 ACK(已重试 {retries} 次)")
        self.last_cmd_error = "NO_ACK"
        return False

    # === 高层动作 ===
    def action(self, action_id: int, hold_ms: int = 0):
        """ACTION_PLAY 0x09: [action_id u8, repeat u8, hold_ms u16 LE]

        hold_ms(仅 id 5..8 有效,2026-09-17 加):
          = 0 → ramp 完成后保持终点姿态(不回 STAND)
          > 0 → ramp 完成后保持 hold_ms,再渐进回 STAND

        固定发 4 字节负载;旧固件只读前 2 字节,多出的会被忽略(向后兼容)。
        """
        payload = bytes([action_id, 1]) + struct.pack("<H", min(int(hold_ms), 65535))
        return self.cmd(CMD_ACTION_PLAY, payload)

    def motion(self, motion_id: int, duration_ms: int, direction: int = 0):
        """MOTION_PLAY: WALK(id=5) 追加有符号 direction 字节。"""
        payload = bytes([motion_id]) + struct.pack("<I", duration_ms)
        if motion_id == 5:
            if direction not in (-1, 0, 1):
                raise ValueError("WALK direction 必须是 -1, 0 或 1")
            payload += struct.pack("<b", direction)
        return self.cmd(CMD_MOTION_PLAY, payload)

    def sit(self):
        return self.action(ACTION_SIT_DOWN)

    def stand(self):
        return self.action(ACTION_STAND_UP)

    def squat(self, times=1, hold_s=1.0):
        """蹲下起立循环 times 次,每姿态保持 hold_s 秒"""
        for i in range(times):
            print(f"--- squat {i + 1}/{times} ---")
            if not self.sit():
                return False
            time.sleep(ACTION_RAMP_MS[ACTION_SIT_DOWN] / 1000.0 + hold_s)
            if not self.stand():
                return False
            time.sleep(ACTION_RAMP_MS[ACTION_STAND_UP] / 1000.0 + hold_s)
        return True

    def trot(self, seconds=10.0):
        return self.motion(2, int(seconds * 1000))

    def bob(self, seconds=15.0):
        return self.motion(3, int(seconds * 1000))

    def walk(self, direction: int, seconds=8.0):
        return self.motion(5, int(seconds * 1000), direction)

    # 2026-09-16:def beep() 移除,蜂鸣器链路作废

    def emergency_stop(self):
        return self.cmd(CMD_EMERGENCY_STOP)


# === seq 组合解析: "sit 2; stand 1; beep 2; squat 3; wait 1.5" ===
def run_seq(link: DogLink, spec: str):
    steps = [s.strip() for s in spec.split(";") if s.strip()]
    print(f"=== 动作组合: {len(steps)} 步 ===")
    for i, step in enumerate(steps, 1):
        parts = step.split()
        name = parts[0].lower()
        args = parts[1:]
        print(f"[{i}/{len(steps)}] {step}")
        if name == "sit":
            hold = float(args[0]) if args else 0.0
            link.sit()
            time.sleep(RAMP_MS / 1000.0 + hold)
        elif name in ("stand", "up"):
            hold = float(args[0]) if args else 0.0
            link.stand()
            time.sleep(RAMP_MS / 1000.0 + hold)
        elif name == "squat":
            times = int(args[0]) if args else 1
            hold = float(args[1]) if len(args) > 1 else 1.0
            link.squat(times, hold)
        elif name == "trot":
            sec = float(args[0]) if args else 10.0
            link.trot(sec)
            time.sleep(sec)
        elif name == "bob":
            sec = float(args[0]) if args else 15.0
            link.bob(sec)
            time.sleep(sec)
        elif name in ("forward", "backward"):
            sec = float(args[0]) if args else 8.0
            link.walk(1 if name == "forward" else -1, sec)
            time.sleep(sec)
        elif name == "wait":
            time.sleep(float(args[0]) if args else 1.0)
        else:
            print(f"  ? 未知步骤 '{name}',跳过")
    print("=== 组合结束 ===")


def main():
    ap = argparse.ArgumentParser(description="X3 端机器狗控制 CLI")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--no-hb", action="store_true", help="不发心跳(调试用)")
    sub = ap.add_subparsers(dest="cmd_name", required=True)

    p = sub.add_parser("sit");   p.add_argument("hold", type=float, nargs="?", default=0)
    p = sub.add_parser("stand"); p.add_argument("hold", type=float, nargs="?", default=0)
    p = sub.add_parser("up");    p.add_argument("hold", type=float, nargs="?", default=0)
    p = sub.add_parser("squat")
    p.add_argument("times", type=int, nargs="?", default=1)
    p.add_argument("hold", type=float, nargs="?", default=1.0)
    p = sub.add_parser("trot");  p.add_argument("sec", type=float, nargs="?", default=10.0)
    p = sub.add_parser("bob");   p.add_argument("sec", type=float, nargs="?", default=15.0)
    p = sub.add_parser("forward"); p.add_argument("sec", type=float, nargs="?", default=8.0)
    p = sub.add_parser("backward"); p.add_argument("sec", type=float, nargs="?", default=8.0)
    # 2026-09-16:beep 子命令移除,蜂鸣器链路作废
    p = sub.add_parser("seq");   p.add_argument("spec")
    sub.add_parser("stop")
    p = sub.add_parser("pwm");   p.add_argument("id", type=int); p.add_argument("pulse", type=int)
    p = sub.add_parser("raw");   p.add_argument("hexstr")

    a = ap.parse_args()
    link = DogLink(a.port, a.baud, heartbeat=not a.no_hb)
    try:
        if a.cmd_name == "sit":
            link.sit(); time.sleep(RAMP_MS / 1000.0 + a.hold)
        elif a.cmd_name in ("stand", "up"):
            link.stand(); time.sleep(RAMP_MS / 1000.0 + a.hold)
        elif a.cmd_name == "squat":
            link.squat(a.times, a.hold)
        elif a.cmd_name == "trot":
            link.trot(a.sec); time.sleep(a.sec)
        elif a.cmd_name == "bob":
            link.bob(a.sec); time.sleep(a.sec)
        elif a.cmd_name in ("forward", "backward"):
            link.walk(1 if a.cmd_name == "forward" else -1, a.sec)
            time.sleep(a.sec)
        # 2026-09-16:beep 命令移除,蜂鸣器链路作废
        elif a.cmd_name == "seq":
            run_seq(link, a.spec)
        elif a.cmd_name == "stop":
            link.emergency_stop()
        elif a.cmd_name == "pwm":
            payload = bytes([a.id]) + struct.pack("<H", a.pulse)
            link.cmd(CMD_SET_PWM, payload)
        elif a.cmd_name == "raw":
            data = bytes.fromhex(a.hexstr.replace(" ", ""))
            link.send_raw(data)
            r = link.read_ack(data[3] if len(data) > 3 else 0x00, 0.5)
            print("ACK:", r)
    except KeyboardInterrupt:
        print("\n中断")
    finally:
        link.close()   # 心跳停 → 200ms 后 watchdog 自动回 STAND


if __name__ == "__main__":
    main()
