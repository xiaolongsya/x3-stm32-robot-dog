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
import socket
import sys
import time

import kws_protocol as P


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
    def translate_and_send(self, actions: list):
        """LLM actions list → DogLink.action / motion / emergency_stop"""
        for a in actions:
            cmd = a.get("cmd")
            if cmd == "ACTION_PLAY":
                aid = a.get("id")
                dur = a.get("duration_ms", 0)
                self.dog.action(aid, dur)
                print(f"[worker] stm32 → ACTION_PLAY #{aid} dur={dur}ms", flush=True)
            elif cmd == "MOTION_PLAY":
                mid = a.get("id")
                dur = a.get("duration_ms", 5000)
                self.dog.motion(mid, dur)
                print(f"[worker] stm32 → MOTION_PLAY #{mid} dur={dur}ms", flush=True)
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
            if actions and not false_wake:
                self.translate_and_send(actions)
            elif false_wake:
                print(f"[worker] 录音 {duration_s:.2f}s 误唤醒跳过动作", flush=True)
            else:
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
                    if self.state != "busy":
                        print(
                            "[worker] WARNING: recording_done but state != busy",
                            flush=True,
                        )
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