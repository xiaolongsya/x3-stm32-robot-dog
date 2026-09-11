/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stepping.c
  * @brief   py-apple-dynamics V7.3 PA_GAIT.trot + IK + servo_output 移植
  *          适配 STM32G431KBT6(2026-09-11)
  *
  * 移植模块对应(从 py-apple-dynamics V7.3 SRC):
  *   PA_GAIT.trot          → stepping_trot_step() 的对角调度
  *   padog.swing_curve_generate  → stepping_swing_curve()
  *   padog.support_curve_generate → stepping_support_curve()
  *   PA_IK.ik case=0       → stepping_ik_case0()
  *   PA_ATTITUDE.cal_ges   → 内联简化(PIT=ROL=X=0 → 仅传 -Hc 偏移)
  *   padog.servo_output    → 内联(用实测 init_*,不用 py-apple 的 ±90)
  *
  * 与旧 gait.c 的关键差异(2026-09-11 重写):
  *   1. init_* 直接从 STAND_PWM 反推(SERVO_*_STAND 常量)
  *   2. 镜像通过符号翻转(腿1/4 vs 腿2/3),不用 py-apple 的 ±90 偏移
  *   3. 加 SERVO_LIMIT clamp,防止 IK 偶发偏差打到机械极限
  ******************************************************************************
  */
/* USER CODE END Header */

#include "stepping.h"
#include "main.h"
#include <math.h>
#include <stdio.h>

/* === 8 路舵机 init_* 角度(从 STAND_PWM 反推)============================
 * 计算公式:init_deg = (STAND_PWM - 500) * 180 / 2000
 * 这些角度是 STAND 时(ham=0, shank=0)舵机的目标角度
 * 对应 SERVO_*_STAND 常量在 main.h
 */
#define INIT_DEG_FROM_PWM(pwm)  ((float)((pwm) - 500) * (180.0f / 2000.0f))

/* 腿1 = FR:大腿=servo3, 小腿=servo2 */
static const float init_1h = INIT_DEG_FROM_PWM(SERVO_SHOULDER_FR_STAND);  /* FR 肩 = 1200 → 63° */
static const float init_1s = INIT_DEG_FROM_PWM(SERVO_SHIN_FR_STAND);      /* FR 小腿 = 1500 → 90° */

/* 腿2 = FL:大腿=servo4, 小腿=servo5 */
static const float init_2h = INIT_DEG_FROM_PWM(SERVO_SHOULDER_FL_STAND);  /* FL 肩 = 1820 → 118.8° */
static const float init_2s = INIT_DEG_FROM_PWM(SERVO_SHIN_FL_STAND);      /* FL 小腿 = 1500 → 90° */

/* 腿3 = BL:大腿=servo6, 小腿=servo7 */
static const float init_3h = INIT_DEG_FROM_PWM(SERVO_SHOULDER_BL_STAND);  /* BL 肩 = 1850 → 121.5° */
static const float init_3s = INIT_DEG_FROM_PWM(SERVO_SHIN_BL_STAND);      /* BL 小腿 = 1400 → 72° */

/* 腿4 = BR:大腿=servo1, 小腿=servo0 */
static const float init_4h = INIT_DEG_FROM_PWM(SERVO_SHOULDER_BR_STAND);  /* BR 肩 = 1150 → 58.5° */
static const float init_4s = INIT_DEG_FROM_PWM(SERVO_SHIN_BR_STAND);      /* BR 小腿 = 1600 → 99° */

/* === 8 路舵机安全限位(踏步过程)===========================================
 * 索引按 servo_id 0-7:BR小腿/BR肩/FR小腿/FR肩/FL肩/FL小腿/BL肩/BL小腿
 * MIN/MAX 是 IK 输出后必须 clamp 的范围(机械外再留 ±100µs 余量)
 *
 * 推导依据:
 *   - h_lift=10mm IK 输出(shin 变化 ≈ 12.3°,thigh 变化 3~8°)
 *   - shin STAND ±180µs ≈ ±16° → 覆盖抬腿 + 安全余量
 *   - thigh STAND ±300µs ≈ ±27° → 覆盖抬腿 + 安全余量
 *   - 单边限制(FL thigh STAND=1820,不能向低超过 1500;FR thigh=1200 不能向高超过 1500)
 */
typedef struct {
  uint16_t stand;
  uint16_t min;
  uint16_t max;
} ServoLimit;

static const ServoLimit SERVO_LIMIT[8] = {
  /*0  BR 小腿 */ {SERVO_SHIN_BR_STAND,       1420, 1780},
  /*1  BR 肩   */ {SERVO_SHOULDER_BR_STAND,   900,  1450},
  /*2  FR 小腿 */ {SERVO_SHIN_FR_STAND,       1320, 1680},
  /*3  FR 肩   */ {SERVO_SHOULDER_FR_STAND,   900,  1500},
  /*4  FL 肩   */ {SERVO_SHOULDER_FL_STAND,   1520, 2100},
  /*5  FL 小腿 */ {SERVO_SHIN_FL_STAND,       1320, 1680},
  /*6  BL 肩   */ {SERVO_SHOULDER_BL_STAND,   1550, 2100},
  /*7  BL 小腿 */ {SERVO_SHIN_BL_STAND,       1220, 1580},
};

/* === 步态参数 =============================================================
 * l1=80, l2=69 (与 config_s.py 一致,CLAUDE.md 已有)
 * Hc=110 (py-apple 默认身高,我们机械可能不同 — 待实测校正)
 * Tf=0.5 (摆动半周期归一化)
 * T_STEP=0.01 (100Hz × 0.01 = 1.0 周期 = 1 秒,10Hz 步态)
 * h_lift=10mm (用户拍板:保守起步)
 *
 * IK STAND 参考点(脚在 (±l/2, -Hc),即 py-apple cal_ges 默认站立)的输出:
 *   前腿(FR/FL, x_in=+l/2=+71): ham_std ≈ 96.7°, shank_std ≈ 57.1°
 *   后腿(BL/BR, x_in=-l/2=-71): ham_std ≈ 30.8°, shank_std ≈ 57.1°
 *   推导:代入 PA_IK.ik case=0(2026-09-11)
 *
 *   ⚠️ PA_IK.ik 在 x>0 vs x<0 走不同分支 → ham 输出不对称
 *      公式用每腿的 ham_std 作基准即可对齐 STAND
 */
#define STEP_L1          80.0f
#define STEP_L2          69.0f
#define STEP_HC          110.0f   /* 髋到脚垂直距离(mm) */
#define STEP_TF          0.5f     /* trot 摆动半周期 */
#define STEP_T_INC       0.01f    /* 每 tick 相位增量 */
#define STEP_H_LIFT      10.0f    /* 抬腿高度(mm),用户拍板 2026-09-11 */
#define STEP_HAM_STD_FRONT  96.7f /* 前腿 STAND IK ham 输出(FR/FL) */
#define STEP_HAM_STD_BACK   30.8f /* 后腿 STAND IK ham 输出(BL/BR) */
#define STEP_SHANK_STD      57.1f /* 所有腿 STAND IK shank 输出 */

/* === 状态 ===============================================================*/
static volatile SteppingState step_state = STEPPING_IDLE;
static volatile float step_t_phase = 0.0f;

/* === 调试:最近一次算的 ham/shank/show ==================================*/
static float dbg_ham[4]  = {0};
static float dbg_shank[4] = {0};
static uint16_t dbg_pwm[8] = {0};

/* === 工具函数 ===========================================================*/

/* 钳制 acos 输入到 [-1, 1](避免 acos 域外 NaN) */
static inline float clamp_cos(float v) {
  if (v >  1.0f) return  1.0f;
  if (v < -1.0f) return -1.0f;
  return v;
}

/* 角度 (度) → PWM (µs),clamp 到 [500, 2500] */
static inline uint16_t angle_to_pwm(float angle_deg) {
  float pwm = 500.0f + angle_deg * (2000.0f / 180.0f);
  if (pwm < 500.0f)  pwm = 500.0f;
  if (pwm > 2500.0f) pwm = 2500.0f;
  return (uint16_t)pwm;
}

/* 应用 SERVO_LIMIT 表 clamp 到安全范围 */
static inline uint16_t clamp_servo_pwm(uint8_t id, uint16_t pwm) {
  if (pwm < SERVO_LIMIT[id].min) return SERVO_LIMIT[id].min;
  if (pwm > SERVO_LIMIT[id].max) return SERVO_LIMIT[id].max;
  return pwm;
}

/* === 摆动相足端轨迹(移植自 padog.swing_curve_generate)===================
 *  t ∈ [0, Tf]:X 匀加速 → 三次 Hermite → 提前到顶
 *               Z 立方上升 → 二次下降
 *  xt=X 目标位移(原地踏步=0),zh=抬腿高度,x0/z0=起点,xv0=起步速度
 */
static void stepping_swing_curve(float t, float Tf, float xt, float zh,
                                 float x0, float z0, float xv0,
                                 float *xf_out, float *zf_out) {
  float xf = 0.0f, zf = 0.0f;
  float Tf2 = Tf * Tf, Tf4 = Tf2 * Tf2;

  if (t >= 0.0f && t < Tf / 4.0f) {
    xf = (-4.0f * xv0 / Tf) * t * t + xv0 * t + x0;
  } else if (t >= Tf / 4.0f && t < 3.0f * Tf / 4.0f) {
    xf = ((-4.0f * Tf * xv0 - 16.0f * xt + 16.0f * x0) * t * t * t) / (Tf * Tf2)
       + (( 7.0f * Tf * xv0 + 24.0f * xt - 24.0f * x0) * t * t) / Tf2
       + ((-15.0f * Tf * xv0 - 36.0f * xt + 36.0f * x0) * t) / (4.0f * Tf)
       + ( 9.0f * Tf * xv0 + 16.0f * xt) / 16.0f;
  } else {
    xf = xt;
  }

  if (t >= 0.0f && t < Tf / 2.0f) {
    zf = (16.0f * z0 - 16.0f * zh) * t * t * t / Tf4
       + (12.0f * zh - 12.0f * z0) * t * t / Tf2 + z0;
  } else {
    zf = (4.0f * z0 - 4.0f * zh) * t * t / Tf2
       - (4.0f * z0 - 4.0f * zh) * t / Tf + z0;
  }
  if (zf <= 0.0f) zf = 0.0f;  /* 防穿地 */

  *xf_out = xf;
  *zf_out = zf;
}

/* === 支撑相(移植自 padog.support_curve_generate)=======================
 *  X 线性回退,Z 常数
 */
static void stepping_support_curve(float t, float Tf, float x_past,
                                   float t_past, float zf,
                                   float *xf_out, float *zf_out) {
  float average = x_past / (1.0f - Tf);
  float xf = x_past - average * (t - t_past);
  *xf_out = xf;
  *zf_out = zf;
}

/* === IK case=0(移植自 PA_IK.ik,4 腿展开)===============================
 *  串联腿反运动学,余弦定理
 *  输入:腿 i 髋坐标系 (x_i, y_i),腿长 l1/l2
 *  输出:ham_i(大腿角,度),shank_i(小腿角,度)
 *  注意:PA_IK 中 x 取反,我们在函数内做
 */
static void stepping_ik_case0(float l1, float l2,
                              float x_in, float y_in,
                              float *ham_out, float *shank_out) {
  float x = -x_in;  /* py-apple IK 内部约定 */

  /* shank = π - acos((x² + y² - l1² - l2²) / (-2·l1·l2)) */
  float cos_shank = (x * x + y_in * y_in - l1 * l1 - l2 * l2) / (-2.0f * l1 * l2);
  float shank_rad = (float)M_PI - acosf(clamp_cos(cos_shank));

  /* fai = acos((l1² + x² + y² - l2²) / (2·l1·√(x²+y²))) */
  float r = sqrtf(x * x + y_in * y_in);
  float cos_fai = (l1 * l1 + x * x + y_in * y_in - l2 * l2) / (2.0f * l1 * r);
  float fai = acosf(clamp_cos(cos_fai));

  float ham_rad;
  if (x > 0.0f) {
    ham_rad = fabsf(atanf(y_in / x)) - fai;
  } else if (x < 0.0f) {
    ham_rad = (float)M_PI - fabsf(atanf(y_in / x)) - fai;
  } else {
    ham_rad = (float)M_PI - 1.5707f - fai;
  }

  *shank_out = 180.0f * shank_rad / (float)M_PI;
  *ham_out   = 180.0f * ham_rad   / (float)M_PI;
}

/* === 单步执行:推进相位 + 算 4 腿 IK + 输出 8 路 PWM ====================*/
static void stepping_trot_step(void) {
  float xs, zs, xp, zp;
  float x1, x2, x3, x4, y1, y2, y3, y4;
  const float Hc = STEP_HC;

  /* PA_GAIT.trot 调度(原地踏步,x_target=0) */
  if (step_t_phase < STEP_TF) {
    /* 腿 1+3 swing,腿 2+4 support */
    stepping_swing_curve(step_t_phase, STEP_TF, 0.0f, STEP_H_LIFT,
                         0.0f, 0.0f, 0.0f, &xs, &zs);
    stepping_support_curve(STEP_TF + step_t_phase, STEP_TF,
                           0.0f, STEP_TF, 0.0f, &xp, &zp);
    x1 = xs; x3 = xs; x2 = xp; x4 = xp;
    y1 = zs; y3 = zs; y2 = zp; y4 = zp;
  } else {
    /* 腿 2+4 swing,腿 1+3 support */
    stepping_swing_curve(step_t_phase - STEP_TF, STEP_TF, 0.0f, STEP_H_LIFT,
                         0.0f, 0.0f, 0.0f, &xs, &zs);
    stepping_support_curve(step_t_phase, STEP_TF,
                           0.0f, STEP_TF, 0.0f, &xp, &zp);
    x1 = xp; x3 = xp; x2 = xs; x4 = xs;
    y1 = zp; y3 = zp; y2 = zs; y4 = zs;
  }

  /* PA_ATTITUDE.cal_ges 简化:PIT=ROL=X=0 → 仅传 -Hc 偏移
   * ik_input_y = trot_y + (-Hc) = trot_y - Hc */
  float iy1 = y1 - Hc, iy2 = y2 - Hc, iy3 = y3 - Hc, iy4 = y4 - Hc;

  /* IK 反解(4 腿) */
  float ham1, ham2, ham3, ham4;
  float shank1, shank2, shank3, shank4;
  stepping_ik_case0(STEP_L1, STEP_L2, x1, iy1, &ham1, &shank1);
  stepping_ik_case0(STEP_L1, STEP_L2, x2, iy2, &ham2, &shank2);
  stepping_ik_case0(STEP_L1, STEP_L2, x3, iy3, &ham3, &shank3);
  stepping_ik_case0(STEP_L1, STEP_L2, x4, iy4, &ham4, &shank4);

  /* 舵机角度公式(基于 IK STAND 参考点,不用 py-apple 的 ±90 偏移)
   *
   * 我们 STAND(脚在 (±l/2, -Hc),py-apple cal_ges 默认)时 IK 输出:
   *   前腿(FR/FL, x_in=+71): ham_std ≈ 96.7°, shank_std ≈ 57.1°
   *   后腿(BL/BR, x_in=-71): ham_std ≈ 30.8°, shank_std ≈ 57.1°
   *
   * 公式(腿1/4 vs 腿2/3 镜像,sign 翻转):
   *   a_thigh = init_h ± (ham - ham_std_leg)
   *   a_shin  = init_s ± (shank - shank_std)
   *
   * 验证:STAND 时 ham=ham_std, shank=shank_std → a_thigh = init_h, a_shin = init_s ✅
   *
   * ⚠️ 方向符号 (±) 由舵机装配方向决定,首次实跑若方向反了翻转所有 ± 号
   */
  float a_thigh_1 = init_1h + (ham1 - STEP_HAM_STD_FRONT);  /* FR 肩 */
  float a_shin_1  = init_1s + (shank1 - STEP_SHANK_STD);    /* FR 小腿 */
  float a_thigh_2 = init_2h - (ham2 - STEP_HAM_STD_FRONT);  /* FL 肩 (镜像) */
  float a_shin_2  = init_2s - (shank2 - STEP_SHANK_STD);    /* FL 小腿 */
  float a_thigh_3 = init_3h - (ham3 - STEP_HAM_STD_BACK);   /* BL 肩 (镜像) */
  float a_shin_3  = init_3s - (shank3 - STEP_SHANK_STD);    /* BL 小腿 */
  float a_thigh_4 = init_4h + (ham4 - STEP_HAM_STD_BACK);   /* BR 肩 */
  float a_shin_4  = init_4s + (shank4 - STEP_SHANK_STD);    /* BR 小腿 */

  /* 角度 → PWM → clamp → set_servo_pulse */
  uint16_t p3 = clamp_servo_pwm(3, angle_to_pwm(a_thigh_1));  /* FR 肩 */
  uint16_t p2 = clamp_servo_pwm(2, angle_to_pwm(a_shin_1));   /* FR 小腿 */
  uint16_t p4 = clamp_servo_pwm(4, angle_to_pwm(a_thigh_2));  /* FL 肩 */
  uint16_t p5 = clamp_servo_pwm(5, angle_to_pwm(a_shin_2));   /* FL 小腿 */
  uint16_t p6 = clamp_servo_pwm(6, angle_to_pwm(a_thigh_3));  /* BL 肩 */
  uint16_t p7 = clamp_servo_pwm(7, angle_to_pwm(a_shin_3));   /* BL 小腿 */
  uint16_t p1 = clamp_servo_pwm(1, angle_to_pwm(a_thigh_4));  /* BR 肩 */
  uint16_t p0 = clamp_servo_pwm(0, angle_to_pwm(a_shin_4));   /* BR 小腿 */

  set_servo_pulse(3, p3);
  set_servo_pulse(2, p2);
  set_servo_pulse(4, p4);
  set_servo_pulse(5, p5);
  set_servo_pulse(6, p6);
  set_servo_pulse(7, p7);
  set_servo_pulse(1, p1);
  set_servo_pulse(0, p0);

  /* 调试数据缓存(给 stepping_show 用) */
  dbg_ham[0] = ham1;   dbg_shank[0] = shank1;
  dbg_ham[1] = ham2;   dbg_shank[1] = shank2;
  dbg_ham[2] = ham3;   dbg_shank[2] = shank3;
  dbg_ham[3] = ham4;   dbg_shank[3] = shank4;
  dbg_pwm[0] = p0; dbg_pwm[1] = p1; dbg_pwm[2] = p2; dbg_pwm[3] = p3;
  dbg_pwm[4] = p4; dbg_pwm[5] = p5; dbg_pwm[6] = p6; dbg_pwm[7] = p7;
}

/* === 应用 STAND 到 8 路舵机 =============================================*/
static void stepping_apply_stand(void) {
  set_servo_pulse(0, SERVO_SHIN_BR_STAND);
  set_servo_pulse(1, SERVO_SHOULDER_BR_STAND);
  set_servo_pulse(2, SERVO_SHIN_FR_STAND);
  set_servo_pulse(3, SERVO_SHOULDER_FR_STAND);
  set_servo_pulse(4, SERVO_SHOULDER_FL_STAND);
  set_servo_pulse(5, SERVO_SHIN_FL_STAND);
  set_servo_pulse(6, SERVO_SHOULDER_BL_STAND);
  set_servo_pulse(7, SERVO_SHIN_BL_STAND);
}

/* === 接口实现 ===========================================================*/
void stepping_init(void) {
  step_state = STEPPING_IDLE;
  step_t_phase = 0.0f;
}

void stepping_start_trot(void) {
  /* 先应用 STAND,确保启动时是站立 */
  stepping_apply_stand();
  step_state = STEPPING_TROT;
  step_t_phase = 0.0f;
#ifdef HAL_TIM6_MODULE_ENABLED
  if (HAL_TIM_Base_Start_IT(&htim6) != HAL_OK) {
    printf("ERR stepping: TIM6 start failed\n");
    step_state = STEPPING_IDLE;
    return;
  }
  printf("OK trot started (T=100Hz, h=%.0fmm)\n", STEP_H_LIFT);
#else
  printf("ERR stepping: TIM6 not configured\n");
  step_state = STEPPING_IDLE;
#endif
}

void stepping_stop(void) {
#ifdef HAL_TIM6_MODULE_ENABLED
  HAL_TIM_Base_Stop_IT(&htim6);
#endif
  step_state = STEPPING_IDLE;
  step_t_phase = 0.0f;
  stepping_apply_stand();
  printf("OK trot stopped, returned to STAND\n");
}

void stepping_tick(void) {
  if (step_state != STEPPING_TROT) return;
  step_t_phase += STEP_T_INC;
  if (step_t_phase >= 1.0f) step_t_phase -= 1.0f;
  stepping_trot_step();
}

SteppingState stepping_get_state(void) {
  return step_state;
}

void stepping_show(void) {
  printf("phase=%.3f ham=[%.1f,%.1f,%.1f,%.1f] shank=[%.1f,%.1f,%.1f,%.1f]\n",
         step_t_phase,
         dbg_ham[0], dbg_ham[1], dbg_ham[2], dbg_ham[3],
         dbg_shank[0], dbg_shank[1], dbg_shank[2], dbg_shank[3]);
  printf("pwm=[%u,%u,%u,%u,%u,%u,%u,%u]\n",
         dbg_pwm[0], dbg_pwm[1], dbg_pwm[2], dbg_pwm[3],
         dbg_pwm[4], dbg_pwm[5], dbg_pwm[6], dbg_pwm[7]);
}