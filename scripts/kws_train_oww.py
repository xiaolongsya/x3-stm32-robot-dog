"""kws_train_oww.py — 用 openWakeWord 训练 '小龙' 唤醒词(简化版)

绕过 Piper TTS 合成,直接用真人录音:
- positive: voice_data/positive/xiaolong_*.wav (100 条)
- negative: voice_data/negative/*.wav (200 条:其他名字 + 噪声切片)

参考: love-bot 训练经验(2026-09, openWakeWord 0.6.0)
"""
import os
import sys
import shutil
import random
from pathlib import Path

# 路径
BASE = Path(os.path.expanduser("~")) / "kws_xiaolong"
SRC_POS = Path("/mnt/c/Users/17402/Desktop/机器狗/voice_data/positive")
SRC_NEG = Path("/mnt/c/Users/17402/Desktop/机器狗/voice_data/negative")
MODEL_DIR = BASE / "model_xiaolong"

# 切分比: 80% train, 20% test
SPLIT = 0.8
SEED = 42

random.seed(SEED)


def split_and_copy(src_dir, out_train, out_test, prefix):
    """把 src_dir/*.wav 按 80/20 切到 out_train/out_test"""
    files = sorted(src_dir.glob("*.wav"))
    random.shuffle(files)
    n_train = int(len(files) * SPLIT)
    for i, f in enumerate(files):
        dst = out_train if i < n_train else out_test
        shutil.copy2(f, dst / f.name)
    return n_train, len(files) - n_train


def main():
    print("=" * 60)
    print(f"[train] 准备数据 → {MODEL_DIR}")
    print("=" * 60)

    # 1. 目录结构
    pos_train = MODEL_DIR / "positive_train"
    pos_test = MODEL_DIR / "positive_test"
    neg_train = MODEL_DIR / "negative_train"
    neg_test = MODEL_DIR / "negative_test"
    feat_dir = MODEL_DIR / "features"
    for d in [pos_train, pos_test, neg_train, neg_test, feat_dir]:
        d.mkdir(parents=True, exist_ok=True)

    # 2. 复制数据
    n_pos_tr, n_pos_te = split_and_copy(SRC_POS, pos_train, pos_test, "pos")
    n_neg_tr, n_neg_te = split_and_copy(SRC_NEG, neg_train, neg_test, "neg")
    print(f"[train] 正样本: train={n_pos_tr} test={n_pos_te}")
    print(f"[train] 负样本: train={n_neg_tr} test={n_neg_te}")

    # 3. 提特征(openWakeWord 内置 AudioFeatures + melspec)
    print()
    print("=" * 60)
    print(f"[train] 提取特征(openWakeWord AudioFeatures)")
    print("=" * 60)

    from openwakeword.utils import AudioFeatures

    # openWakeWord 内置特征(预训练 melspec + embedding)
    # batch_size 默认 32, 取决于 CPU
    feature_extractor = AudioFeatures(
        melspec_model_path="/home/xiaolong/openwakeword_models/melspectrogram.onnx",
        embedding_model_path="/home/xiaolong/openwakeword_models/embedding_model.onnx",
        inference_framework="onnx",
        ncpu=4,
    )

    import soundfile as sf

    def extract_features_for_paths(paths):
        """对每个 wav 路径提 embedding,返回 (n_clips, frames, 96)"""
        all_feats = []
        for i, p in enumerate(paths):
            audio, sr = sf.read(str(p))
            if sr != 16000:
                # 重采样到 16kHz
                import scipy.signal as sps
                audio = sps.resample(audio, int(len(audio) * 16000 / sr))
            if audio.ndim > 1:
                audio = audio.mean(axis=1)

            # 切到 1.6 秒(love-bot 配置 1.6s)
            target_samples = int(1.6 * 16000)
            if len(audio) >= target_samples:
                audio = audio[:target_samples]
            else:
                audio = np.pad(audio, (0, target_samples - len(audio)))

            # 转 16-bit int PCM (openwakeword 接受 int16)
            audio_int16 = (audio * 32767).astype(np.int16)

            # 单条 embed
            emb = feature_extractor.embed_clips(audio_int16.reshape(1, -1), batch_size=1)
            all_feats.append(emb[0])
            if (i + 1) % 20 == 0:
                print(f"    已处理 {i+1}/{len(paths)}")
        return np.stack(all_feats, axis=0)

    # 提正样本训练特征
    print("[train] 提正样本训练特征...")
    pos_tr_paths = sorted([str(p) for p in pos_train.glob("*.wav")])
    pos_tr_features = extract_features_for_paths(pos_tr_paths)
    np.save(feat_dir / "positive_features_train.npy", pos_tr_features)
    print(f"  shape: {pos_tr_features.shape}")

    print("[train] 提正样本测试特征...")
    pos_te_paths = sorted([str(p) for p in pos_test.glob("*.wav")])
    pos_te_features = extract_features_for_paths(pos_te_paths)
    np.save(feat_dir / "positive_features_test.npy", pos_te_features)
    print(f"  shape: {pos_te_features.shape}")

    # 提负样本特征
    print("[train] 提负样本训练特征...")
    neg_tr_paths = sorted([str(p) for p in neg_train.glob("*.wav")])
    neg_tr_features = extract_features_for_paths(neg_tr_paths)
    np.save(feat_dir / "negative_features_train.npy", neg_tr_features)
    print(f"  shape: {neg_tr_features.shape}")

    print("[train] 提负样本测试特征...")
    neg_te_paths = sorted([str(p) for p in neg_test.glob("*.wav")])
    neg_te_features = extract_features_for_paths(neg_te_paths)
    np.save(feat_dir / "negative_features_test.npy", neg_te_features)
    print(f"  shape: {neg_te_features.shape}")

    # 4. 训练
    print()
    print("=" * 60)
    print(f"[train] 训练模型(FCN, 1536→128→128→1)")
    print("=" * 60)

    import torch
    import torch.nn as nn

    def pad_to_16_frames(feats):
        """feats: (N, T, 96) → padding/截断到 (N, 16, 96)"""
        N, T, D = feats.shape
        out = np.zeros((N, 16, D), dtype=np.float32)
        for i in range(N):
            t = min(T, 16)
            out[i, :t] = feats[i, :t]
        return out

    pos_tr_features = pad_to_16_frames(pos_tr_features)
    pos_te_features = pad_to_16_frames(pos_te_features)
    neg_tr_features = pad_to_16_frames(neg_tr_features)
    neg_te_features = pad_to_16_frames(neg_te_features)
    print(f"[train] padding 后特征 shape: {pos_tr_features.shape}")

    # FCN 模型(参照 openwakeword Net)
    class WakeFCN(nn.Module):
        def __init__(self, input_dim=16*96, layer_dim=128, n_blocks=1):
            super().__init__()
            self.layer1 = nn.Linear(input_dim, layer_dim)
            self.layernorm1 = nn.LayerNorm(layer_dim)
            self.relu1 = nn.ReLU()
            self.blocks = nn.ModuleList()
            for _ in range(n_blocks):
                self.blocks.append(nn.Sequential(
                    nn.Linear(layer_dim, layer_dim),
                    nn.LayerNorm(layer_dim),
                    nn.ReLU(),
                ))
            self.last_layer = nn.Linear(layer_dim, 1)

        def forward(self, x):
            # x: (B, 16, 96)
            x = x.flatten(1)
            x = self.relu1(self.layernorm1(self.layer1(x)))
            for block in self.blocks:
                x = block(x)
            # 输出保持 (B, 1) — openWakeWord 要求
            return torch.sigmoid(self.last_layer(x))

    device = torch.device("cpu")
    model = WakeFCN().to(device)

    # 构造 DataLoader
    from torch.utils.data import DataLoader, TensorDataset

    X_tr = np.concatenate([pos_tr_features, neg_tr_features], axis=0)
    y_tr = np.concatenate([np.ones(pos_tr_features.shape[0]),
                           np.zeros(neg_tr_features.shape[0])]).astype(np.float32)
    X_te = np.concatenate([pos_te_features, neg_te_features], axis=0)
    y_te = np.concatenate([np.ones(pos_te_features.shape[0]),
                           np.zeros(neg_te_features.shape[0])]).astype(np.float32)

    print(f"[train] 训练集: X={X_tr.shape}, y={y_tr.shape}")
    print(f"[train] 测试集: X={X_te.shape}, y={y_te.shape}")

    train_ds = TensorDataset(torch.from_numpy(X_tr).float(),
                              torch.from_numpy(y_tr).float())
    train_loader = DataLoader(train_ds, batch_size=32, shuffle=True, drop_last=False)

    val_ds = TensorDataset(torch.from_numpy(X_te).float(),
                            torch.from_numpy(y_te).float())
    val_loader = DataLoader(val_ds, batch_size=32, shuffle=False)

    optimizer = torch.optim.Adam(model.parameters(), lr=1e-3, weight_decay=1e-4)
    criterion = nn.BCELoss()

    best_acc = 0.0
    best_path = MODEL_DIR / "xiaolong.pt"
    for epoch in range(1, 21):
        model.train()
        train_loss, train_correct, train_total = 0.0, 0, 0
        for xb, yb in train_loader:
            xb, yb = xb.to(device), yb.to(device)
            optimizer.zero_grad()
            out = model(xb).squeeze(-1)
            loss = criterion(out, yb)
            loss.backward()
            optimizer.step()
            train_loss += loss.item() * xb.size(0)
            train_correct += ((out > 0.5).float() == yb).sum().item()
            train_total += xb.size(0)

        model.eval()
        val_correct, val_total = 0, 0
        with torch.no_grad():
            for xb, yb in val_loader:
                xb, yb = xb.to(device), yb.to(device)
                out = model(xb).squeeze(-1)
                val_correct += ((out > 0.5).float() == yb).sum().item()
                val_total += xb.size(0)

        train_acc = train_correct / train_total
        val_acc = val_correct / val_total
        print(f"[epoch {epoch:02d}] loss={train_loss/train_total:.4f} "
              f"train_acc={train_acc:.3f} val_acc={val_acc:.3f}")

        if val_acc > best_acc:
            best_acc = val_acc
            torch.save(model.state_dict(), best_path)
            print(f"            ✅ 保存 best.pt (val_acc={val_acc:.3f})")

    print()
    print("=" * 60)
    print(f"[train] best val_acc={best_acc:.3f}")
    print("=" * 60)

    # 5. 导出 ONNX
    print("[export] 导出 ONNX...")
    model.load_state_dict(torch.load(best_path, map_location="cpu"))
    model.eval()

    dummy = torch.randn(1, 16, 96)
    onnx_path = MODEL_DIR / "xiaolong.onnx"
    torch.onnx.export(
        model,
        dummy,
        onnx_path,
        input_names=["embedding"],
        output_names=["score"],
        dynamic_axes={"embedding": {0: "batch"}, "score": {0: "batch"}},
        opset_version=18,
    )
    print(f"[export] ✅ {onnx_path} ({os.path.getsize(onnx_path)} 字节)")

    print()
    print("=" * 60)
    print(f"[export] 模型部署提示:")
    print(f"  - WSL2: ~/kws_xiaolong/model_xiaolong/xiaolong.onnx")
    print(f"  - Pi 推理需要 melspec+embedding 预处理(openwakeword 自动)")
    print(f"  - 用 openwakeword.Model(wakeword_model_paths=['xiaolong.onnx']) + predict()")
    print("=" * 60)


if __name__ == "__main__":
    import numpy as np
    main()
