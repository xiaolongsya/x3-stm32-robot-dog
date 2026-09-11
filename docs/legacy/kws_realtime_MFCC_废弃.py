#!/usr/bin/env python3
"""kws_realtime.py — Pi 端实时 KWS 唤醒检测

用法:
    arecord -D plughw:1,0 -f S16_LE -r 16000 -c 1 -t raw \\
        | python3 /usr/local/bin/kws_realtime.py [阈值] [模型路径]

输出: 实时日志
    [21:30:45] 🌟 唤醒 '小龙' (prob=0.94)
"""
import sys
import os
import time
import numpy as np
import onnxruntime as ort

# 共享 MFCC 模块(跟训练同源,保证 100% 一致)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mfcc_lib import mfcc_from_audio, TARGET_FRAMES, N_MFCC, SR

THRESHOLD = float(sys.argv[1]) if len(sys.argv) > 1 else 0.7
MODEL_PATH = sys.argv[2] if len(sys.argv) > 2 else "/tmp/kws_xiaolong.onnx"


def main():
    if not os.path.isfile(MODEL_PATH):
        print(f"[ERR] 模型不存在: {MODEL_PATH}", flush=True)
        sys.exit(1)

    sess = ort.InferenceSession(MODEL_PATH)
    print(f"[kws] ✅ 模型加载 OK: {MODEL_PATH}", flush=True)
    print(f"[kws] 阈值: {THRESHOLD}", flush=True)
    print(f"[kws] MFCC: 共享 numpy 模块(n_mfcc={N_MFCC}, sr={SR}, frames={TARGET_FRAMES})", flush=True)
    print(f"==========================================", flush=True)
    print(f"[kws] 现在请开始念 '小龙' 测试唤醒", flush=True)
    print(f"==========================================", flush=True)

    chunk_bytes = SR * 2  # 16-bit mono = 2 bytes/sample
    ring = bytearray()
    chunk_idx = 0
    last_wake_ts = 0.0
    cooldown = 1.5

    stdin = sys.stdin.buffer
    while True:
        data = stdin.read(4096)
        if not data:
            break
        ring.extend(data)

        while len(ring) >= chunk_bytes:
            chunk = bytes(ring[:chunk_bytes])
            del ring[:chunk_bytes]

            samples = np.frombuffer(chunk, dtype=np.int16).astype(np.float32)
            samples /= 32768.0

            feat = mfcc_from_audio(samples)
            inp = feat[np.newaxis, :, :]
            logits = sess.run(None, {"features": inp})[0][0]
            exp = np.exp(logits - logits.max())
            probs = exp / exp.sum()
            wake_prob = float(probs[1])

            ts_str = time.strftime("%H:%M:%S")
            now = time.time()
            if wake_prob >= THRESHOLD and (now - last_wake_ts) > cooldown:
                print(f"[{ts_str}] 🌟 唤醒 '小龙' (prob={wake_prob:.3f})", flush=True)
                last_wake_ts = now
            else:
                chunk_idx += 1
                if chunk_idx % 5 == 0:
                    bar = "█" * int(wake_prob * 20) + "░" * (20 - int(wake_prob * 20))
                    print(f"[{ts_str}] prob={wake_prob:.3f} [{bar}]", flush=True)


if __name__ == "__main__":
    main()
