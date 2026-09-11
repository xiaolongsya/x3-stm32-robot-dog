#!/bin/bash
# ctl — 机器狗声控管理脚本(Pi 端 /usr/local/bin/ctl)
#   ctl start [阈值]   后台启动声控,日志写 /tmp/kws_ctl.log
#   ctl log            实时看日志(tail -f)
#   ctl stop           停止
#   ctl status         看运行状态
#   ctl fg [阈值]      前台运行(直接看输出,Ctrl+C 停)

LOG=/tmp/kws_ctl.log
THRESHOLD="${2:-0.7}"

case "$1" in
  start)
    pkill -9 arecord 2>/dev/null
    pkill -9 -f kws_voice_control 2>/dev/null
    sleep 1
    : > "$LOG"
    nohup bash -c "arecord -D plughw:1,0 -f S16_LE -r 16000 -c 1 -t raw 2>/dev/null | \
      python3 -u /usr/local/bin/kws_voice_control.py $THRESHOLD" \
      >> "$LOG" 2>&1 &
    sleep 4
    echo "✅ 已启动(阈值 $THRESHOLD),日志: $LOG"
    echo "   看日志:  ctl log"
    echo "   停止:    ctl stop"
    echo "--- 前几行 ---"
    head -20 "$LOG"
    ;;
  log)
    if [ ! -f "$LOG" ]; then echo "日志不存在,先 ctl start"; exit 1; fi
    echo "=== 实时日志 $LOG (Ctrl+C 退出,不影响后台运行) ==="
    tail -f "$LOG"
    ;;
  stop)
    pkill -9 arecord 2>/dev/null
    pkill -9 -f kws_voice_control 2>/dev/null
    echo "🛑 已停止"
    ;;
  status)
    if pgrep -f kws_voice_control > /dev/null; then
      echo "✅ 运行中 (PID: $(pgrep -f kws_voice_control | tr '\n' ' '))"
      echo "日志: $LOG"
    else
      echo "⭕ 未运行"
    fi
    ;;
  fg)
    pkill -9 arecord 2>/dev/null
    pkill -9 -f kws_voice_control 2>/dev/null
    sleep 1
    arecord -D plughw:1,0 -f S16_LE -r 16000 -c 1 -t raw 2>/dev/null | \
      python3 -u /usr/local/bin/kws_voice_control.py "${2:-0.5}"
    ;;
  *)
    echo "用法: ctl {start [阈值] | log | stop | status | fg [阈值]}"
    echo
    echo "  start [阈值]  后台启动(默认阈值 0.7),日志 /tmp/kws_ctl.log"
    echo "  log           实时看日志(tail -f,Ctrl+C 不影响后台)"
    echo "  stop          停止"
    echo "  status        看运行状态"
    echo "  fg [阈值]     前台运行,直接看输出"
    exit 1
    ;;
esac
