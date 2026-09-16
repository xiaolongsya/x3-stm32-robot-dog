/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    buzzer.c
  * @brief   PA11 蜂鸣器控制(2026-09-14)
  ******************************************************************************
  */
/* USER CODE END Header */

#include "buzzer.h"
#include "main.h"

/* PA11 默认低;on 拉高,off 拉低 */
#define BUZZER_PORT   GPIOA
#define BUZZER_PIN    GPIO_PIN_11

static volatile uint32_t off_at_ms = 0;  /* 到期时间(0=空闲) */

void buzzer_init(void) {
  GPIO_InitTypeDef gpio = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  gpio.Pin   = BUZZER_PIN;
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(BUZZER_PORT, &gpio);
  HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
  off_at_ms = 0;
}

void buzzer_on(uint16_t duration_ms) {
  HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_SET);
  if (duration_ms == 0) {
    off_at_ms = 0;   /* 0 = 不限时 */
  } else {
    off_at_ms = HAL_GetTick() + duration_ms;
  }
}

void buzzer_off(void) {
  HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
  off_at_ms = 0;
}

void buzzer_tick(void) {
  /* ⚠️ TIM6 ISR — 不能 printf */
  if (off_at_ms == 0) return;
  if ((int32_t)(HAL_GetTick() - off_at_ms) >= 0) {
    buzzer_off();
  }
}
