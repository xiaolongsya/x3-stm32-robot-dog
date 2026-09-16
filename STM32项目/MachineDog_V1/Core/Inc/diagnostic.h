/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    diagnostic.h
  * @brief   STM32 ↔ X3 串口诊断标记模块(2026-09-16 重构)
  *
  * 背景:
  *   之前把诊断字符('1' '2' '3' ... 'C')塞在 main.c 的 USER CODE 2 区,
  *   导致 CubeMX Generate Code 反复把 main() 闭合 } 吞掉,
  *   每次都要手动补 } 才能编译.
  *
  * 重构(2026-09-16):
  *   把诊断字符搬到独立 diagnostic.c, main.c 只剩 2 行调用
  *   → CubeMX 模板不再触发 } 丢失 bug
  *
  * 用法:
  *   MX 初始化完成后调 diagnostic_init()
  *   进 main loop 前调 diagnostic_mainloop()
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __DIAGNOSTIC_H
#define __DIAGNOSTIC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 阶段标记字符(配合 PC 端 tabby 监听 115200 8N1)
 * '1' = USART1 UART_Init 完成
 * '2' = TIM6/7 init 完成
 * '3' = setvbuf 完成
 * '5' = 8 路 PWM 启动完成
 * '6' = 上电 BOOT 字节 'B' 前
 * 'B' = 上电 BOOT 标识
 * 'C' = 进 main loop
 */
void diagnostic_init(void);       /* USER CODE 2 区末尾调, 发 '1' '2' '3' '5' '6' 'B' */
void diagnostic_mainloop(void);   /* 进 while(1) 之前调, 发 'C' */

#ifdef __cplusplus
}
#endif

#endif /* __DIAGNOSTIC_H */
