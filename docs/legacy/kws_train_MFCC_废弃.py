#!/usr/bin/env python3
"""KWS 训练脚本 — "小龙" 唤醒词检测

数据: voice_data/positive/*.wav (标签=1) + voice_data/negative/*.wav (标签=0)
特征: MFCC 40 维 × 100 帧 (1 秒 @ 16kHz,hop=160)
模型: 小 CNN (1D conv),适合 Pi 实时推理
输出: PyTorch checkpoint + ONNX
"""
import os
import sys
import glob
import random
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
import torch.optim as optim
from torch.utils.data import Dataset, DataLoader
import soundfile as sf
from scipy import signal as scipy_signal
import torchaudio
import torchaudio.transforms as T

# 用共享 MFCC 模块(保证训练和推理 MFCC 一致)
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mfcc_lib import mfcc_from_audio, TARGET_FRAMES, N_MFCC, SR

DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "voice_data")
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "model")
os.makedirs(OUT_DIR, exist_ok=True)

SR = 16000
DURATION = 1.0       # 1 秒窗口
N_MFCC = 40
HOP = 160
N_FFT = 400
TARGET_FRAMES = int(SR / HOP)  # 100 帧
SEED = 42


# ===================== 数据集 =====================

class KWSDataset(Dataset):
    def __init__(self, pos_files, neg_files, augment=True):
        self.files = [(f, 1) for f in pos_files] + [(f, 0) for f in neg_files]
        random.shuffle(self.files)
        self.augment = augment
        self.target_samples = int(SR * DURATION)

    def __len__(self):
        return len(self.files)

    def _load(self, path):
        # 直接 soundfile 读,跟推理一致
        audio, sr = sf.read(path)
        if sr != SR:
            # 简单线性重采样(数据少,够用)
            audio = np.array(scipy_signal.resample(audio, int(len(audio) * SR / sr)), dtype=np.float32)
        # mono
        if audio.ndim > 1:
            audio = audio.mean(axis=1)
        # 中心 1 秒窗口(随机偏移 ±0.5 秒用于增强)
        if len(audio) > self.target_samples:
            offset = random.randint(0, len(audio) - self.target_samples)
            audio = audio[offset:offset + self.target_samples].copy()
        else:
            audio = np.pad(audio, (0, self.target_samples - len(audio)))
        # 音量抖动 ±3 dB
        if self.augment:
            gain = random.uniform(0.7, 1.4)
            audio = audio * gain
        return audio.astype(np.float32)

    def __getitem__(self, idx):
        path, label = self.files[idx]
        audio = self._load(path)
        feat = mfcc_from_audio(audio)  # (40, 100) — 跟推理同源
        return feat, label


# ===================== 模型 =====================

class KWSModel(nn.Module):
    """小 CNN,输入 (batch, 40, 100),输出 (batch, 2)"""

    def __init__(self, n_mfcc=N_MFCC, n_classes=2):
        super().__init__()
        self.conv1 = nn.Conv1d(n_mfcc, 32, kernel_size=3, padding=1)
        self.bn1 = nn.BatchNorm1d(32)
        self.conv2 = nn.Conv1d(32, 64, kernel_size=3, padding=1)
        self.bn2 = nn.BatchNorm1d(64)
        self.pool = nn.MaxPool1d(2)
        self.dropout = nn.Dropout(0.3)
        # 100 -> 50 -> 25
        self.fc1 = nn.Linear(64 * 25, 64)
        self.fc2 = nn.Linear(64, n_classes)

    def forward(self, x):
        x = self.pool(F.relu(self.bn1(self.conv1(x))))   # (B, 32, 50)
        x = self.pool(F.relu(self.bn2(self.conv2(x))))   # (B, 64, 25)
        x = self.dropout(x)
        x = x.flatten(1)                                  # (B, 1600)
        x = F.relu(self.fc1(x))
        x = self.fc2(x)
        return x


# ===================== 训练 =====================

def train():
    random.seed(SEED)
    np.random.seed(SEED)
    torch.manual_seed(SEED)

    pos_files = sorted(glob.glob(os.path.join(DATA_DIR, "positive", "*.wav")))
    neg_files = sorted(glob.glob(os.path.join(DATA_DIR, "negative", "*.wav")))
    print(f"[train] pos={len(pos_files)} neg={len(neg_files)}")

    # 80/20 split
    random.shuffle(pos_files)
    random.shuffle(neg_files)
    pos_split = int(0.8 * len(pos_files))
    neg_split = int(0.8 * len(neg_files))
    train_ds = KWSDataset(pos_files[:pos_split], neg_files[:neg_split], augment=True)
    val_ds = KWSDataset(pos_files[pos_split:], neg_files[neg_split:], augment=False)
    print(f"[train] train={len(train_ds)} val={len(val_ds)}")

    train_loader = DataLoader(train_ds, batch_size=16, shuffle=True)
    val_loader = DataLoader(val_ds, batch_size=16, shuffle=False)

    device = torch.device("cpu")
    model = KWSModel().to(device)
    optimizer = optim.Adam(model.parameters(), lr=1e-3, weight_decay=1e-4)
    scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=30)
    criterion = nn.CrossEntropyLoss()

    best_val_acc = 0.0
    best_path = os.path.join(OUT_DIR, "kws_xiaolong_best.pt")

    for epoch in range(1, 31):
        model.train()
        train_loss, train_correct, train_total = 0.0, 0, 0
        for feat, label in train_loader:
            feat, label = feat.to(device), label.to(device)
            optimizer.zero_grad()
            out = model(feat)
            loss = criterion(out, label)
            loss.backward()
            optimizer.step()
            train_loss += loss.item() * feat.size(0)
            train_correct += (out.argmax(1) == label).sum().item()
            train_total += feat.size(0)

        scheduler.step()

        model.eval()
        val_correct, val_total = 0, 0
        with torch.no_grad():
            for feat, label in val_loader:
                feat, label = feat.to(device), label.to(device)
                out = model(feat)
                val_correct += (out.argmax(1) == label).sum().item()
                val_total += feat.size(0)

        train_acc = train_correct / train_total
        val_acc = val_correct / val_total
        print(f"[epoch {epoch:02d}] loss={train_loss/train_total:.4f} "
              f"train_acc={train_acc:.3f} val_acc={val_acc:.3f}")

        if val_acc > best_val_acc:
            best_val_acc = val_acc
            torch.save(model.state_dict(), best_path)
            print(f"            ✅ 保存 best.pt (val_acc={val_acc:.3f})")

    print(f"\n[train] best val_acc={best_val_acc:.3f}")
    print(f"[train] checkpoint: {best_path}")
    return best_path


# ===================== 导出 ONNX =====================

def export_onnx(ckpt_path):
    model = KWSModel()
    model.load_state_dict(torch.load(ckpt_path, map_location="cpu"))
    model.eval()

    dummy = torch.randn(1, N_MFCC, TARGET_FRAMES)
    onnx_path = os.path.join(OUT_DIR, "kws_xiaolong.onnx")
    torch.onnx.export(
        model,
        dummy,
        onnx_path,
        input_names=["features"],
        output_names=["logits"],
        dynamic_axes={"features": {0: "batch"}, "logits": {0: "batch"}},
        opset_version=13,
    )
    print(f"[onnx] 导出: {onnx_path}")
    print(f"[onnx] 大小: {os.path.getsize(onnx_path)} 字节")

    # 验证 ONNX
    import onnx
    m = onnx.load(onnx_path)
    onnx.checker.check_model(m)
    print(f"[onnx] ✅ ONNX 模型检查通过")
    return onnx_path


if __name__ == "__main__":
    ckpt = train()
    export_onnx(ckpt)
    print(f"\n[done] 模型已导出到 {OUT_DIR}")
