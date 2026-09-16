/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    watchdog.h
  * @brief   UART 心跳超时 → 自动 STAND 守护(2026-09-14)
  *
  * 原理:
  *   - TIM7 1kHz 中断里扫描 last_heartbeat_ms
  *   - 超过 WATCHDOG_TIMEOUT_MS (默认 200ms) 没有任何有效帧
  *     → 调 stepping_stop() 回 STAND,关闭动作
  *   - X3 每 100ms 发 HEARTBEAT,200ms 阈值给一帧余量
  *
  * 关键约束:
  *   - TIM7 优先级必须低于 TIM6(步态),否则破坏 100Hz 实时性
  *   - watchdog_reset() 必须在主循环和 IDLE 中断里都能调(只写 volatile)
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __WATCHDOG_H
#define __WATCHDOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 心跳超时阈值(ms),X3 发 100ms 周期,200ms = 允许丢 1 帧 */
#define WATCHDOG_TIMEOUT_MS   200u

void watchdog_init(void);
void watchdog_reset(void);          /* 收到任何有效帧 / HEARTBEAT 时调 */
void watchdog_poll(void);           /* TIM7 1kHz ISR 调 */
uint32_t watchdog_last_heartbeat(void);  /* 调试用,返回上次喂狗的 HAL_GetTick() */

#ifdef __cplusplus
}
#endif

#endif /* __WATCHDOG_H */
