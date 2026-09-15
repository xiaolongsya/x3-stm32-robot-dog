#!/usr/bin/env python3
"""record_wakeword.py — X3 端交互式唤醒词数据采集

用法:
    # 正样本 100 次,每次 2 秒(念 "小龙")
    python3 record_wakeword.py positive 100 2 /home/root/voice_data/positive

    # 负样本 100 次,每次 2 秒(10 词 × 10 轮,自动轮换)
    python3 record_wakeword.py negative 100 2 /home/root/voice_data/negative

    # 环境噪声 连续录 300 秒(5 分钟)
    python3 record_wakeword.py env 300 /home/root/voice_data/env.wav

设备: plughw:0,0 — X3 USB 麦 card 0 device 0 (MUSIC-BOOST MB-306, 1b3f:0004)
格式: 16kHz / mono / S16_LE (openWakeWord 标准)

前置:
    arecord (alsa-utils) — X3 镜像已预装

中止: Ctrl+C(已录的保留)
"""
import os
import sys
import subprocess
import time
from pathlib import Path

DEV = "plughw:0,0"
SR = 16000
CH = 1

# 负面词表:10 个常见两字人名,避开"小龙"避免语义混淆。
# 改这里能换词表(每条 i 按 (i-1) % len 取词,10 词 × 10 轮 = 100 条)
NEG_WORDS = [
    "小明", "小红", "小芳", "小华", "小刚",
    "小亮", "小峰", "小军", "小杰", "小强",
]


def record_wav(out: Path, dur_sec: int):
    """调 arecord 录 dur_sec 秒到 out,失败抛 RuntimeError。"""
    # 杀掉残留 arecord(防设备忙)
    subprocess.run(["pkill", "-9", "arecord"], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.2)

    cmd = [
        "arecord", "-D", DEV,
        "-d", str(dur_sec),
        "-f", "S16_LE", "-r", str(SR), "-c", str(CH),
        str(out),
    ]
    # timeout 保险:设备异常时不会无限 hang
    proc = subprocess.run(
        cmd, timeout=dur_sec + 5,
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )
    if proc.returncode != 0 or not out.exists() or out.stat().st_size < 1000:
        raise RuntimeError(
            f"录音失败 rc={proc.returncode},stderr={proc.stderr.decode(errors='ignore')[:200]}"
        )


def mode_positive(count: int, dur: int, outdir: Path):
    outdir.mkdir(parents=True, exist_ok=True)
    print("=" * 56, flush=True)
    print(f"[record_wakeword] 模式: 正样本 (positive)", flush=True)
    print(f"[record_wakeword] 次数: {count} | 时长: {dur}s | 输出: {outdir}", flush=True)
    print(f"[record_wakeword] 设备: {DEV}", flush=True)
    print(f"[record_wakeword] 内容: 念 '小龙' (中文两字)", flush=True)
    print(f"[record_wakeword] 节奏: 按 Enter → 录 {dur}s → 报进度 → 等 Enter", flush=True)
    print(f"[record_wakeword] 中止: Ctrl+C (已录的保留)", flush=True)
    print("=" * 56, flush=True)
    print(flush=True)

    ok, fail = 0, 0
    t0 = time.time()
    for i in range(1, count + 1):
        out = outdir / f"positive_{i:03d}.wav"
        try:
            input(f"\n[{i:3d}/{count}] 念 '小龙' → 按 [Enter] 开始录 {dur}s ...")
            print(f"    ⏺️  录音中 ...", end="", flush=True)
            record_wav(out, dur)
            sz = out.stat().st_size
            ok += 1
            print(f" ✅ ({sz} 字节)", flush=True)
        except (KeyboardInterrupt, EOFError):
            print(f"\n[中断] 已录 {ok}/{count},剩 {count - ok} 条未录。", flush=True)
            sys.exit(130)
        except Exception as e:
            fail += 1
            print(f" ❌ {e}", flush=True)

    dt = time.time() - t0
    print(flush=True)
    print("=" * 56, flush=True)
    print(f"[record_wakeword] ✅ 完成: {ok}/{count} (失败 {fail})", flush=True)
    print(f"[record_wakeword] 耗时: {dt:.0f}s ({dt / 60:.1f} 分钟)", flush=True)
    print("=" * 56, flush=True)


def mode_negative(count: int, dur: int, outdir: Path):
    outdir.mkdir(parents=True, exist_ok=True)
    print("=" * 56, flush=True)
    print(f"[record_wakeword] 模式: 负样本 (negative)", flush=True)
    print(f"[record_wakeword] 次数: {count} | 时长: {dur}s | 输出: {outdir}", flush=True)
    print(f"[record_wakeword] 设备: {DEV}", flush=True)
    print(f"[record_wakeword] 词表({len(NEG_WORDS)} 词): {', '.join(NEG_WORDS)}", flush=True)
    print(f"[record_wakeword] 节奏: 按 Enter → 念屏幕提示词 → 录 {dur}s", flush=True)
    print(f"[record_wakeword] 中止: Ctrl+C (已录的保留)", flush=True)
    print("=" * 56, flush=True)
    print(flush=True)

    ok, fail = 0, 0
    t0 = time.time()
    for i in range(1, count + 1):
        word = NEG_WORDS[(i - 1) % len(NEG_WORDS)]
        out = outdir / f"negative_{word}_{i:03d}.wav"
        try:
            input(f"\n[{i:3d}/{count}] 念 '{word}' → 按 [Enter] 开始录 {dur}s ...")
            print(f"    ⏺️  录音中 ...", end="", flush=True)
            record_wav(out, dur)
            sz = out.stat().st_size
            ok += 1
            print(f" ✅ ({sz} 字节)", flush=True)
        except (KeyboardInterrupt, EOFError):
            print(f"\n[中断] 已录 {ok}/{count},剩 {count - ok} 条未录。", flush=True)
            sys.exit(130)
        except Exception as e:
            fail += 1
            print(f" ❌ {e}", flush=True)

    dt = time.time() - t0
    print(flush=True)
    print("=" * 56, flush=True)
    print(f"[record_wakeword] ✅ 完成: {ok}/{count} (失败 {fail})", flush=True)
    print(f"[record_wakeword] 耗时: {dt:.0f}s ({dt / 60:.1f} 分钟)", flush=True)
    print("=" * 56, flush=True)


def mode_env(dur_sec: int, outfile: Path):
    outfile.parent.mkdir(parents=True, exist_ok=True)
    print("=" * 56, flush=True)
    print(f"[record_wakeword] 模式: 环境噪声 (env)", flush=True)
    print(f"[record_wakeword] 时长: {dur_sec}s ({dur_sec / 60:.1f} 分钟)", flush=True)
    print(f"[record_wakeword] 设备: {DEV}", flush=True)
    print(f"[record_wakeword] 输出: {outfile}", flush=True)
    print(f"[record_wakeword] 内容: 不说话,让背景声自然进来", flush=True)
    print(f"[record_wakeword]      (风扇 / 别人说话 / 音乐 / 安静都行)", flush=True)
    print(f"[record_wakeword] 中止: Ctrl+C", flush=True)
    print("=" * 56, flush=True)
    print(flush=True)

    t0 = time.time()
    try:
        record_wav(outfile, dur_sec)
    except KeyboardInterrupt:
        print("\n[中断]", flush=True)
        sys.exit(130)

    dt = time.time() - t0
    sz = outfile.stat().st_size if outfile.exists() else 0
    print(flush=True)
    print("=" * 56, flush=True)
    print(f"[record_wakeword] ✅ 完成 ({dt:.0f}s, {sz} 字节)", flush=True)
    print(f"[record_wakeword] 下一步:", flush=True)
    print(f"  python3 slice_env.py {outfile} 2 <out_dir>", flush=True)
    print("=" * 56, flush=True)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    mode = sys.argv[1]
    if mode == "positive":
        if len(sys.argv) != 5:
            print("[ERR] 用法: record_wakeword.py positive <count> <dur_sec> <outdir>")
            sys.exit(1)
        count, dur, outdir = int(sys.argv[2]), int(sys.argv[3]), Path(sys.argv[4])
        mode_positive(count, dur, outdir)
    elif mode == "negative":
        if len(sys.argv) != 5:
            print("[ERR] 用法: record_wakeword.py negative <count> <dur_sec> <outdir>")
            sys.exit(1)
        count, dur, outdir = int(sys.argv[2]), int(sys.argv[3]), Path(sys.argv[4])
        mode_negative(count, dur, outdir)
    elif mode == "env":
        if len(sys.argv) != 4:
            print("[ERR] 用法: record_wakeword.py env <dur_sec> <outfile>")
            sys.exit(1)
        dur, outfile = int(sys.argv[2]), Path(sys.argv[3])
        mode_env(dur, outfile)
    else:
        print(f"[ERR] 未知模式: {mode}")
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
