#!/usr/bin/env python3
"""dump_train_mfcc.py — 用训练时 torchaudio MFCC 提取样本特征,导出到 .npy
用于和 Pi 端 numpy MFCC 实现对比验证一致性
"""
import os
import sys
import numpy as np
import torch
import torchaudio
import torchaudio.transforms as T
import soundfile as sf

DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "voice_data")
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "model")
os.makedirs(OUT_DIR, exist_ok=True)

SR = 16000
N_MFCC = 40
HOP = 160
N_FFT = 400
N_MELS = 80
TARGET_FRAMES = SR // HOP  # 100


def extract_mfcc(path):
    audio, sr = sf.read(path)
    assert sr == SR, f"sample rate mismatch: {sr}"
    audio = audio[:TARGET_FRAMES * HOP].astype(np.float32)
    wf = torch.from_numpy(audio).unsqueeze(0)
    mfcc_fn = T.MFCC(
        sample_rate=SR,
        n_mfcc=N_MFCC,
        melkwargs={"n_fft": N_FFT, "hop_length": HOP, "n_mels": N_MELS, "center": True},
    )
    feat = mfcc_fn(wf).squeeze(0).numpy()  # (40, T)
    if feat.shape[1] > TARGET_FRAMES:
        feat = feat[:, :TARGET_FRAMES]
    elif feat.shape[1] < TARGET_FRAMES:
        feat = np.pad(feat, ((0, 0), (0, TARGET_FRAMES - feat.shape[1])))
    return feat


if __name__ == "__main__":
    paths = [
        os.path.join(DATA_DIR, "positive", "xiaolong_001.wav"),
        os.path.join(DATA_DIR, "negative", "negative_001.wav"),
        os.path.join(DATA_DIR, "negative", "noise_001.wav"),
    ]
    out = {}
    for p in paths:
        if not os.path.isfile(p):
            print(f"[skip] {p} (not found)")
            continue
        feat = extract_mfcc(p)
        out[os.path.basename(p)] = feat
        print(f"[dump] {os.path.basename(p)} shape={feat.shape} "
              f"range=[{feat.min():.3f}, {feat.max():.3f}] mean={feat.mean():.3f}")
    np.savez(os.path.join(OUT_DIR, "train_mfcc_dump.npz"), **out)
    print(f"\n[done] saved to {OUT_DIR}/train_mfcc_dump.npz")
