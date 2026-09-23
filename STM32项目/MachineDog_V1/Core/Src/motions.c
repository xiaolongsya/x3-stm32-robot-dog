/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    motions.c
  * @brief   机器狗 v1 动作实现(2026-09-14 整理)
  *
 * 当前 5 个动作:
  *   - STAND:写 8 路到 SERVO_*_STAND 然后保持(标定/观察)
  *   - TROT:写 8 路到 TROT_STAND,start_delay_s 后启动 stepping,保持到 stop
  *   - BOB:写 8 路到 STAND → hold 5s → 写 8 路到"标准值/跪下"→ hold 5s → 循环
 *   - SHIN_TEST:8 路同步线性 ramp 测小腿范围(找最大值)
 *   - WALK:FR→FL→BL→BR,每腿先摆动再四脚推地
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

/* === 8 路"标准值"表(= 跪下姿态,2026-09-17 加)===
 * 6 路 = SERVO_NEUTRAL_US(1500);2 个**物理偏移全局存在**,必须带:
 *   id=4 FL 肩  = 1500 + 100 = 1600
 *   id=7 BL 小腿 = 1500 +  80 = 1580
 * 用于:上电安全起点 / motion_play_by_id 切动作过渡 / BOB 蹲姿
 */
static const uint16_t neutral_pwm[8] = {
  SERVO_NEUTRAL_US,           /* 0  BR 小腿 */
  SERVO_NEUTRAL_US,           /* 1  BR 肩 */
  SERVO_NEUTRAL_US,           /* 2  FR 小腿 */
  SERVO_NEUTRAL_US,           /* 3  FR 肩 */
  SERVO_NEUTRAL_FL_SHOULDER,  /* 4  FL 肩  :1500 + 100 物理偏移 = 1600 */
  SERVO_NEUTRAL_US,           /* 5  FL 小腿 */
  SERVO_NEUTRAL_US,           /* 6  BL 肩 */
  SERVO_NEUTRAL_BL_SHIN,      /* 7  BL 小腿:1500 +  80 物理偏移 = 1580 */
};

/* === 应用"标准值/跪下"到 8 路舵机(对外可见)=====================
 * 2026-09-17 修:此前是"8 路全 1500",漏掉了 FL 肩 +100 / BL 小腿 +80
 * 两个物理偏移 —— 它们在**任何姿态**下都存在,不只 STAND
 *
 * 调用方:motions.c 内部(上电安全起点 / 切动作过渡 / BOB 蹲姿)
 *        + commands.c 的 center / sit 文本命令
 */
void motion_apply_neutral(void) {
  for (uint8_t i = 0; i < 8; i++) {
    set_servo_pulse(i, neutral_pwm[i]);
  }
}

/* === SIT 真实下蹲姿态(2026-09-16 重整)==                          =======================
 * 2026-09-16 重大修正:BL 小腿 STAND 1580→1480(实测值重校,不是"取消偏置")
 *   - 旧代码 SIT_REAL_PWM[7]=1980 但 SERVO_LIMIT[7]=[1430,1730] 宽度只有 300
 *     → 1980 被 clamp 到 1730, BL 只走 150/500 (30%) 所以蹲下"瞬间到位"
 *   - 新 STAND=1480, SERVO_LIMIT=[1480,2180], target=1980 → +500 与其他小腿一致
 * 4 小腿向狗头方向倾斜(身体降低,蹲下) + 4 肩保持 STAND
 * 全部从 SERVO_*_STAND 派生,在 SERVO_LIMIT 范围内
 *
 * 小腿向狗头方向倾斜(身体降低)(PWM 偏移 ±500,左右舵机反装镜像):
 *   - BR shin: STAND=1600, 蹲=1100 (P 减,右小腿"减小=向前")
 *   - FR shin: STAND=1600, 蹲=1100 (P 减,右小腿)
 *   - FL shin: STAND=1400, 蹲=1900 (P 增,左小腿"增大=向前")
 *   - BL shin: STAND=1480, 蹲=1980 (P 增,左小腿,2026-09-16 改)
 * 4 肩保持 STAND 不动(几何上只小腿变短,肩不动)
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
 * 2026-09-17 重整:BL 小腿中立位补上 +80 物理偏移(1500 → 1580)
 *   此前写 1500 是漏了偏移 —— BL 的"中立"要与其他小腿的 1500 **物理等价**,
 *   BL 有 +80 物理偏移,所以实际 PWM = 1580
 * 6 路用 STAND,BR shin 改中立(STAND=1600=MAX 是前倾,改 1500 中立避免水平分量干扰 trot)
 *   - BR shin:1500(中立)
 *   - BL shin:1580(中立 = 1500 + 80 物理偏移)
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
  SERVO_NEUTRAL_BL_SHIN,         /* 7  BL 小腿:1580(中立 = 1500+80 偏移,2026-09-17 改) */
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

/* === ACTION_PLAY hold 状态(2026-09-17 加)===
 * 语义(ACTION_PLAY id 5..8 的 hold_ms):
 *   - hold_ms = 0 → ramp 完成后保持终点姿态(现状,不回 STAND)
 *   - hold_ms > 0 → ramp 完成后保持 hold_ms,再渐进回 STAND
 *
 * 用途:"蹲下3秒" = 坐下(ramp 800ms)→ 保持 3s → 自动回 STAND
 * 实现在文件尾部的 motion_play_action() / motion_hold_tick()
 */
static uint32_t g_action_hold_ms    = 0;
static uint32_t g_action_hold_start = 0;
static uint8_t  g_action_holding    = 0;
static void     motion_hold_tick(void);   /* 前向声明:motion_poll() 要调 */

/* 清 hold 状态 — 任何切动作前必须调
 * 反例:hold 期间收到 MOTION_PLAY(trot)/EMERGENCY_STOP,若不清理,
 *       old hold 到点会凭空插一个"回 STAND"的 ramp 打断新动作
 */
static void motion_hold_reset(void) {
  g_action_holding = 0;
  g_action_hold_ms = 0;
}

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

static int8_t walk_direction = 1;

static void motion_walk_setup(void) {
  apply_stand();
}

/* BOB:跳 STAND → hold 5s → 跳"标准值/跪下"→ hold 5s → 循环 */
static void motion_bob_setup(void) {
  apply_stand();
  /* 进入 STANDING 状态,5s 后切到 SIT */
}

/* SHIN_TEST:8 路同步线性 ramp 由 stepping_start_trot 触发 */
static void motion_shin_test_setup(void) { }

/* === BOB 状态机 ===================================================*/
typedef enum {
  BOB_STANDING = 0,    /* HOLD STAND 5s */
  BOB_TO_SIT,          /* 跳标准值/跪下 */
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

static void motion_gait_tick(void) {
  if (current == NULL) return;
  if (phase != MOTION_PHASE_RUN) return;

  uint32_t now = HAL_GetTick();

  if (trot_started_ms == 0u) {
    if ((now - boot_tick_ms) >= (uint32_t)current->start_delay_s * 1000u) {
      if (current->id == MOTION_WALK) {
        stepping_start_walk(walk_direction);
      } else {
        stepping_start_trot();
      }
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
        motion_apply_neutral();
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
  { MOTION_TROT,      "trot",      motion_trot_setup,      motion_gait_tick,      5,  30 },
  { MOTION_BOB,       "bob",       motion_bob_setup,       motion_bob_tick,       0,  0   },
  { MOTION_SHIN_TEST, "shin_test", motion_shin_test_setup, motion_shin_test_tick, 0,  0   },
  { MOTION_WALK,      "walk",      motion_walk_setup,      motion_gait_tick,      0,  8   },
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

  /* 停掉 stepping(切动作前先停步态,避免硬切) */
  /* 注意:在 isr-context 不能调,但 motion_play_by_id 来自 main loop / 命令处理,安全 */
  stepping_stop();

  /* 取消 ramp 残留(2026-09-16 修:H1/H2/H6 一并修)
   *
   * 根因:ramp 在 tick() 内每帧写 8 路 PWM,如果切动作不先 ramp_cancel(),
   *   ramp.active 仍为 1,主循环 ramp_tick() 会立刻把目标 PWM 覆盖回来
   *
   * 影响 3 条路径:
   *   - H1: EMERGENCY_STOP (commands.c:197 motion_play_by_id(1,0)) — STAND 被 ramp 覆盖
   *   - H2: 任意 MOTION_PLAY 切动作                          — 目标姿态被 ramp 覆盖
   *   - H6: WATCHDOG 超时回 STAND (watchdog.c motion_play_by_id(1,0)) — 同 H1
   *
   * 安全:ramp_cancel 只翻 flag,g_pwm[] 保持当前值;新动作的 setup() 会
   *   重新写 8 路,所以"取消 ramp 后舵机姿态"由新动作决定,不丢控制
   */
  ramp_cancel();

  /* 清 ACTION_PLAY 的 hold 残留(2026-09-17)
   * 本函数被 commands.c 的 MOTION_PLAY(0x01)/ EMERGENCY_STOP(0x06) 直接调用,
   * 绕过了 motion_play_action() 的 hold 清理 —— 不补这一行的话,
   * hold 期间收到踏步/急停后,旧 hold 到点会凭空插一个"回 STAND"的 ramp */
  motion_hold_reset();

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
  motion_apply_neutral();   /* 安全起点 */
  if (current->setup) current->setup();
}

void motion_play_walk(int8_t direction, uint32_t duration_ms) {
  if (direction == 0) {
    motion_play_by_id(MOTION_TROT, duration_ms);
    return;
  }
  if (direction != 1 && direction != -1) return;
  walk_direction = direction;
  motion_play_by_id(MOTION_WALK, duration_ms);
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

  /* 安全起点:8 路"标准值/跪下"(含 FL 肩 +100 / BL 小腿 +80 物理偏移) */
  motion_apply_neutral();

  if (current->setup) current->setup();
  phase = MOTION_PHASE_RUN;
}

void motion_poll(void) {
  /* 1) ramp 推进(任意动作下都可能活跃) */
  ramp_tick();

  /* 2) ACTION_PLAY hold 到期检查(2026-09-17):
   *    hold 到期 → 渐进回 STAND。不依赖 current,所以放在 tick 派发前 */
  motion_hold_tick();

  if (current == NULL || current->tick == NULL) return;
  current->tick();
}

/* === ACTION_PLAY 入口 (2026-09-16 加 / 2026-09-17 加 hold)===
 * id 1..4: 走 motion_play_by_id()(既有动作),hold_ms 当动作时长传下去
 * id 5..8: ramp 动作(SIT_DOWN / STAND_UP / SIT_TO_STAND / STAND_TO_SIT)
 *
 * hold_ms 语义(仅 id 5..8):
 *   = 0  → ramp 完成后保持终点姿态,不回 STAND(现状行为)
 *   > 0  → ramp 完成后保持 hold_ms,再渐进回 STAND
 * 注:ramp 本身时长固定(SIT 800ms / STAND 1200ms),不受 hold_ms 影响
 *
 * 实现:任何 ramp 启动前先 stepping_stop() 防止相位错位
 */
#define SIT_RAMP_MS  800u
#define STAND_RAMP_MS 1200u  // 站起慢一点，更平滑

/* ramp 完成回调
 * - 有 hold:第一次完成 → 进 hold(保持姿态,不清 current)
 * - 无 hold / hold 结束后的"回 STAND"ramp 完成 → 清状态(ACT_IDLE)
 */
static void on_ramp_complete_idle(void) {
  if (g_action_hold_ms > 0u && !g_action_holding) {
    /* 第一次 ramp 完成 → 进入 hold 保持态 */
    g_action_holding    = 1;
    g_action_hold_start = HAL_GetTick();
    return;   /* 不清 current,保持终点姿态 */
  }
  /* 无 hold,或 hold 结束后的回 STAND ramp 完成 */
  g_action_holding = 0;
  g_action_hold_ms = 0;
  current = NULL;
  phase   = MOTION_PHASE_DONE;
}

/* hold 到期 → 渐进回 STAND(由 motion_poll 每帧调)
 * 注意:回 STAND 也用 ramp_sit_to_target,完成后走 on_ramp_complete_idle 的
 *       "清状态"分支(g_action_hold_ms 已置 0)
 */
static void motion_hold_tick(void) {
  if (!g_action_holding) return;
  if ((HAL_GetTick() - g_action_hold_start) < g_action_hold_ms) return;

  g_action_holding = 0;
  g_action_hold_ms = 0;   /* 清掉,让回 STAND 的 ramp 完成走清状态分支 */

  uint16_t stand_pwm[8];
  for (uint8_t i = 0; i < 8; i++) stand_pwm[i] = SERVO_STEP[i].stand;
  ramp_sit_to_target(stand_pwm, STAND_RAMP_MS, on_ramp_complete_idle);
}

void motion_play_action(uint8_t id, uint32_t hold_ms) {
  /* 任何 ramp 启动前先停 stepping */
  stepping_stop();

  /* 清上一次的 hold 残留 */
  motion_hold_reset();

  if (id <= 4) {
    /* 既有动作:STAND/TROT/BOB/SHIN_TEST(hold_ms 当动作时长,语义不变) */
    motion_play_by_id(id, hold_ms);
    return;
  }
  if (id < 5 || id > 8) return;

  /* ramp 类动作不在 MOTION_TABLE 里,清掉 current 防止上一个 motion 的 tick
   * 继续跑(例如 TROT 的 start_delay 到点会启动 stepping,打断 ramp) */
  current = NULL;
  phase   = MOTION_PHASE_RUN;

  g_action_hold_ms = hold_ms;

  switch (id) {
    case ACTION_SIT_DOWN:       /* 5: 任意 → SIT_REAL */
    case ACTION_STAND_TO_SIT:   /* 8: 别名 */
      ramp_cancel();
      ramp_sit_to_target(SIT_REAL_PWM, SIT_RAMP_MS, on_ramp_complete_idle);
      break;
    case ACTION_STAND_UP:       /* 6: 任意 → STAND */
    case ACTION_SIT_TO_STAND:   /* 7: 别名 */
    {
      ramp_cancel();
      uint16_t stand_pwm[8];
      for (uint8_t i = 0; i < 8; i++) stand_pwm[i] = SERVO_STEP[i].stand;
      ramp_sit_to_target(stand_pwm, STAND_RAMP_MS, on_ramp_complete_idle);
      break;
    }
    default:
      break;
  }
}
