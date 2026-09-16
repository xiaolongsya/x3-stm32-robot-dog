/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    watchdog.c
  * @brief   TIM7 1kHz 心跳超时守护(2026-09-14)
  *
  * 实现:TIM7 1kHz 中断(prescaler=16999,period=9)
  *   每 tick 检查 now - last_heartbeat_ms,超阈值则回 STAND
  *   stepping_stop() 会发 STOP 命令给 TIM6 步态状态机
  *   motion_play_by_id(1, 0) 切到 MOTION_STAND(无限)
  *
  * 注意:
  *   - watchdog_reset() 写 volatile 变量,在 ISR / 主循环都能调,无锁
  *   - TIM7 优先级 = 1(TIM6 = 0),保证步态不被 watchdog ISR 打断太久
  ******************************************************************************
  */
/* USER CODE END Header */

#include "watchdog.h"
#include "main.h"
#include "stepping.h"
#include "motions.h"

extern TIM_HandleTypeDef htim7;   /* 在 tim.c 定义 */

static volatile uint32_t last_heartbeat_ms = 0;
static volatile uint8_t  wd_triggered = 0;   /* 防重复触发(只切一次) */

void watchdog_init(void) {
  last_heartbeat_ms = HAL_GetTick();
  wd_triggered = 0;
  /* TIM7 1kHz:170MHz / (16999+1) / (9+1) = 1kHz */
  if (HAL_TIM_Base_Start_IT(&htim7) != HAL_OK) {
    Error_Handler();
  }
}

void watchdog_reset(void) {
  last_heartbeat_ms = HAL_GetTick();
  wd_triggered = 0;   /* 重新连接后清掉,允许下次超时再次切 STAND */
}

void watchdog_poll(void) {
  /* ⚠️ TIM7 ISR — 不能 printf */
  uint32_t now = HAL_GetTick();
  uint32_t elapsed = now - last_heartbeat_ms;
  if (elapsed >= WATCHDOG_TIMEOUT_MS && !wd_triggered) {
    /* 超时:回 STAND,关闭所有动作 */
    stepping_stop();
    motion_play_by_id(1, 0);   /* MOTION_STAND,无限 */
    wd_triggered = 1;
  }
}

uint32_t watchdog_last_heartbeat(void) {
  return last_heartbeat_ms;
}
