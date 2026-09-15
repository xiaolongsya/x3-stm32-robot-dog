"""train_xiaolong.py — PC 端训练 "小龙" 唤醒词模型

基于 docs/legacy/kws_train_oww.py 改写:
- 数据路径改为 CLI 参数(不再硬编码 /mnt/c/.../机器狗/voice_data)
- 输出目录可指定
- 加入环境噪声切片目录(可选)
- 训练参数可调

用法:
    # 默认:从 ../voice_data 读,输出到 ./model_xiaolong
    python train_xiaolong.py

    # 显式指定
    python train_xiaolong.py \
        --pos-dir ../voice_data/positive \
        --neg-dirs ../voice_data/negative ../voice_data/negative_env \
        --out-dir ./model_xiaolong \
        --epochs 20 \
        --batch-size 32

前置:
    pip install torch soundfile scipy numpy openwakeword onnxruntime
"""
import argparse
import os
import sys
import shutil
import random
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn


def parse_args():
    ap = argparse.ArgumentParser(description="训练 '小龙' 唤醒词模型")
    ap.add_argument("--pos-dir", type=Path,
                    default=Path(__file__).parent.parent / "voice_data" / "positive",
                    help="正样本目录")
    ap.add_argument("--neg-dirs", type=Path, nargs="+",
                    default=[
                        Path(__file__).parent.parent / "voice_data" / "negative",
                        Path(__file__).parent.parent / "voice_data" / "negative_env",
                    ],
                    help="负样本目录(可多个)")
    ap.add_argument("--out-dir", type=Path,
                    default=Path(__file__).parent / "model_xiaolong",
                    help="输出目录")
    ap.add_argument("--epochs", type=int, default=20)
    ap.add_argument("--batch-size", type=int, default=32)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--split", type=float, default=0.8, help="train/test 切分比")
    ap.add_argument("--seed", type=int, default=42)
    return ap.parse_args()


def split_and_copy(src_dir: Path, out_train: Path, out_test: Path, prefix: str):
    """把 src_dir/*.wav 按 --split 切到 out_train/out_test(就地软链接避免复制)"""
    files = sorted(src_dir.glob("*.wav"))
    if not files:
        raise RuntimeError(f"{src_dir} 下没有 wav")
    random.shuffle(files)
    n_train = int(len(files) * args.split)
    n_test = len(files) - n_train
    for i, f in enumerate(files):
        dst = out_train if i < n_train else out_test
        link = dst / f.name
        if link.exists() or link.is_symlink():
            link.unlink()
        try:
            os.symlink(f, link)
        except OSError:
            # Windows 没权限做 symlink 就复制
            shutil.copy2(f, link)
    return n_train, n_test


def extract_features_for_paths(paths):
    """对每个 wav 提 embedding,返回 (n_clips, frames, 96)"""
    import soundfile as sf
    from openwakeword.utils import AudioFeatures

    feature_extractor = AudioFeatures(inference_framework="onnx", ncpu=4)

    all_feats = []
    for i, p in enumerate(paths):
        audio, sr = sf.read(str(p))
        if sr != 16000:
            import scipy.signal as sps
            audio = sps.resample(audio, int(len(audio) * 16000 / sr))
        if audio.ndim > 1:
            audio = audio.mean(axis=1)

        target_samples = int(1.6 * 16000)
        if len(audio) >= target_samples:
            audio = audio[:target_samples]
        else:
            audio = np.pad(audio, (0, target_samples - len(audio)))

        audio_int16 = (audio * 32767).astype(np.int16)

        emb = feature_extractor.embed_clips(audio_int16.reshape(1, -1), batch_size=1)
        all_feats.append(emb[0])
        if (i + 1) % 20 == 0:
            print(f"    已处理 {i + 1}/{len(paths)}")
    return np.stack(all_feats, axis=0)


def pad_to_16_frames(feats):
    """feats: (N, T, 96) → padding/截断到 (N, 16, 96)"""
    N, T, D = feats.shape
    out = np.zeros((N, 16, D), dtype=np.float32)
    for i in range(N):
        t = min(T, 16)
        out[i, :t] = feats[i, :t]
    return out


class WakeFCN(nn.Module):
    """FCN: input_dim → 128 → 128 → 1 (sigmoid)

    输入 (B, 16, 96) flatten → (B, 1536) → Linear → LayerNorm → ReLU
                   → n_blocks × [Linear → LayerNorm → ReLU]
                   → Linear → sigmoid
    """

    def __init__(self, input_dim=16 * 96, layer_dim=128, n_blocks=1):
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
        x = x.flatten(1)
        x = self.relu1(self.layernorm1(self.layer1(x)))
        for block in self.blocks:
            x = block(x)
        return torch.sigmoid(self.last_layer(x))


def main_with_args(_args):
    global args
    args = _args

    random.seed(args.seed)
    np.random.seed(args.seed)

    out_dir: Path = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    print("=" * 60)
    print(f"[train] 输出目录: {out_dir}")
    print("=" * 60)

    pos_train = out_dir / "positive_train"
    pos_test = out_dir / "positive_test"
    neg_train = out_dir / "negative_train"
    neg_test = out_dir / "negative_test"
    feat_dir = out_dir / "features"
    for d in [pos_train, pos_test, neg_train, neg_test, feat_dir]:
        d.mkdir(parents=True, exist_ok=True)

    # ── 切分正样本 ──
    n_pos_tr, n_pos_te = split_and_copy(args.pos_dir, pos_train, pos_test, "pos")
    print(f"[train] 正样本: train={n_pos_tr} test={n_pos_te}")

    # ── 合并多个负样本目录 ──
    n_neg_tr_total, n_neg_te_total = 0, 0
    for neg_src in args.neg_dirs:
        if not neg_src.exists():
            print(f"[train] ⚠️  跳过不存在: {neg_src}")
            continue
        n_tr, n_te = split_and_copy(neg_src, neg_train, neg_test, neg_src.name)
        print(f"[train] 负样本({neg_src.name}): train={n_tr} test={n_te}")
        n_neg_tr_total += n_tr
        n_neg_te_total += n_te
    print(f"[train] 负样本合计: train={n_neg_tr_total} test={n_neg_te_total}")

    # ── 提特征 ──
    print()
    print("=" * 60)
    print("[train] 提取特征 (openWakeWord AudioFeatures + embed_clips)")
    print("=" * 60)

    def _extract(split_dir, tag):
        paths = sorted([str(p) for p in split_dir.glob("*.wav")])
        if not paths:
            raise RuntimeError(f"{split_dir} 没有 wav")
        print(f"[train] 提 {tag} 特征 ({len(paths)} 条)...")
        feats = extract_features_for_paths(paths)
        out = feat_dir / f"{tag}.npy"
        np.save(out, feats)
        print(f"  shape: {feats.shape} → {out}")
        return feats

    pos_tr = _extract(pos_train, "positive_features_train")
    pos_te = _extract(pos_test, "positive_features_test")
    neg_tr = _extract(neg_train, "negative_features_train")
    neg_te = _extract(neg_test, "negative_features_test")

    pos_tr = pad_to_16_frames(pos_tr)
    pos_te = pad_to_16_frames(pos_te)
    neg_tr = pad_to_16_frames(neg_tr)
    neg_te = pad_to_16_frames(neg_te)
    print(f"[train] padding 后 shape: {pos_tr.shape}")

    # ── 训练 ──
    print()
    print("=" * 60)
    print(f"[train] FCN 训练 ({args.epochs} epoch, batch={args.batch_size}, lr={args.lr})")
    print("=" * 60)

    from torch.utils.data import DataLoader, TensorDataset

    device = torch.device("cpu")
    model = WakeFCN().to(device)
    print(f"[train] 模型参数: {sum(p.numel() for p in model.parameters())}")

    X_tr = np.concatenate([pos_tr, neg_tr], axis=0)
    y_tr = np.concatenate([np.ones(pos_tr.shape[0]),
                           np.zeros(neg_tr.shape[0])]).astype(np.float32)
    X_te = np.concatenate([pos_te, neg_te], axis=0)
    y_te = np.concatenate([np.ones(pos_te.shape[0]),
                           np.zeros(neg_te.shape[0])]).astype(np.float32)
    print(f"[train] 训练集: X={X_tr.shape}, y={y_tr.shape}")
    print(f"[train] 测试集: X={X_te.shape}, y={y_te.shape}")

    train_loader = DataLoader(
        TensorDataset(torch.from_numpy(X_tr).float(), torch.from_numpy(y_tr).float()),
        batch_size=args.batch_size, shuffle=True, drop_last=False,
    )
    val_loader = DataLoader(
        TensorDataset(torch.from_numpy(X_te).float(), torch.from_numpy(y_te).float()),
        batch_size=args.batch_size, shuffle=False,
    )

    optimizer = torch.optim.Adam(model.parameters(), lr=args.lr, weight_decay=1e-4)
    criterion = nn.BCELoss()

    best_acc = 0.0
    best_pt = out_dir / "xiaolong.pt"
    for epoch in range(1, args.epochs + 1):
        model.train()
        t_loss, t_correct, t_total = 0.0, 0, 0
        for xb, yb in train_loader:
            xb, yb = xb.to(device), yb.to(device)
            optimizer.zero_grad()
            out = model(xb).squeeze(-1)
            loss = criterion(out, yb)
            loss.backward()
            optimizer.step()
            t_loss += loss.item() * xb.size(0)
            t_correct += ((out > 0.5).float() == yb).sum().item()
            t_total += xb.size(0)

        model.eval()
        v_correct, v_total = 0, 0
        with torch.no_grad():
            for xb, yb in val_loader:
                xb, yb = xb.to(device), yb.to(device)
                out = model(xb).squeeze(-1)
                v_correct += ((out > 0.5).float() == yb).sum().item()
                v_total += xb.size(0)

        t_acc = t_correct / t_total
        v_acc = v_correct / v_total
        print(f"[epoch {epoch:02d}/{args.epochs}] "
              f"loss={t_loss / t_total:.4f} "
              f"train_acc={t_acc:.3f} val_acc={v_acc:.3f}")

        if v_acc > best_acc:
            best_acc = v_acc
            torch.save(model.state_dict(), best_pt)
            print(f"            ✅ 保存 best.pt (val_acc={v_acc:.3f})")

    print()
    print("=" * 60)
    print(f"[train] best val_acc = {best_acc:.3f}")
    print("=" * 60)

    # ── 导出 ONNX ──
    print("[export] 导出 ONNX...")
    model.load_state_dict(torch.load(best_pt, map_location="cpu"))
    model.eval()

    dummy = torch.randn(1, 16, 96)
    onnx_path = out_dir / "xiaolong.onnx"
    torch.onnx.export(
        model, dummy, onnx_path,
        input_names=["embedding"], output_names=["score"],
        dynamic_axes={"embedding": {0: "batch"}, "score": {0: "batch"}},
        opset_version=18,
    )
    print(f"[export] ✅ {onnx_path} ({os.path.getsize(onnx_path)} 字节)")
    print()
    print("=" * 60)
    print("[export] 部署提示:")
    print(f"  PC:  {onnx_path}")
    print(f"  X3:  scp 到 /home/root/kws/xiaolong.onnx")
    print(f"       openwakeword.Model(wakeword_model_paths=['/home/root/kws/xiaolong.onnx'])")
    print("=" * 60)


if __name__ == "__main__":
    args = parse_args()
    main_with_args(args)
