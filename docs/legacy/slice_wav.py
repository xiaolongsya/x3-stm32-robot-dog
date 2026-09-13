#!/usr/bin/env python3
"""slice_wav.py — 把长 WAV 按固定秒数切成 N 个片段(纯 Python wave 模块)

用法: slice_wav.py <输入WAV> <片段秒> <输出目录> [前缀] [--resume]

例  : slice_wav.py /tmp/env_noise_xxx.wav 2 /tmp/voice_data/negative/noise

约定: 输出 WAV 16kHz mono(因为 KWS 用)。如果输入不是,会报错。
"""
import wave
import os
import sys
import argparse


def main():
    ap = argparse.ArgumentParser(description="把长 WAV 切成固定秒数片段")
    ap.add_argument("input", help="输入 WAV 路径")
    ap.add_argument("slice_sec", type=float, help="每片段秒数")
    ap.add_argument("outdir", help="输出目录")
    ap.add_argument("--prefix", default="noise", help="文件名前缀(默认 noise)")
    ap.add_argument("--resume", action="store_true", help="跳过已存在的片段")
    args = ap.parse_args()

    if not os.path.isfile(args.input):
        print(f"[ERR] 输入不存在: {args.input}")
        sys.exit(1)

    os.makedirs(args.outdir, exist_ok=True)

    with wave.open(args.input, "rb") as wf:
        n_ch = wf.getnchannels()
        samp_w = wf.getsampwidth()
        fr = wf.getframerate()
        n_fr = wf.getnframes()
        dur = n_fr / fr

        # KWS 强约束: 必须 16kHz mono 16bit
        if n_ch != 1 or fr != 16000 or samp_w != 2:
            print(f"[WARN] 输入是 {n_ch}ch {fr}Hz {samp_w*8}bit,非 16kHz mono 16bit")
            print(f"       仍按原样切片,但训练时可能需重采样")

        slice_frames = int(args.slice_sec * fr)
        if slice_frames <= 0:
            print(f"[ERR] 片段秒数过小或非正: {args.slice_sec}")
            sys.exit(1)

        n_slices = n_fr // slice_frames
        remainder = n_fr % slice_frames

        print(f"[slice_wav] 输入    : {args.input}")
        print(f"[slice_wav] 总时长  : {dur:.2f} 秒 ({n_fr} 帧)")
        print(f"[slice_wav] 通道    : {n_ch}")
        print(f"[slice_wav] 采样率  : {fr} Hz")
        print(f"[slice_wav] 位深    : {samp_w*8} bit")
        print(f"[slice_wav] 片段    : {args.slice_sec}s × {slice_frames} 帧")
        print(f"[slice_wav] 预计生成: {n_slices} 个片段")
        if remainder > 0:
            print(f"[slice_wav] 末尾    : {remainder/fr:.2f}s 不足一片(丢弃)")
        print(f"[slice_wav] 输出    : {args.outdir}/{args.prefix}_NNN.wav")
        print()

        ok = 0
        skip = 0
        for i in range(n_slices):
            out_path = os.path.join(args.outdir, f"{args.prefix}_{i+1:03d}.wav")
            if args.resume and os.path.isfile(out_path) and os.path.getsize(out_path) > 1000:
                skip += 1
                continue

            wf.setpos(i * slice_frames)
            frames = wf.readframes(slice_frames)
            with wave.open(out_path, "wb") as out:
                out.setnchannels(n_ch)
                out.setsampwidth(samp_w)
                out.setframerate(fr)
                out.writeframes(frames)
            ok += 1

            # 每 20 个报一次进度
            if (i + 1) % 20 == 0:
                print(f"[slice_wav] 进度: {i+1}/{n_slices}")

    print()
    print(f"[slice_wav] ✅ 完成")
    print(f"[slice_wav] 新生成: {ok}")
    if skip > 0:
        print(f"[slice_wav] 跳过  : {skip} (--resume)")


if __name__ == "__main__":
    main()
