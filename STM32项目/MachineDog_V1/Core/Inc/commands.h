/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    commands.h
  * @brief   STM32 ↔ X3 二进制命令协议(精简版,2026-09-14)
  *
  * 帧格式:
  *   [0] 0xAA   帧头
  *   [1] 0x55   帧头
  *   [2] len    data 字节数 (0..255)
  *   [3] cmd    命令码 (1B)
  *   [4..4+len-1] data
  *   [4+len]    csum8 = XOR(len, cmd, data[0..len-1])
  *
  * 响应(ACK):
  *   [0] 0xAA  [1] 0x55  [2] len  [3] (cmd | 0x80)  [4] status  [data...]  [csum8]
  *   status: 0=OK  1=CRC_ERR  2=BAD_CMD  3=BAD_PARAM  4=BAD_LEN
  *
  * 命令集(6 个 + 兼容文本):
  *   0x01 MOTION_PLAY    data: [id u8, duration_ms u32 LE]
  *                       id=1..4 → motions 表;duration_ms=0 表示无限(到 HEARTBEAT 停止)
  *   0x03 SET_PWM        data: [(id u8, pulse u16 LE) * N], N = len/3
  *   0x05 HEARTBEAT      data: []  (仅用于 reset watchdog)
  *   0x06 EMERGENCY_STOP data: []  (立即回 STAND,等价 MOTION_PLAY id=1)
  *   0x07 ~~BUZZER_ON~~   data: ~~[freq_hz u16 LE, dur_ms u16 LE]~~
  *                       2026-09-16 作废,X3 不发,STM32 收到回 BAD_CMD(2)
  *   0x08 ~~BUZZER_OFF~~  data: ~~[]  (立即停蜂鸣器)~~
  *                       2026-09-16 作废,同上
  *   0x09 ACTION_PLAY    data: [action_id u8, repeat u8, hold_ms u16 LE]
  *                       action_id: 5=SIT_DOWN 6=STAND_UP 7=SIT_TO_STAND 8=STAND_TO_SIT
  *                                  (1..4 走 MOTION_PLAY 命令,本命令不处理)
  *                       repeat:     忽略(由 X3 端循环调用实现)
  *                       hold_ms:    ramp 完成后保持时长,2026-09-17 加
  *                                   = 0 → 保持终点姿态不回 STAND
  *                                   > 0 → 保持 hold_ms 后渐进回 STAND
  *                                   (旧格式 len==2 无此字段,按 0 处理)
  *
  * 默认动作:MOTION_STAND(上电后什么都不动,X3 不发命令狗也不会自己跑)
  *
  * 接收策略:DMA + IDLE 中断,收满后 commands_poll() 在 main loop 里 dispatch
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __COMMANDS_H
#define __COMMANDS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 接收缓冲(USART1 DMA + IDLE,够长以防一帧数据多)
 *
 * ⚠️ 约束(2026-09-17 核查 L3,记录在案):
 *   接收用 HAL_UARTEx_ReceiveToIdle_DMA + DMA_NORMAL(不是 CIRCULAR),
 *   且 HAL_UARTEx_RxEventCallback 里 HAL_UART_DMAStop() 关掉自动重启,
 *   由 commands_poll() 处理完再显式重启。
 *
 *   含义:两次 poll 之间 DMA 是停的,这一窗口内到达的字节不会被收。
 *   当前**安全** —— X3 端 dog_uart.cmd() 是 send+等 ACK 串行,每帧 5~10 字节,
 *   两帧间隔远大于主循环周期,不会溢出 64 字节。
 *
 *   若将来改成"一次 burst 发 N 帧不等 ACK":N 帧字节数会超过 64,
 *   且 DMA 重启前到达的字节会丢 → 必须同步改成 DMA_CIRCULAR +
 *   commands_poll() 追读差量。**改 burst 前先改这里**,否则会静默丢帧。
 */
#define COMMANDS_RX_BUF_SIZE   64

void     commands_init(void);
void     commands_poll(void);   /* main loop 调用,处理已收到的帧 */

/* 主动发送接口(目前供 ACK 用) */
void     commands_send_ack(uint8_t cmd, uint8_t status, const uint8_t *data, uint8_t len);
void     commands_send_text(const char *s);  /* 调试用,纯文本无帧头 */

#ifdef __cplusplus
}
#endif

#endif /* __COMMANDS_H */
