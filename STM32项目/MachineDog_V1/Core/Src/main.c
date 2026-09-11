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
#include "gait.h"
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
/* === 8 路舵机默认 PWM(2026-09-11)===
 * 用户调试用:从这里直接改,烧录后立即生效(配合 Clean+Build)
 * 方向约定(从舵机后方看):
 *   右腿顺时针 = PWM 减小
 *   右腿逆时针 = PWM 增大(但实际效果是腿伸直)
 *   左腿逆时针 = PWM 增大
 *   左腿顺时针 = PWM 减小
 * 默认全 1500(标定基线)
 */
/* === 站立姿态(用户实测调整,2026-09-11)===
 * 后腿肩部微调过,接近 4 脚同步落地,这是高度极限
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
/* 2026-09-11 清理:删除蹲下姿态(SERVO_*_SIT)、身高控制(SERVO_NEUTRAL_US 不再用于身高)。
 * 原因:对平衡性和舵机能力要求较高,蹲下起立动作偶尔卡死起不来,有风险,先放弃。
 * 后续要恢复:git log 找 "feat(stm32):站立/蹲下姿态" 提交(2026-09-11),恢复常量和函数即可。*/
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

/* 当前 8 路舵机 PWM(每次 set_servo_pulse 时更新,供 cal save 用) */
static uint16_t current_pwm[8];

static void set_servo_pulse(uint8_t id, uint16_t pulse) {
  if (pulse < 500 || pulse > 2500) return;
  current_pwm[id] = pulse;
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
 * 这是舵机标定的中立位,所有 STAND 偏移相对此计算。
 * (2026-09-11 清理:删除身高控制相关代码,SERVO_NEUTRAL_US 不再用于身高)
 */
#define SERVO_NEUTRAL_US   1500

/* UART 接收命令解析(2026-09-11 扩展 +cal +step):
 * 格式: "<servo_id> <pulse>\n"   例如 "0 1500\n"   -> 设 servo0=1500
 *       "all <pulse>\n"           设全部 8 路
 *       "center\n"                设全部 1500(居中,标定基线)
 *       "stand\n"                 站立姿态(用 stand_pwm 数组,可由 cal save 覆盖)
 *       "cal raw"                 8 路舵机设 1500(机械零位,开始标定)
 *       "cal save"                当前 8 路 PWM 保存为 STAND
 *       "cal show"                报告当前 STAND 数组
 *       "step trot"               启动 trot 踏步(原地抬腿落下,100Hz)
 *       "step stop"               停止踏步,回 STAND
 */
static char rx_buf[32];
static uint8_t rx_idx = 0;

static void parse_uart_command(const char *cmd) {
  unsigned int id = 0, pulse = 0;
  /* 用 strncmp 前置判别 "all" / "cal" / "step",避免 sscanf("%u %u") 误拦截 */
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
  } else if (strcmp(cmd, "cal raw") == 0) {
    /* 标定模式:8 路舵机设 1500 µs(机械零位)
     * 用户观察机械几何(目标:大腿垂直地面,小腿水平向前)
     * 然后用 <id> <pulse> 微调,最后 cal save 保存 */
    gait_cal_raw();
    printf("OK cal raw: 8 servos at 1500\n");
  } else if (strcmp(cmd, "cal save") == 0) {
    /* 把当前 8 路 PWM 保存为 STAND(运行时覆盖,无需重编译) */
    gait_cal_save_stand_array(current_pwm);
  } else if (strcmp(cmd, "cal show") == 0) {
    gait_cal_show_stand();
  } else if (strcmp(cmd, "step trot") == 0) {
    gait_start_trot();
  } else if (strcmp(cmd, "step stop") == 0) {
    gait_stop();
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
    /* 站立姿态:从 stand_pwm 数组读取(默认从 SERVO_*_STAND 初始化,可被 cal save 覆盖) */
    const uint16_t *sp = gait_get_stand_pwm();
    for (uint8_t i = 0; i < 8; i++) set_servo_pulse(i, sp[i]);
    printf("OK stand\n");
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

  /* === 上电默认姿态(2026-09-11 改)===
   * 默认: 从 stand_pwm 数组读取 STAND(初值 = SERVO_*_STAND 常量,可被 cal save 覆盖)
   * gait_init() 必须在 HAL_TIM_PWM_Start 之后调(否则 htim 还没启动)
   */
  gait_init();
  const uint16_t *sp = gait_get_stand_pwm();
  for (uint8_t i = 0; i < 8; i++) set_servo_pulse(i, sp[i]);
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
