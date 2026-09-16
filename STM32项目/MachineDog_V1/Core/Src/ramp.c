/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ramp.c
  * @brief   8 路 PWM 同步渐进 ramp (2026-09-16 动作组扩展)
  ******************************************************************************
  */
/* USER CODE END Header */

#include "ramp.h"
#include "main.h"
#include "stepping.h"   /* stepping_stop(), SERVO_STEP */

/* === 跟踪 8 路 PWM(供 ramp 起点采样)=============================
 * 写策略:任何经 motions.c 的 PWM 写入都同步更新这个数组
 *   - apply_stand / motion_apply_neutral / ramp_tick
 *   - set_pwm() 内统一入口
 *
 * 初值 = 8 路"标准值"(含 FL 肩 +100 / BL 小腿 +80 物理偏移)。
 * 实际上上电时 motion_init() 会立刻调 motion_apply_neutral() 同步一遍,
 * 这里只是 ramp 在任何写入之前就被启动时的兜底。
 */
static uint16_t g_pwm[8] = {
  SERVO_NEUTRAL_US,          SERVO_NEUTRAL_US,          /* 0 BR 小腿  / 1 BR 肩    */
  SERVO_NEUTRAL_US,          SERVO_NEUTRAL_US,          /* 2 FR 小腿  / 3 FR 肩    */
  SERVO_NEUTRAL_FL_SHOULDER, SERVO_NEUTRAL_US,          /* 4 FL 肩(+100) / 5 FL 小腿 */
  SERVO_NEUTRAL_US,          SERVO_NEUTRAL_BL_SHIN,     /* 6 BL 肩    / 7 BL 小腿(+80) */
};

/* 暴露给 main.c:set_servo_pulse 内调 motions_track_pwm */
void motions_track_pwm(uint8_t id, uint16_t pulse) {
  if (id < 8) g_pwm[id] = pulse;
}

/* motions.c 内部统一写入口 */
extern void set_servo_pulse(uint8_t id, uint16_t pulse);
static void set_pwm(uint8_t id, uint16_t pulse) {
  if (pulse < 500 || pulse > 2500) return;
  g_pwm[id] = pulse;
  set_servo_pulse(id, pulse);
}

/* === Ramp 状态机 ==================================================*/
typedef struct {
  uint16_t start_pwm[8];
  uint16_t target_pwm[8];
  uint32_t total_ms;
  uint32_t start_tick_ms;
  ramp_complete_cb_t on_complete;
  volatile uint8_t active;
} RampState;

static RampState g_ramp = { .active = 0 };

/* === Ramp API =====================================================*/

void ramp_sit_to_target(const uint16_t target_pwm[8], uint32_t total_ms,
                        ramp_complete_cb_t on_complete) {
  if (target_pwm == NULL || total_ms == 0) return;
  for (uint8_t i = 0; i < 8; i++) g_ramp.target_pwm[i] = target_pwm[i];
  /* 起点 = 当前 PWM(g_pwm) — 避免跳变 */
  for (uint8_t i = 0; i < 8; i++) g_ramp.start_pwm[i] = g_pwm[i];
  g_ramp.total_ms = total_ms;
  g_ramp.start_tick_ms = HAL_GetTick();
  g_ramp.on_complete = on_complete;
  g_ramp.active = 1;
  /* 立即写第一帧(elapsed=0 → 全 start_pwm) */
  for (uint8_t i = 0; i < 8; i++) set_pwm(i, g_ramp.start_pwm[i]);
}

void ramp_cancel(void) { g_ramp.active = 0; }

uint8_t ramp_is_active(void) { return g_ramp.active; }

void ramp_tick(void) {
  if (!g_ramp.active) return;

  uint32_t now = HAL_GetTick();
  uint32_t elapsed = now - g_ramp.start_tick_ms;

  if (elapsed >= g_ramp.total_ms) {
    /* 最后一帧:写 target(8 路同步) */
    for (uint8_t i = 0; i < 8; i++) set_pwm(i, g_ramp.target_pwm[i]);
    g_ramp.active = 0;
    if (g_ramp.on_complete) g_ramp.on_complete();
    return;
  }

  
  /* 线性插值 8 路 — 每帧所有 8 路都更新 */
  for (uint8_t i = 0; i < 8; i++) {
    int32_t span = (int32_t)g_ramp.target_pwm[i] - (int32_t)g_ramp.start_pwm[i];
    int32_t pwm  = (int32_t)g_ramp.start_pwm[i]
                 + (span * (int32_t)elapsed) / (int32_t)g_ramp.total_ms;
    /* 使用 SERVO_STEP 的安全范围 */
    uint16_t min_pwm = SERVO_STEP[i].min;
    uint16_t max_pwm = SERVO_STEP[i].max;
    if (pwm < (int32_t)min_pwm)  pwm = min_pwm;
    if (pwm > (int32_t)max_pwm)  pwm = max_pwm;
    set_pwm((uint8_t)i, (uint16_t)pwm);
  }
}