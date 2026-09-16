#!/usr/bin/env python3
"""asr_only.py — 跑 10 条 wav 只看 ASR 转写,清晰输出

2026-09-16
用法(S父目录 PC端):
    brain/.venv/Scripts/python.exe -m brain.tests.asr_only
"""
import asyncio
import os
import sys

import soundfile as sf

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from brain.asr import ASRClient

WAV_DIR = os.path.join(os.path.dirname(__file__), "voice_commands")

# 期望文本(录制前用户告诉我的)
EXPECTED = {
    "01.wav": "你好呀,你可以蹲下吗?",
    "02.wav": "踏步3秒",
    "03.wav": "立正!",
    "04.wav": "蹲下5秒钟再站起来把",
    "05.wav": "摆动一下左前腿",
    "06.wav": "摆动一下左肩部",
    "07.wav": "蹲下起立5次",
    "08.wav": "小狗狗,你可以蹲下吗?",
    "09.wav": "小狗狗,蹲一个",
    "10.wav": "往前倾一点",
}


async def main():
    asr = ASRClient()
    print("\n=== ASR transcribe (10 wavs, X3 USB mic) ===\n")
    print(f"{'idx':<5} {'expected':<26} {'ASR output':<30} {'match':<6} {'ms':>6}")
    print("-" * 80)

    for fname in sorted(EXPECTED.keys()):
        wav_path = os.path.join(WAV_DIR, fname)
        if not os.path.exists(wav_path):
            print(f"{fname:<5} (missing)")
            continue

        pcm, sr = sf.read(wav_path, dtype="int16")
        pcm_bytes = pcm.tobytes()

        text = await asyncio.to_thread(asr.transcribe, pcm_bytes)
        expected = EXPECTED[fname]

        # 简单匹配:ASR 输出是否包含期望的关键字
        keywords = expected.replace("?", "").replace("!", "").replace(",", "").split()
        hit = sum(1 for kw in keywords if kw in (text or ""))
        match = f"{hit}/{len(keywords)}"

        print(f"{fname:<5} {expected[:24]:<26} {text or '(empty)':<30} {match:<6}")

    print()


if __name__ == "__main__":
    asyncio.run(main())