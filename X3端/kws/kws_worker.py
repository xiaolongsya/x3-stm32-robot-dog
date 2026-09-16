#!/usr/bin/env python3
"""kws_worker.py — X3 端 KWS 业务进程 (2026-09-16 重构)

职责:
  - 独占 /dev/ttyS3 (STM32 串口)
  - 100ms STM32 heartbeat (复用 dog_uart.DogLink)
  - connect Unix socket → 听 wake_detected / recording_done
  - 上送 PC brain → 翻译 actions → 发 STM32 帧
  - 发回 ready 通知 listener

不做:
  - 不碰麦克风 (arecord 在 listener 那边)
  - 不调 oww.predict (KWS 在 listener 那边)
  - 不做 KWS 评分 / VAD 录音

用法 (由 start_kws_dual.sh 启动):
  python3 /root/kws/kws_worker.py --pc-url ws://192.168.160.91:8765

依赖: pyserial / websockets / numpy / dog_uart (X3 上已装)
"""
import argparse
import asyncio
import base64
import json
import os
import socket
import sys
import time

import kws_protocol as P


# dog_uart.py 在 X3 端根目录 (X3 上是 /root/),不在 /root/kws/。
# 这里让 import 时自动找上级目录,无论 worker 从哪启动都不会失败。
def _bootstrap_dog_uart():
    try:
        from dog_uart import DogLink  # noqa: F401
        return
    except ModuleNotFoundError:
        pass
    # X3 部署: /root/kws/kws_worker.py → 上级 /root/ 是 dog_uart 所在
    here = os.path.dirname(os.path.abspath(__file__))
    parent = os.path.dirname(here)
    if parent and parent not in sys.path:
        sys.path.insert(0, parent)
    try:
        from dog_uart import DogLink  # noqa: F401
    except ModuleNotFoundError as e:
        raise SystemExit(
            f"[worker] dog_uart.py 找不到: {e}\n"
            f"  X3 部署应在 /root/dog_uart.py;\n"
            f"  PC 测试时需保证 dog_uart.py 在 sys.path。"
        )


_bootstrap_dog_uart()


# === JSON line 协议 ===
def recv_json_line(sock: socket.socket) -> dict | None:
    """同步读一条 newline-delimited JSON,读到 \\n 为止。None = EOF"""
    buf = b""
    while True:
        b = sock.recv(1)
        if not b:
            return None
        if b == b"\n":
            break
        buf += b
        if len(buf) > 1_000_000:
            raise ValueError("JSON message too large")
    try:
        return json.loads(buf.decode())
    except json.JSONDecodeError as e:
        raise ValueError(f"bad JSON: {buf[:80]!r}") from e


def send_json_line(sock: socket.socket, obj: dict):
    sock.sendall((json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8"))


class KWSWorker:
    """独占 STM32 + WS brain + Unix socket client"""

    def __init__(self, port: str, baud: int, pc_url: str, socket_path: str):
        from dog_uart import DogLink
        self.dog = DogLink(port, baud, heartbeat=True)  # 100ms heartbeat 自动起
        self.pc_url = pc_url
        self.socket_path = socket_path
        self.state = "idle"   # "idle" / "busy"
        self.sock: socket.socket | None = None

    # === socket 连接管理 ===
    def connect_socket(self) -> socket.socket:
        """阻塞 connect listener socket,失败 1s 重试"""
        while True:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1_048_576)
            try:
                s.connect(self.socket_path)
                print(f"[worker] 已连 listener {self.socket_path}", flush=True)
                return s
            except (FileNotFoundError, ConnectionRefusedError) as e:
                try:
                    s.close()
                except Exception:
                    pass
                print(f"[worker] listener 未就绪: {e}, retry 1s", flush=True)
                time.sleep(1)

    def send_ready(self):
        """通知 listener:任务完成,可以继续接 wake"""
        if self.sock is None:
            return
        try:
            send_json_line(self.sock, {"type": "ready", "ts": time.time()})
        except OSError as e:
            print(f"[worker] ERR send ready: {e}", flush=True)

    # === LLM actions → STM32 ===
    # 2026-09-16 修 M3:多条 ramp 类 action 之间要等待 STM32 实际执行完
    # 不然会"瞬间 cancel + 重启 ramp",前 N-1 条全部浪费
    #
    # 时长(STM32 motions.c):
    #   - ACTION_PLAY SIT (id=5/8):  SIT_RAMP_MS = 800ms
    #   - ACTION_PLAY STAND (id=6/7): STAND_RAMP_MS = 1200ms
    #   - MOTION_PLAY (id=1..4): 时长不等,默认 30s/无限
    #   - EMERGENCY_STOP: 瞬间
    #
    # 实际我们不等 STM32 跑完完整 ramp,只插一段"最小可视时长"避免立刻被下一条覆盖
    # 这样 6 条 [蹲立蹲立蹲立] ≈ 6 × 1s ≈ 6s 完成,有明显节奏
    RAMP_MIN_HOLD_MS = {
        # ACTION_PLAY id → 最小 hold ms(SIT_RAMP_MS 或 STAND_RAMP_MS)
        5: 1000,   # SIT_DOWN 800ms + 留 200ms 视觉停留
        6: 1400,   # STAND_UP 1200ms + 200ms
        7: 1400,   # SIT_TO_STAND 同 STAND_UP
        8: 1000,   # STAND_TO_SIT 同 SIT_DOWN
    }

    def translate_and_send(self, actions: list):
        """LLM actions list → DogLink.action / motion / emergency_stop"""
        for a in actions:
            cmd = a.get("cmd")
            if cmd == "ACTION_PLAY":
                aid = a.get("id")
                dur = a.get("duration_ms", 0)
                self.dog.action(aid, dur)
                print(f"[worker] stm32 → ACTION_PLAY #{aid} dur={dur}ms", flush=True)
                # M3 fix: ramp 类动作之间 sleep 等 STM32 跑完,避免下一条立刻覆盖
                hold_ms = self.RAMP_MIN_HOLD_MS.get(aid, 0)
                if hold_ms > 0:
                    time.sleep(hold_ms / 1000.0)
            elif cmd == "MOTION_PLAY":
                mid = a.get("id")
                dur = a.get("duration_ms", 5000)
                self.dog.motion(mid, dur)
                print(f"[worker] stm32 → MOTION_PLAY #{mid} dur={dur}ms", flush=True)
                # MOTION_PLAY 不插 sleep,它的 duration_ms 由 STM32 tick 自动结束
            elif cmd == "EMERGENCY_STOP":
                self.dog.emergency_stop()
                print(f"[worker] stm32 → EMERGENCY_STOP", flush=True)
            else:
                print(f"[worker] 跳过非法 action: {a}", flush=True)

    # === WS brain 上送 ===
    async def send_to_brain(self, pcm: bytes) -> dict | None:
        """WS 上送 PC brain,等 action/error 响应"""
        import websockets
        try:
            async with websockets.connect(self.pc_url) as ws:
                print(f"[worker] WS 已连 {self.pc_url}", flush=True)
                await ws.send(json.dumps({
                    "type": "utterance",
                    "pcm": base64.b64encode(pcm).decode(),
                    "session_id": "x3_local",
                }))
                print(f"[worker] → utterance ({len(pcm)} bytes)", flush=True)
                while True:
                    raw = await ws.recv()
                    msg = json.loads(raw)
                    print(f"[worker] ← {msg}", flush=True)
                    if msg["type"] in ("action", "error"):
                        return msg
        except Exception as e:
            print(f"[worker] WS 错误: {e}", flush=True)
            return None

    async def handle_recording_done(self, msg: dict):
        """VAD 录音结束 → 上送 brain → 翻译 STM32 → ready"""
        pcm_bytes = base64.b64decode(msg["pcm_b64"])
        duration_s = msg.get("duration_s", 0.0)
        false_wake = duration_s < 2.0
        print(f"[worker] rec done {duration_s:.2f}s, 上送 brain", flush=True)

        resp = await self.send_to_brain(pcm_bytes)
        if resp and resp.get("type") == "action":
            actions = resp.get("actions", [])
            reply = resp.get("reply", "")
            print(f"[worker] brain reply: {reply}", flush=True)
            # 2026-09-16 修 H3:EMERGENCY_STOP 必须无条件执行,不进 false_wake 旁路
            # 根因:用户短促说"停!"或"立正!"可能 < 2s,被误判 false_wake 后静默丢
            # 修复:把 actions 拆成"紧急动作"(EMERGENCY_STOP,立即执行)
            #        和"普通动作"(其他,受 false_wake 旁路保护)
            if actions:
                urgent = [a for a in actions if a.get("cmd") == "EMERGENCY_STOP"]
                normal = [a for a in actions if a.get("cmd") != "EMERGENCY_STOP"]
                if urgent:
                    print(f"[worker] 紧急动作 {len(urgent)} 条无视 false_wake", flush=True)
                    self.translate_and_send(urgent)
                if normal and not false_wake:
                    self.translate_and_send(normal)
                elif normal and false_wake:
                    print(f"[worker] 录音 {duration_s:.2f}s 误唤醒跳过 {len(normal)} 条普通动作", flush=True)
                if not actions:
                    print("[worker] actions 空,不当误唤醒", flush=True)
        elif resp and resp.get("type") == "error":
            print(f"[worker] brain error: {resp.get('code')} {resp.get('reply')}", flush=True)
        else:
            print("[worker] brain 无响应", flush=True)

        # 任务完成 → 通知 listener → 切回 idle
        self.state = "idle"
        self.send_ready()

    def handle_wake(self, msg: dict):
        """收到 wake_detected → 切 busy (score 仅日志)"""
        score = msg.get("score", 0.0)
        rms = msg.get("rms", 0)
        print(f"[worker] wake score={score:.3f} rms={rms}", flush=True)
        self.state = "busy"

    # === 主循环 ===
    def run(self):
        self.sock = self.connect_socket()
        try:
            while True:
                msg = recv_json_line(self.sock)
                if msg is None:
                    print("[worker] listener 断开,重连 ...", flush=True)
                    try:
                        self.sock.close()
                    except Exception:
                        pass
                    self.sock = self.connect_socket()
                    continue

                mtype = msg.get("type")
                if mtype == "wake_detected":
                    if self.state == "busy":
                        # worker 还在处理上一个 utterance,丢弃本次 wake
                        print(
                            f"[worker] busy, drop wake score={msg.get('score',0):.2f}",
                            flush=True,
                        )
                        continue
                    self.handle_wake(msg)
                elif mtype == "recording_done":
                    # 2026-09-16 修 H4:busy 状态拒绝第二条 recording_done
                    # 根因:用户连说两句话时,前一句的 actions 还没发完 STM32,
                    #   第二条 recording_done 触发新的 handle_recording_done,
                    #   asyncio.run 把前一个 event loop 中断,actions 列表后半段丢失
                    # 修复:busy 时直接 drop,不进入 handle_recording_done
                    if self.state == "busy":
                        print(
                            "[worker] busy, drop recording_done (前一句还没处理完)",
                            flush=True,
                        )
                        continue
                    self.state = "busy"  # 提前占位,避免 race
                    # 启动 asyncio 处理 WS 上送 + STM32 翻译
                    asyncio.run(self.handle_recording_done(msg))
                elif mtype == "error":
                    print(f"[worker] listener error: {msg}", flush=True)
                else:
                    print(f"[worker] unknown msg: {mtype}", flush=True)
        except KeyboardInterrupt:
            print("\n[worker] Bye", flush=True)
        finally:
            try:
                self.sock.close()
            except Exception:
                pass
            try:
                self.dog.close()
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser(description="X3 端 KWS 业务 worker")
    ap.add_argument("--port",   default=P.DEFAULT_PORT)
    ap.add_argument("--baud",   type=int, default=P.DEFAULT_BAUD)
    ap.add_argument("--pc-url", default=P.DEFAULT_PC_URL)
    ap.add_argument("--socket", default=P.SOCKET_PATH)
    args = ap.parse_args()

    print(
        f"[worker] 启动 port={args.port} baud={args.baud} "
        f"pc={args.pc_url} socket={args.socket}",
        flush=True,
    )

    KWSWorker(args.port, args.baud, args.pc_url, args.socket).run()


if __name__ == "__main__":
    main()