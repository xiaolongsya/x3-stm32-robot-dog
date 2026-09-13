#!/bin/bash
# record_manual.sh — Pi 端手动录音脚本(按回车开始,完全自己控制节奏)
# 用法: ./record_manual.sh [时长秒] [输出WAV路径]
# 默认: 2 秒, /tmp/recordings/manual_<时间戳>.wav
# 设备: plughw:1,0 (USB Microphone 1b3f:0004)
# 格式: S16_LE / 16kHz / mono (sherpa-onnx KWS 标准采样率)

set -e

DURATION="${1:-2}"
if [ -n "$2" ]; then
    OUT="$2"
else
    OUT="/tmp/recordings/manual_$(date +%Y%m%d_%H%M%S).wav"
fi

# 杀掉之前 hang 的 arecord(防设备忙)
pkill -9 arecord 2>/dev/null || true
sleep 0.5

mkdir -p "$(dirname "$OUT")"

echo
echo "=========================================="
echo "[record_manual] 设备  : plughw:1,0 (USB Microphone 1b3f:0004)"
echo "[record_manual] 时长  : ${DURATION} 秒(可 Ctrl+C 中止)"
echo "[record_manual] 格式  : S16_LE / 16000Hz / mono"
echo "[record_manual] 输出  : $OUT"
echo "=========================================="
echo
read -rp ">>> 准备好后按 [回车] 开始录音,或 Ctrl+C 退出 <<<"
echo
echo "[record_manual] ⏺️   录音中..."

# timeout 防止设备异常导致无限 hang
timeout $((DURATION + 3)) arecord -D plughw:1,0 -d "$DURATION" -f S16_LE -r 16000 -c 1 "$OUT"

SIZE=$(stat -c%s "$OUT")
# 16kHz * 2字节 * 1通道 = 32000 字节/秒,减去 44 字节 WAV 头
DURATION_ACT=$(awk -v s=$SIZE 'BEGIN{printf "%.2f", (s-44)/32000}')

echo
echo "[record_manual] ✅ 完成"
echo "[record_manual] 文件  : $OUT"
echo "[record_manual] 大小  : ${SIZE} 字节 (≈${DURATION_ACT} 秒)"
echo
echo ">>> 想再录一条?直接再跑一次 record_manual 即可 <<<"
echo ">>> 跑完后告诉我文件路径,我 scp 回 PC 给你听 <<<"
