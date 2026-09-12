/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stepping.c
  * @brief   机器狗 v1 原地踏步(2026-09-12 重写,基于远场近似 + 收腿模型)
  *
  * 设计:远场近似 1mm ≈ 4µs + 收腿模型(2026-09-12 用户拍板)
  *   - 完全抛弃 PA-apple IK + py-apple swing 曲线
  *   - 抬腿 = 收腿 = 右腿 PWM 减 + 左腿 PWM 增
  *   - 抬腿曲线:sin²(π × phase/0.5),边界连续无跳变
  *   - ISR 内不 printf(避免阻塞 UART,与 Pi no-reply bug 同源问题)
  *
  * 参数(2026-09-12 用户拍板):
  *   - H_LIFT     = 5 mm   (保守起步)
  *   - T          = 2.0 s  (100Hz × T_INC=0.005)
  *   - PWM_PER_MM = 4      (远场近似实测 1mm ≈ 4µs)
  *
  * UART 命令(兼容旧接口,在 main.c parse_uart_command 注册):
  *   step trot    启动原地踏步(主循环调用,可 printf)
  *   step stop    停止踏步,回 STAND(主循环调用,可 printf)
  *   step show    打印当前 phase + h + 8 路 PWM(主循环调用,可 printf)
  *
  * 腿编号约定(对角 trot):
  *   腿 1 = FR (小腿=id 2, 肩=id 3)
  *   腿 2 = FL (小腿=id 5, 肩=id 4)
  *   腿 3 = BL (小腿=id 7, 肩=id 6)
  *   腿 4 = BR (小腿=id 0, 肩=id 1)
  *
  *   phase ∈ [0, 0.5):腿 1+3 swing,腿 2+4 support
  *   phase ∈ [0.5, 1.0):腿 2+4 swing,腿 1+3 support
  *
  * 收腿定义(2026-09-12 用户拍板):
  *   - 抬腿本质 = 收腿 = 身体降低方向
  *   - 大腿后旋 + 小腿后旋(脚相对身体往上,身体不动或微沉)
  *   - 右腿 PWM 减,左腿 PWM 增(左右舵机反向安装)
  *
  * 安全:
  *   - 第一帧(phase=0)自动是 STAND(sin²(0)=0,ds_pwm=0)
  *   - SERVO_LIMIT clamp 防止越界
  *   - ISR 内不 printf,避免阻塞 UART
  *
  * 多 agent 审查(2026-09-12):
  *   - 8 路 PWM 全程在 SERVO_LIMIT 内 ✓
  *   - 启动无跳变(phase=0 → STAND)✓
  *   - PWM 变化率峰值 62.8µs/秒(SG90 slew rate 跟得上)✓
  *   - ISR 不 printf 修 MEDIUM 风险 ✓
  ******************************************************************************
  */
/* USER CODE END Header */

#include "stepping.h"
#include "main.h"
#include <math.h>
#include <stdio.h>

/* === 参数(2026-09-12 用户拍板)====================================*/
#define STEP_H_LIFT_MM    5.0f    /* 抬腿高度 (mm) */
#define STEP_PWM_PER_MM   4.0f    /* 远场近似 1mm ≈ 4µs PWM 调整 */
#define STEP_T_INC        0.005f  /* 每 tick 相位增量,周期 2.0s @ 100Hz */
#define STEP_TF           0.5f    /* 半周期 (对角 trot) */

/* === 8 路舵机收腿参数表 ==========================================
 *
 * 收腿方向:右腿 PWM 减,左腿 PWM 增(基于"抬腿 = 收腿 = 身体降低")
 *
 *   id  名称      STAND   is_right  SERVO_LIMIT(min,max)
 *   0   BR 小腿   1600    right     (1400, 1600)  ⚠ STAND=MAX
 *   1   BR 肩     1150    right     (1000, 2000)
 *   2   FR 小腿   1500    right     (1400, 1600)
 *   3   FR 肩     1200    right     (1000, 2000)
 *   4   FL 肩     1820    left      (1000, 2000)
 *   5   FL 小腿   1500    left      (1400, 1600)
 *   6   BL 肩     1850    left      (1000, 2000)
 *   7   BL 小腿   1400    left      (1400, 1600)  ⚠ STAND=MIN
 *
 * ⚠ BR/BL 小腿 STAND 抵 SERVO_LIMIT 边界,但收腿方向都还有 180µs 余量
 *   (装配公差导致,不动硬约束)
 */
typedef struct {
  uint16_t stand;      /* STAND PWM (从 SERVO_*_STAND 常量) */
  uint8_t  is_right;   /* 1=右腿 (PWM 减 = 收腿),0=左腿 (PWM 增 = 收腿) */
  uint16_t min;        /* SERVO_LIMIT 下限 */
  uint16_t max;        /* SERVO_LIMIT 上限 */
} ServoStep;

static const ServoStep SERVO_STEP[8] = {
  /*0  BR 小腿 */ {SERVO_SHIN_BR_STAND,      1, 1400, 1600},
  /*1  BR 肩   */ {SERVO_SHOULDER_BR_STAND,  1, 1000, 2000},
  /*2  FR 小腿 */ {SERVO_SHIN_FR_STAND,      1, 1400, 1600},
  /*3  FR 肩   */ {SERVO_SHOULDER_FR_STAND,  1, 1000, 2000},
  /*4  FL 肩   */ {SERVO_SHOULDER_FL_STAND,  0, 1000, 2000},
  /*5  FL 小腿 */ {SERVO_SHIN_FL_STAND,      0, 1400, 1600},
  /*6  BL 肩   */ {SERVO_SHOULDER_BL_STAND,  0, 1000, 2000},
  /*7  BL 小腿 */ {SERVO_SHIN_BL_STAND,      0, 1400, 1600},
};

/* === 状态 =========================================================*/
static volatile SteppingState step_state = STEPPING_IDLE;
static volatile float step_t_phase = 0.0f;

/* === 调试数据 (ISR 写, main loop 读) ==============================*/
static volatile float     dbg_phase = 0.0f;
static volatile float     dbg_h_mm  = 0.0f;
static volatile uint16_t  dbg_pwm[8] = {0};

/* === 工具函数 =====================================================*/

/* clamp PWM 到 SERVO_LIMIT 安全范围 */
static inline uint16_t stepping_clamp_pwm(uint8_t id, int16_t pwm) {
  if (pwm < (int16_t)SERVO_STEP[id].min) return SERVO_STEP[id].min;
  if (pwm > (int16_t)SERVO_STEP[id].max) return SERVO_STEP[id].max;
  return (uint16_t)pwm;
}

/* === 应用 STAND 到 8 路舵机 =======================================*/
static void stepping_apply_stand(void) {
  for (uint8_t i = 0; i < 8; i++) {
    set_servo_pulse(i, SERVO_STEP[i].stand);
  }
}

/* === 单步执行:推进相位 + 算 8 路 PWM ==============================
 *
 * ⚠️ TIM6 ISR 调用 — 不能 printf,只 set_servo_pulse + 写 dbg
 *
 * 算法:
 *   phase < 0.5:腿 1+3 swing (FR id 2,3 + BL id 6,7),腿 2+4 support
 *   phase ≥ 0.5:腿 2+4 swing (FL id 4,5 + BR id 0,1),腿 1+3 support
 *
 *   swing 腿:h = H_LIFT × sin²(π × phase_in_swing)
 *             ds_pwm = h × PWM_PER_MM  ∈ [0, 20] µs
 *             右腿: pwm = STAND - ds_pwm
 *             左腿: pwm = STAND + ds_pwm
 *   support 腿: pwm = STAND(不动)
 *
 *   第一帧(phase=0): sin²(0)=0 → ds_pwm=0 → 全 STAND(无跳变)
 */
static void stepping_trot_step(void) {
  /* 当前摆动腿的 phase_in_swing (归一到 [0,1)) 和 id 位掩码 */
  float phase_in_swing;
  uint8_t swing_mask;

  if (step_t_phase < STEP_TF) {
    /* 腿 1+3 swing: FR(2,3) + BL(6,7) */
    phase_in_swing = step_t_phase * 2.0f;
    swing_mask = (1u << 2) | (1u << 3) | (1u << 6) | (1u << 7);
  } else {
    /* 腿 2+4 swing: FL(4,5) + BR(0,1) */
    phase_in_swing = (step_t_phase - STEP_TF) * 2.0f;
    swing_mask = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5);
  }

  /* sin² 抬腿曲线:边界连续,峰值 1,首末导数为 0(无 jerk) */
  float sin_val = sinf((float)M_PI * phase_in_swing);
  float h_mm = STEP_H_LIFT_MM * sin_val * sin_val;
  int16_t ds_pwm = (int16_t)(h_mm * STEP_PWM_PER_MM + 0.5f);  /* 四舍五入 → [0, 20] */

  /* 应用 8 路 PWM + 缓存 dbg */
  for (uint8_t id = 0; id < 8; id++) {
    int16_t pwm;

    if (swing_mask & (1u << id)) {
      /* 摆动腿:右减 / 左增(收腿) */
      if (SERVO_STEP[id].is_right) {
        pwm = (int16_t)SERVO_STEP[id].stand - ds_pwm;
      } else {
        pwm = (int16_t)SERVO_STEP[id].stand + ds_pwm;
      }
    } else {
      /* 支撑腿:不动 */
      pwm = (int16_t)SERVO_STEP[id].stand;
    }

    uint16_t clamped = stepping_clamp_pwm(id, pwm);
    set_servo_pulse(id, clamped);
    dbg_pwm[id] = clamped;
  }

  dbg_phase = step_t_phase;
  dbg_h_mm  = h_mm;
}

/* === 接口实现 =====================================================*/

void stepping_init(void) {
  /* 在 HAL_TIM_PWM_Start 之后调;不启动 TIM6,需 stepping_start_trot 才开 */
  step_state = STEPPING_IDLE;
  step_t_phase = 0.0f;
  /* 不应用 STAND,由 main() 初始化时直接写 STAND (沿用旧模式) */
}

void stepping_start_trot(void) {
  /* ⚠️ 主循环调用 — 可 printf */
  stepping_apply_stand();
  step_state = STEPPING_TROT;
  step_t_phase = 0.0f;

  if (HAL_TIM_Base_Start_IT(&htim6) != HAL_OK) {
    printf("ERR stepping: TIM6 start failed\n");
    step_state = STEPPING_IDLE;
    return;
  }
  printf("OK trot started (T=2.0s, h=5mm)\n");
}

void stepping_stop(void) {
  /* ⚠️ 主循环调用 — 可 printf */
  HAL_TIM_Base_Stop_IT(&htim6);
  step_state = STEPPING_IDLE;
  step_t_phase = 0.0f;
  stepping_apply_stand();
  printf("OK trot stopped, returned to STAND\n");
}

void stepping_tick(void) {
  /* ⚠️ TIM6 ISR 调用 — 不能 printf */
  if (step_state != STEPPING_TROT) return;

  step_t_phase += STEP_T_INC;
  if (step_t_phase >= 1.0f) step_t_phase -= 1.0f;

  stepping_trot_step();
}

SteppingState stepping_get_state(void) {
  return step_state;
}

void stepping_show(void) {
  /* ⚠️ 主循环调用 — 可 printf (给 USB 串口观察用) */
  printf("phase=%.3f h=%.2fmm pwm=[%u,%u,%u,%u,%u,%u,%u,%u]\n",
         dbg_phase, dbg_h_mm,
         dbg_pwm[0], dbg_pwm[1], dbg_pwm[2], dbg_pwm[3],
         dbg_pwm[4], dbg_pwm[5], dbg_pwm[6], dbg_pwm[7]);
}
