/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : 8 路舵机来回摆动 + UART 状态上报 + 身高控制(v1 测试版)
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "i2c.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "stepping.h"
#include "motions.h"
#include "commands.h"   /* 2026-09-14 X3 协议入口 */
#include "watchdog.h"   /* 2026-09-14 心跳超时守护 */
#include "buzzer.h"     /* 2026-09-14 蜂鸣器 PA11 */
#include "diagnostic.h" /* 2026-09-16 上电诊断标记 (从 main.c 搬出, 见 diagnostic.c) */
#include "ramp.h"       /* 2026-09-16 SIT/STAND 渐进 ramp, motion_poll() 自动调 ramp_tick() */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* 8 路舵机 STAND PWM 常量在 main.h(SERVO_*_STAND)
 * 2026-09-11 废弃 gait.c 后,STAND 不再运行时覆盖,改用常量直读
 */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* 8 路舵机映射表 (servo_id 0-7):
 *  0=PA2=TIM15_CH1=servo0  4=PA6=TIM3_CH1=servo4
 *  1=PA3=TIM15_CH2=servo1  5=PA7=TIM17_CH1=servo5
 *  2=PA4=TIM3_CH2=servo2   6=PB0=TIM3_CH3=servo6
 *  3=PA5=TIM2_CH1=servo3   7=PA8=TIM1_CH1=servo7
 *
 * 腿分布(2026-09-10 用户确认,对称):
 *  前肩: servo3(FR), servo4(FL)
 *  前小腿: servo2(FR), servo5(FL)
 *  后肩: servo1(BR), servo6(BL)
 *  后小腿: servo0(BR), servo7(BL)
 */

/* 当前 8 路舵机 PWM(2026-09-11 移除,cal save 废弃后不需要跟踪) */

/* 非 static:供未来 stepping 模块调用(原型在 main.h)
 * 2026-09-11 移除 current_pwm 跟踪,gait.c 已删,无其他模块需要 */
void set_servo_pulse(uint8_t id, uint16_t pulse) {
  if (pulse < 500 || pulse > 2500) return;
  /* 2026-09-16:同步给 ramp.c 的 g_pwm 跟踪,ramp 起点采样用 */
  motions_track_pwm(id, pulse);
  switch (id) {
    case 0: __HAL_TIM_SET_COMPARE(&htim15, TIM_CHANNEL_1, pulse); break;
    case 1: __HAL_TIM_SET_COMPARE(&htim15, TIM_CHANNEL_2, pulse); break;
    case 2: __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, pulse); break;
    case 3: __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, pulse); break;
    case 4: __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, pulse); break;
    case 5: __HAL_TIM_SET_COMPARE(&htim17, TIM_CHANNEL_1, pulse); break;
    case 6: __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, pulse); break;
    case 7: __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, pulse); break;
  }
}

/* 2026-09-14:text 命令解析搬到 commands.c(text 兼容模式),
 * binary 协议也由 commands.c 处理。这里不再有 parse_uart_command / uart_poll。*/
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_I2C1_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_TIM17_Init();
  MX_USART1_UART_Init();
  MX_TIM15_Init();
  MX_TIM6_Init();
  MX_TIM7_Init();
  MX_TIM1_Init();
  /* USER CODE BEGIN 2 */
  /* ⚠️ 2026-09-12 no-reply 修复:禁用 stdout 缓冲
   * 否则 newlib 可能把 printf 输出缓存在 FILE 里,直到 \n 才 flush,
   * 期间如果进程卡死,数据全丢 */
  setvbuf(stdout, NULL, _IONBF, 0);

  /* 2026-09-16 重构:诊断字符搬到 diagnostic.c(避免 CubeMX Generate Code 丢 })*/


  /* === 2026-09-15 BOOT 标识(诊断 STM32 ↔ X3 链路)===
   * X3 端跑 listen_booted.py 监听 ttyS3,收到 BOOT 说明:
   *   - STM32 跑到了 main()
   *   - USART1 TX 通路正常(PA9 输出)
   * 蜂鸣器(PBeeper 接 PA11)响 = STM32 跑到这里
   * 8 路被强制设 1500(蹲下 / 腿伸直) = 证明 PWM 输出也正常
   */
  /* ⚠️ CubeMX 不自动调 HAL_TIM_PWM_MspPostInit -> 必须手动启动 HAL_TIM_PWM_Start
   * 注意:set_servo_pulse 必须在 HAL_TIM_PWM_Start 之后 */
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);    /* PA8  = TIM1_CH1 = servo7 */
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);    /* PA5  = TIM2_CH1 = servo3 */
  HAL_TIM_PWM_Start(&htim15, TIM_CHANNEL_1);   /* PA2  = TIM15_CH1 = servo0 */
  HAL_TIM_PWM_Start(&htim15, TIM_CHANNEL_2);   /* PA3  = TIM15_CH2 = servo1 */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);    /* PA6  = TIM3_CH1 = servo4 */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2);    /* PA4  = TIM3_CH2 = servo2 */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_3);    /* PB0  = TIM3_CH3 = servo6 */
  HAL_TIM_PWM_Start(&htim17, TIM_CHANNEL_1);   /* PA7  = TIM17_CH1 = servo5 */

  /* === 上电默认姿态(2026-09-14 整理)===
   * 1) stepping_init() 初始化步态状态机(暂不启 TIM6,等 motion 触发)
   * 2) motion_init() 装载 MOTION_ID + 启动对应动作:
   *    - 先写 8 路 = SERVO_NEUTRAL_US(1500) 作为安全起点
   *    - 调 current->setup() 跳到对应姿态(STAND / TROT_STAND / SIT)
   *
   * 默认动作(MOTION_ID = MOTION_TROT,踏步测试):
   *    - 上电:8 路跳 TROT_STAND(中立位,无前倾)
   *    - 5s 后启动 stepping 对角 trot
   *    - 30s 后踏步结束,8 路回 STAND
   *
   * 切换其他模式:改 motions.h 的 MOTION_ID
   *    MOTION_STAND    :上电跳 STAND 后保持
   *    MOTION_BOB      :上电跳 STAND,5s 后蹲下,5s 后回 STAND,循环
   *    MOTION_SHIN_TEST:8 路同步线性 ramp 测小腿范围
   */
  stepping_init();
  motion_init();
  /* 2026-09-14:X3 协议 + 心跳守护 */
  commands_init();   /* USART1 DMA + IDLE 启动 */
  watchdog_init();   /* TIM7 1kHz 启动,X3 不发命令 200ms 后自动 STAND */

  /* 上电诊断标记(发 '1' '2' '3' '5' '6' 'B',tabby 看到 = STM32 跑到这步) */
  diagnostic_init();
  /* 2026-09-16:蜂鸣器初始化 */
  buzzer_init();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  diagnostic_mainloop();  /* 发 'C' = 进 main loop */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* === 主循环(2026-09-14:X3 上线后)===
     * 1) motion_poll():推进动作状态机
     * 2) commands_poll():解析 USART1 DMA + IDLE 收到的二进制 / 文本帧
     * 注:watchdog 由 TIM7 1kHz ISR 驱动,不需要在主循环调
     */
    motion_poll();
    commands_poll();
  /* USER CODE END 3 */
  }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV1;
  RCC_OscInitStruct.PLL.PLLN = 21;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
