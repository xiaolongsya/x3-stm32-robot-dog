#!/usr/bin/env python3
"""kws_realtime_log.py — X3 端实时 KWS 唤醒日志

实时监听麦克风,score ≥ 阈值时打印唤醒日志(中文 + 时间),其余时间打进度条。
阈值默认 0.7,冷却 1.5 秒(同一次唤醒只报一次)。

用法 (从麦克风 stdin):
    arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 -t raw 2>/dev/null \\
        | python3 /root/kws/kws_realtime_log.py

用法 (从 wav 文件,调试用):
    arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 -d 30 out.wav
    python3 /root/kws/kws_realtime_log.py --file out.wav

参数:
    --threshold 0.7   唤醒阈值
    --cooldown  1.5   唤醒冷却秒数
    --model PATH      模型路径(默认 /root/kws/xiaolong.onnx,可用 env KWS_MODEL 覆盖)
    --quiet           安静模式(只打唤醒,不打印进度条)

中止: Ctrl+C
"""
import argparse
import os
import sys
import time
import wave
import numpy as np

DEFAULT_MODEL = "/root/kws/xiaolong.onnx"
CHUNK = 1280   # 80ms @ 16kHz
RATE = 16000


def parse_args():
    ap = argparse.ArgumentParser(description="X3 端实时 KWS 唤醒日志")
    ap.add_argument("--threshold", type=float, default=0.7,
                    help="唤醒阈值(默认 0.7)")
    ap.add_argument("--cooldown", type=float, default=1.5,
                    help="唤醒冷却秒数(默认 1.5)")
    ap.add_argument("--model", type=str,
                    default=os.environ.get("KWS_MODEL", DEFAULT_MODEL),
                    help=f"模型路径(默认 {DEFAULT_MODEL})")
    ap.add_argument("--file", type=str, default=None,
                    help="从 wav 文件读取(默认 stdin,搭配 arecord)")
    ap.add_argument("--quiet", action="store_true",
                    help="安静模式(只打唤醒,不打印进度条)")
    return ap.parse_args()


def open_stream(file_path):
    """返回 (read_fn, sample_rate),读一次返回 CHUNK 字节;读完返回 b''."""
    if file_path is None:
        # stdin:arecord pipe
        chunk_bytes = CHUNK * 2
        def _read():
            data = sys.stdin.buffer.read(chunk_bytes)
            return data, RATE
        return _read

    # wav 文件:按 16kHz/mono/S16_LE 假设(脚本只测自家训练模型)
    wf = wave.open(file_path, "rb")
    assert wf.getframerate() == RATE, f"wav 采样率必须 {RATE},实际 {wf.getframerate()}"
    assert wf.getnchannels() == 1, "wav 必须是单声道"
    assert wf.getsampwidth() == 2, "wav 必须是 int16"
    chunk_bytes = CHUNK * 2
    def _read():
        data = wf.readframes(CHUNK)
        return data, RATE
    return _read


def main():
    args = parse_args()

    if not os.path.isfile(args.model):
        print(f"[ERR] 模型不存在: {args.model}", flush=True)
        sys.exit(1)

    try:
        from openwakeword.model import Model
    except ImportError:
        print("[ERR] openwakeword 未装,先: pip3 install openwakeword onnxruntime",
              flush=True)
        sys.exit(1)

    oww = Model(wakeword_models=[args.model], inference_framework="onnx")
    print(f"[kws] ✅ 模型: {args.model}", flush=True)
    print(f"[kws] 阈值: {args.threshold}  冷却: {args.cooldown}s", flush=True)
    if args.file:
        print(f"[kws] 输入源: 文件 {args.file}", flush=True)
    else:
        print(f"[kws] 输入源: stdin(搭配 arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 -t raw)",
              flush=True)
    print(f"[kws] 监听中 —— 说 '小龙' 测试(80ms 一段实时打分)", flush=True)
    print("=" * 64, flush=True)

    last_wake_ts = 0.0
    peak_recent = 0.0      # 最近 25 帧峰值
    tick = 0
    read_fn = open_stream(args.file)

    while True:
        chunk, _sr = read_fn()
        if not chunk or len(chunk) < CHUNK * 2:
            break

        pcm = np.frombuffer(chunk, dtype=np.int16)
        preds = oww.predict(pcm)

        ts = time.strftime("%H:%M:%S")
        now = time.time()

        # 取唯一挂载的模型 key(只部署了 xiaolong 一个)
        if not preds:
            continue
        key = next(iter(preds.keys()))
        s = preds[key]
        try:
            score = float(s[-1]) if isinstance(s, (np.ndarray, list)) else float(s)
        except (TypeError, IndexError):
            continue

        if score > peak_recent:
            peak_recent = score

        if score >= args.threshold and (now - last_wake_ts) > args.cooldown:
            print(f"[{ts}] 🌟 唤醒 '小龙' (score={score:.3f}, 近期峰值={peak_recent:.3f})",
                  flush=True)
            last_wake_ts = now
            peak_recent = 0.0
        elif not args.quiet:
            tick += 1
            if tick % 25 == 0:  # 每 ≈2 秒打一行
                bar = "█" * int(min(score, 1.0) * 20) + "░" * (20 - int(min(score, 1.0) * 20))
                print(f"[{ts}] score={score:.3f} (peak={peak_recent:.3f}) [{bar}]",
                      flush=True)
                peak_recent = 0.0


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[kws] 已停止", flush=True)
        sys.exit(0)
