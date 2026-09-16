#!/bin/bash
# rec.sh — 在 Git Bash / WSL / Linux / macOS 上用 arecord/ffmpeg 录 16k 单声道 wav
# 用法: ./rec.sh sit.wav 3       # 录 sit.wav 持续 3 秒
#       ./rec.sh sit.wav         # 录到 Ctrl+C

set -e
OUT="$1"
DUR="${2:-0}"   # 0 = 录到 Ctrl+C

if [ -z "$OUT" ]; then
    echo "用法: $0 <output.wav> [duration_sec]"
    echo "  持续秒数默认 0 = 按 Ctrl+C 结束"
    exit 1
fi

# 优先 ffmpeg,fallback arecord
if command -v ffmpeg > /dev/null; then
    if [ "$DUR" -gt 0 ] 2>/dev/null; then
        ffmpeg -f avfoundation -i ":0" -ar 16000 -ac 1 -t "$DUR" "$OUT" -y
    else
        ffmpeg -f avfoundation -i ":0" -ar 16000 -ac 1 "$OUT" -y
    fi
elif command -v arecord > /dev/null; then
    if [ "$DUR" -gt 0 ] 2>/dev/null; then
        arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 -d "$DUR" "$OUT"
    else
        arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 "$OUT"
    fi
else
    echo "需要 ffmpeg 或 arecord(alsa-utils),二选一装上"
    exit 1
fi

echo "保存: $OUT"