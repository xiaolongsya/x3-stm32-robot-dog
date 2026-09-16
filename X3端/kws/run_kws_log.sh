#!/bin/bash
# run_kws_log.sh — X3 端 "小龙" 唤醒词实时日志一键启动
#
# 用法:
#   ./run_kws_log.sh                # 默认阈值 0.7 + 麦实时监听
#   ./run_kws_log.sh 0.5            # 自定义阈值
#   ./run_kws_log.sh 0.7 /tmp/x.wav # 从 wav 文件(调试)
#
# 中止: Ctrl+C

set -e

# openwakeword 装在 --user,需要显式注入 site-packages
export PYTHONUSERBASE=/root/.local
export PYTHONPATH=/root/.local/lib/python3.10/site-packages:${PYTHONPATH:-}

THRESHOLD="${1:-0.7}"
WAV_FILE="${2:-}"

cd /root/kws

# 麦克风录音 → 管道喂给推理脚本
if [ -z "$WAV_FILE" ]; then
    exec arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 -t raw 2>/dev/null \
        | python3 /root/kws/kws_realtime_log.py --threshold "$THRESHOLD"
else
    exec python3 /root/kws/kws_realtime_log.py \
        --threshold "$THRESHOLD" --file "$WAV_FILE"
fi
