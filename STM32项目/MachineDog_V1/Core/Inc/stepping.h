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
  *   3. 加 SERVO_LIMIT 安全限位(防止 IK 偶发偏差打到机械极限)
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
  * 腿编号约定(对角步态,2026-09-11):
  *   腿1=FR (大腿=servo3, 小腿=servo2)  → 公式: thigh=init_1h - ham, shin=init_1s + shank
  *   腿2=FL (大腿=servo4, 小腿=servo5)  → 公式: thigh=init_2h + ham, shin=init_2s - shank(镜像)
  *   腿3=BL (大腿=servo6, 小腿=servo7)  → 公式: thigh=init_3h + ham, shin=init_3s - shank(镜像)
  *   腿4=BR (大腿=servo1, 小腿=servo0)  → 公式: thigh=init_4h - ham, shin=init_4s + shank
  *
  *   trot:腿 1+3 swing vs 腿 2+4 support(对角交替)
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