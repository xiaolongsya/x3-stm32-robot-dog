/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    buzzer.h
  * @brief   蜂鸣器控制(2026-09-14 加)
  *
  * 硬件(网表确认):
  *   BUZZER_CTRL = PA11 (U1.21) → R5 → Q1 → BUZZER1
  *   有源蜂鸣器(2.7kHz 内部振荡),高电平响,低电平停
  *   PA11 不与其他外设冲突(空闲脚)
  *
  * 时序:
  *   buzzer_on(duration_ms)   拉高 PA11,记录到期时间
  *   buzzer_off()             立即拉低
  *   buzzer_tick()            100Hz 扫描,到期自动 off(在 TIM6 ISR 调)
  *
  * 注意:不阻塞、不 printf。
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __BUZZER_H
#define __BUZZER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void buzzer_init(void);                  /* MX_GPIO_Init 之后调,PA11 配输出推挽 */
void buzzer_on(uint16_t duration_ms);    /* 响 N 毫秒,0 = 不限时(等 buzzer_off) */
void buzzer_off(void);                   /* 立即停 */
void buzzer_tick(void);                  /* TIM6 ISR 调(100Hz),到期自动 off */

#ifdef __cplusplus
}
#endif

#endif /* __BUZZER_H */
