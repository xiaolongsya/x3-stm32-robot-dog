/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    motions.c
  * @brief   机器狗 v1 动作实现(2026-09-13 新建)
  *
  * 当前注册动作(无通信模式编译时通过 MOTION_ID 切换):
  *   - STAND:占位,啥也不做,舵机保持上电默认 STAND
  *   - TROT :上电站立 N 秒 → 启动 stepping trot(走回 2026-09-12 实现的步态)
  *   - WALK :TODO 上电站立 → 前进走步(预留)
  *
  * 加新动作:
  *   1. 写 static void motion_xxx_setup(void) / static void motion_xxx_tick(void)
  *   2. 在 MOTION_TABLE 末尾加一行 { MOTION_XXX, "xxx", motion_xxx_setup, motion_xxx_tick, 5, 0 }
  *   3. 在 motions.h 的 MOTION_ID 默认改成你的 ID
  ******************************************************************************
  */
/* USER CODE END Header */

#include "motions.h"
#include "stepping.h"
#include "main.h"

/* htim6 在 tim.c 定义,stepping.c 也在用,这里 extern */
extern TIM_HandleTypeDef htim6;

/* ============================================================
 * 状态:主循环维护,ISR 不直接读
 * ============================================================ */
static MotionPhase phase = MOTION_PHASE_INIT;
static uint32_t    boot_tick_ms = 0;
static const Motion *current = NULL;

/* ============================================================
 * 各动作实现
 * ============================================================ */

/* --- STAND:啥也不做,保持上电默认 STAND --- */
static void motion_stand_setup(void) {
  /* 主循环初始化时已写 STAND,这里不做任何事 */
}
static void motion_stand_tick(void) { }

/* --- TROT:先站 N 秒,再启动 stepping trot ---
 *
 * 注意:stepping.c 的 stepping_start_trot() 会:
 *   1. 写 STAND 到 8 路
 *   2. 启 TIM6 ISR(100Hz)
 *   3. ISR 内 stepping_tick() 自动推进相位
 * 这里只是延迟调用它。
 */
static uint32_t trot_started_ms = 0;

static void motion_trot_setup(void) {
  trot_started_ms = 0;
}
static void motion_trot_tick(void) {
  if (current == NULL) return;
  if (phase != MOTION_PHASE_RUN) return;

  uint32_t now = HAL_GetTick();
  if (trot_started_ms == 0 && (now - boot_tick_ms) >= (uint32_t)current->start_delay_s * 1000u) {
    /* 到点了:启动 trot */
    stepping_start_trot();
    trot_started_ms = now;
  }
}

/* --- WALK:TODO --- */
static void motion_walk_setup(void) { }
static void motion_walk_tick(void)  { }

/* ============================================================
 * 动作注册表
 * ============================================================ */
const Motion MOTION_TABLE[] = {
  { MOTION_STAND, "stand", motion_stand_setup, motion_stand_tick, 0,  0 },
  { MOTION_TROT,  "trot",  motion_trot_setup,  motion_trot_tick,  10, 0 },  /* 10s 后开始踏步,无限 */
  { MOTION_WALK,  "walk",  motion_walk_setup,  motion_walk_tick,  10, 0 },  /* TODO */
  { 0, NULL, NULL, NULL, 0, 0 }  /* 哨兵 */
};

/* ============================================================
 * 对外接口
 * ============================================================ */
MotionPhase motion_get_phase(void) { return phase; }
const char *motion_get_name(void)  { return current ? current->name : "(none)"; }

/* 主循环初始化阶段调一次 */
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
    /* 默认站 */
    current = &MOTION_TABLE[0];
  }

  if (current->setup) current->setup();
  phase = MOTION_PHASE_RUN;
}

/* 主循环调一次(放 main while 开头);负责相位推进 + 启动延迟 */
void motion_poll(void) {
  if (current == NULL || current->tick == NULL) return;
  current->tick();
}
