#!/bin/bash
# record_env_noise.sh — Pi 端持续录音环境噪声
# 用法: ./record_env_noise.sh [时长分钟] [输出WAV路径]
# 默认: 5 分钟, /tmp/voice_data/env_noise_<时间戳>.wav
# 设备: plughw:1,0 (USB Microphone 1b3f:0004)
# 格式: S16_LE / 16kHz / mono

set -e

MINUTES="${1:-5}"
if [ -n "$2" ]; then
    OUT="$2"
else
    OUT="/tmp/voice_data/env_noise_$(date +%Y%m%d_%H%M%S).wav"
fi
SECONDS=$((MINUTES * 60))

pkill -9 arecord 2>/dev/null || true
sleep 0.5

mkdir -p "$(dirname "$OUT")"

echo "=========================================="
echo "[record_env_noise] 设备  : plughw:1,0 (USB Microphone)"
echo "[record_env_noise] 时长  : ${MINUTES} 分钟 = ${SECONDS} 秒"
echo "[record_env_noise] 输出  : $OUT"
echo "[record_env_noise] 格式  : S16_LE / 16000Hz / mono"
echo "=========================================="
echo
echo "[record_env_noise] ⏺️   开始录环境噪声(可走开或制造典型背景声)"
echo "[record_env_noise]     TV/风扇/别人说话/音乐/安静...任意场景"
echo "[record_env_noise]     中止: Ctrl+C"
echo

# timeout 多 5 秒保险
timeout $((SECONDS + 5)) arecord -D plughw:1,0 -d "$SECONDS" -f S16_LE -r 16000 -c 1 "$OUT" 2>&1 | grep -v "^\s*$" || true

SIZE=$(stat -c%s "$OUT")
DURATION_ACT=$(awk -v s=$SIZE 'BEGIN{printf "%.2f", (s-44)/32000}')

echo
echo "[record_env_noise] ✅ 完成"
echo "[record_env_noise] 文件  : $OUT"
echo "[record_env_noise] 大小  : ${SIZE} 字节"
echo "[record_env_noise] 时长  : ≈${DURATION_ACT} 秒 (≈$(awk -v s=$DURATION_ACT 'BEGIN{printf "%.2f", s/60}') 分钟)"
echo
echo "[record_env_noise] 下一步: 跑 slice_wav.py 切成 2 秒片段当负样本"
