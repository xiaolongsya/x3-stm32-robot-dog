/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    motions.h
  * @brief   机器狗 v1 动作模块(2026-09-14 整理)
  *
  * 设计动机:
  *   - 不同动作(站立 / 踏步 / 蹲起循环 / 小腿测试)的代码全部放 motions.c/h 里
  *   - main.c 只负责:初始化 → 注册动作 → 选一个 → 主循环轮询
  *   - 加新动作:在 MotionTable 末尾加一行 + 写 motion_xxx() 函数,不动 main.c
  *
  * 工作模式:
  *   - 编译时通过 MOTION_ID 宏选择要跑的动作(无通信期间用这个)
  *   - 运行时未来可扩展为 UART 命令切动作(暂不实现)
  *
  * 调度:
  *   - 每个动作有自己的 setup() 和可选的 tick() 回调(供 TIM6 ISR 调)
  *   - 动作的 setup() 在上电时跑一次,可选 delay_s(秒)后开始
  *   - 动作跑完后保持状态(不回 STAND)
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __MOTIONS_H
#define __MOTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>

/* === 选择要跑的动作(无通信模式默认,改这里切换)=== */
/* 4 个动作(2026-09-14 整理):
 *   MOTION_STAND  - 上电跳 STAND 然后保持(标定/观察用)
 *   MOTION_TROT   - 上电跳 TROT_STAND → start_delay_s 后启动踏步
 *   MOTION_BOB    - 上电跳 STAND → hold 5s → 蹲下 → hold 5s → 循环
 *   MOTION_SHIN_TEST - 8 路同步线性 ramp 测小腿范围(找最大值)
 *
 * 默认 MOTION_STAND(2026-09-14:X3 上线后上电啥都不动,等 X3 命令);
 * 改这里切其他动作(无 X3 时调试用):
 *   #define MOTION_ID  MOTION_TROT      // 上电就踏步
 *   #define MOTION_ID  MOTION_BOB       // 蹲起循环
 *   #define MOTION_ID  MOTION_SHIN_TEST // 小腿测试
 */
#ifndef MOTION_ID
#define MOTION_ID  MOTION_STAND
#endif

#define MOTION_STAND     1
#define MOTION_TROT      2
#define MOTION_BOB       3  /* 站立↔蹲下 循环 */
#define MOTION_SHIN_TEST 4  /* 小腿范围测试 */

/* === ACTION_PLAY (0x09) 高级动作 id (2026-09-16 加)== */
#define ACTION_SIT_DOWN       5   /* 任意 → SIT_REAL 渐进 800ms */
#define ACTION_STAND_UP       6   /* 任意 → STAND 渐进 800ms */
#define ACTION_SIT_TO_STAND   7   /* SIT_TO_STAND 别名 = STAND_UP */
#define ACTION_STAND_TO_SIT   8   /* STAND_TO_SIT 别名 = SIT_DOWN */

/* === 小腿范围测试参数(2026-09-13 临时)===========================
 * 改这个值烧录 → 4 条小腿同时偏移 → 看哪个先堵转 → 找极限
 * 正值 = 小腿向"收"方向;负值 = 向"伸"方向
 * 右小腿(BR id 0, FR id 2):1500 - offset(P 减 = 收)
 * 左小腿(FL id 5, BL id 7):1500 + offset(P 增 = 收,镜像)
 * 2026-09-16:BL 小腿取消 +80 偏置,与其他小腿对齐,纯 1500 ± offset
 */
#define TEST_SHIN_OFFSET  600

typedef enum {
  MOTION_PHASE_INIT = 0,   /* 还未到 start_delay */
  MOTION_PHASE_RUN,        /* 正在执行 */
  MOTION_PHASE_DONE        /* 已结束,保持最终状态 */
} MotionPhase;

/* === 动作函数原型 ===
 *
 * setup():主循环 main() 初始化阶段调一次
 *   - 设舵机到 STAND/TROT_STAND 等基线姿态
 *   - 准备定时器 / 状态变量
 *   - 允许有 start_delay 秒的"先站立再动"逻辑
 *
 * tick():TIM6 ISR 调(100Hz),推进动作相位;不需要时可置 NULL
 *   - 严禁 printf(阻塞 UART)
 *
 * 返回 phase:主循环 / debug 用
 */
typedef struct {
  uint8_t        id;
  const char    *name;
  void         (*setup)(void);   /* 一次 */
  void         (*tick)(void);    /* 每 10ms(可选) */
  uint16_t       start_delay_s; /* 上电后多久开始 */
  uint16_t       duration_s;   /* 持续多久(0=无限) */
} Motion;

MotionPhase motion_get_phase(void);
const char *motion_get_name(void);

/* === 运行时切动作(X3 上线后用,2026-09-14 加)===
 * 切换到指定 id 的动作,duration_ms=0 表示无限(等下一次切 / watchdog 切回)
 * 实现细节:沿用 motion_init() 的状态机,但允许带运行时参数
 */
void motion_play_by_id(uint8_t id, uint32_t duration_ms);

/* === ACTION_PLAY 入口 (2026-09-16 加)===
 * id 1..4: 走 motion_play_by_id()(既有)
 * id 5..8: ramp 动作(本函数处理)
 * duration_ms >0 → ramp 时长(ms);=0 → SIT_RAMP_MS 默认
 */
void motion_play_action(uint8_t id, uint32_t duration_ms);

/* === 调度接口(主循环) ===
 *
 * motion_init():
 *   - 在 HAL_TIM_PWM_Start 之后调一次
 *   - 写 8 路到 SERVO_NEUTRAL_US(1500) 作为安全起点
 *   - 装载 MOTION_ID 选中的动作,调其 setup() 启动
 *
 * motion_poll():
 *   - main while(1) 开头调一次
 *   - 内部顺序:ramp_tick() → current->tick()(如果当前有 ramp)
 *   - ramp_tick 推进当前活跃的 RAMP,完成后切 RAMP_DONE
 *   - current->tick 推进各动作的状态机(BOB / TROT)
 */
void motion_init(void);
void motion_poll(void);

/* 注册表入口(C 数组末尾哨兵,以后加动作在 motions.c 里 append) */
extern const Motion MOTION_TABLE[];

#ifdef __cplusplus
}
#endif

#endif /* __MOTIONS_H */
