# X3 端 KWS 唤醒词训练数据采集

唤醒词 "小龙" 的训练数据采集 + 训练脚本。X3 2.0 + STM32G431 机器狗用。

## 设备

- **USB 麦**：MUSIC-BOOST MB-306 (`1b3f:0004`, `snd-usb-audio`)
- **ALSA 节点**：`plughw:0,0` (card 0, device 0)
- **录音格式**：16kHz / mono / S16_LE

> Pi 时代脚本硬编码 `plughw:1,0` —— **X3 上是 card 0**，所有脚本统一改为 `plughw:0,0`。

## 数据采集流程

### 1. 上传脚本到 X3

```bash
# 在 PC 上 (WSL / Git Bash)
scp -i ~/.ssh/id_ed25519 -r X3端/kws root@192.168.33.112:/home/root/kws
```

### 2. 采 100 条正样本(念 "小龙")

SSH 进 X3,然后:

```bash
ssh -i ~/.ssh/id_ed25519 -t root@192.168.33.112
python3 /home/root/kws/record_wakeword.py positive 100 2 /home/root/voice_data/positive
```

每条按 Enter → 立刻录 2 秒 → 显示进度 → 等 Enter。
100 条约 5 分钟(算上按 Enter 的时间)。

### 3. 采 100 条负样本(10 词 × 10 轮)

词表固定 10 个两字人名(小明/小红/小芳/小华/小刚/小亮/小峰/小军/小杰/小强),每条按提示念:

```bash
python3 /home/root/kws/record_wakeword.py negative 100 2 /home/root/voice_data/negative
```

### 4. 采 5 分钟环境噪声

录一段连续 5 分钟的环境噪声(不说话,正常活动 / 风扇 / 别人说话都行):

```bash
python3 /home/root/kws/record_wakeword.py env 300 /home/root/voice_data/env.wav
```

### 5. 切片环境噪声为 2 秒片段

```bash
python3 /home/root/kws/slice_env.py /home/root/voice_data/env.wav 2 /home/root/voice_data/negative_env
```

300 秒 / 2 秒 ≈ 150 段,作为额外负样本补充(防误唤醒)。

### 6. 回传数据到 PC

```bash
# 在 PC 上
scp -i ~/.ssh/id_ed25519 -r root@192.168.33.112:/home/root/voice_data ./voice_data
```

## 数据规模汇总

| 类别 | 来源 | 数量 |
|---|---|---|
| 正样本 | `positive/` | 100 |
| 负样本 (词) | `negative/` | 100 |
| 负样本 (环境) | `negative_env/` | ~150 |
| **合计** | — | **~350** |

每个 wav 2 秒,16kHz/mono/16bit = **64 KB/条**, 总计 ~22 MB。

## 下一步:训练

数据回传到 PC 后,跑 `train_xiaolong.py`(在 PC 上训练)。

训练出的 `xiaolong.onnx` 再 scp 回 X3 部署(`kws_realtime_oww.py` 留待从 legacy 迁过来)。
