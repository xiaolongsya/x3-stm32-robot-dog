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
 * 8 路舵机 STAND PWM 标定参数(2026-09-11 实测 4 脚承重,2026-09-14 整理)
 * ====================================================================
 * 标定目标: 4 脚贴地承重(大腿略后倾 + 小腿略下倾,见下方 STAND 常量)
 *
 * 物理含义(2026-09-16 用户拍板):
 *   - PWM = 1500 = 舵机中位 = 大腿垂直地面 + 小腿平行地面(直角,部件极值位)
 *     此时膝盖最低、小腿不占垂直高度
 *   - 1500 是 8 路舵机的"标准值" = **跪下姿态**(比蹲下还低,不能用于站立)
 *   - STAND 是 4 脚贴地的实测站立姿态,作为步态参考基线,**非机械端点**
 *   - 从 1500(跪下)到 STAND:8 路舵机全部**向后**移动
 *   - ⚠️ 没有绝对高度参照:髋关节固定在躯干上不动,只有膝盖/脚在动
 *
 * ★ 方向规则(2026-09-16 用户拍板,**唯一判定标准**):
 *   "向前" = 沿狗头前进方向(水平方向,不是垂直高度)
 *
 *   两条记忆规律(可推出全部 8 路):
 *     1. 同侧肩/小腿方向相反
 *     2. 左右同部位方向相反
 *
 *   完整对照表:
 *     id=0 BR 小腿  P 增 = 向后  |  P 减 = 向前
 *     id=1 BR 肩    P 增 = 向前  |  P 减 = 向后
 *     id=2 FR 小腿  P 增 = 向后  |  P 减 = 向前
 *     id=3 FR 肩    P 增 = 向前  |  P 减 = 向后
 *     id=4 FL 肩    P 增 = 向后  |  P 减 = 向前
 *     id=5 FL 小腿  P 增 = 向前  |  P 减 = 向后
 *     id=6 BL 肩    P 增 = 向后  |  P 减 = 向前
 *     id=7 BL 小腿  P 增 = 向前  |  P 减 = 向后
 *
 *   步态用法:
 *     抬腿(swing)   = 小腿向狗头方向倾斜(右小腿 P 减 / 左小腿 P 增)
 *     推进(support) = 大腿(肩)向后摆(脚蹬地,反作用力推身体向前)
 *
 * ★★★ 2 个特殊舵机(机械装配偏差):
 *   id=4 FL 肩:相对其他标准舵机 +100 偏置(标准值 1600)
 *   id=7 BL 小腿:相对其他标准舵机 +80 偏置(标准值 1580)
 *   以后统一改 8 个 STAND 时,这两个单独调整,其他 6 个走标准值
 *
 * 安全活动范围(2026-09-16 stepping.c 里 SERVO_STEP):
 *   - 4 小腿:宽度都 = 700
 *     BR(900-1600),FR(900-1600),FL(1400-2100),BL(1480-2180)
 *   - 4 肩:宽度都 = 1600
 *     BR(700-2300),FR(700-2300),FL(800-2400),BL(700-2300)
 *   范围根据 STAND ±X 自动生成,详细见 stepping.c 的 SERVO_STEP 表
 * ==================================================================== */

/* --- 前左腿 FL --- */
#define SERVO_SHOULDER_FL_STAND   2000  /* 左前肩:PA6/TIM3_CH1/servo4  ↑身体高 / ↓身体低 */
#define SERVO_SHIN_FL_STAND       1400  /* 左前小腿:PA7/TIM17_CH1/servo5 ↑身体高 / ↓身体低 */

/* --- 前右腿 FR --- */
#define SERVO_SHOULDER_FR_STAND   1100  /* 右前肩:PA5/TIM2_CH1/servo3  ↑身体高 / ↓身体低 */
#define SERVO_SHIN_FR_STAND       1600  /* 右前小腿:PA4/TIM3_CH2/servo2 ↑身体高 / ↓身体低 */

/* --- 后左腿 BL --- */
#define SERVO_SHOULDER_BL_STAND   1900  /* 左后肩:PB0/TIM3_CH3/servo6  ↑身体高 / ↓身体低 */
#define SERVO_SHIN_BL_STAND       1480  /* 左后小腿:PA8/TIM1_CH1/servo7 ↑身体高 / ↓身体低 */
                                                          /* 2026-09-16:取消 +80 偏置 (1580→1480,正站立) */
                                                          /* 2026-09-16:CubeMX 配回 CH1 (PA8) */

/* --- 后右腿 BR --- */
#define SERVO_SHOULDER_BR_STAND   1100  /* 右后肩:PA3/TIM15_CH2/servo1  ↑身体高 / ↓身体低 */
                                                          /* 2026-09-16:从 TIM2_CH4 改成 TIM15_CH2 (CubeMX 配置) */
#define SERVO_SHIN_BR_STAND       1600  /* 右后小腿:PA2/TIM15_CH1/servo0 ↑身体高 / ↓身体低 */
                                                          /* 2026-09-16:从 TIM2_CH3 改成 TIM15_CH1 (CubeMX 配置) */
/* === 标定基线(center 命令使用)== */
#define SERVO_NEUTRAL_US   1500
/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */
void set_servo_pulse(uint8_t id, uint16_t pulse);   /* 2026-09-15 防 CubeMX 重生成清 */
/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
