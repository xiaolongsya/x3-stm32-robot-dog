/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    motions.h
  * @brief   机器狗 v1 动作模块(2026-09-13 新建)
  *
  * 设计动机:
  *   - 不同动作(站立 / 踏步 / 走路 / 以后扩展)的代码全部放在 motions.c/h 里
  *   - main.c 只负责:初始化 → 注册动作 → 选一个 → 主循环轮询
  *   - 加新动作:在 MotionTable 末尾加一行 + 写一个 motion_xxx() 函数,不动 main.c
  *
  * 工作模式:
  *   - 编译时通过 MOTION_ID 宏选择要跑的动作(无通信期间用这个)
  *   - 运行时未来可扩展为 UART 命令切动作(暂不实现)
  *
  * 调度:
  *   - 每个动作有自己的 entry 函数和可选的 tick 回调(供 TIM6 ISR 调)
  *   - 动作的 setup() 在上电时跑一次,可选 delay_s(秒)后开始
  *   - 动作跑完后保持状态(不回 STAND);setup() 完成后舵机 = STAND
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

/* === 选择要跑的动作(无通信模式默认,改这里切换不同动作测试)=== */
/* 选项:
 *   MOTION_STAND  - 上电站立,什么也不做(标定/观察用)
 *   MOTION_TROT   - 上电默认站立,5s 后开始原地踏步 N 秒(无限)再回 STAND
 *   MOTION_WALK   - 上电默认站立,5s 后开始前进走 N 秒(暂未实现,预留给下一步)
 */
#ifndef MOTION_ID
#define MOTION_ID  MOTION_STAND
#endif

#define MOTION_STAND  1
#define MOTION_TROT   2
#define MOTION_WALK   3  /* TODO 预留给下一步 */

typedef enum {
  MOTION_PHASE_INIT = 0,   /* 还未到 start_delay */
  MOTION_PHASE_RUN,        /* 正在执行 */
  MOTION_PHASE_DONE        /* 已结束,保持最终状态 */
} MotionPhase;

/* === 动作函数原型 ===
 *
 * setup():主循环 main() 初始化阶段调一次
 *   - 设舵机到 STAND(基线姿态)
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

/* 注册表入口(C 数组末尾哨兵,以后加动作在 motions.c 里 append) */
extern const Motion MOTION_TABLE[];

#ifdef __cplusplus
}
#endif

#endif /* __MOTIONS_H */
