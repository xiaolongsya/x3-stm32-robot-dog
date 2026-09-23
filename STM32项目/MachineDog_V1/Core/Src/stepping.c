/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stepping.c
  * @brief   机器狗 v1 原地踏步(2026-09-14 整理)
  *
  * 算法(2026-09-17 最新版):
  *   对角 trot:phase < 0.5 一对 swing,phase >= 0.5 另一对 swing
  *   swing 腿:shin + thigh 用同一个三角波同步动作
  *     shin_offset  = triangle × STEP_TROT_OFFSET
  *     thigh_offset = shin_offset × STEP_RATIO_SHIN_TO_THIGH_X10 / 10
  *   **swing 和 support 都用 TROT_STAND 作基准**(2026-09-17 统一)
 *     swing 腿:TROT_STAND ± offset;support 腿:保持 TROT_STAND
  *
  * 抬腿模型(机械反装镜像):
  *   - 抬腿 = 小腿向狗头方向倾斜 = 右腿 PWM 减 + 左腿 PWM 增
  *   - 三角波 ramp(0→peak→0),峰值在 phase=0.5
  *   - 第一帧(phase=0)8 路全 TROT_STAND(无跳变)
 *   - 相位边界三角波归零,且 swing/support 基准相同 → 切换无跳变
 *     (2026-09-17 修:此前 swing 用 SERVO_STEP.stand、support 用 trot_stand_pwm,
 *      基准不同导致 BR/BL 小腿每个相位边界突跳 100µs)
  *
  * 参数(2026-09-14 用户拍板,可调):
  *   STEP_TROT_OFFSET      单腿摆幅(PWM,默认 500)
  *   STEP_TROT_PERIOD      1 个完整周期秒数(默认 0.25)
  *   STEP_RATIO_SHIN_TO_THIGH_X10 shin:thigh 比例 /10(默认 3,即 0.3)
  *
  * UART 命令(在 main.c parse_uart_command 注册):
  *   step trot    启动原地踏步(主循环,无 printf)
  *   step stop    停止踏步,回 STAND(主循环,无 printf)
  *   step show    调试输出(主循环,空实现)
  *
  * 腿编号约定(对角 trot):
  *   腿 1 = FR (小腿=id 2, 肩=id 3)
  *   腿 2 = FL (小腿=id 5, 肩=id 4)
  *   腿 3 = BL (小腿=id 7, 肩=id 6)
  *   腿 4 = BR (小腿=id 0, 肩=id 1)
  *
  *   phase < 0.5 :腿 1+3 swing,腿 2+4 support
  *   phase >= 0.5:腿 2+4 swing,腿 1+3 support
  *
  * STAND 含义:
  *   - 1500 = 舵机中位 = 大腿垂直 + 小腿水平(直角,部件极值位)
  *   - STAND = 4 脚贴地实测姿态(前倾,BR/BL 小腿到极限)
  *   - 6 路标准 STAND 来自用户标定,2 路(FL 肩 +100 / BL 小腿 +80)含机械偏置
  *
  * 安全:
  *   - SERVO_LIMIT clamp(见 SERVO_STEP 表,8 路按 ±350/±800 分配)
  *   - ISR 内不 printf,防 printf 卡死 main loop
  ******************************************************************************
  */
/* USER CODE END Header */

#include "stepping.h"
#include "main.h"
#include <math.h>
#include "ramp.h"   /* ramp_is_active() */

/* htim6 在 tim.c 定义,stepping.c 引用(TIM6 100Hz 步态中断) */
extern TIM_HandleTypeDef htim6;

/* === 参数(2026-09-14 用户拍板 v8:8 路线性抬腿调试版)===
 *
 * 【STEP_TROT_OFFSET】8 路同时线性 ramp 偏移幅度(PWM)
 *   右腿(STAND - offset):小腿/大腿 P 减 = 向狗头方向倾斜
 *   左腿(STAND + offset):小腿/大腿 P 增 = 向狗头方向倾斜
 *   phase=0/1:全 STAND(0 偏移),phase=0.5:全 ±offset(最大偏移)
 *   改这个值试不同抬腿幅度
 */
#define STEP_TROT_OFFSET  500

/* 【STEP_TROT_PERIOD】1 个完整 ramp 周期(秒),设 0.6 = 0.6 秒
 *   ISR 频率 100Hz,所以 phase 每 tick 增加 1/(PERIOD*100)
 *   设 0.6 → 每 tick +0.0167 → 60 ticks 一周期 → 0.6s
 *   设 1.0 → 每 tick +0.01  → 100 ticks 一周期 → 1.0s
 *   设 0.4 → 每 tick +0.025  → 40 ticks 一周期 → 0.4s
 */
#define STEP_TROT_PERIOD  0.25f

/* 【STEP_RATIO_SHIN_TO_THIGH_X10】小腿 : 大腿 = ratio : 10
 *   默认 10 = 1:1(小腿大腿同样摆幅,目前调试用)
 *   改小 → 小腿动得比大腿多
 *   改大 → 大腿动得比小腿多
 *   实际 shin's offset = STEP_TROT_OFFSET
 *       thigh's offset = STEP_TROT_OFFSET × ratio / 10
 *
 * 【0.6s 周期原则】shin 和 thigh 用同一个三角波同时完成动作
 *   不需要任何 phase delay,在 0.6s 内它们各自走完自己的轨迹
 *   delay 参数已删,如果想调整,改这个 ratio 即可
 */
#define STEP_RATIO_SHIN_TO_THIGH_X10  3

/* === 内部计算:phase 每 tick 增量 === */
#define STEP_T_INC  (1.0f / (STEP_TROT_PERIOD * 100.0f))  /* 自动算:0.6s → 0.0167 */

/* WALK:FR→FL→BL→BR,每腿 2s,完整一轮 8s。 */
#define WALK_TICKS_PER_LEG 200u
#define WALK_CYCLE_TICKS   (4u * WALK_TICKS_PER_LEG)
#define WALK_STANCE_TICKS  (WALK_CYCLE_TICKS - WALK_TICKS_PER_LEG)
#define WALK_RAMP_TICKS    100u
#define WALK_LIFT_US       500u
#define WALK_PUSH_US       150u
#define WALK_PI            3.14159265f

/* === 8 路舵机抬腿参数表 ==========================================
 *
 * 抬腿方向:右腿 PWM 减,左腿 PWM 增(抬腿 = 小腿向狗头方向倾斜)
 *
 * STAND 值全部从 main.h 的 SERVO_*_STAND 引用(单一真相源)
 * SERVO_LIMIT 根据 2026-09-14 用户拍板统一规则:
 *   - 4 小腿宽度都 = 700
 *   - 4 肩宽度都 = 1600
 * FL 肩 因为 +100 机械偏置,需要更宽限位
 * 2026-09-16:BL 小腿取消 +80 偏置,STAND=1480,实际行程 1480-2180
 *
 *   id  名称      STAND   is_right  SERVO_LIMIT(min,max)
 *   0   BR 小腿   SERVO_SHIN_BR_STAND     right     (900, 1600)
 *   1   BR 肩     SERVO_SHOULDER_BR_STAND right     (700, 2300)
 *   2   FR 小腿   SERVO_SHIN_FR_STAND     right     (900, 1600)
 *   3   FR 肩     SERVO_SHOULDER_FR_STAND right     (700, 2300)
 *   4   FL 肩     SERVO_SHOULDER_FL_STAND left      (800, 2400)
 *   5   FL 小腿   SERVO_SHIN_FL_STAND     left      (1400, 2100)
 *   6   BL 肩     SERVO_SHOULDER_BL_STAND left      (700, 2300)
 *   7   BL 小腿   SERVO_SHIN_BL_STAND     left      (1480, 2180)
 */
/* stepping.h 里 typedef + extern,这里定义实体 */
const ServoStep SERVO_STEP[8] = {
  /*0  BR 小腿 */ {SERVO_SHIN_BR_STAND,      1, 900,  1600},
  /*1  BR 肩   */ {SERVO_SHOULDER_BR_STAND,  1, 700,  2300},
  /*2  FR 小腿 */ {SERVO_SHIN_FR_STAND,      1, 900,  1600},
  /*3  FR 肩   */ {SERVO_SHOULDER_FR_STAND,  1, 700,  2300},
  /*4  FL 肩   */ {SERVO_SHOULDER_FL_STAND,  0, 800,  2400},
  /*5  FL 小腿 */ {SERVO_SHIN_FL_STAND,      0, 1400, 2100},
  /*6  BL 肩   */ {SERVO_SHOULDER_BL_STAND,  0, 700,  2300},
  /*7  BL 小腿 */ {SERVO_SHIN_BL_STAND,      0, 1480, 2180},
};

/* === 状态 =========================================================*/
static volatile SteppingState step_state = STEPPING_IDLE;
static volatile float step_t_phase = 0.0f;
static volatile uint16_t walk_tick = 0;
static volatile uint16_t walk_elapsed_ticks = 0;
static volatile int8_t walk_dir = 1;

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

/* === 应用 TROT_STAND 到 8 路舵机(2026-09-17 重整)===
 *
 * 与 SERVO_STEP[i].stand 的区别:
 *   - STAND:正站立(BL 小腿 2026-09-16 实测 1580→1480)
 *   - TROT_STAND:BR shin 改中立(STAND=1600=MAX 是前倾,改 1500 中立避免水平分量干扰 trot)
 *     - BR shin:1500(中立)
 *     - BL shin:1580(中立 = 1500 + 80 **物理偏移**,2026-09-17 改)
 *       此前写 1500 是漏了偏移;BL 的"中立"要与其他小腿的 1500 物理等价
 * 其他 6 路用 STAND 不动
 *
 * 用于 trot 起踏/停踏瞬间,身体不前倾
 *
 * ⚠️ 本表与 motions.c 的同名表是**两份拷贝**,改一处必须同步另一处
 */
static const uint16_t trot_stand_pwm[8] = {
  1500,                          /* 0  BR 小腿(STAND=1600=MAX,改中立) */
  SERVO_SHOULDER_BR_STAND,       /* 1  BR 肩(=STAND 1100) */
  SERVO_SHIN_FR_STAND,           /* 2  FR 小腿(=STAND 1600) */
  SERVO_SHOULDER_FR_STAND,       /* 3  FR 肩(=STAND 1100) */
  SERVO_SHOULDER_FL_STAND,       /* 4  FL 肩(=STAND 2000) */
  SERVO_SHIN_FL_STAND,           /* 5  FL 小腿(=STAND 1400) */
  SERVO_SHOULDER_BL_STAND,       /* 6  BL 肩(=STAND 1900) */
  SERVO_NEUTRAL_BL_SHIN,         /* 7  BL 小腿(1580 = 1500+80 偏移,2026-09-17 改) */
};

static void stepping_apply_trot_stand(void) {
  for (uint8_t i = 0; i < 8; i++) {
    set_servo_pulse(i, trot_stand_pwm[i]);
  }
}

/* === 单步执行:推进相位 + 算 8 路 PWM ==============================
 *
 * ⚠️ TIM6 ISR 调用 — 不能 printf,只 set_servo_pulse + 写 dbg
 *
 * 算法(2026-09-14 v10:对角 trot,正常踏步模式):
 *   phase < 0.5 :腿 1+3 swing (FR id 2,3 + BL id 6,7)
 *                腿 2+4 support (FL id 4,5 + BR id 0,1) — 保持 TROT_STAND
 *   phase >= 0.5:腿 2+4 swing (FL id 4,5 + BR id 0,1)
 *                腿 1+3 support (FR id 2,3 + BL id 6,7) — 保持 TROT_STAND
 *
 *   swing 腿: shin + thigh 用同一个三角波同时动作
 *     shin_offset  = triangle × STEP_TROT_OFFSET
 *     thigh_offset = shin_offset × STEP_RATIO_SHIN_TO_THIGH_X10 / 10
 *
 *   support 腿: 保持 TROT_STAND(中立位)
 *
 *   第一帧(phase=0): 全 0 → 全 TROT_STAND(无跳变)
 *   peak (phase=0.5): swing 对角腿 ±offset,support 腿 STAND
 */
static void stepping_trot_step(void) {
  /* phase 0~1 完整 ramp 周期,0→0.5 抬腿,0.5→1 落腿 */
  uint8_t swing_mask;
  float phase_in_swing;

  if (step_t_phase < 0.5f) {
    phase_in_swing = step_t_phase * 2.0f;
    swing_mask = (1u << 2) | (1u << 3) | (1u << 6) | (1u << 7);
  } else {
    phase_in_swing = (step_t_phase - 0.5f) * 2.0f;
    swing_mask = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5);
  }

  float triangle = 1.0f - 2.0f * fabsf(phase_in_swing - 0.5f);

  for (uint8_t id = 0; id < 8; id++) {
    int16_t pwm;
    int16_t this_offset;

    if (id == 1 || id == 3 || id == 4 || id == 6) {
      /* 大腿:用 ratio 缩放后的偏移,跟小腿同三角波 */
      this_offset = (int16_t)(triangle * (float)STEP_TROT_OFFSET
                              * (float)STEP_RATIO_SHIN_TO_THIGH_X10 / 10.0f + 0.5f);
    } else {
      /* 小腿 */
      this_offset = (int16_t)(triangle * (float)STEP_TROT_OFFSET + 0.5f);
    }

    if (swing_mask & (1u << id)) {
      /* Swing 腿:右腿 P 减(向狗头方向倾斜/抬腿),左腿 P 增(镜像)
       *
       * 2026-09-17 修:基准从 SERVO_STEP[id].stand( STAND 值)改成
       *   trot_stand_pwm[id](TROT 中立值)—— 与 support 分支统一。
       *
       * 修前问题:swing 用 STAND、support 用 TROT_STAND,两者不相等,
       *   而三角波在相位边界归零 → 每次 swing↔support 切换都突跳
       *   |STAND − TROT_STAND|:BR 小腿 100µs、BL 小腿 100µs,每踏一步抖两下。
       *   统一基准后跳变数学上归零(boundary 两侧都是 trot_stand_pwm[id])。
       *
       * 附带好处:BR 小腿的 STAND=1600 是"前倾"值,原先 swing 绕它摆动会
       *   带着前倾分量;改用中立 1500 后与 support 一致,不再抖动。
       */
      if (SERVO_STEP[id].is_right) {
        pwm = (int16_t)trot_stand_pwm[id] - this_offset;
      } else {
        pwm = (int16_t)trot_stand_pwm[id] + this_offset;
      }
    } else {
      /* Support 腿:保持 TROT_STAND(中立位) */
      pwm = (int16_t)trot_stand_pwm[id];
    }

    uint16_t clamped = stepping_clamp_pwm(id, pwm);
    set_servo_pulse(id, clamped);
    dbg_pwm[id] = clamped;
  }

  dbg_phase = step_t_phase;
}

/* 每条腿有 2s 摆动、6s 支撑。摆动时抬小腿并将肩向行进方向送脚;
 * 支撑时小腿落地,肩沿反方向缓慢推地。肩在摆动/支撑边界连续。
 * 启动前 1s 逐渐增加摆幅,避免从 STAND 突跳到周期中的肩位置。
 */
static float walk_smoothstep(float t) {
  return t * t * (3.0f - 2.0f * t);
}

static void stepping_walk_step(void) {
  static const uint8_t swing_shin[4] = {2, 5, 7, 0}; /* FR FL BL BR */
  static const uint8_t swing_shoulder[4] = {3, 4, 6, 1};
  float ramp = (walk_elapsed_ticks < WALK_RAMP_TICKS)
                 ? (float)walk_elapsed_ticks / (float)WALK_RAMP_TICKS : 1.0f;

  for (uint8_t leg = 0; leg < 4; leg++) {
    uint16_t phase = (uint16_t)((walk_tick + WALK_CYCLE_TICKS
                         - (uint16_t)leg * WALK_TICKS_PER_LEG) % WALK_CYCLE_TICKS);
    uint8_t shin_id = swing_shin[leg];
    uint8_t shoulder_id = swing_shoulder[leg];
    int16_t lift = 0;
    float shoulder_pos;
    if (phase < WALK_TICKS_PER_LEG) {
      float t = (float)phase / (float)(WALK_TICKS_PER_LEG - 1u);
      float wave = sinf(WALK_PI * t);
      lift = (int16_t)(wave * wave * (float)WALK_LIFT_US * ramp + 0.5f);
      shoulder_pos = -1.0f + 2.0f * walk_smoothstep(t);
    } else {
      float t = (float)(phase - WALK_TICKS_PER_LEG)
                / (float)(WALK_STANCE_TICKS - 1u);
      shoulder_pos = 1.0f - 2.0f * walk_smoothstep(t);
    }

    int16_t signed_shoulder = (int16_t)(shoulder_pos * (float)WALK_PUSH_US
                                        * (float)walk_dir * ramp);
    int16_t shin_pwm = (int16_t)SERVO_STEP[shin_id].stand
                       + (SERVO_STEP[shin_id].is_right ? -lift : lift);
    int16_t shoulder_pwm = (int16_t)SERVO_STEP[shoulder_id].stand
                           + (SERVO_STEP[shoulder_id].is_right
                              ? signed_shoulder : -signed_shoulder);
    dbg_pwm[shin_id] = stepping_clamp_pwm(shin_id, shin_pwm);
    dbg_pwm[shoulder_id] = stepping_clamp_pwm(shoulder_id, shoulder_pwm);
    set_servo_pulse(shin_id, dbg_pwm[shin_id]);
    set_servo_pulse(shoulder_id, dbg_pwm[shoulder_id]);
  }
  dbg_phase = (float)walk_tick / (float)WALK_CYCLE_TICKS;
  walk_tick = (uint16_t)((walk_tick + 1u) % WALK_CYCLE_TICKS);
  if (walk_elapsed_ticks < WALK_RAMP_TICKS) walk_elapsed_ticks++;
}

/* === 接口实现 =====================================================*/

void stepping_init(void) {
  /* 在 HAL_TIM_PWM_Start 之后调;不启动 TIM6,需 stepping_start_trot 才开 */
  step_state = STEPPING_IDLE;
  step_t_phase = 0.0f;
  /* 不应用 STAND,由 main() 初始化时直接写 STAND (沿用旧模式) */
}

void stepping_start_trot(void) {
  /* 主循环调用 — 不 printf (2026-09-12 防 printf 阻塞 UART 卡死 main loop) */
  /* 2026-09-13:用 TROT_STAND(中立位)替代前倾 STAND,起踏更稳 */
  /* 如果 ramp 活跃，完全不能启动 stepping */
  if (ramp_is_active()) {
    return;
  }

  stepping_apply_trot_stand();
  step_state = STEPPING_TROT;
  step_t_phase = 0.0f;

  if (HAL_TIM_Base_Start_IT(&htim6) != HAL_OK) {
    step_state = STEPPING_IDLE;
    return;
  }
}

void stepping_start_walk(int8_t dir) {
  if (dir == 0) {
    stepping_start_trot();
    return;
  }
  if ((dir != 1 && dir != -1) || ramp_is_active()) return;

  stepping_apply_stand();
  walk_dir = dir;
  walk_tick = 0;
  walk_elapsed_ticks = 0;
  step_state = STEPPING_WALK;
  if (HAL_TIM_Base_Start_IT(&htim6) != HAL_OK) {
    step_state = STEPPING_IDLE;
  }
}

void stepping_stop(void) {
  /* 主循环调用 — 不 printf (2026-09-12 防 printf 阻塞 UART 卡死 main loop) */
  HAL_TIM_Base_Stop_IT(&htim6);
  step_state = STEPPING_IDLE;
  step_t_phase = 0.0f;
  walk_tick = 0;
  walk_elapsed_ticks = 0;
  stepping_apply_stand();
}

void stepping_tick(void) {
  /* ⚠️ TIM6 ISR 调用 — 不能 printf */
  if (step_state != STEPPING_TROT && step_state != STEPPING_WALK) return;

  /* 如果 ramp 活跃，立即停止 stepping（蹲下时踏步必须停） */
  if (ramp_is_active()) {
    step_state = STEPPING_IDLE;
    return;
  }

  if (step_state == STEPPING_WALK) {
    stepping_walk_step();
  } else {
    step_t_phase += STEP_T_INC;
    if (step_t_phase >= 1.0f) step_t_phase -= 1.0f;
    stepping_trot_step();
  }
}

SteppingState stepping_get_state(void) {
  return step_state;
}

void stepping_show(void) {
  /* 调试接口(主循环) — 临时禁用 printf (2026-09-12 防阻塞 main loop)
   * 修 usart.c __io_putchar 短 timeout 后可恢复:
   *   printf("phase=%.3f h=%.2fmm pwm=[%u,%u,%u,%u,%u,%u,%u,%u]\n",
   *          dbg_phase, dbg_h_mm,
   *          dbg_pwm[0], dbg_pwm[1], dbg_pwm[2], dbg_pwm[3],
   *          dbg_pwm[4], dbg_pwm[5], dbg_pwm[6], dbg_pwm[7]);
   */
}
