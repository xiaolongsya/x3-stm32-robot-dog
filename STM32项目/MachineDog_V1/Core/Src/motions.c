/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    motions.c
  * @brief   机器狗 v1 动作实现(2026-09-14 整理)
  *
  * 当前 4 个动作(2026-09-14 整理后):
  *   - STAND:写 8 路到 SERVO_*_STAND 然后保持(标定/观察)
  *   - TROT:写 8 路到 TROT_STAND,start_delay_s 后启动 stepping,保持到 stop
  *   - BOB:写 8 路到 STAND → hold 5s → 写 8 路到 SIT(1500)→ hold 5s → 循环
  *   - SHIN_TEST:8 路同步线性 ramp 测小腿范围(找最大值)
  *
  * STAND 值来源:全部从 main.h 的 SERVO_*_STAND 读取(单一真相源)
  *
  * 加新动作:
  *   1. 写 motion_xxx_setup() 和 motion_xxx_tick() 函数
  *   2. 在 MotionTable 末尾加一行
  *   3. 在 motions.h 的 MOTION_ID 默认改你的 ID
  ******************************************************************************
  */
/* USER CODE END Header */

#include "motions.h"
#include "stepping.h"
#include "main.h"

/* htim6 在 tim.c 定义,stepping.c 引用(TIM6 100Hz 步态中断) */
extern TIM_HandleTypeDef htim6;

/* === 应用 STAND 到 8 路舵机(原始前倾标定)============================
 * 从 SERVO_STEP[].stand 读取(SERVO_STEP 从 main.h 的 SERVO_*_STAND 派生)
 */
static void apply_stand(void) {
  for (uint8_t i = 0; i < 8; i++) {
    set_servo_pulse(i, SERVO_STEP[i].stand);
  }
}

/* === 应用 SIT 到 8 路舵机(蹲下 = 全 1500 中位)=====================*/
static void apply_sit_neutral(void) {
  for (uint8_t i = 0; i < 8; i++) {
    set_servo_pulse(i, SERVO_NEUTRAL_US);
  }
}

/* === 应用 TROT_STAND 到 8 路舵机(中立,无前倾)======================
 * 6 路用 STAND(前后肩 + 前 4 个小腿之外的 6 路),2 个小腿改成中立
 *   - BR shin:1500(STAND=1600,改中立避免前倾水平分量)
 *   - BL shin:1500-40=1460(STAND=1580 含 +80 偏置,改中立保留偏差)
 * 用于 trot 起踏/停踏瞬间,身体不前倾
 */
static const uint16_t trot_stand_pwm[8] = {
  1500,                          /* 0  BR 小腿:1500(中立,避开 STAND=1600 前倾) */
  SERVO_SHOULDER_BR_STAND,       /* 1  BR 肩:STAND 1100 */
  SERVO_SHIN_FR_STAND,           /* 2  FR 小腿:STAND 1600 */
  SERVO_SHOULDER_FR_STAND,       /* 3  FR 肩:STAND 1100 */
  SERVO_SHOULDER_FL_STAND,       /* 4  FL 肩:STAND 2000(+100 偏置) */
  SERVO_SHIN_FL_STAND,           /* 5  FL 小腿:STAND 1400 */
  SERVO_SHOULDER_BL_STAND,       /* 6  BL 肩:STAND 1900 */
  1460,                          /* 7  BL 小腿:1500-40=1460(STAND 1580 含 +80 偏置,中立) */
};

static void apply_trot_stand(void) {
  for (uint8_t i = 0; i < 8; i++) {
    set_servo_pulse(i, trot_stand_pwm[i]);
  }
}

/* === 状态(主循环维护) ============================================*/
static MotionPhase phase = MOTION_PHASE_INIT;
static uint32_t    boot_tick_ms = 0;
static const Motion *current = NULL;

/* === 各动作 setup() ================================================*/

/* STAND:已经跳到 STAND,保持不动 */
static void motion_stand_setup(void) { }

/* TROT:跳到 TROT_STAND,启动 stepping 由 motion_trot_tick 在 start_delay 后触发 */
static void motion_trot_setup(void) {
  apply_trot_stand();
}

/* BOB:跳 STAND → hold 5s → 跳 SIT(1500)→ hold 5s → 循环 */
static void motion_bob_setup(void) {
  apply_stand();
  /* 进入 STANDING 状态,5s 后切到 SIT */
}

/* SHIN_TEST:8 路同步线性 ramp 由 stepping_start_trot 触发 */
static void motion_shin_test_setup(void) { }

/* === BOB 状态机 ===================================================*/
typedef enum {
  BOB_STANDING = 0,    /* HOLD STAND 5s */
  BOB_TO_SIT,          /* 跳 SIT(1500) */
  BOB_SITTING,         /* HOLD SIT 5s */
  BOB_TO_STAND         /* 跳 STAND */
} BobPhase;

#define BOB_HOLD_MS  5000u

static BobPhase  bob_phase = BOB_STANDING;
static uint32_t  bob_phase_start_ms = 0;

/* === 各动作 tick() ================================================*/
static void motion_stand_tick(void) { }
static void motion_shin_test_tick(void) { }

/* TROT:start_delay 后启动 stepping,duration 后停止 */
static uint32_t trot_started_ms = 0;

static void motion_trot_tick(void) {
  if (current == NULL) return;
  if (phase != MOTION_PHASE_RUN) return;

  uint32_t now = HAL_GetTick();

  if (trot_started_ms == 0u) {
    if ((now - boot_tick_ms) >= (uint32_t)current->start_delay_s * 1000u) {
      stepping_start_trot();
      trot_started_ms = now;
    }
  } else if (current->duration_s > 0u) {
    if ((now - trot_started_ms) >= (uint32_t)current->duration_s * 1000u) {
      stepping_stop();
      phase = MOTION_PHASE_DONE;
    }
  }
}

/* BOB:STANDING 5s → 跳 SIT → SITTING 5s → 跳 STAND */
static void motion_bob_tick(void) {
  if (current == NULL) return;
  if (phase != MOTION_PHASE_RUN) return;

  uint32_t now = HAL_GetTick();
  uint32_t elapsed = now - bob_phase_start_ms;

  switch (bob_phase) {
    case BOB_STANDING:
      if (elapsed >= BOB_HOLD_MS) {
        apply_sit_neutral();
        bob_phase = BOB_SITTING;
        bob_phase_start_ms = now;
      }
      break;

    case BOB_TO_SIT:
      /* setup 已经写完 SIT,直接进 SITTING */
      bob_phase = BOB_SITTING;
      bob_phase_start_ms = now;
      break;

    case BOB_SITTING:
      if (elapsed >= BOB_HOLD_MS) {
        apply_stand();
        bob_phase = BOB_STANDING;
        bob_phase_start_ms = now;
      }
      break;

    case BOB_TO_STAND:
      /* setup 已经写完 STAND,直接进 STANDING */
      bob_phase = BOB_STANDING;
      bob_phase_start_ms = now;
      break;
  }
}

/* === 注册表(4 个动作,2026-09-14 整理后) =========================*/
const Motion MOTION_TABLE[] = {
  { MOTION_STAND,     "stand",     motion_stand_setup,     motion_stand_tick,     0,  0   },
  { MOTION_TROT,      "trot",      motion_trot_setup,      motion_trot_tick,      5,  30 },
  { MOTION_BOB,       "bob",       motion_bob_setup,       motion_bob_tick,       0,  0   },
  { MOTION_SHIN_TEST, "shin_test", motion_shin_test_setup, motion_shin_test_tick, 0,  0   },
  { 0, NULL, NULL, NULL, 0, 0 }  /* 哨兵 */
};

/* === 对外接口 ====================================================*/
MotionPhase motion_get_phase(void) { return phase; }
const char *motion_get_name(void)  { return current ? current->name : "(none)"; }

void motion_init(void) {
  boot_tick_ms = HAL_GetTick();
  phase = MOTION_PHASE_INIT;

  /* 找 MOTION_ID 对应的动作 */
  current = NULL;
  for (uint8_t i = 0; MOTION_TABLE[i].name != NULL; i++) {
    if (MOTION_TABLE[i].id == MOTION_ID) {
      current = &MOTION_TABLE[i];
      break;
    }
  }
  if (current == NULL) {
    current = &MOTION_TABLE[0];
  }

  /* 安全起点:8 路全 1500(腿完全伸直/居中) */
  apply_sit_neutral();

  if (current->setup) current->setup();
  phase = MOTION_PHASE_RUN;
}

void motion_poll(void) {
  if (current == NULL || current->tick == NULL) return;
  current->tick();
}
