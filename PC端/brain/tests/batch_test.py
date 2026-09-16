#!/usr/bin/env python3
"""batch_test.py — 批量跑 10 条 wav 通过 ASR + LLM,汇总输出

2026-09-16

用法(S父目录 PC端):
    brain/.venv/Scripts/python.exe -m brain.tests.batch_test

输出:
    每条: ASR text / LLM actions / LLM reply / 总耗时
    汇总: 平均延迟 / 最大延迟 / 错误率 / 兜底率
"""
import asyncio
import os
import sys
import time

import soundfile as sf

# 让 Python 找到 brain package
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from brain.asr import ASRClient
from brain.llm import LLMClient
from brain.prompts import SYSTEM_PROMPT, build_user_prompt

WAV_DIR = os.path.join(os.path.dirname(__file__), "voice_commands")
COMMANDS = [
    ("01.wav", "你好呀,你可以蹲下吗?"),
    ("02.wav", "踏步3秒"),
    ("03.wav", "立正!"),
    ("04.wav", "蹲下5秒钟再站起来把"),
    ("05.wav", "摆动一下左前腿"),
    ("06.wav", "摆动一下左肩部"),
    ("07.wav", "蹲下起立5次"),
    ("08.wav", "小狗狗,你可以蹲下吗?"),
    ("09.wav", "小狗狗,蹲一个"),
    ("10.wav", "往前倾一点"),
]


async def main():
    asr = ASRClient()
    llm = LLMClient()

    results = []
    print("=" * 80)
    print(f" {'idx':<5} {'期望':<18} {'ASR':<14} {'actions':<32} {'reply':<14} {'耗时ms':>6}")
    print("=" * 80)

    for fname, expected in COMMANDS:
        wav_path = os.path.join(WAV_DIR, fname)
        if not os.path.exists(wav_path):
            print(f" {fname} 缺失,跳过")
            continue

        pcm, sr = sf.read(wav_path, dtype="int16")
        pcm_bytes = pcm.tobytes()

        t0 = time.time()
        text = await asyncio.to_thread(asr.transcribe, pcm_bytes)
        asr_ms = int((time.time() - t0) * 1000)

        if not text:
            results.append({"idx": fname, "expected": expected, "asr": "(空)", "actions": [], "reply": "(空)", "ms": asr_ms})
            print(f" {fname:<5} {expected[:16]:<18} {'(空)':<14} {'':<32} {'':<14} {asr_ms:>6}")
            continue

        # LLM
        t1 = time.time()
        user_prompt = build_user_prompt([], text)
        result = await llm.chat(SYSTEM_PROMPT, user_prompt)
        llm_ms = int((time.time() - t1) * 1000)
        total_ms = int((time.time() - t0) * 1000)

        actions = result["actions"]
        reply = result["reply"]

        # 简短 action 显示
        actions_short = ", ".join(
            f"{a['cmd']}#{a.get('id','')}" + (f"/{a.get('duration_ms','?')}ms" if 'duration_ms' in a else "")
            for a in actions
        ) or "(空)"

        results.append({
            "idx": fname, "expected": expected, "asr": text,
            "actions": actions, "reply": reply, "ms": total_ms,
            "asr_ms": asr_ms, "llm_ms": llm_ms,
        })
        print(
            f" {fname:<5} {expected[:16]:<18} {text[:12]:<14} "
            f"{actions_short[:30]:<32} {reply[:12]:<14} {total_ms:>6}"
        )

    # 汇总
    print("=" * 80)
    print("\n【汇总】")
    asr_times = [r["asr_ms"] for r in results if "asr_ms" in r]
    llm_times = [r["llm_ms"] for r in results if "llm_ms" in r]
    total_times = [r["ms"] for r in results if "asr_ms" in r]
    empty_asr = sum(1 for r in results if r.get("asr") == "(空)")
    empty_actions = sum(1 for r in results if not r.get("actions"))

    if total_times:
        print(f"  ASR  : avg={sum(asr_times)/len(asr_times):.0f}ms max={max(asr_times)}ms")
        print(f"  LLM  : avg={sum(llm_times)/len(llm_times):.0f}ms max={max(llm_times)}ms")
        print(f"  TOTAL: avg={sum(total_times)/len(total_times):.0f}ms max={max(total_times)}ms")
        print(f"  ASR 空输出: {empty_asr}/{len(results)}")
        print(f"  LLM 兜底 (actions=空): {empty_actions}/{len(results)}")
        over_2s = sum(1 for t in total_times if t > 2000)
        print(f"  > 2s 延迟: {over_2s}/{len(total_times)}")
    else:
        print("  没跑出任何结果")

    print("\n【期望 vs 实际】:")
    for r in results:
        if "actions" not in r: continue
        actions_short = " ".join(f"{a['cmd']}#{a.get('id','')}" for a in r.get("actions", [])) or "(空)"
        print(f"  {r['idx']}: 期望='{r['expected'][:18]}'")
        print(f"         ASR ='{r.get('asr','(空)')}'")
        print(f"         actions={actions_short}")
        print(f"         reply ='{r.get('reply','(空)')}'")
        print()

    await llm.close()


if __name__ == "__main__":
    asyncio.run(main())