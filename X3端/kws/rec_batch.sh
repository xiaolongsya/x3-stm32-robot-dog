#!/bin/bash
# rec_batch.sh — 在 X3 上从 USB 麦录制 10 条语音命令测试集
# 2026-09-16
#
# 用法(SX3 上):
#   chmod +x /root/kws/rec_batch.sh
#   /root/kws/rec_batch.sh
#
# 输出: /root/kws/test_wavs/{01..10}.wav (16k mono s16_le)
# 然后 scp 回 PC:PC端/brain/tests/voice_commands/
#
# 测试命令(用户先给我说好的):
#   01: 你好呀,你可以蹲下吗?
#   02: 踏步3秒
#   03: 立正!
#   04: 蹲下5秒钟再站起来把
#   05: 摆动一下左前腿            (STM32 没这能力,LLM 应回'不会')
#   06: 摆动一下左肩部            (同上)
#   07: 蹲下起立5次
#   08: 小狗狗,你可以蹲下吗?
#   09: 小狗狗,蹲一个
#   10: 往前倾一点               (同上 05,微调类)

set -e

OUT_DIR="/root/kws/test_wavs"
SEC=3
ALSA_DEV="plughw:0,0"

mkdir -p "$OUT_DIR"

commands=(
    "01:你好呀,你可以蹲下吗?"
    "02:踏步3秒"
    "03:立正!"
    "04:蹲下5秒钟再站起来把"
    "05:摆动一下左前腿"
    "06:摆动一下左肩部"
    "07:蹲下起立5次"
    "08:小狗狗,你可以蹲下吗?"
    "09:小狗狗,蹲一个"
    "10:往前倾一点"
)

echo "=========================================="
echo " 10 条语音命令录制 (X3 USB 麦 ${ALSA_DEV})"
echo " 输出: $OUT_DIR"
echo " 每条 ${SEC}s"
echo " 提前 0.5s 静默(模拟'唤醒后蹲下起立延迟')"
echo "=========================================="

for cmd in "${commands[@]}"; do
    num="${cmd%%:*}"
    text="${cmd#*:}"
    out="$OUT_DIR/${num}.wav"

    echo ""
    echo "=========================================="
    echo "[$num] 请说: $text"
    read -rp "按 Enter 开始录音 (${SEC}s 自动停)..."

    # arecord -d 自动停
    arecord -D "$ALSA_DEV" -f S16_LE -r 16000 -c 1 -d "$SEC" "$out" 2>/dev/null
    size=$(stat -c '%s' "$out" 2>/dev/null || echo 0)
    echo "[OK] $out ($size bytes, $(echo "scale=2; $size / 32000" | bc)s)"
done

echo ""
echo "=========================================="
echo " 全部 10 条录完"
echo "=========================================="
ls -la "$OUT_DIR"

echo ""
echo "scp 回 PC:"
echo "  # 先在 X3 上跑 'ip a' 查实际 IP(用户用热点,IP 经常变):"
echo "  export X3_IP=\$(ip a show wlan0 | awk '/inet /{print \$2}' | cut -d/ -f1)"
echo "  scp -i ~/.ssh/id_ed25519 -r root@\${X3_IP}:/root/kws/test_wavs \\"
echo "    C:/Users/17402/Desktop/机器狗/PC端/brain/tests/voice_commands/"