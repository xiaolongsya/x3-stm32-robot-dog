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
static void set_servo_pulse(uint8_t id, uint16_t pulse) {
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

/* === 身高控制(2026-09-10 新增)==========================================
 * 几何(用户实测 + 远场近似):
 *   L_shin = 68 mm,  R_shin = 21.22 mm
 *   L_shin / R_shin = 3.2 (身体高度变化 / 传动杆端位移)
 *   θ_servo(小腿舵机,逆时针从狗前面看)= 身体抬升对应方向
 *   PWM = 500 + (deg/180)*2000
 *
 * 简化公式(远场近似):
 *   Δh ≈ (L_shin / R_shin) × ds ≈ 3.2 × ds (mm)
 *   ds ≈ R_servo × sin θ_s ≈ 50.14 × sin θ_s
 *   => Δh ≈ 3.2 × 50.14 × sin θ_s ≈ 160 × sin θ_s (mm)
 *   => sin θ_s ≈ Δh / 160
 *
 *   1 mm 身体抬升 => sin θ_s ≈ 0.00625 => θ_s ≈ 0.358° (≈ 4 µs PWM 增量)
 *   所以 1 mm 身体抬升 ≈ 4 µs PWM 增量
 *
 * ⚠️ 这是远场近似,实测后需要重新校准 HEIGHT_US_PER_MM 常量。
 */
#define SERVO_NEUTRAL_US     1580   /* 默认站起来一点 ~20 mm(原 1500 = 标定基线)*/
#define SHIN_SERVO_COUNT     4
#define HEIGHT_US_PER_MM     4    /* 1 mm 身体抬升 ≈ 4 µs PWM 增量(待实测) */
#define HEIGHT_DELTA_MAX_MM  40   /* 安全上限 ±40 mm */

/* 4 路小腿舵机的 servo 编号: 0=BR, 2=FR, 5=FL, 7=BL */
static const uint8_t SHIN_SERVO_IDS[SHIN_SERVO_COUNT] = {0, 2, 5, 7};
static uint16_t shin_pwm_state[SHIN_SERVO_COUNT] = {1500, 1500, 1500, 1500};

/* 设置 4 路小腿舵机同步(只动小腿,肩部舵机不动) */
static void set_all_shin_pwm(uint16_t pwm) {
  for (uint8_t i = 0; i < SHIN_SERVO_COUNT; i++) {
    set_servo_pulse(SHIN_SERVO_IDS[i], pwm);
    shin_pwm_state[i] = pwm;
  }
}

/* 应用身体高度增量(相对默认 SERVO_NEUTRAL_US)
 * delta_mm: 正数 = 抬升, 负数 = 下降
 * 安全检查: PWM 范围 500~2500, 高度限制 ±HEIGHT_DELTA_MAX_MM
 */
static void apply_height_delta(int16_t delta_mm) {
  /* 安全范围检查 */
  if (delta_mm > HEIGHT_DELTA_MAX_MM) delta_mm = HEIGHT_DELTA_MAX_MM;
  if (delta_mm < -HEIGHT_DELTA_MAX_MM) delta_mm = -HEIGHT_DELTA_MAX_MM;
  int32_t pwm = (int32_t)SERVO_NEUTRAL_US + (int32_t)delta_mm * HEIGHT_US_PER_MM;
  if (pwm < 500) pwm = 500;
  if (pwm > 2500) pwm = 2500;
  set_all_shin_pwm((uint16_t)pwm);
  printf("OK dh=%dmm -> shin_pwm=%lu (servo0/2/5/7)\n",
         delta_mm, (unsigned long)pwm);
}

/* UART 接收命令解析:
 * 格式: "<servo_id> <pulse>\n"  例如 "0 1500\n" -> 设 servo0=1500
 *       "all <pulse>\n"          设全部 8 路(保留旧命令)
 *       "center\n"               设全部 1500 (居中,保留旧命令)
 *       "h <delta_mm>\n"         设身体高度增量(新增,2026-09-10)
 *                                 例: "h 15\n"  -> 抬升 15 mm
 *                                     "h -10\n" -> 下降 10 mm
 */
static char rx_buf[32];
static uint8_t rx_idx = 0;

static void parse_uart_command(const char *cmd) {
  unsigned int id = 0, pulse = 0;
  int16_t delta = 0;
  /* === 修复 1:用 strncmp 前置判别 "all" 和 "h"
   * 原 bug: sscanf("%u %u") 遇到 "all" 返回 0 但不消耗,
   *         导致 "all 1500" 走 else 分支全失败 → ERR fmt
   */
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
  } else if (cmd[0] == 'h' && cmd[1] == ' ') {
    /* === 修复 2:h 命令前置判别,避免被 sscanf("%u %u") 拦截 === */
    if (sscanf(cmd + 2, "%hd", &delta) == 1) {
      apply_height_delta(delta);
    } else {
      printf("ERR fmt\n");
    }
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
  } else {
    /* === 修复 3:加回显便于调试 === */
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
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_TIM1_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_TIM17_Init();
  MX_USART1_UART_Init();

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

  /* 默认姿态: 8 路全部 1580 µs(抬升 ~20 mm,基于 1mm = 4µs 远场近似)
     = 身体从标定基线提高一点,验证烧录是否生效(用户调试用)*/
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 1580);
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 1580);
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, 1580);
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_4, 1580);
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, 1580);
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 1580);
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, 1580);
  __HAL_TIM_SET_COMPARE(&htim17, TIM_CHANNEL_1, 1580);
  /* USER CODE END 2 */

  while (1)
  {
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
  * @brief System Clock Configuration - HSE 8MHz / PLL x42 / SYSCLK 168MHz
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV1;
  RCC_OscInitStruct.PLL.PLLN = 42;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) { Error_Handler(); }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK) { Error_Handler(); }
}

/* USER CODE BEGIN 4 */
/* USER CODE END 4 */

void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}
#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif