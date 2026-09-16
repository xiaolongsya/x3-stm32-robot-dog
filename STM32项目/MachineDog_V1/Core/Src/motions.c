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
#include "ramp.h"

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

/* === SIT 真实下蹲姿态(2026-09-16 重整)==                          =======================
 * 2026-09-16 重大修正:取消 BL shin +80 偏置 (STAND 1580→1480)
 *   - 旧代码 SIT_REAL_PWM[7]=1980 但 SERVO_LIMIT[7]=[1430,1730] 宽度只有 300
 *     → 1980 被 clamp 到 1730, BL 只走 150/500 (30%) 所以蹲下"瞬间到位"
 *   - 新 STAND=1480, SERVO_LIMIT=[1480,2180], target=1980 → +500 与其他小腿一致
 * 4 小腿向"腿收"方向偏移 + 4 肩保持 STAND
 * 全部从 SERVO_*_STAND 派生,在 SERVO_LIMIT 范围内
 *
 * 腿"收"方向 (PWM 偏移 ±500,左右舵机镜像):
 *   - BR shin: STAND=1600, 收=1100 (P 减,右腿)
 *   - FR shin: STAND=1600, 收=1100 (P 减,右腿)
 *   - FL shin: STAND=1400, 收=1900 (P 增,左腿镜像)
 *   - BL shin: STAND=1480, 收=1980 (P 增,左腿镜像,2026-09-16 改)
 * 4 肩保持 STAND 不动
 */
static const uint16_t SIT_REAL_PWM[8] = {
  1100,                          /* 0  BR shin:1600 → 1100(P 减,收 500) */
  SERVO_SHOULDER_BR_STAND,       /* 1  BR shoulder:STAND 1100 */
  1100,                          /* 2  FR shin:1600 → 1100(P 减,收 500) */
  SERVO_SHOULDER_FR_STAND,       /* 3  FR shoulder:STAND 1100 */
  SERVO_SHOULDER_FL_STAND,       /* 4  FL shoulder:STAND 2000(+100 偏置) */
  1900,                          /* 5  FL shin:1400 → 1900(P 增,镜像,收 500) */
  SERVO_SHOULDER_BL_STAND,       /* 6  BL shoulder:STAND 1900 */
  1980,                          /* 7  BL shin:1480 → 1980(P 增,镜像,收 500, 2026-09-16 改) */
};


/* === 应用 TROT_STAND 到 8 路舵机(中立,无前倾)======================
 * 2026-09-16 重整:取消 BL shin +80 偏置,BL 也用纯中立 1500
 * 6 路用 STAND,BR shin 改中立(STAND=1600=MAX 是前倾,改 1500 中立避免水平分量干扰 trot)
 *   - BR shin:1500(中立)
 *   - BL shin:1500(中立,2026-09-16 从 1460 改 1500)
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
  1500,                          /* 7  BL 小腿:1500(中立,2026-09-16 改) */
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

/* === duration 运行时覆盖 (2026-09-16 修复)===
 * 背景:MOTION_TABLE 是 const,在 flash (ld .rodata → FLASH)
 *   原代码 ((Motion*)current)->duration_s = X 是 no-op,运行时写入被忽略,
 *   导致 X3 发 trot 5 实际跑 30s(表的默认值)
 * 修复:用这个 RAM 变量覆盖,0 表示沿用表的 duration_s
 */
static uint16_t g_duration_override_s = 0;

/* === 各动作 setup() ================================================*/

/* STAND:跳到 STAND(用户标定的站立姿态,4 脚承重),保持不动
 * 2026-09-14 修:之前 setup() 空实现,以为 motion_init() 已经写过 STAND,
 * 实际 X3 上线后 MOTION_ID 默认 MOTION_STAND,setup 必须显式写 8 路到 STAND */
static void motion_stand_setup(void) {
  apply_stand();
}

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
  } else {
    /* 2026-09-16 修复:g_duration_override_s 优先于 current->duration_s
     * 见 motion_play_by_id(),MOTION_TABLE 在 flash,运行时改 const 无效 */
    uint16_t dur_s = g_duration_override_s ? g_duration_override_s : current->duration_s;
    if (dur_s > 0u && (now - trot_started_ms) >= (uint32_t)dur_s * 1000u) {
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

/* === 运行时切动作(2026-09-14 加,供 X3 UART 命令调用)===
 *
 * 与 motion_init() 区别:
 *   - motion_init() 在 main() 启动时调一次,用编译时 MOTION_ID
 *   - motion_play_by_id() 在 X3 命令时调,id 来自协议,可选 duration_ms
 *
 * duration_ms 含义(对 TROT/BOB):
 *   - 0 = 无限(等下一次切或 watchdog 切回)
 *   - >0 = 持续时长(到时 tick() 自动切 DONE,但保留最终姿态不回 STAND)
 *
 * 实现:复用 motion_init() 的逻辑,但允许覆盖 start_delay_s / duration_s
 */
void motion_play_by_id(uint8_t id, uint32_t duration_ms) {
  /* 找 id 对应的动作 */
  const Motion *next = NULL;
  for (uint8_t i = 0; MOTION_TABLE[i].name != NULL; i++) {
    if (MOTION_TABLE[i].id == id) { next = &MOTION_TABLE[i]; break; }
  }
  if (next == NULL) return;

  /* 停掉 stepping(切动作前先收腿,避免硬切) */
  /* 注意:在 isr-context 不能调,但 motion_play_by_id 来自 main loop / 命令处理,安全 */
  stepping_stop();

  /* 切到新动作 */
  current = next;
  /* 覆盖 duration(2026-09-16 修复)
   * ⚠️ MOTION_TABLE 在 flash (ld .rodata → FLASH),改 const 是 no-op。
   *    必须用 RAM 变量 g_duration_override_s,让 motion_trot_tick 取用。
   * 语义:
   *   - duration_ms > 0 → 用入参作为本次动作 duration(秒)
   *   - duration_ms = 0 → 0 = 沿用表的 duration_s (0 表示无限)
   */
  g_duration_override_s = (uint16_t)(duration_ms / 1000u);

  /* 同步状态变量 */
  boot_tick_ms = HAL_GetTick();
  trot_started_ms = 0;
  bob_phase = BOB_STANDING;
  bob_phase_start_ms = HAL_GetTick();
  phase = MOTION_PHASE_RUN;

  /* 应用初始姿态(setup) */
  apply_sit_neutral();   /* 安全起点 */
  if (current->setup) current->setup();
}

void motion_init(void) {
  boot_tick_ms = HAL_GetTick();
  g_duration_override_s = 0;  /* 上电默认用表的 duration_s */
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
  /* 1) ramp 推进(任意动作下都可能活跃) */
  ramp_tick();

  if (current == NULL || current->tick == NULL) return;
  current->tick();
}

/* === ACTION_PLAY 入口 (2026-09-16 加)===
 * id 1..4: 走 motion_play_by_id()(既有动作)
 * id 5..8: ramp 动作(SIT_DOWN / STAND_UP / SIT_TO_STAND / STAND_TO_SIT)
 *
 * duration_ms 语义:
 *   = 0  → 用 SIT_RAMP_MS 默认 800ms
 *   > 0  → 用入参作为 ramp 时长(典型 500~1500ms)
 *
 * 实现:任何 ramp 启动前先 stepping_stop() 防止相位错位
 */
#define SIT_RAMP_MS  800u
#define STAND_RAMP_MS 1200u  // 站起慢一点，更平滑

/* ramp 完成回调:清状态(状态机视角的 ACT_IDLE 由 current==NULL 表达) */
static void on_ramp_complete_idle(void) {
  /* ramp 完成后清 current(状态机进入 ACT_IDLE) */
  current = NULL;
  phase   = MOTION_PHASE_DONE;
}

void motion_play_action(uint8_t id, uint32_t duration_ms) {
  /* 任何 ramp 启动前先停 stepping */
  stepping_stop();

  if (id <= 4) {
    /* 既有动作:STAND/TROT/BOB/SHIN_TEST */
    motion_play_by_id(id, duration_ms);
    return;
  }
  if (id < 5 || id > 8) return;

  uint32_t ms = (duration_ms > 0) ? duration_ms : SIT_RAMP_MS;

  switch (id) {
    case ACTION_SIT_DOWN:       /* 5: 任意 → SIT_REAL */
    case ACTION_STAND_TO_SIT:   /* 8: 别名 */
      ramp_cancel();
      ramp_sit_to_target(SIT_REAL_PWM, ms, on_ramp_complete_idle);
      break;
    case ACTION_STAND_UP:       /* 6: 任意 → STAND */
    case ACTION_SIT_TO_STAND:   /* 7: 别名 */
    {
      ramp_cancel();
      uint16_t stand_pwm[8];
      for (uint8_t i = 0; i < 8; i++) stand_pwm[i] = SERVO_STEP[i].stand;
      /* stand 动作专用 ramp 时间，覆盖默认的 ms */
      uint32_t stand_ms = (duration_ms > 0) ? duration_ms : STAND_RAMP_MS;
      ramp_sit_to_target(stand_pwm, stand_ms, on_ramp_complete_idle);
      break;
    }
    default:
      break;
  }
}
