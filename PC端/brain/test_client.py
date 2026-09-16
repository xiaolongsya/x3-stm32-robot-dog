#!/usr/bin/env python3
# test_client.py — 模拟 X3 客户端发 wav,验证 WS 全链路
"""
2026-09-16 初版

用法:
  python test_client.py --wav tests/sample.wav
  python test_client.py --wav tests/sample.wav --uri ws://192.168.1.10:8765
"""
import argparse
import asyncio
import base64
import json
import sys

import soundfile as sf
import websockets


async def send_wav(uri: str, wav_path: str):
    pcm, sr = sf.read(wav_path, dtype="int16")
    if sr != 16000:
        print(f"[client] WARN: sample rate {sr} != 16000")
    pcm_bytes = pcm.tobytes()
    print(f"[client] connecting {uri} ...")
    async with websockets.connect(uri) as ws:
        print(f"[client] sending utterance ({len(pcm_bytes)} bytes, sr={sr})")
        await ws.send(json.dumps({
            "type": "utterance",
            "pcm": base64.b64encode(pcm_bytes).decode(),
            "session_id": "test_x3_1",
        }))
        print(f"[client] waiting for response ...")
        while True:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=30.0)
            except asyncio.TimeoutError:
                print(f"[client] TIMEOUT waiting for response")
                return
            msg = json.loads(raw)
            print(f"[client] ← {json.dumps(msg, ensure_ascii=False, indent=2)}")
            if msg.get("type") in ("action", "error"):
                return


def main():
    ap = argparse.ArgumentParser(description="模拟 X3 客户端发 wav")
    ap.add_argument("--wav", required=True, help="16kHz wav 文件")
    ap.add_argument("--uri", default="ws://127.0.0.1:8765", help="brain WS 地址")
    args = ap.parse_args()
    try:
        asyncio.run(send_wav(args.uri, args.wav))
    except KeyboardInterrupt:
        print("\n[client] Bye")


if __name__ == "__main__":
    main()