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
#include "i2c.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "stepping.h"
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
 *  0=PA2=TIM2_CH3=servo0   4=PA6=TIM3_CH1=servo4
 *  1=PA3=TIM2_CH4=servo1   5=PA7=TIM17_CH1=servo5
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
  switch (id) {
    case 0: __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, pulse); break;
    case 1: __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_4, pulse); break;
    case 2: __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, pulse); break;
    case 3: __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, pulse); break;
    case 4: __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, pulse); break;
    case 5: __HAL_TIM_SET_COMPARE(&htim17, TIM_CHANNEL_1, pulse); break;
    case 6: __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, pulse); break;
    case 7: __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, pulse); break;
  }
}

/* === 标定基线(2026-09-11 保留)===
 * center 命令:把全部 8 路舵机设到 SERVO_NEUTRAL_US(1500µs = 90°)
 * SERVO_NEUTRAL_US 常量已在 main.h 定义
 */

/* UART 接收命令解析(2026-09-11):
 * 格式: "<servo_id> <pulse>\n"   例如 "0 1500\n"   -> 设 servo0=1500
 *       "all <pulse>\n"           设全部 8 路
 *       "center\n"                设全部 1500(居中,标定基线)
 *       "stand\n"                 站立姿态(SERVO_*_STAND 常量)
 *       "step trot"               启动原地踏步(100Hz TIM6,抬腿 10mm 保守值)
 *       "step stop"               停止踏步,回 STAND
 *       "step show"               打印当前 ham/shank/8 路 PWM
 */
static char rx_buf[32];
static uint8_t rx_idx = 0;

static void parse_uart_command(const char *cmd) {
  unsigned int id = 0, pulse = 0;
  /* 用 strncmp 前置判别 "all" / "step",避免 sscanf("%u %u") 误拦截 */
  if (strncmp(cmd, "all ", 4) == 0) {
    if (sscanf(cmd + 4, "%u", &pulse) == 1) {
      if (pulse >= 500 && pulse <= 2500) {
        for (uint8_t i = 0; i < 8; i++) set_servo_pulse(i, (uint16_t)pulse);
        printf("OK all=%u\n", pulse);
      } else {
        printf("ERR pulse range\n");
      }
    } else {
      printf("ERR fmt\n");
    }
  } else if (strcmp(cmd, "step trot") == 0) {
    stepping_start_trot();
  } else if (strcmp(cmd, "step stop") == 0) {
    stepping_stop();
  } else if (strcmp(cmd, "step show") == 0) {
    stepping_show();
  } else if (sscanf(cmd, "%u %u", &id, &pulse) == 2) {
    if (id <= 7) {
      set_servo_pulse((uint8_t)id, (uint16_t)pulse);
      printf("OK s%u=%u\n", id, pulse);
    } else {
      printf("ERR id>7\n");
    }
  } else if (strcmp(cmd, "center") == 0) {
    for (uint8_t i = 0; i < 8; i++) set_servo_pulse(i, SERVO_NEUTRAL_US);
    printf("OK center\n");
  } else if (strcmp(cmd, "stand") == 0) {
    /* 站立姿态:从 SERVO_*_STAND 常量直接读取 */
    set_servo_pulse(0, SERVO_SHIN_BR_STAND);
    set_servo_pulse(1, SERVO_SHOULDER_BR_STAND);
    set_servo_pulse(2, SERVO_SHIN_FR_STAND);
    set_servo_pulse(3, SERVO_SHOULDER_FR_STAND);
    set_servo_pulse(4, SERVO_SHOULDER_FL_STAND);
    set_servo_pulse(5, SERVO_SHIN_FL_STAND);
    set_servo_pulse(6, SERVO_SHOULDER_BL_STAND);
    set_servo_pulse(7, SERVO_SHIN_BL_STAND);
    printf("OK stand\n");
  } else if (strcmp(cmd, "sit") == 0) {
    /* 蹲下:8 路全 1500(标定基线,腿完全伸直/居中) */
    for (uint8_t i = 0; i < 8; i++) set_servo_pulse(i, SERVO_NEUTRAL_US);
    printf("OK sit\n");
  } else {
    /* 加回显便于调试 */
    printf("ERR fmt: '%s'\n", cmd);
  }
}

static void uart_poll(void) {
  uint8_t c;
  if (HAL_UART_Receive(&huart1, &c, 1, 0) == HAL_OK) {
    if (c == '\n' || c == '\r') {
      if (rx_idx > 0) {
        rx_buf[rx_idx] = 0;
        parse_uart_command(rx_buf);
        rx_idx = 0;
      }
    } else if (rx_idx < sizeof(rx_buf) - 1) {
      rx_buf[rx_idx++] = c;
    }
  }
}
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
  MX_I2C1_Init();
  MX_TIM1_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_TIM17_Init();
  MX_USART1_UART_Init();
  MX_TIM6_Init();
  /* USER CODE BEGIN 2 */
  /* ⚠️ CubeMX 不自动调 HAL_TIM_PWM_MspPostInit -> 必须手动启动 HAL_TIM_PWM_Start */
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);    /* PA8  = TIM1_CH1 = servo7 */
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);    /* PA5  = TIM2_CH1 = servo3 */
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3);    /* PA2  = TIM2_CH3 = servo0 */
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_4);    /* PA3  = TIM2_CH4 = servo1 */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);    /* PA6  = TIM3_CH1 = servo4 */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2);    /* PA4  = TIM3_CH2 = servo2 */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_3);    /* PB0  = TIM3_CH3 = servo6 */
  HAL_TIM_PWM_Start(&htim17, TIM_CHANNEL_1);   /* PA7  = TIM17_CH1 = servo5 */

  /* === 上电默认姿态(2026-09-11)===
   * 默认: 直接用 SERVO_*_STAND 常量写 8 路舵机 = 站立姿态
   * stepping_init() 必须在 HAL_TIM_PWM_Start 之后调(虽然不上 TIM6)
   */
  stepping_init();
  set_servo_pulse(0, SERVO_SHIN_BR_STAND);
  set_servo_pulse(1, SERVO_SHOULDER_BR_STAND);
  set_servo_pulse(2, SERVO_SHIN_FR_STAND);
  set_servo_pulse(3, SERVO_SHOULDER_FR_STAND);
  set_servo_pulse(4, SERVO_SHOULDER_FL_STAND);
  set_servo_pulse(5, SERVO_SHIN_FL_STAND);
  set_servo_pulse(6, SERVO_SHOULDER_BL_STAND);
  set_servo_pulse(7, SERVO_SHIN_BL_STAND);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* === 修复 2026-09-11 ===
     * 原 bug: HAL_Delay(10) + timeout=0 + FIFO 关闭 → 7 字节命令 0.6ms
     * 全部到齐时只有第 1 字节留住,后面 6 字节溢出丢失,永远拼不出
     * 完整命令,永远不回复。
     *
     * 修复: 取消 HAL_Delay, 主循环尽可能快地轮询接收。
     * 115200 baud 字节间隔 ~87µs,主循环跑得够快就能完整接收。
     */
    uart_poll();
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
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV1;
  RCC_OscInitStruct.PLL.PLLN = 42;
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
