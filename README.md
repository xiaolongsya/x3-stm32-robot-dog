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
- **Sensor stack**: MPU6050 IMU + SSD1306 OLED + HC-SR04 ultrasonic + battery monitor + buzzer
- **Local gait**: STM32 runs balance & stepping; high-level MCU sends commands via UART
- **Open-source friendly**: MIT license, KiCad files, gerbers, BOM, STM32 firmware all public

## 📐 Architecture

```
┌────────────────────────────────────┐
│ Sunrise X3 2.0 (vision/voice)     │
│ ├─ USB camera (object detection)  │
│ ├─ USB microphone (wake word)     │
│ └─ UART → STM32 (high-level cmds) │
└────────────┬───────────────────────┘
             │ 115200 baud (USART1 PA9/PA10)
┌────────────┴───────────────────────┐
│ STM32G431 (real-time control)      │
│ ├─ 8× MG90S servos (PA2~PA7 + PB0 + PA8) │
│ ├─ MPU6050 IMU (I2C1, PA15/SCL + PB7/SDA) │
│ ├─ HC-SR04 ultrasonic (PB4/PB5)     │
│ ├─ SSD1306 OLED display (I2C)       │
│ ├─ Battery monitor (PA0 ADC)        │
│ └─ Buzzer (PA11 PWM)                │
└────────────────────────────────────┘
```

> Earlier revisions used Orange Pi Zero3 as the high-level MCU; it was retired in 2026-09 due to UART RX issues + performance limits. Sunset X3 2.0 is the current pick.

## 🛠️ Hardware

| Module | Components | Cost |
|---|---|---|
| MCU | STM32G431KBT6 (LQFP-32, 0.8mm pitch) | ¥15 |
| Power | 2× LM2596 TO-263-5 + 33µH inductors + 1N5825 diodes + AMS1117 | ¥12 |
| I2C bus | MPU6050 + SSD1306 OLED + 2× 4.7kΩ pull-up | ¥8 |
| Servos | 8× MG90S + 2× 12-pin headers (H5/H6) | ¥80 |
| Battery | 2S 18650 6800mAh (蓝火新能源) + XT30 pigtail | ¥5 |
| Sensors | HC-SR04 + buzzer + battery monitor (R15/R16) | ¥5 |
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
| STM32 Firmware | ✅ 站立 + 蹲起循环 + 对角踏步;8 路舵机已标定 |
| High-level MCU | ⏳ 旭日 X3 2.0 待到货 |

### v1 板子当前状态(2026-09-14 整理)

- **8 路舵机 STAND 标定完成**(2026-09-14 最新):
  - 6 路标准:BR/FR 小腿=1600,BR/FR 肩=1100,FL 小腿=1400,BL 肩=1900
  - 2 路机械偏置:FL 肩=2000(+100),BL 小腿=1580(+80)
  - PWM 中位 1500 = 大腿垂直 + 小腿水平(几何最高)
- **安全限位 SERVO_STEP 表**(2026-09-14):
  - 4 小腿宽度都 = 700
  - 4 肩宽度都 = 1600
  - FL 肩 / BL 小腿 因偏置需要更宽限位
- **固件进度**:
  - STM32CubeMX 8 外设配齐 (TIM1/2/3/17 × 8 路 PWM + I2C1 + USART1 + SWD + TIM6)
  - SYSCLK = 168 MHz (HSE 8MHz × PLL ×42 / 2)
  - `motions.c/h`(2026-09-14 整理):4 个动作(STAND / TROT / BOB / SHIN_TEST),编译时 `MOTION_ID` 切换
  - `stepping.c/h`(2026-09-14 整理):对角 trot,8 路同步线性 ramp(sin² 抬腿曲线),PWM ±OFFSET 在 SERVO_LIMIT 内
- **当前 UART 命令**(实测代码,2026-09-11):
  - `<id 0-7> <pulse>` 设单路舵机、`all <pulse>` 设全部 8 路、`center` 全部 1500、`stand` STAND 姿态
  - `step trot` 启动 trot 踏步(TIM6 100Hz)、`step stop` 停止、`step show` 调试输出
- **踏步参数**(2026-09-14):
  - `STEP_TROT_OFFSET=500`、`STEP_TROT_PERIOD=0.25`、`STEP_RATIO_SHIN_TO_THIGH_X10=3`
- **历史**:
  - Pi 时代脚本全部归档 `docs/legacy/`(2026-09-13)
  - 香橙派 Zero3 已出二手(2026-09-13),改用旭日 X3 2.0

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
