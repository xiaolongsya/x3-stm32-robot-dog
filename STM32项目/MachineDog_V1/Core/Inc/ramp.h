/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ramp.h
  * @brief   8 路 PWM 同步渐进 ramp (2026-09-16 动作组扩展)
  *
  * 设计动机:
  *   - SIT_DOWN / STAND_UP 等动作必须 8 路同步渐进(单帧所有路更新)
  *   - 用 HAL_GetTick() + motion_poll() 软件驱动,不依赖任何 TIM ISR
  *
  * 用法:
  *   ramp_sit_to_target(target_pwm[8], total_ms, on_complete);
  *   ramp_tick();  // 每帧在主循环调
  *   ramp_cancel();
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __RAMP_H
#define __RAMP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef void (*ramp_complete_cb_t)(void);

/* === 对外 API === */
void ramp_sit_to_target(const uint16_t target_pwm[8], uint32_t total_ms,
                        ramp_complete_cb_t on_complete);

/* === PWM 跟踪(2026-09-16 加)===
 * main.c 的 set_servo_pulse() 每次写入都调这个,ramp.c 用 g_pwm[]
 * 记录 8 路当前值,ramp_sit_to_target() 以它为起点 → 任何姿态起 ramp 都无跳变
 */
void motions_track_pwm(uint8_t id, uint16_t pulse);
void ramp_cancel(void);
uint8_t ramp_is_active(void);
void ramp_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* __RAMP_H */