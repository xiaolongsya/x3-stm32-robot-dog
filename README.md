# desktop-quadruped

> A 4-legged robot dog for your desk: Pi Zero3 + STM32G431, 8-servo direct drive,
> fully open-source hardware (KiCad schematic + 80×80 PCB + Gerbers) and firmware.

![Status](https://img.shields.io/badge/status-v1%20schematic%20complete-green)
![License](https://img.shields.io/badge/license-MIT-blue)
![Hardware](https://img.shields.io/badge/hardware-open--source-orange)

---

## ✨ Features

- **Dual MCU architecture**: Pi Zero3 (Linux, vision/voice) + STM32G431KBT6 (real-time gait)
- **8-servo direct drive**: MG90S × 8 (no PCA9685) on STM32 timers
- **Compact PCB**: 4-layer, 80×80mm (JLC free-coupon sized)
- **Standalone power**: 2× LM2596 + AMS1117, separate logic & servo rails
- **Sensor stack**: MPU6050 IMU + SSD1306 OLED + HC-SR04 ultrasonic + battery monitor + buzzer
- **Local gait**: STM32 handles balance & stepping, Pi just sends high-level commands
- **Open-source friendly**: MIT license, KiCad files, gerbers, BOM, STM32 firmware all public

## 📐 Architecture

```
┌──────────────────────────────────┐
│ Pi Zero3 (vision/voice master)  │
│ ├─ USB camera (object detection) │
│ ├─ USB microphone (wake word)    │
│ └─ UART → STM32 (high-level cmds) │
└────────────┬─────────────────────┘
             │ 115200 baud
┌────────────┴─────────────────────┐
│ STM32G431 (real-time control)     │
│ ├─ 8× MG90S servos (PA2~PA7 + PB0 + PA8) │
│ ├─ MPU6500 IMU (I2C1, PA15/SCL + PB7/SDA) │
│ ├─ HC-SR04 ultrasonic (PB4/PB5)     │
│ ├─ SSD1306 OLED display (I2C)       │
│ ├─ Battery monitor (PA0 ADC)        │
│ └─ Buzzer (PA11 PWM)                │
└──────────────────────────────────┘
```

## 🛠️ Hardware

| Module | Components | Cost |
|---|---|---|
| MCU | STM32G431KBT6 (LQFP-32, 0.8mm pitch) + Pi Zero3 | ¥15 |
| Power | 2× LM2596 TO-263-5 + 33µH inductors + 1N5825 diodes + AMS1117 | ¥12 |
| I2C bus | MPU6050 + SSD1306 OLED + 2× 4.7kΩ pull-up | ¥8 |
| Servos | 8× MG90S + 2× 12-pin headers (H5/H6) | ¥80 |
| Battery | 2S 18650 6800mAh (蓝火新能源) + XT30 pigtail | ¥5 |
| Sensors | HC-SR04 + buzzer + battery monitor (R15/R16) | ¥5 |
| Switch | Ship-type SW1 + 3.3V bus capacitors | ¥1 |
| Connectors | USB-C (debug), J1/J2 (debug), CN2 (Pi power), OLED | ¥2 |
| **Total** | **52 components** | **~¥127** |

## 📐 PCB

- **Size**: 80×80 mm
- **Layers**: 4 (signal + GND + power + signal)
- **Mounting**: 4× M3 screws at corners (72.16×70.16mm spacing)
- **Tool**: Designed for JLCPCB fabrication

## 📚 Documentation

- [`CLAUDE.md`](CLAUDE.md) — AI 协作规则 + 项目速览
- [`网表/`](网表/) — 立创 EDA 网表
- [`数据手册/`](数据手册/) — IC 数据手册

## 🚀 Quick Start

1. Clone this repo
2. Open `网表/Netlist_控制板pcb_2026-09-02.tel` in your EDA tool (KiCad / EasyEDA)
3. Generate Gerbers and order PCB from JLC
4. Source components per BOM
5. Solder (recommended order: power → MCU → sensors → connectors)
6. Flash STM32 firmware
7. Connect Pi via UART, run agent

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
| v1 PCB Layout | ✅ Complete (3 轮审查通过,0 阻断) |
| v1 Fabrication | ✅ Complete |
| v1 Board Bring-up | 🔧 In progress (新板子焊接中,旧板子调试损伤退役) |
| STM32 Firmware | ✅ 站立姿态 + UART 命令接口;踏步骨架已就绪(TIM6 100Hz 已配);IMU/I2C 暂未用 |
| Pi Agent Software | ⏳ Pending |

### v1 板子当前状态(2026-09-09)

- **二次焊接完成**:补焊 NRST 复位电路后,上电即跑(无需每次按 Reset)
- **8 路舵机标定完成**:全部居中 (Pulse=1500, 90°) → **大腿垂直地面 + 小腿水平向前**(用户实测 4 脚都承重,2026-09-11)
- **固件进度**:
 - STM32CubeMX 8 外设配齐 (TIM1/2/3/17 × 8 路 PWM + I2C1 + USART1 + SWD)
 - SYSCLK = 168 MHz (HSE 8MHz × PLL ×42 / 2)
 - 修复 3 个 bug: TIM17 Pulse=0 / HAL_TIM_PWM_Start 漏调 / NRST 虚焊
 - main.c 加 UART 接收 Pi 命令接口 (命令格式见 CLAUDE.md)
- **commit 节点**:
 - `288b487` v1 PCB 8路舵机驱动验证(里程碑)
 - `faf46d0` 8路舵机标定 + UART 接收 Pi 命令接口
- **当前 UART 命令**(2026-09-11 实测):
 - `<id> <pulse>` 设单路舵机(0-7)、`all <pulse>` 设全部 8 路、`center` 设全部 1500(标定基线)、`stand` 设站立姿态(SERVO_*_STAND 常量)、`sit` 8 路回 1500(等同 center,代码里还保留,几何意义仅是"伸直居中")
 - **步态**:`step trot`(启动 trot 踏步,TIM6 100Hz 已配)、`step stop`(停止踏步)、`step show`(打印当前 ham/shank/8 路 PWM)
- **已弃用(2026-09-11)**:
 - `cal raw/save/show` 标定命令(代码已删,2026-09-11 commit a810cce 后)
 - `h <delta_mm>` 身高控制(代码已删,见 git 历史 `feat(stm32):站立/蹲下姿态 + sit/stand UART 命令(2026-09-11)`)
- **踏步骨架(2026-09-11 写入,stepping.c/h,2026-09-12 修 TIM6 启动)**:
 - 移植 PA_GAIT.trot + PA_IK.ik case=0 + PA_ATTITUDE.cal_ges 简化版
 - 步态参数:抬腿 10mm / TIM6 100Hz / 步进 0.01 → 1 周期 1s(1Hz 步态)
 - TIM6 配置:Prescaler=16999、Period=99、NVIC TIM6_DAC_IRQn 已使能 → 100Hz
 - TIM6 中断已在 stm32g4xx_it.c 调 stepping_tick()
- **待办**:
 - 试跑 step trot(TIM6 启动 bug 已修,但实测前不要直接跑 — 舵机可能跳变,先开电源用手扶住)
 - I2C 读 MPU-6500 IMU 数据(代码里 i2c.c 已配,但没有任何 HAL_I2C_* 调用)

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

详见 [`机器狗v1控制板_投板前审查报告.md`](机器狗v1控制板_投板前审查报告.md)(本地项目文件,随仓库分发)。
