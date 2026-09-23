# desktop-quadruped

> A 4-legged robot dog for your desk: STM32G431 direct 8-servo drive,
> fully open-source hardware (KiCad schematic + 80×80 PCB + Gerbers) and firmware.

![Status](https://img.shields.io/badge/status-v1%20debugging-yellow)
![License](https://img.shields.io/badge/license-MIT-blue)
![Hardware](https://img.shields.io/badge/hardware-open--source-orange)

---

## ✨ Features

- **Single-MCU baseline** + **optional high-level MCU**: STM32G431KBT6 (real-time gait) + Sunrise X3 2.0 (vision/voice, when available)
- **8-servo direct drive**: MG90S × 8 (no PCA9685) on STM32 timers
- **Compact PCB**: 4-layer, 80×80mm (JLC free-coupon sized)
- **Standalone power**: 2× LM2596 + AMS1117, separate logic & servo rails
- **Sensor stack**: MPU6050 IMU + SSD1306 OLED + HC-SR04 ultrasonic + battery monitor (蜂鸣器 2026-09-16 链路作废,硬件保留不驱动)
- **Local gait**: STM32 runs balance & stepping; high-level MCU sends commands via UART
- **Open-source friendly**: MIT license, KiCad files, gerbers, BOM, STM32 firmware all public

## 📐 Architecture

```
┌────────────────────────────────────────────────────────┐
│ PC 大脑 (Python) — ws://<本机IP>:8765                  │
│ ├─ ASR: FunASR SenseVoiceSmall (INT8, GPU)            │
│ ├─ LLM: Ollama qwen3:8b-q8_0 (Modelfile 名 xiaolong)  │
│ └─ JSON Schema 约束 + 3 轮历史 + 错误兜底              │
└──────────────────────┬─────────────────────────────────┘
                       │ WS (newline-delimited JSON)
┌──────────────────────┴─────────────────────────────────┐
│ Sunrise X3 2.0 — KWS 双进程 (Unix sock)               │
│ ├─ kws_listener.py: 独占麦克风 → oww.predict()         │
│ │   ring buffer + VAD → wake_detected / recording_done │
│ ├─ kws_worker.py:   独占 STM32 串口                    │
│ │   WS 上送 PCM + 翻译 actions → STM32 帧              │
│ └─ UART → STM32 (high-level cmds)                      │
└──────────────────────┬─────────────────────────────────┘
                       │ 115200 baud (USART1 PA9/PA10)
┌──────────────────────┴─────────────────────────────────┐
│ STM32G431 (real-time control)                          │
│ ├─ 8× MG90S servos (PA2~PA7 + PB0 + PA8)              │
│ ├─ MPU6050 IMU (I2C1, PA15/SCL + PB7/SDA)             │
│ ├─ HC-SR04 ultrasonic (PB4/PB5)                        │
│ ├─ SSD1306 OLED display (I2C)                          │
│ └─ Battery monitor (PA0 ADC) — PA11 蜂鸣器 2026-09-16 链路作废 │
└────────────────────────────────────────────────────────┘
```

完整链路图 + 每跳数据格式见 [`🧠 链路与协议](#-链路与协议)`](#-链路与协议) 章节。

> Earlier revisions used Orange Pi Zero3 as the high-level MCU; it was retired in 2026-09 due to UART RX issues + performance limits. Sunset X3 2.0 is the current pick.
> KWS 早期是单进程 `kws_record_send.py`,2026-09-16 重构为双进程根除 openWakeWord stateful buffer 二次唤醒。

## 🛠️ Hardware

| Module | Components | Cost |
|---|---|---|
| MCU | STM32G431KBT6 (LQFP-32, 0.8mm pitch) | ¥15 |
| Power | 2× LM2596 TO-263-5 + 33µH inductors + 1N5825 diodes + AMS1117 | ¥12 |
| I2C bus | MPU6050 + SSD1306 OLED + 2× 4.7kΩ pull-up | ¥8 |
| Servos | 8× MG90S + 2× 12-pin headers (H5/H6) | ¥80 |
| Battery | 2S 18650 6800mAh (蓝火新能源) + XT30 pigtail | ¥5 |
| Sensors | HC-SR04 + battery monitor (R15/R16); ~~buzzer~~(2026-09-16 已废) | ¥5 |
| Switch | Ship-type SW1 + 3.3V bus capacitors | ¥1 |
| Connectors | USB-C (debug), J1/J2 (debug), OLED | ¥2 |
| **Total** | **52 components** | **~¥127** |

## 📐 PCB

- **Size**: 80×80 mm
- **Layers**: 4 (signal + GND + power + signal)
- **Mounting**: 4× M3 screws at corners (72.16×70.16mm spacing)
- **Tool**: Designed for JLCPCB fabrication

## 📚 Documentation

- [`CLAUDE.md`](CLAUDE.md) — AI 协作规则 + 项目速览(开发者入口)
- [`运动学参数.md`](运动学参数.md) — 几何参数 + 远场近似
- [`机器狗v1控制板_投板前审查报告.md`](机器狗v1控制板_投板前审查报告.md) — 3 轮审查 + 5 个 critical 修复
- [`网表/`](网表/) — 立创 EDA 网表
- [`数据手册/`](数据手册/) — IC 数据手册
- [`docs/legacy/`](docs/legacy/) — 归档:Pi 时代脚本 + 模型(2026-09-13)
- [`PC端/brain/README.md`](PC端/brain/README.md) — PC 大脑服务架构 + 启动 + 协议 + 性能
- [`X3端/kws/README.md`](X3端/kws/README.md) — KWS 训练数据 + 双进程运行时架构

## 🧠 链路与协议(2026-09-16 打通)

完整数据流:语音 → ASR → LLM → 动作 → 舵机。

```
[USB 麦 MB-306]
   │ arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 -t raw
   ▼
[X3] kws_listener.py ─────────────────► kws_worker.py
   │ oww.predict() 80ms/帧              │ DogLink 100ms 心跳
   │ ring buffer 1.5s 预录              │ WS brain + 翻译 actions
   │ VAD 静音 1.44s / 最长 8s           │
   │ wake_detected / recording_done     │ STM32 UART @ 115200
   │ (Unix sock /tmp/kws_listener.sock) │
   ▼                                    ▼
[PC] brain.py :8765                    [STM32] commands.c
   │ ASR FunASR SenseVoiceSmall (INT8)  │ 0x01 MOTION_PLAY
   │ 后处理 数字归一 + 同义词纠正        │ 0x06 EMERGENCY_STOP
   │ LLM Ollama xiaolong (qwen3:8b)     │ 0x09 ACTION_PLAY
   │ JSON Schema + 3 轮历史 + 兜底       │ 100ms 心跳
   │ WS 上行 utterance + PCM base64     │
   │ WS 下行 action + actions + reply   │
   ▼                                    ▼
   └──────────── 反向数据流 ◄─────────────┘
```

### 每跳数据格式

| # | 跳 | 格式 | 关键参数 |
|---|---|---|---|
| 1 | USB 麦 → X3 | 16kHz mono S16_LE raw | arecord 块 80ms |
| 2 | X3 内部 IPC | newline-delimited JSON | `/tmp/kws_listener.sock` |
| 3 | X3 → PC | WS JSON | `ws://<本机IP>:8765` |
| 4 | PC 内部 | 字符串(text) | ASR 后处理,LLM JSON Schema 约束 |
| 5 | PC → X3 | WS JSON | actions list,cmd ∈ {ACTION_PLAY, MOTION_PLAY, EMERGENCY_STOP} |
| 6 | X3 → STM32 | 二进制帧 | `[0xAA][0x55][len][cmd][data][csum8]`,csum8=XOR |
| 7 | STM32 → 舵机 | 50Hz PWM | 500-2500µs,8 路 |

### KWS 唤醒词 "小龙"

- 模型:`/root/kws/xiaolong.onnx`(PC `X3端/kws/train_xiaolong.py` 训练后 scp)
- 阈值:**0.85**(实测 0.7 误唤醒,0.85 完美)
- 数据:100 正样本 + 100 负样本词 + 150 切片环境噪(详见 X3 README)

### STM32 二进制协议(权威:`STM32项目/MachineDog_V1/Core/Inc/commands.h`)

| cmd | 名称 | data | 备注 |
|---|---|---|---|
| `0x01` | MOTION_PLAY | id 1..4:`[id u8, dur_ms u32 LE]`;id 5:`[5, dur_ms u32 LE, direction i8]` | id 5=WALK;direction +1 前进、-1 后退、0 原地踏步 |
| `0x03` | SET_PWM | `[(id u8, pulse u16 LE) * N]` | 直写 8 路,pulse ∈ [500,2500] |
| `0x05` | HEARTBEAT | `[]` | 100ms 一发,防 watchdog 200ms 超时 |
| `0x06` | EMERGENCY_STOP | `[]` | stepping_stop + 回 STAND |
| `0x09` | ACTION_PLAY | `[action_id u8, repeat u8]` | id 5..8 = SIT_DOWN/STAND_UP/... (走 ramp SIT/STAND) |
| ~~`0x07-08`~~ | ~~BUZZER~~ | ~~已废 2026-09-16~~ | STM32 收到回 BAD_CMD |

ACK:`[0xAA][0x55][len][cmd|0x80][status][data][csum8]`,`status`:0=OK / 1=CRC / 2=BAD_CMD / 3=BAD_PARAM / 4=BAD_LEN

### 端到端性能目标(实测目标,2026-09-16)

- ASR:<500ms(SenseVoiceSmall INT8,GPU)
- LLM:<1.5s(qwen3:8b-q8_0,think=False,num_predict=300)
- 总端到端:<2s(唤醒 → 舵机动作)
- STM32 watchdog:200ms 无心跳回 STAND,X3 必须 100ms 一发心跳

### 启动顺序

```bash
# 1. PC 端启 brain WS server
cd PC端/brain && ./run.ps1    # Windows; bash: ./run.sh
# (自动检查 Ollama + 拉 qwen3:8b + 装 venv + 启 brain)

# 2. X3 端启 KWS 双进程(SSH 进 X3)
/root/kws/start_kws_dual.sh
# 默认连 ws://192.168.160.91:8765(可 PC_URL=ws://...:8765 覆盖)
```

## 🚀 Quick Start

1. Clone this repo
2. Open `网表/Netlist_控制板pcb_2026-09-02.tel` in your EDA tool (KiCad / EasyEDA)
3. Generate Gerbers and order PCB from JLC
4. Source components per BOM
5. Solder (recommended order: power → MCU → sensors → connectors)
6. Flash STM32 firmware (default = stand; change `MOTION_ID` in `motions.h` to test different motions)
7. (When ready) Connect high-level MCU via UART, run agent

## 🤝 Contributing

PRs welcome! See [`CLAUDE.md`](CLAUDE.md) for project rules and conventions.

## 📄 License

This project is licensed under the **MIT License** — see [`LICENSE`](LICENSE).

## 🙏 Acknowledgments

- STM32 / MPU6050 / SSD1306 / LM2596 datasheets and reference designs
- Open-source community

## 📊 Project Status

| Phase | Status |
|---|---|
| v1 Schematic | ✅ Complete (52 components) |
| v1 PCB Layout | ✅ Complete (3 rounds review, 0 blockers) |
| v1 Fabrication | ✅ Complete |
| v1 Board Bring-up | ✅ Complete (2026-09-09) |
| STM32 Firmware | ✅ 站立 + 蹲起循环 + 对角踏步 + X3 协议 + ramp SIT/STAND；WALK 前进/后退已实现,待烧录实测 |
| X3 (KWS + STM32 翻译) | ✅ 上线 (UART ↔ STM32, KWS 双进程, WS PC brain) |
| PC 大脑 (ASR + LLM) | ✅ 上线 (FunASR + Ollama qwen3:8b,WS @ :8765) |
| 端到端链路 | ✅ 唤醒 → ASR → LLM → STM32 跑通 (2026-09-16) |

### v1 板子当前状态(2026-09-16 重整)

- **8 路舵机 STAND 标定完成**(2026-09-16 重整为"正站立"):
  - 6 路标准:BR/FR 小腿=1600, BR/FR 肩=1100, FL 小腿=1400, **BL 小腿=1480** (取消 +80 偏置)
  - 2 路机械偏置:FL 肩=2000 (+100), BL 肩=1900
  - PWM 中位 1500 = 大腿垂直 + 小腿水平(直角,部件极值位)
- **安全限位 SERVO_STEP 表**(2026-09-16):
  - 4 小腿宽度都 = 700: BR/FR(900-1600), FL(1400-2100), **BL(1480-2180)**
  - 4 肩宽度都 = 1600: BR/FR/BL(700-2300), FL(800-2400)
- **固件模块清单**:
  - STM32CubeMX 9 外设 (TIM1/2/3/15/17 × 8 路 PWM + I2C1 + USART1 + SWD + TIM6/TIM7)
  - SYSCLK = 168 MHz (HSI 16MHz × PLL ×21)
  - `commands.c/h` (2026-09-14) — X3 ↔ STM32 二进制协议 (USART1 DMA + IDLE)
  - `motions.c/h` — 5 动作 (STAND / TROT / BOB / SHIN_TEST / WALK) + ramp SIT/STAND 入口
  - `stepping.c/h` (2026-09-14) — 对角 trot,8 路同步线性 ramp (TIM6 100Hz ISR)
  - `ramp.c/h` (2026-09-16) — 8 路 PWM 同步渐进 ramp (SIT/STAND 用,主循环调 ramp_tick)
  - `watchdog.c/h` (2026-09-14) — TIM7 1kHz,200ms 无心跳自动回 STAND
  - ~~`buzzer.c/h` (2026-09-14) — PA11 有源蜂鸣器~~ (2026-09-16 作废)
- **当前二进制协议命令集**(STM32 commands.h):
  - `0x01 MOTION_PLAY [id u8, dur_ms u32 LE]` (id 1..4 走 motions 表)
  - `0x03 SET_PWM [(id u8, pulse u16 LE) * N]`
  - `0x05 HEARTBEAT []` (X3 心跳 100ms 一发)
  - `0x06 EMERGENCY_STOP []`
  - ~~`0x07-08 BUZZER_ON/OFF`~~ (2026-09-16 作废)
  - `0x09 ACTION_PLAY [action_id u8, repeat u8]` (id 5..8 走 ramp SIT/STAND)
- **X3 端工具**:
  - `/root/dog_uart.py` (X3端/dog_uart.py) — sit / stand / squat / trot / bob / seq / stop / pwm / raw (DogLink 类,100ms 心跳守护)
  - `/root/kws/kws_listener.py` + `kws_worker.py` — KWS 双进程 (2026-09-16 重构)
  - `/root/kws/start_kws_dual.sh` — KWS 双进程一键启动 (后台 worker + 前台 arecord|listener)
- **STM32 Debug 重编译提醒**(2026-09-16):`Debug/MachineDog_V1.elf` 最后编译 Sep 12,而源码 Sep 14-16 新增 5 个 .c(motions / commands / ramp / watchdog / diagnostic)。CubeIDE 重新打开工程 → Project → Rebuild Index → Build 即可,无需手动改 subdir.mk
- **踏步参数**(2026-09-14):
  - `STEP_TROT_OFFSET=500`、`STEP_TROT_PERIOD=0.25`、`STEP_RATIO_SHIN_TO_THIGH_X10=3`
- **ramp SIT/STAND 时长**(2026-09-16):
  - `SIT_RAMP_MS = 800`、`STAND_RAMP_MS = 1200`
- **历史**:
  - Pi 时代脚本全部归档 `docs/legacy/`(2026-09-13)
  - 香橙派 Zero3 已出二手(2026-09-13),改用旭日 X3 2.0
  - 2026-09-14 X3 到货 + 上线 USART1 DMA 协议
  - 2026-09-16 ramp 模块 + BL 校准重构 (取消 +80 偏置) + duration_s fix
  - 2026-09-16 链路打通: PC brain (FunASR + Ollama) + X3 KWS 双进程 (单进程 → 双进程根因修复) + STM32 协议稳定
  - 2026-09-16 蜂鸣器全链路作废 (PCB 元件留,固件/X3/PC 不驱动)
  - 2026-09-16 清理简化版 / 监控版 KWS 脚本 (kws_with_action / start_kws_action / kws_realtime_log / run_kws_log)

---

## 🔍 v1 PCB 投板前审查记录(2026-09-02)

3 轮 ultracode 审查,找到并修复 4 个 critical + 1 个新发现的 critical:

| # | 问题 | 修复 |
|---|---|---|
| C1 | SDA→PB6(无 I2C),SCL→PB7(只能 SDA)— I2C 跑不通 | SDA→PB7,SCL→PA15,R10/R11 改 4.7kΩ |
| C2 | NRST/PA1 引脚反接 — 按 KEY1 复位,RESET1 无效 | 两网对调 |
| C3 | C1/C2/RESET1/KEY1 4 个 pad 没接 GND | 全部接 GND |
| C4 | F1 = SMD2920-500-24 = 5A/24V(确认) | 不动 |
| **C5** | **LM2596 ON/OFF 拉到 BAT_PROTECT → 待机模式 → 整板不上电** | **删 R4/R14,U2.5/U10.5 直连 GND** |

详见 [`机器狗v1控制板_投板前审查报告.md`](机器狗v1控制板_投板前审查报告.md)。
