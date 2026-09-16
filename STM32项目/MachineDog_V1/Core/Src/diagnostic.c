/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    diagnostic.c
  * @brief   STM32 ↔ X3 串口诊断标记(2026-09-16 从 main.c 搬出)
  *
  * 注意: 这里只发字符, 不做任何 if 判断 / 状态机.
  *       如果卡死在某 init 里, tabby 看不到后续字符 = 卡在那一步.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "diagnostic.h"
#include "usart.h"

void diagnostic_init(void) {
  /* 阶段标记: 按调用顺序逐字符发出
   * 每个字符 50ms timeout, 8N1 115200 下 < 100us 真正发送耗时,
   * timeout 只是保护 HAL_UART_Transmit 不死等
   */
  HAL_UART_Transmit(&huart1, (uint8_t*)"1", 1, 50);  /* UART init OK */
  HAL_UART_Transmit(&huart1, (uint8_t*)"2", 1, 50);  /* TIM init OK */
  HAL_UART_Transmit(&huart1, (uint8_t*)"3", 1, 50);  /* setvbuf OK */
  HAL_UART_Transmit(&huart1, (uint8_t*)"5", 1, 50);  /* 8 路 PWM OK */
  HAL_UART_Transmit(&huart1, (uint8_t*)"6", 1, 50);  /* BOOT 字节前 */
  HAL_UART_Transmit(&huart1, (uint8_t*)"B", 1, 100); /* 上电 BOOT 标识 */
}

void diagnostic_mainloop(void) {
  HAL_UART_Transmit(&huart1, (uint8_t*)"C", 1, 50);  /* 进 main loop */
}
