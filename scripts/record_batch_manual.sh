#!/bin/bash
# record_batch_manual.sh — 批量录音(每条按 enter 立即开始)
# 用法: ./record_batch_manual.sh [次数] [单次秒] [输出目录] [文件名前缀]
# 例  : ./record_batch_manual.sh 100 2 /tmp/xiaolong xiaolong

set -e

COUNT="${1:-100}"
DURATION="${2:-2}"
OUTDIR="${3:-/tmp/xiaolong}"
PREFIX="${4:-xiaolong}"
HINT="${5:-}"

mkdir -p "$OUTDIR"

# 防设备忙
pkill -9 arecord 2>/dev/null || true
sleep 0.5

echo "=========================================="
echo "[record_batch_manual] 批量录音(每条 enter 立即开始)"
echo "[record_batch_manual] 次数    : $COUNT"
echo "[record_batch_manual] 单次时长: ${DURATION} 秒"
echo "[record_batch_manual] 输出目录: $OUTDIR"
echo "[record_batch_manual] 前缀    : ${PREFIX}_NNN.wav"
echo "[record_batch_manual] 内容    : 念 '小龙' 两个字"
echo "[record_batch_manual] 节奏    : 按 [回车] → 立即录 ${DURATION} 秒 → 报进度 → 等回车"
echo "[record_batch_manual] 中止    : Ctrl+C (已录的保留)"
if [ -n "$HINT" ]; then
    echo "[record_batch_manual] 提示    : $HINT"
fi
echo "=========================================="
echo

OK_COUNT=0
FAIL_COUNT=0
TOTAL_BYTES=0
START_TIME=$(date +%s)

for i in $(seq -f "%03g" 1 $COUNT); do
    OUTFILE="$OUTDIR/${PREFIX}_${i}.wav"

    echo "=========================================="
    echo "[$i/$COUNT] $OUTFILE"
    echo ">>> 按 [回车] 立即开始录 ${DURATION} 秒(念 '小龙') <<<"
    read -r

    echo "⏺️  录音中..."

    if timeout $((DURATION + 3)) arecord -D plughw:1,0 -d "$DURATION" -f S16_LE -r 16000 -c 1 "$OUTFILE" 2>/dev/null; then
        SIZE=$(stat -c%s "$OUTFILE" 2>/dev/null || echo 0)
        if [ "$SIZE" -gt 1000 ]; then
            OK_COUNT=$((OK_COUNT + 1))
            TOTAL_BYTES=$((TOTAL_BYTES + SIZE))
            echo "✅ [$i/$COUNT] OK (${SIZE} 字节)"
        else
            FAIL_COUNT=$((FAIL_COUNT + 1))
            echo "❌ [$i/$COUNT] 文件过小/失败"
        fi
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "❌ [$i/$COUNT] 录音失败(超时或设备错)"
    fi

    if [ "$i" != "$COUNT" ]; then
        NEXT_FMT=$(printf "%03d" "$((10#$i + 1))")
        echo ">>> 按 [回车] 录第 ${NEXT_FMT} 条,Ctrl+C 中止 <<<"
    fi
done

END_TIME=$(date +%s)
TOTAL=$((END_TIME - START_TIME))

echo
echo "=========================================="
echo "[record_batch_manual] ✅ 全部完成!"
echo "[record_batch_manual] 成功    : $OK_COUNT / $COUNT"
echo "[record_batch_manual] 失败    : $FAIL_COUNT"
echo "[record_batch_manual] 总大小  : $TOTAL_BYTES 字节"
echo "[record_batch_manual] 总耗时  : ${TOTAL} 秒 (≈$((TOTAL / 60)) 分钟)"
echo "[record_batch_manual] 输出目录: $OUTDIR"
echo "=========================================="
