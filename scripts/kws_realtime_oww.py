#!/usr/bin/env python3
"""kws_realtime_oww.py — Pi 端实时 KWS 唤醒检测(用 openWakeWord)

用法:
    arecord -D plughw:1,0 -f S16_LE -r 16000 -c 1 -t raw 2>/dev/null \\
        | python3 /usr/local/bin/kws_realtime_oww.py [阈值] [模型路径]

输出: 实时日志
    [21:30:45] 🌟 唤醒 '小龙' (score=0.94)
"""
import sys
import os
import time
import numpy as np

THRESHOLD = float(sys.argv[1]) if len(sys.argv) > 1 else 0.5
MODEL_PATH = sys.argv[2] if len(sys.argv) > 2 else "/tmp/xiaolong.onnx"
COOLDOWN = 1.5  # 秒,避免连续触发


def main():
    from openwakeword.model import Model

    if not os.path.isfile(MODEL_PATH):
        print(f"[ERR] 模型不存在: {MODEL_PATH}", flush=True)
        sys.exit(1)

    # 加载 openWakeWord 模型(自动初始化 melspec + embedding)
    oww = Model(wakeword_model_paths=[MODEL_PATH])
    print(f"[kws] ✅ 模型加载 OK: {MODEL_PATH}", flush=True)
    print(f"[kws] 阈值: {THRESHOLD}", flush=True)
    print(f"[kws] 冷却: {COOLDOWN}s", flush=True)
    print(f"==========================================", flush=True)
    print(f"[kws] 现在请开始念 '小龙' 测试唤醒", flush=True)
    print(f"==========================================", flush=True)

    # openWakeWord 用 chunk_size=1280 samples (80ms @ 16kHz)
    CHUNK = 1280
    chunk_bytes = CHUNK * 2  # 16-bit mono
    last_wake_ts = 0.0
    score_history = []  # 最近 5 个 score,显示均值
    tick = 0

    stdin = sys.stdin.buffer
    while True:
        chunk = stdin.read(chunk_bytes)
        if not chunk or len(chunk) < chunk_bytes:
            break

        # int16 → numpy
        pcm = np.frombuffer(chunk, dtype=np.int16)

        # openWakeWord 推理
        predictions = oww.predict(pcm)

        ts_str = time.strftime("%H:%M:%S")
        for label, scores in predictions.items():
            if scores is None:
                continue
            # 取最新 score(可能 ndarray/list/scalar)
            try:
                if isinstance(scores, (np.ndarray, list)):
                    score = float(scores[-1])
                else:
                    score = float(scores)
            except (TypeError, IndexError):
                continue

            score_history.append(score)
            if len(score_history) > 5:
                score_history.pop(0)
            avg_score = sum(score_history) / len(score_history)

            now = time.time()
            if score >= THRESHOLD and (now - last_wake_ts) > COOLDOWN:
                print(f"[{ts_str}] 🌟 唤醒 '小龙' (score={score:.3f})", flush=True)
                last_wake_ts = now
                score_history.clear()
            else:
                # 每 25 块(≈2 秒)打一次状态,避免刷屏
                tick += 1
                if tick % 25 == 0:
                    bar = "█" * int(avg_score * 20) + "░" * (20 - int(avg_score * 20))
                    print(f"[{ts_str}] score={avg_score:.3f} [{bar}]", flush=True)


if __name__ == "__main__":
    main()
