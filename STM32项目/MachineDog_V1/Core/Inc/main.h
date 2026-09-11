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
/* === 8 路舵机 STAND PWM(2026-09-11,从 main.c 移到这里)===
 * 默认值(可由 UART `cal save` 在运行时覆盖)
 * 索引按 servo_id 0-7:BR小腿/BR肩/FR小腿/FR肩/FL肩/FL小腿/BL肩/BL小腿
 */
/* 4 路肩部舵机(180 度范围 500-2500) */
#define SERVO_SHOULDER_BR_STAND   1200  /* PA3  = TIM2_CH4 = servo1 = BR 肩(−300 弯曲)*/
#define SERVO_SHOULDER_FR_STAND   1300  /* PA5  = TIM2_CH1 = servo3 = FR 肩(−200 弯曲)*/
#define SERVO_SHOULDER_FL_STAND   1620  /* PA6  = TIM3_CH1 = servo4 = FL 肩(+120 弯曲)*/
#define SERVO_SHOULDER_BL_STAND   1750  /* PB0  = TIM3_CH3 = servo6 = BL 肩(+250 弯曲)*/
/* 4 路小腿舵机 */
#define SERVO_SHIN_BR_STAND        1600  /* PA2  = TIM2_CH3 = servo0 = BR 小腿(+100 抬升)*/
#define SERVO_SHIN_FR_STAND        1600  /* PA4  = TIM3_CH2 = servo2 = FR 小腿(+100 抬升)*/
#define SERVO_SHIN_FL_STAND        1400  /* PA7  = TIM17_CH1 = servo5 = FL 小腿(−100 抬升)*/
#define SERVO_SHIN_BL_STAND        1400  /* PA8  = TIM1_CH1 = servo7 = BL 小腿(−100 抬升)*/

/* === 标定基线(center 命令使用)=== */
#define SERVO_NEUTRAL_US   1500
/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* 8 路舵机 PWM 输出(给 gait.c 用) */
void set_servo_pulse(uint8_t id, uint16_t pulse);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
