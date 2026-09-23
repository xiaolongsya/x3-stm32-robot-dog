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

---

## 运行时架构 — 双进程 KWS (2026-09-16 重构)

X3 端运行时拆成两个进程,通过 Unix domain socket 通信,**根因解决** openWakeWord stateful audio buffer 在单进程长任务流里被冻结导致的二次唤醒。

```
arecord ──► kws_listener.py(独占麦克风)──── Unix socket ────► kws_worker.py(独占 STM32 串口)
              • oww.predict() 永远 80ms 一帧                       • DogLink 100ms heartbeat
              • RMS 预筛 + score ≥ 0.85                            • 听 wake_detected → 切 busy
              • ring buffer 永远滚动(预录 1.5s)                   • 听 recording_done → WS brain
              • 唤醒 → 发 wake_detected                              → 翻译 actions → STM32 帧
              • VAD 录音 → 发 recording_done                        • 发回 ready 通知 listener
```

**为什么拆**:
- openWakeWord 是 stateful 模型,内部 6s sliding audio buffer
- 单进程里 oww 被 13s 长任务(录音 + ASR + LLM + STM32)冻结 → buffer 残留"小龙"音频 → 二次唤醒
- 拆双进程后 listener 永远调 predict,buffer 持续被新鲜 PCM 替换,根因消除
- 早期 `kws_record_send.py` 单进程版本用 cooldown + oww buffer reset 治标(commit 84ec2ac + 8fd5d1c),已 deprecated

**文件清单**:
| 文件 | 行数 | 职责 |
|---|---|---|
| `kws_protocol.py` | 141 | IPC 常量 + JSON line schema + PCM 编解码(纯 stdlib) |
| `kws_listener.py` | 191 | 独占麦克风,永远 oww.predict(),ring buffer + VAD 录音 |
| `kws_worker.py` | 239 | 独占 STM32,听 wake/recording,WS brain + 翻译 actions |
| `start_kws_dual.sh` | 40 | 一键拉起 worker(后台) + arecord\|listener(前台),trap cleanup |

**用法**:
```bash
# 在 X3 上,前端会话跑:
/root/kws/start_kws_dual.sh
# 默认连 ws://192.168.160.91:8765,可用 PC_URL=ws://... 覆盖
```

**回退**: 旧版单进程 `kws_record_send.py` 保留,标注 deprecated,出问题可一键切回:
```bash
# 双进程先停(前台 Ctrl+C 或杀 worker+listener)
# 然后直接跑单进程 fallback(它自带 oww buffer reset 治标补丁)
python3 /root/kws/kws_record_send.py --pc-url ws://192.168.160.91:8765
```

**架构决策 commit 链**:
- `86a07a2` — kws_protocol.py(IPC 协议)
- `72503f0` — kws_listener.py + start_kws_dual.sh(KWS 常驻)
- `9bee03e` — kws_worker.py(业务进程,复用 DogLink)
- `e6c6ca5` — 标 kws_record_send.py 为 deprecated

## 单腿离地诊断

前进步态若出现机身朝摆动腿倾倒，先停止 WALK。托住机身、让四脚不承重，且确认
`kws_worker.py` 未占用串口，然后在 X3 上逐条运行：

```bash
python3 /root/kws/leg_lift_diagnostic.py FR --supported
python3 /root/kws/leg_lift_diagnostic.py FL --supported
python3 /root/kws/leg_lift_diagnostic.py BL --supported
python3 /root/kws/leg_lift_diagnostic.py BR --supported
```

脚本只移动指定小腿，从 STAND 缓慢收腿 300us、保持 1 秒、再返回 STAND；
其他三条腿不推地。观察小腿转向、脚端是否上移、有无卡滞或舵机无力。
运行中按 Ctrl+C 会发送急停并回 STAND。若 300us 看不清，可加
`--amplitude 500`，仅在机身被托住时使用。

两段式步态刷入 STM32 后，可手扶机身运行单轮测试：

```bash
python3 -u /root/kws/test_phased_walk.py --supported
```

脚本先回 STAND、倒数 3 秒，再发一轮 8 秒 WALK，按 1 秒间隔打印预计阶段；
结束或按 Ctrl+C 时发急停回 STAND。阶段提示来自固件时间表，不代表实测脚位。
`--direction backward` 可测后退；前进承重稳定前先不测后退。
