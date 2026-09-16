#!/bin/bash
# start_kws_dual.sh — X3 端 KWS 双进程一键启动 (2026-09-16 重构)
#
# 启动顺序:
#   1. 清 socket 残留
#   2. 启动 worker (后台, DogLink 100ms STM32 heartbeat 自动起)
#   3. trap cleanup (Ctrl+C / 异常退出都杀干净)
#   4. arecord pipe → listener (前台, Ctrl+C 即停)
#
# PC_URL 通过环境变量覆盖, 默认 ws://192.168.160.91:8765

set -e
cd /root/kws

# Python user-installed 包能找到 (openwakeword 在 /root/.local)
export PYTHONUSERBASE=/root/.local
export PYTHONPATH=/root/.local/lib/python3.10/site-packages:${PYTHONPATH:-}

# 1) 清 socket 残留 (上一次崩溃可能留下)
rm -f /tmp/kws_listener.sock

# 2) 启动 worker (后台)
PC_URL="${PC_URL:-ws://192.168.160.91:8765}"
python3 /root/kws/kws_worker.py --pc-url "$PC_URL" &
WORKER_PID=$!

# 3) trap cleanup: Ctrl+C / 异常退出都杀干净
cleanup() {
    echo "[start_kws_dual] 清理: 杀 worker=$WORKER_PID, listener, arecord"
    kill "$WORKER_PID" 2>/dev/null || true
    pkill -f kws_listener 2>/dev/null || true
    pkill -f arecord 2>/dev/null || true
    rm -f /tmp/kws_listener.sock
    exit 0
}
trap cleanup EXIT INT TERM

echo "[start_kws_dual] worker PID=$WORKER_PID"
sleep 1  # 给 worker 时间 connect socket (listener 阻塞在 accept 等)

# 4) arecord pipe → listener (前台, 占主终端)
exec arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 -t raw 2>/dev/null \
    | python3 /root/kws/kws_listener.py