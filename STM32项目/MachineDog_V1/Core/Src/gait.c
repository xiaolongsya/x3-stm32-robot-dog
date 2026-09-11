/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gait.c
  * @brief   py-apple-dynamics 步态 + IK + 标定辅助 移植到 STM32G431KBT6
  *
  * 移植范围(2026-09-11):
  *   - PA_GAIT.trot 对角小跑步态(trot,x_target=0 即原地踏步)
  *   - PA_IK.ik case=0 串联腿反运动学(余弦定理)
  *   - PA_ATTITUDE.cal_ges 简化版(只支持 PIT=ROL=X=0 水平姿态)
  *
  * 腿编号约定(用户拍板,2026-09-11):
  *   腿1=FR  大腿=servo3 (FR肩)  小腿=servo2 (FR小腿)
  *   腿2=FL  大腿=servo4 (FL肩)  小腿=servo5 (FL小腿)
  *   腿3=BL  大腿=servo6 (BL肩)  小腿=servo7 (BL小腿)
  *   腿4=BR  大腿=servo1 (BR肩)  小腿=servo0 (BR小腿)
  *   trot (腿1+3) vs (腿2+4) 交替 = (FR+BL) vs (FL+BR) 真正对角步态
  *
  * 舵机输出公式(来自 py-apple-dynamics padog.servo_output case=0):
  *   腿1/4: 大腿_angle = init_*h + 90 - ham  ; 小腿_angle = init_*s - 90 + shank
  *   腿2/3: 大腿_angle = init_*h - 90 + ham  ; 小腿_angle = init_*s + 90 - shank
  *   init_* = stand_pwm[i] 反推的角度(STAND 时舵机角度)
  *   shank 暂不应用 mechan_offset_corr(机械不同,后续实测再加)
  *
  * 标定流程(2026-09-11):
  *   1. UART 发 `cal raw`     → 8 路舵机设 1500 µs(机械零位)
  *   2. 用户观察机械几何(目标:大腿垂直地面,小腿水平向前)
  *   3. 用 `<id> <pulse>` 逐路微调到"标准姿态"
  *   4. UART 发 `cal save`    → 把当前 8 路 PWM 保存为 stand_pwm[8]
  *   5. 之后 stand/step trot 命令都用新 STAND
  *
  * 调度:由 TIM6 100Hz 中断调用 gait_tick(),TIM6 配置:
  *   Prescaler=16999 (170MHz → 10kHz),Period=99 (10kHz/100 = 100Hz)
  *   ⚠️ 用户需要在 CubeMX 加 TIM6 + 使能 NVIC TIM6 global interrupt
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "gait.h"
#include "main.h"      /* HAL, set_servo_pulse, SERVO_*_STAND */
#include <math.h>
#include <stdio.h>

/* === 8 路 STAND PWM(运行时可标定)==========================================
 * 索引按 servo_id 0-7:BR小腿/BR肩/FR小腿/FR肩/FL肩/FL小腿/BL肩/BL小腿
 * 默认从 SERVO_*_STAND 常量初始化,可由 `cal save` UART 命令覆盖
 */
static uint16_t stand_pwm[8] = {
  SERVO_SHIN_BR_STAND,      /* [0] BR 小腿 */
  SERVO_SHOULDER_BR_STAND,  /* [1] BR 肩 */
  SERVO_SHIN_FR_STAND,      /* [2] FR 小腿 */
  SERVO_SHOULDER_FR_STAND,  /* [3] FR 肩 */
  SERVO_SHOULDER_FL_STAND,  /* [4] FL 肩 */
  SERVO_SHIN_FL_STAND,      /* [5] FL 小腿 */
  SERVO_SHOULDER_BL_STAND,  /* [6] BL 肩 */
  SERVO_SHIN_BL_STAND,      /* [7] BL 小腿 */
};

/* === gait 状态 ============================================================*/
static volatile GaitState gait_state = GAIT_IDLE;
static volatile float gait_t_phase = 0.0f;   /* 相位 [0, 1.0) */

/* === 步态参数 =============================================================*/
#define GAIT_T_STEP       0.1f    /* 每 10ms 步进 0.1,1 周期 100ms = 10Hz */
#define GAIT_TF           0.5f    /* trot 摆动半周期(归一化) */
#define GAIT_BODY_HEIGHT  110.0f  /* 髋到脚距离 Hc (mm),py-apple 默认 110 */
#define GAIT_LIFT_H       15.0f   /* 抬腿高度 (mm),用户拍板 2026-09-11 */
#define GAIT_L1           80.0f   /* 大腿长 (mm),py-apple config_s.py */
#define GAIT_L2           69.0f   /* 小腿长 (mm),py-apple config_s.py */

/* === 钳制 acos 输入到 [-1, 1] ============================================*/
static inline float clamp_cos(float v) {
  if (v > 1.0f) return 1.0f;
  if (v < -1.0f) return -1.0f;
  return v;
}

/* === 舵机角度 (度) → PWM (µs) ============================================*/
static inline uint16_t angle_to_pwm(float angle_deg) {
  float pwm = 500.0f + angle_deg * (2000.0f / 180.0f);
  if (pwm < 500.0f) pwm = 500.0f;
  if (pwm > 2500.0f) pwm = 2500.0f;
  return (uint16_t)pwm;
}

/* === STAND PWM → STAND 角度 (度) =========================================*/
static inline float stand_pwm_to_angle(uint8_t servo_id) {
  return (float)(stand_pwm[servo_id] - 500) * (180.0f / 2000.0f);
}

/* === swing_curve_generate (来自 padog.py:315) =============================
 * 摆动相足端轨迹 (t∈[0, Tf]):
 *   X: 匀加速 → 三次 Hermite → 提前到顶
 *   Z: 立方上升 (到 zh) → 二次下降, zf<=0 时强制 0
 * 输入:t=局部时间, Tf=摆动半周期, xt=X目标, zh=抬腿高度, x0/z0=起点, xv0=起步速度
 * 输出:xf, zf (足端 X/Z 偏移)
 */
static void swing_curve_generate(float t, float Tf, float xt, float zh,
                                 float x0, float z0, float xv0,
                                 float *xf_out, float *zf_out) {
  float xf = 0.0f, zf = 0.0f;
  float Tf2 = Tf * Tf;
  float Tf4 = Tf2 * Tf2;

  /* X 方向 */
  if (t >= 0.0f && t < Tf / 4.0f) {
    xf = (-4.0f * xv0 / Tf) * t * t + xv0 * t + x0;
  } else if (t >= Tf / 4.0f && t < 3.0f * Tf / 4.0f) {
    /* 三次 Hermite 平滑过渡到 xt */
    xf = ((-4.0f * Tf * xv0 - 16.0f * xt + 16.0f * x0) * t * t * t) / (Tf * Tf2)
       + ((7.0f  * Tf * xv0 + 24.0f * xt - 24.0f * x0) * t * t) / Tf2
       + ((-15.0f * Tf * xv0 - 36.0f * xt + 36.0f * x0) * t) / (4.0f * Tf)
       + (9.0f * Tf * xv0 + 16.0f * xt) / 16.0f;
  } else {
    xf = xt;
  }

  /* Z 方向 */
  if (t >= 0.0f && t < Tf / 2.0f) {
    zf = (16.0f * z0 - 16.0f * zh) * t * t * t / Tf4
       + (12.0f * zh - 12.0f * z0) * t * t / Tf2 + z0;
  } else {
    zf = (4.0f * z0 - 4.0f * zh) * t * t / Tf2 - (4.0f * z0 - 4.0f * zh) * t / Tf + z0;
  }
  if (zf <= 0.0f) zf = 0.0f;  /* 防穿地 */

  *xf_out = xf;
  *zf_out = zf;
}

/* === support_curve_generate (来自 padog.py:346) ===========================
 * 支撑相足端轨迹 (t∈[Tf, 1]):X 线性回退,Z 常数(脚不离地)
 */
static void support_curve_generate(float t, float Tf, float x_past,
                                   float t_past, float zf,
                                   float *xf_out, float *zf_out) {
  float average = x_past / (1.0f - Tf);
  float xf = x_past - average * (t - t_past);
  *xf_out = xf;
  *zf_out = zf;
}

/* === gait_ik_case0 (来自 PA_IK.py case=0) ================================
 * 串联腿反运动学(余弦定理),4 腿展开
 * 输入:腿 i 髋坐标系足端 (x_i, y_i),腿长 l1/l2
 * 输出:ham_i (大腿角,度), shank_i (小腿角,度)
 */
static void gait_ik_case0(float l1, float l2,
                          float x1, float x2, float x3, float x4,
                          float y1, float y2, float y3, float y4,
                          float *ham1, float *ham2, float *ham3, float *ham4,
                          float *shank1, float *shank2, float *shank3, float *shank4) {
  float fai;
  float r;

  /* 腿 1 */
  x1 = -x1;
  *shank1 = (float)M_PI - acosf(clamp_cos((x1*x1 + y1*y1 - l1*l1 - l2*l2) / (-2.0f * l1 * l2)));
  r = sqrtf(x1*x1 + y1*y1);
  fai = acosf(clamp_cos((l1*l1 + x1*x1 + y1*y1 - l2*l2) / (2.0f * l1 * r)));
  if (x1 > 0.0f)        *ham1 = fabsf(atanf(y1 / x1)) - fai;
  else if (x1 < 0.0f)   *ham1 = (float)M_PI - fabsf(atanf(y1 / x1)) - fai;
  else                  *ham1 = (float)M_PI - 1.5707f - fai;
  *shank1 = 180.0f * (*shank1) / (float)M_PI;
  *ham1   = 180.0f * (*ham1)   / (float)M_PI;

  /* 腿 2 */
  x2 = -x2;
  *shank2 = (float)M_PI - acosf(clamp_cos((x2*x2 + y2*y2 - l1*l1 - l2*l2) / (-2.0f * l1 * l2)));
  r = sqrtf(x2*x2 + y2*y2);
  fai = acosf(clamp_cos((l1*l1 + x2*x2 + y2*y2 - l2*l2) / (2.0f * l1 * r)));
  if (x2 > 0.0f)        *ham2 = fabsf(atanf(y2 / x2)) - fai;
  else if (x2 < 0.0f)   *ham2 = (float)M_PI - fabsf(atanf(y2 / x2)) - fai;
  else                  *ham2 = (float)M_PI - 1.5707f - fai;
  *shank2 = 180.0f * (*shank2) / (float)M_PI;
  *ham2   = 180.0f * (*ham2)   / (float)M_PI;

  /* 腿 3 */
  x3 = -x3;
  *shank3 = (float)M_PI - acosf(clamp_cos((x3*x3 + y3*y3 - l1*l1 - l2*l2) / (-2.0f * l1 * l2)));
  r = sqrtf(x3*x3 + y3*y3);
  fai = acosf(clamp_cos((l1*l1 + x3*x3 + y3*y3 - l2*l2) / (2.0f * l1 * r)));
  if (x3 > 0.0f)        *ham3 = fabsf(atanf(y3 / x3)) - fai;
  else if (x3 < 0.0f)   *ham3 = (float)M_PI - fabsf(atanf(y3 / x3)) - fai;
  else                  *ham3 = (float)M_PI - 1.5707f - fai;
  *shank3 = 180.0f * (*shank3) / (float)M_PI;
  *ham3   = 180.0f * (*ham3)   / (float)M_PI;

  /* 腿 4 */
  x4 = -x4;
  *shank4 = (float)M_PI - acosf(clamp_cos((x4*x4 + y4*y4 - l1*l1 - l2*l2) / (-2.0f * l1 * l2)));
  r = sqrtf(x4*x4 + y4*y4);
  fai = acosf(clamp_cos((l1*l1 + x4*x4 + y4*y4 - l2*l2) / (2.0f * l1 * r)));
  if (x4 > 0.0f)        *ham4 = fabsf(atanf(y4 / x4)) - fai;
  else if (x4 < 0.0f)   *ham4 = (float)M_PI - fabsf(atanf(y4 / x4)) - fai;
  else                  *ham4 = (float)M_PI - 1.5707f - fai;
  *shank4 = 180.0f * (*shank4) / (float)M_PI;
  *ham4   = 180.0f * (*ham4)   / (float)M_PI;
}

/* === gait_trot_step (整合 trot + cal_ges + IK + servo_output) ============
 * 单步执行:推进相位 + 算 4 腿 IK + 输出 8 路 PWM
 * ⚠️ 不调 cal_attitude(PIT=ROL=X=0 水平姿态),cal_ges 直接返回 (0, -Hc, 0, -Hc, 0, -Hc, 0, -Hc)
 */
static void gait_trot_step(void) {
  float x1, x2, x3, x4, y1, y2, y3, y4;
  float xs, zs, xp, zp;
  const float Hc = GAIT_BODY_HEIGHT;

  /* PA_GAIT.trot(t, x_target=0, z_target=h, r1=1, r4=1, r2=1, r3=1)
   * 原地踏步:r1=r4=1(腿1和腿4前进方向倍率),实际 x_target=0 时不影响 */
  if (gait_t_phase < GAIT_TF) {
    /* 腿 1+3 swing,腿 2+4 support */
    swing_curve_generate(gait_t_phase, GAIT_TF, 0.0f, GAIT_LIFT_H,
                         0.0f, 0.0f, 0.0f, &xs, &zs);
    support_curve_generate(GAIT_TF + gait_t_phase, GAIT_TF, 0.0f, GAIT_TF, 0.0f, &xp, &zp);
    x1 = xs; x3 = xs; x2 = xp; x4 = xp;
    y1 = zs; y3 = zs; y2 = zp; y4 = zp;
  } else {
    /* 腿 2+4 swing,腿 1+3 support */
    swing_curve_generate(gait_t_phase - GAIT_TF, GAIT_TF, 0.0f, GAIT_LIFT_H,
                         0.0f, 0.0f, 0.0f, &xs, &zs);
    support_curve_generate(gait_t_phase, GAIT_TF, 0.0f, GAIT_TF, 0.0f, &xp, &zp);
    x1 = xp; x3 = xp; x2 = xs; x4 = xs;
    y1 = zp; y3 = zp; y2 = zs; y4 = zs;
  }

  /* cal_attitude 简化:PIT=ROL=X=0 → (ges_x, ges_y) = (0, -Hc)
   * ik_input_y = trot_y + ges_y = trot_y - Hc */
  float Hc_neg = -Hc;
  float iy1 = y1 + Hc_neg, iy2 = y2 + Hc_neg, iy3 = y3 + Hc_neg, iy4 = y4 + Hc_neg;
  float ix1 = x1, ix2 = x2, ix3 = x3, ix4 = x4;

  /* IK 反解(余弦定理,余弦定理要 x_i 取反,已在 gait_ik_case0 内做) */
  float ham1, ham2, ham3, ham4;
  float shank1, shank2, shank3, shank4;
  gait_ik_case0(GAIT_L1, GAIT_L2, ix1, ix2, ix3, ix4,
                iy1, iy2, iy3, iy4,
                &ham1, &ham2, &ham3, &ham4,
                &shank1, &shank2, &shank3, &shank4);

  /* 舵机角度公式(腿 1/4 与腿 2/3 镜像,见文件头注释) */
  /* STAND 角度 = stand_pwm 反推 */
  float init_1h = stand_pwm_to_angle(3);  /* FR 肩 */
  float init_1s = stand_pwm_to_angle(2);  /* FR 小腿 */
  float init_2h = stand_pwm_to_angle(4);  /* FL 肩 */
  float init_2s = stand_pwm_to_angle(5);  /* FL 小腿 */
  float init_3h = stand_pwm_to_angle(6);  /* BL 肩 */
  float init_3s = stand_pwm_to_angle(7);  /* BL 小腿 */
  float init_4h = stand_pwm_to_angle(1);  /* BR 肩 */
  float init_4s = stand_pwm_to_angle(0);  /* BR 小腿 */

  /* 腿 1/4: 大腿 +90-ham, 小腿 -90+shank */
  float a_thigh_1 = init_1h + 90.0f - ham1;
  float a_shin_1  = init_1s - 90.0f + shank1;
  /* 腿 2/3: 大腿 -90+ham, 小腿 +90-shank */
  float a_thigh_2 = init_2h - 90.0f + ham2;
  float a_shin_2  = init_2s + 90.0f - shank2;
  float a_thigh_3 = init_3h - 90.0f + ham3;
  float a_shin_3  = init_3s + 90.0f - shank3;
  /* 腿 4 (BR): +90-ham 大腿, -90+shank 小腿 */
  float a_thigh_4 = init_4h + 90.0f - ham4;
  float a_shin_4  = init_4s - 90.0f + shank4;

  /* 角度 → PWM(已 clamp 500~2500) */
  set_servo_pulse(3, angle_to_pwm(a_thigh_1));  /* FR 肩 */
  set_servo_pulse(2, angle_to_pwm(a_shin_1));   /* FR 小腿 */
  set_servo_pulse(4, angle_to_pwm(a_thigh_2));  /* FL 肩 */
  set_servo_pulse(5, angle_to_pwm(a_shin_2));   /* FL 小腿 */
  set_servo_pulse(6, angle_to_pwm(a_thigh_3));  /* BL 肩 */
  set_servo_pulse(7, angle_to_pwm(a_shin_3));   /* BL 小腿 */
  set_servo_pulse(1, angle_to_pwm(a_thigh_4));  /* BR 肩 */
  set_servo_pulse(0, angle_to_pwm(a_shin_4));   /* BR 小腿 */
}

/* === 应用 stand_pwm[8] 到 8 路舵机 ========================================*/
static void gait_apply_stand(void) {
  for (uint8_t i = 0; i < 8; i++) set_servo_pulse(i, stand_pwm[i]);
}

/* === 接口实现 ============================================================*/
void gait_init(void) {
  gait_state = GAIT_IDLE;
  gait_t_phase = 0.0f;
  /* 不启动 TIM6,等 gait_start_trot() */
}

void gait_start_trot(void) {
  /* 退出标定模式前先应用 stand_pwm,确保起始位置是 STAND */
  gait_apply_stand();
  gait_state = GAIT_TROT;
  gait_t_phase = 0.0f;
#ifdef HAL_TIM6_MODULE_ENABLED
  /* ⚠️ 用户需要在 main.c 的 TIM6_IRQHandler 里调 HAL_TIM_Base_Start_IT(&htim6) */
  if (HAL_TIM_Base_Start_IT(&htim6) != HAL_OK) {
    printf("ERR gait: TIM6 start failed\n");
    gait_state = GAIT_IDLE;
    return;
  }
  printf("OK gait trot started (T=100ms, h=%.0fmm)\n", GAIT_LIFT_H);
#else
  printf("ERR gait: TIM6 not configured, please add TIM6 in CubeMX (Prescaler=16999, Period=99)\n");
  gait_state = GAIT_IDLE;
#endif
}

void gait_stop(void) {
#ifdef HAL_TIM6_MODULE_ENABLED
  HAL_TIM_Base_Stop_IT(&htim6);
#endif
  gait_state = GAIT_IDLE;
  gait_t_phase = 0.0f;
  /* 退出踏步,8 路舵机回到 STAND */
  gait_apply_stand();
  printf("OK gait stopped, returned to STAND\n");
}

void gait_tick(void) {
  if (gait_state != GAIT_TROT) return;
  /* 推进相位 */
  gait_t_phase += GAIT_T_STEP;
  if (gait_t_phase >= 1.0f) gait_t_phase -= 1.0f;
  /* 计算 + 输出 8 路 PWM */
  gait_trot_step();
}

GaitState gait_get_state(void) {
  return gait_state;
}

/* === 标定辅助(供 main.c 的 cal 命令调) ===================================*/
void gait_cal_save_stand_array(const uint16_t pwm[8]) {
  for (uint8_t i = 0; i < 8; i++) stand_pwm[i] = pwm[i];
  printf("OK stand saved: [");
  for (uint8_t i = 0; i < 8; i++) printf("%s%d", i ? "," : "", stand_pwm[i]);
  printf("]\n");
}

void gait_cal_show_stand(void) {
  printf("stand_pwm: ");
  for (uint8_t i = 0; i < 8; i++) printf("%d ", stand_pwm[i]);
  printf("\n");
}

const uint16_t *gait_get_stand_pwm(void) {
  return stand_pwm;
}

void gait_cal_raw(void) {
  /* 8 路舵机设 1500 µs(机械零位)
   * ⚠️ 同时要更新 main.c 的 current_pwm(避免 cal save 时保存错的值) */
  for (uint8_t i = 0; i < 8; i++) set_servo_pulse(i, 1500);
  printf("OK 8 servos set to 1500 (mechanical zero)\n");
}