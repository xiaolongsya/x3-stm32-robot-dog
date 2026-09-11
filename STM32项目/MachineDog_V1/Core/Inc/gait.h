/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gait.h
  * @brief   PA_GAIT.trot + PA_IK.case=0 移植到 STM32G431KBT6 (2026-09-11)
  *
  * 步态:对角小跑步态(trot)。原地踏步 = x_target=0。
  * 调度:由 TIM6 100Hz 中断调用 gait_tick(),内部推进相位 + IK + 8 路 PWM 输出。
  *
  * 依赖:gait.c 必须由用户在 CubeMX 加 TIM6 (Prescaler=16999, Period=99) +
  *      使能 TIM6 global interrupt (NVIC),然后在 TIM6_IRQHandler 里调 gait_tick()。
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef GAIT_H
#define GAIT_H

#include <stdint.h>

/* === 步态状态机 ===========================================================*/
typedef enum {
  GAIT_IDLE = 0,     /* 待机,所有腿保持当前姿态(进入状态时已设回 STAND) */
  GAIT_TROT,         /* 对角小跑步态(原地踏步 = x_target=0) */
} GaitState;

/* === 接口 =================================================================*/
/**
 * @brief 初始化 gait 模块(在 main() 调用 HAL_TIM_PWM_Start 之后)
 *        不启动 TIM6,需要 gait_start_trot() 才开中断
 */
void gait_init(void);

/**
 * @brief 启动 trot 步态(原地踏步,x_target=0,h=15mm,r1=r4=±1)
 *        启动 TIM6 中断,每 10ms 调 gait_tick()
 */
void gait_start_trot(void);

/**
 * @brief 停止步态,关闭 TIM6 中断,所有腿回 STAND 姿态
 */
void gait_stop(void);

/**
 * @brief 周期调用(100Hz,即每 10ms 一次)
 *        在 TIM6_IRQHandler 里调;内部推进相位 + IK + PWM 输出
 *        ⚠️ 不要在主循环里手动调,避免和 TIM6 重复触发
 */
void gait_tick(void);

/**
 * @brief 查询当前步态状态
 */
GaitState gait_get_state(void);

#endif /* GAIT_H */