/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stepping.h
  * @brief   机器狗 v1 原地踏步接口(2026-09-12 重写)
  *
  * 设计:远场近似 1mm ≈ 4µs + 收腿模型(2026-09-12 用户拍板)
  *   - 完全抛弃 PA-apple IK + py-apple swing 曲线
  *   - 抬腿 = 收腿 = 右腿 PWM 减 + 左腿 PWM 增
  *   - 抬腿曲线:sin²(π × phase/0.5),边界连续
  *   - ISR 内不 printf(避免阻塞 UART,修 MEDIUM 风险)
  *
  * 参数(2026-09-12 拍板):
  *   - H_LIFT     = 5 mm
  *   - PWM_PER_MM = 4
  *   - T          = 2.0 s(周期)
  *   - 半周期     = 1.0 s(一只 swing 周期)
  *
  * 调度:TIM6 100Hz 中断 → stepping_tick()
  *   CubeMX:Prescaler=16999,Period=99 (10kHz/100 = 100Hz)
  *
  * UART 命令(兼容旧接口,在 main.c parse_uart_command 注册):
  *   step trot    启动原地踏步(主循环)
  *   step stop    停止踏步,舵机回 STAND(主循环)
  *   step show    打印 phase + h + 8 路 PWM(主循环)
  *
  * 腿编号约定(对角步态,与 SERVO_STEP 表对齐):
  *   腿1=FR (小腿=id 2, 肩=id 3)
  *   腿2=FL (小腿=id 5, 肩=id 4)
  *   腿3=BL (小腿=id 7, 肩=id 6)
  *   腿4=BR (小腿=id 0, 肩=id 1)
  *
  *   trot:phase < 0.5 腿 1+3 swing,腿 2+4 support(对角交替)
  *
  * STAND 物理含义(2026-09-12 用户拍板):
  *   - 1500 = 舵机中位 = 大腿垂直 + 小腿水平(几何最高)
  *   - STAND = 4 脚贴地的实测姿态,作为步态参考基线
  *
  * 收腿定义(2026-09-12 用户拍板):
  *   - 抬腿本质 = 收腿 = 身体降低方向
  *   - 大腿后旋 + 小腿后旋(脚相对身体往上,身体不动或微沉)
  *   - 右腿 PWM 减,左腿 PWM 增(左右舵机反向安装)
  *
  * 安全:
  *   - 第一帧(phase=0)自动是 STAND(无跳变)
  *   - SERVO_LIMIT clamp 防止越界
  *   - ISR 内不 printf(避免阻塞 UART)
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef STEPPING_H
#define STEPPING_H

#include <stdint.h>

/* === 步态状态机 =====================================================*/
typedef enum {
  STEPPING_IDLE = 0,     /* 待机,所有腿 STAND */
  STEPPING_TROT,         /* 对角小跑步态(原地踏步,x_target=0) */
} SteppingState;

/* === 接口 ===========================================================*/

/**
 * @brief  初始化(在 HAL_TIM_PWM_Start 之后调)
 *         不启动 TIM6,不应用 STAND,需要 stepping_start_trot 才开中断
 */
void stepping_init(void);

/**
 * @brief  启动原地踏步(主循环调用,可 printf)
 *         - 应用 STAND,启动 TIM6 100Hz 中断
 *         - phase=0,ds_pwm=0,第一帧是 STAND
 */
void stepping_start_trot(void);

/**
 * @brief  停止踏步,关闭 TIM6,舵机回 STAND(主循环调用,可 printf)
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
 * @brief  调试:打印 phase + h + 8 路 PWM 到 UART(主循环调用,可 printf)
 */
void stepping_show(void);

#endif /* STEPPING_H */
