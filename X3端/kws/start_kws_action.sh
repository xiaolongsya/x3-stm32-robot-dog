#!/bin/bash
# start_kws_action.sh — X3 端 KWS 唤醒 → 蹲下起立 一键启动
# 2026-09-16
#
# 用法:
#   ./start_kws_action.sh           # 默认阈值 0.85
#   ./start_kws_action.sh 0.90      # 自定义阈值
#
# 中止: Ctrl+C

set -e

THRESHOLD="${1:-0.85}"
cd /root/kws

echo "[kws] 阈值 $THRESHOLD,动作 ACTION_PLAY id=7 (= 蹲下起立)"
echo "[kws] STM32 watchdog 200ms,本脚本后台心跳 100ms 一发"
echo "[kws] 按 Ctrl+C 退出"
echo "------------------------------------------------------------"

exec arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 -t raw 2>/dev/null \
    | python3 /root/kws/kws_with_action.py --threshold "$THRESHOLD"