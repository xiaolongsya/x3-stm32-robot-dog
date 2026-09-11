/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32g4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdint.h>
/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* ====================================================================
 * 8 路舵机 STAND PWM 标定参数(2026-09-11,放顶部方便改)
 * ====================================================================
 * 标定目标: 大腿垂直地面 + 小腿水平向前(4 条腿都满足)
 * 可由 UART `cal save` 在运行时覆盖(无需重编译)
 *
 * PWM 方向影响(从舵机后方看):
 *   肩部 PWM 增大 → 大腿向后摆 → 身体变低
 *   肩部 PWM 减小 → 大腿向前摆 → 身体变高
 *   小腿 PWM 增大 → 小腿向上抬 → 身体变高
 *   小腿 PWM 减小 → 小腿向下落 → 身体变低
 *
 * 命名: SERVO_[部位]_[前后]_STAND(左/右 × 前/后 × 肩/小腿)
 *   FL = 前左(Front Left),   FR = 前右(Front Right)
 *   BL = 后左(Back Left),    BR = 后右(Back Right)
 *   肩部 = 大腿舵机,   小腿 = 小腿舵机
 * ==================================================================== */

/* --- 前左腿 FL --- */
#define SERVO_SHOULDER_FL_STAND   1620  /* 左前肩:PA6/TIM3_CH1/servo4  ↑身体高 / ↓身体低 */
#define SERVO_SHIN_FL_STAND       1400  /* 左前小腿:PA7/TIM17_CH1/servo5 ↑身体高 / ↓身体低 */

/* --- 前右腿 FR --- */
#define SERVO_SHOULDER_FR_STAND   1300  /* 右前肩:PA5/TIM2_CH1/servo3  ↑身体高 / ↓身体低 */
#define SERVO_SHIN_FR_STAND       1600  /* 右前小腿:PA4/TIM3_CH2/servo2 ↑身体高 / ↓身体低 */

/* --- 后左腿 BL --- */
#define SERVO_SHOULDER_BL_STAND   1750  /* 左后肩:PB0/TIM3_CH3/servo6  ↑身体高 / ↓身体低 */
#define SERVO_SHIN_BL_STAND       1400  /* 左后小腿:PA8/TIM1_CH1/servo7 ↑身体高 / ↓身体低 */

/* --- 后右腿 BR --- */
#define SERVO_SHOULDER_BR_STAND   1200  /* 右后肩:PA3/TIM2_CH4/servo1  ↑身体高 / ↓身体低 */
#define SERVO_SHIN_BR_STAND       1600  /* 右后小腿:PA2/TIM2_CH3/servo0 ↑身体高 / ↓身体低 */

/* === 标定基线(center 命令使用)== */
#define SERVO_NEUTRAL_US   1500
/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */
/* 8 路舵机 PWM 输出(供 gait.c 用,gait.c 调 8 路舵机)*/
void set_servo_pulse(uint8_t id, uint16_t pulse);
/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
