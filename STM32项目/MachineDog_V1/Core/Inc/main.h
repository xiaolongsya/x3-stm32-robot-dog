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
 * 8 路舵机 STAND PWM 标定参数(2026-09-11 实测 4 脚承重,2026-09-12 立约定)
 * ====================================================================
 * 标定目标: 大腿垂直地面 + 小腿水平向前(4 条腿都满足)
 *
 * 物理含义(2026-09-12 用户拍板):
 *   - PWM = 1500 = 舵机中位 = 大腿垂直地面 + 小腿水平向前(几何最高)
 *   - STAND 是 4 脚贴地的实测姿态,作为步态参考基线,**非机械端点**
 *   - 4 肩 STAND 偏离 1500 是为了平衡需要把大腿稍微折开,前后都有空间
 *   - 4 小腿 STAND 是当前装配下的姿态(BL/BR 偏离 1500 是机械公差)
 *
 * PWM 方向约定(2026-09-12 用户多轮拍板最终版):
 *   - 物理方向一致:肩部"折"=大腿往身体中线靠拢;小腿"伸展"=升高身体
 *   - 左右小腿 PWM 方向相反(舵机反装):右小腿 P 增=伸展,左小腿 P 减=伸展
 *   - 前后肩 PWM 方向相反:前肩 P 减=内折,后肩 P 增=内折
 *   - 8 路 STAND 全部满足:4 肩内折 + 4 小腿极限伸展 ✅
 *
 *   完整对照表:
 *     id=0 BR 小腿  STAND=1600  P 增=伸展(升), P 减=内折(降)
 *     id=1 BR 肩    STAND=1150  P 增=内折(平衡), P 减=外展
 *     id=2 FR 小腿  STAND=1500  P 增=伸展(升), P 减=内折(降)
 *     id=3 FR 肩    STAND=1200  P 增=外展,     P 减=内折(平衡)
 *     id=4 FL 肩    STAND=1820  P 增=外展,     P 减=内折(平衡)
 *     id=5 FL 小腿  STAND=1500  P 增=内折(降), P 减=伸展(升)
 *     id=6 BL 肩    STAND=1850  P 增=内折(平衡), P 减=外展
 *     id=7 BL 小腿  STAND=1400  P 增=内折(降), P 减=伸展(升)
 *
 * 安全活动范围(2026-09-12 用户拍板):
 *   - 4 小腿统一 (1400, 1600)
 *   - 4 肩统一 (1000, 2000)
 * ==================================================================== */

/* --- 前左腿 FL --- */
#define SERVO_SHOULDER_FL_STAND   1820  /* 左前肩:PA6/TIM3_CH1/servo4  ↑身体高 / ↓身体低 */
#define SERVO_SHIN_FL_STAND       1500  /* 左前小腿:PA7/TIM17_CH1/servo5 ↑身体高 / ↓身体低 */

/* --- 前右腿 FR --- */
#define SERVO_SHOULDER_FR_STAND   1200  /* 右前肩:PA5/TIM2_CH1/servo3  ↑身体高 / ↓身体低 */
#define SERVO_SHIN_FR_STAND       1500  /* 右前小腿:PA4/TIM3_CH2/servo2 ↑身体高 / ↓身体低 */

/* --- 后左腿 BL --- */
#define SERVO_SHOULDER_BL_STAND   1850  /* 左后肩:PB0/TIM3_CH3/servo6  ↑身体高 / ↓身体低 */
#define SERVO_SHIN_BL_STAND       1400  /* 左后小腿:PA8/TIM1_CH1/servo7 ↑身体高 / ↓身体低 */

/* --- 后右腿 BR --- */
#define SERVO_SHOULDER_BR_STAND   1150  /* 右后肩:PA3/TIM2_CH4/servo1  ↑身体高 / ↓身体低 */
#define SERVO_SHIN_BR_STAND       1600  /* 右后小腿:PA2/TIM2_CH3/servo0 ↑身体高 / ↓身体低 */

/* === 标定基线(center 命令使用)== */
#define SERVO_NEUTRAL_US   1500
/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);
/* 8 路舵机 PWM 输出 — 在 USER CODE 区外,CubeMX 重生成不会清 */
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
