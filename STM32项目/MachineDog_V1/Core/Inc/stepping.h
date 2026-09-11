/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stepping.h
  * @brief   py-apple-dynamics V7.3 PA_GAIT.trot + IK + servo_output 移植
  *          适配 STM32G431KBT6(2026-09-11)
  *
  * 与旧 gait.c 的关键差异:
  *   1. 不套 py-apple 的 init_*=90° 假设,改用我们机器狗实测 STAND_PWM 反推 init_*
  *   2. 不套 py-apple 的 ±90 数学偏移,镜像通过 ham/shank 符号翻转实现
  *   3. SERVO_LIMIT 安全活动范围(防止 IK 偶发偏差打到机械极限)
  *
  * 来源:
  *   padog.py / PA_GAIT.py / PA_IK.py / PA_ATTITUDE.py(PA-Dynamics V7.3 SRC)
  *
  * 调度:TIM6 100Hz 中断 → stepping_tick()
  *   CubeMX:Prescaler=16999 (170MHz→10kHz),Period=99 (10kHz/100=100Hz)
  *
  * UART 命令:
  *   step trot    启动原地踏步
  *   step stop    停止踏步,舵机回 STAND
  *   step show    打印当前 ham/shank/8 路 PWM(每 0.5s 一次)
  *
  * 腿编号约定(对角步态,2026-09-12 与 stepping.c 对齐):
  *   腿1=FR (大腿=servo3, 小腿=servo2)  → 公式: thigh=init_1h + (ham-HAM_STD_FRONT), shin=init_1s - (shank-SHANK_STD)
  *   腿2=FL (大腿=servo4, 小腿=servo5)  → 公式: thigh=init_2h - (ham-HAM_STD_FRONT), shin=init_2s + (shank-SHANK_STD)(镜像)
  *   腿3=BL (大腿=servo6, 小腿=servo7)  → 公式: thigh=init_3h - (ham-HAM_STD_BACK),  shin=init_3s + (shank-SHANK_STD)(镜像)
  *   腿4=BR (大腿=servo1, 小腿=servo0)  → 公式: thigh=init_4h + (ham-HAM_STD_BACK),  shin=init_4s - (shank-SHANK_STD)
  *
  *   trot:腿 1+3 swing vs 腿 2+4 support(对角交替)
  *
  * STAND 物理含义(2026-09-12 用户拍板):
  *   - 1500 = 舵机中位 = 大腿垂直 + 小腿水平(几何最高)
  *   - STAND = 4 脚贴地的实测姿态,作为步态参考基线(非机械端点)
  *   - SERVO_LIMIT:4 小腿统一 (1400, 1600);4 肩统一 (1000, 2000)
  *
  * PWM 方向约定(2026-09-12 用户拍板):
  *   - 左小腿 PWM 增 → 内折(降低);PWM 减 → 极限伸展(升高)
  *   - 右小腿 PWM 增 → 极限伸展(升高);PWM 减 → 内折(降低)
  *   - 左肩 PWM 增 → 外展;PWM 减 → 内折
  *   - 右肩 PWM 增 → 内折(右舵机反向);PWM 减 → 外展
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef STEPPING_H
#define STEPPING_H

#include <stdint.h>

/* === 步态状态机 ===========================================================*/
typedef enum {
  STEPPING_IDLE = 0,     /* 待机,所有腿 STAND */
  STEPPING_TROT,         /* 对角小跑步态(原地踏步,x_target=0) */
} SteppingState;

/* === 接口 =================================================================*/
/**
 * @brief 初始化(在 HAL_TIM_PWM_Start 之后调)
 *        不启动 TIM6,需要 stepping_start_trot() 才开中断
 */
void stepping_init(void);

/**
 * @brief 启动原地踏步(TIM6 100Hz,抬腿 10mm 保守值)
 */
void stepping_start_trot(void);

/**
 * @brief 停止踏步,关闭 TIM6,舵机回 STAND
 */
void stepping_stop(void);

/**
 * @brief TIM6 100Hz 中断调用 — 推进相位 + 算 8 路 PWM
 *        ⚠️ 不要在主循环手动调
 */
void stepping_tick(void);

/**
 * @brief 查询状态
 */
SteppingState stepping_get_state(void);

/**
 * @brief 调试:打印当前 ham/shank/8 路 PWM 到 UART
 *        用于 USB 串口观察波形
 */
void stepping_show(void);

#endif /* STEPPING_H */