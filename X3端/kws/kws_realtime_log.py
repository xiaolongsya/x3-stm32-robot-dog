#!/usr/bin/env python3
"""kws_realtime_log.py — X3 端实时 KWS 唤醒日志

只做一件事:实时监听麦克风,score > 阈值时打印唤醒日志(中文 + 时间)。

用法:
    arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 -t raw 2>/dev/null \\
        | python3 /home/root/kws/kws_realtime_log.py [阈值]

阈值默认 0.7。
冷却 1.5 秒(同一次唤醒只报一次)。

中止: Ctrl+C
"""
import os
import sys
import time
import numpy as np

THRESHOLD = float(sys.argv[1]) if len(sys.argv) > 1 else 0.7
MODEL_PATH = "/home/root/kws/xiaolong.onnx"
COOLDOWN = 1.5  # 秒
CHUNK = 1280   # 80ms @ 16kHz


def main():
    from openwakeword.model import Model

    if not os.path.isfile(MODEL_PATH):
        print(f"[ERR] 模型不存在: {MODEL_PATH}", flush=True)
        sys.exit(1)

    oww = Model(wakeword_models=[MODEL_PATH])
    print(f"[kws] ✅ 模型: {MODEL_PATH}", flush=True)
    print(f"[kws] 阈值: {THRESHOLD}  冷却: {COOLDOWN}s", flush=True)
    print(f"[kws] 监听中 —— 说 '小龙' 测试(80ms 一段实时打分)", flush=True)
    print("=" * 56, flush=True)

    last_wake_ts = 0.0
    score_history = []  # 平滑用
    tick = 0
    chunk_bytes = CHUNK * 2  # int16 mono

    while True:
        chunk = sys.stdin.buffer.read(chunk_bytes)
        if not chunk or len(chunk) < chunk_bytes:
            break

        pcm = np.frombuffer(chunk, dtype=np.int16)
        preds = oww.predict(pcm)

        ts = time.strftime("%H:%M:%S")
        now = time.time()

        s = preds.get("xiaolong")
        if s is None:
            continue
        try:
            score = float(s[-1]) if isinstance(s, (np.ndarray, list)) else float(s)
        except (TypeError, IndexError):
            continue

        score_history.append(score)
        if len(score_history) > 5:
            score_history.pop(0)
        avg_score = sum(score_history) / len(score_history)

        if score >= THRESHOLD and (now - last_wake_ts) > COOLDOWN:
            print(f"[{ts}] 🌟 唤醒 '小龙' (score={score:.3f})", flush=True)
            last_wake_ts = now
            score_history.clear()
        else:
            tick += 1
            if tick % 25 == 0:  # 每 ≈2 秒打一行
                bar = "█" * int(avg_score * 20) + "░" * (20 - int(avg_score * 20))
                print(f"[{ts}] score={avg_score:.3f} [{bar}]", flush=True)


if __name__ == "__main__":
    main()
