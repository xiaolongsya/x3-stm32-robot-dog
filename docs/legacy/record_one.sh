#!/bin/bash
# record_one.sh — Pi 端单次录音脚本
# 用法: ./record_one.sh [时长秒] [输出WAV路径]
# 默认: 5 秒, /tmp/recordings/record_<时间戳>.wav
# 设备: plughw:1,0(USB 会议麦克风 1b3f:0004)
# 格式: S16_LE / 16kHz / mono(sherpa-onnx KWS 标准采样率)

set -e

DURATION="${1:-5}"
if [ -n "$2" ]; then
    OUT="$2"
else
    OUT="/tmp/recordings/record_$(date +%Y%m%d_%H%M%S).wav"
fi

# 杀掉之前 hang 的 arecord(防设备忙)
pkill -9 arecord 2>/dev/null || true
sleep 0.5

mkdir -p "$(dirname "$OUT")"

echo "[record_one] 设备  : plughw:1,0 (USB Microphone 1b3f:0004)"
echo "[record_one] 时长  : ${DURATION} 秒"
echo "[record_one] 格式  : S16_LE / 16000Hz / mono"
echo "[record_one] 输出  : $OUT"
echo "[record_one] ⏺️   现在开始录音,请对麦克风说话..."
echo

# timeout 防止设备异常导致无限 hang
timeout $((DURATION + 3)) arecord -D plughw:1,0 -d "$DURATION" -f S16_LE -r 16000 -c 1 "$OUT"

SIZE=$(stat -c%s "$OUT")
# 16kHz * 2字节 * 1通道 = 32000 字节/秒,减去 44 字节 WAV 头
DURATION_ACT=$(awk -v s=$SIZE 'BEGIN{printf "%.2f", (s-44)/32000}')

echo
echo "[record_one] ✅ 完成"
echo "[record_one] 文件  : $OUT"
echo "[record_one] 大小  : ${SIZE} 字节 (≈${DURATION_ACT} 秒)"
