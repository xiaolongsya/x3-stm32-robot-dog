/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stepping.h
  * @brief   机器狗 v1 原地踏步接口(2026-09-14 整理)
  *
  * 算法:对角 trot(2026-09-14 最新版)
  *   - 收腿模型:抬腿 = 收腿 = 右腿 PWM 减 + 左腿 PWM 增
  *   - 三角波 ramp(0→peak→0),shin + thigh 同步动作
  *   - swing 腿抬 offset,support 腿保持 TROT_STAND
  *   - ISR 不 printf(避免阻塞 UART)
  *
  * 调度:TIM6 100Hz 中断 → stepping_tick()
  *   CubeMX:Prescaler=16999,Period=99 (10kHz/100 = 100Hz)
  *
  * UART 命令(在 main.c parse_uart_command 注册):
  *   step trot    启动原地踏步(主循环)
  *   step stop    停止踏步,舵机回 STAND(主循环)
  *   step show    调试输出(目前空,接口保留)
  *
  * 腿编号约定(对角步态,与 SERVO_STEP 表对齐):
  *   腿1=FR (小腿=id 2, 肩=id 3)
  *   腿2=FL (小腿=id 5, 肩=id 4)
  *   腿3=BL (小腿=id 7, 肩=id 6)
  *   腿4=BR (小腿=id 0, 肩=id 1)
  *
  *   trot:phase < 0.5 腿 1+3 swing,腿 2+4 support(对角交替)
  *
  * STAND 含义:
  *   - 1500 = 舵机中位 = 大腿垂直 + 小腿水平(几何最高)
  *   - STAND = 4 脚贴地的实测姿态,作步态参考基线
  *
  * 收腿定义:
  *   - 抬腿本质 = 收腿 = 身体降低方向
  *   - 大腿后旋 + 小腿后旋(脚相对身体往上,身体不动或微沉)
  *   - 右腿 PWM 减,左腿 PWM 增(左右舵机反向安装)
  *
  * 详细参数(在 stepping.c 里):
  *   - STEP_TROT_OFFSET,STEP_TROT_PERIOD,STEP_RATIO_SHIN_TO_THIGH_X10
  *   - SERVO_STEP 表(8 路 STAND + SERVO_LIMIT)
  *   - TROT_STAND 表(对角踏步用,中立位)
  *
  * 安全:
  *   - 第一帧(phase=0)自动是 TROT_STAND(无跳变)
  *   - SERVO_LIMIT clamp 防止越界
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef STEPPING_H
#define STEPPING_H

#include <stdint.h>

/* === 步态状态机 =====================================================*/
typedef enum {
  STEPPING_IDLE = 0,     /* 待机,所有腿 STAND */
  STEPPING_TROT,         /* 对角小跑步态(原地踏步) */
} SteppingState;

/* === 接口 ===========================================================*/

/**
 * @brief  初始化(在 HAL_TIM_PWM_Start 之后调)
 *         不启动 TIM6,不应用 STAND,需要 stepping_start_trot 才开中断
 */
void stepping_init(void);

/**
 * @brief  启动原地踏步(主循环调用)
 *         - 应用 TROT_STAND(中立位),启动 TIM6 100Hz 中断
 *         - phase=0,swing 腿无偏移,support 腿 STAND(无跳变)
 */
void stepping_start_trot(void);

/**
 * @brief  停止踏步(主循环调用)
 *         关闭 TIM6,舵机回 STAND(前倾原始标定)
 */
void stepping_stop(void);

/**
 * @brief  TIM6 100Hz 中断调用 — 推进相位 + 算 8 路 PWM
 *         ⚠️ ISR 上下文,不能 printf
 *         ⚠️ 不要在主循环手动调
 */
void stepping_tick(void);

/**
 * @brief  查询状态
 */
SteppingState stepping_get_state(void);

/**
 * @brief  调试输出(主循环调用,可 printf)
 *         当前空实现,接口保留
 */
void stepping_show(void);

#endif /* STEPPING_H */
