/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    commands.c
  * @brief   STM32 ↔ X3 二进制命令解析 + 文本命令兼容期(2026-09-14)
  *
  * 接收流程:
  *   1) commands_init() 启动 USART1 DMA + IDLE 中断
  *   2) 每次 IDLE 中断把已收到的字节数和缓冲交给 commands.c
  *   3) main loop 里 commands_poll() 把累积字节切成帧 → 派发
  *
  * 文本模式(兼容期):
  *   默认 binary。收到文本 "mode text\n" 后切到 text,"mode bin\n" 切回
  *   text 模式沿用 main.c 旧的 parse_uart_command 逻辑(这里直接 include 调用)
  ******************************************************************************
  */
/* USER CODE END Header */

#include "commands.h"
#include "main.h"
#include "usart.h"
#include "stepping.h"
#include "motions.h"
#include "watchdog.h"
#include <string.h>
#include <stdio.h>

/* === 接收缓冲(USART1 DMA 直接写这里) === */
static uint8_t  rx_buf[COMMANDS_RX_BUF_SIZE];
static volatile uint16_t rx_len = 0;   /* IDLE 时填,主循环读 */

/* === 模式切换(binary / text)===
 * 默认 binary(2026-09-14 拍板)。text 模式仅供你手敲调试,X3 上线后永远 binary
 */
typedef enum { MODE_BIN = 0, MODE_TEXT = 1 } CmdMode;
static CmdMode  g_mode = MODE_BIN;

/* === 文本命令解析(从 main.c 搬过来,行为不变)===
 * "all <pulse>" / "step trot" / "step stop" / "step show"
 * "center" / "stand" / "sit" / "<id> <pulse>"
 * 兼容期内继续工作
 */
static void parse_text_command(const char *cmd);
#include <stdlib.h>  /* sscanf */

/* === 文本命令解析实现(从旧 main.c 搬)===
 * 注意:set_servo_pulse 在 main.h 声明
 */
static void parse_text_command(const char *cmd) {
  unsigned int id = 0, pulse = 0;
  if (strncmp(cmd, "all ", 4) == 0) {
    if (sscanf(cmd + 4, "%u", &pulse) == 1) {
      if (pulse >= 500 && pulse <= 2500) {
        for (uint8_t i = 0; i < 8; i++) set_servo_pulse(i, (uint16_t)pulse);
        commands_send_text("OK all\n");
      } else {
        commands_send_text("ERR pulse range\n");
      }
    }
  } else if (strcmp(cmd, "step trot") == 0) {
    stepping_start_trot();
    commands_send_text("OK trot\n");
  } else if (strcmp(cmd, "step stop") == 0) {
    stepping_stop();
    commands_send_text("OK stop\n");
  } else if (strcmp(cmd, "step show") == 0) {
    stepping_show();
  } else if (strcmp(cmd, "center") == 0) {
    /* 2026-09-17:改用 8 路"标准值"(含 FL 肩 +100 / BL 小腿 +80 物理偏移),
     * 不再统一写 1500 —— 偏移在任何姿态下都存在 */
    motion_apply_neutral();
    commands_send_text("OK center\n");
  } else if (strcmp(cmd, "stand") == 0) {
    set_servo_pulse(0, SERVO_SHIN_BR_STAND);
    set_servo_pulse(1, SERVO_SHOULDER_BR_STAND);
    set_servo_pulse(2, SERVO_SHIN_FR_STAND);
    set_servo_pulse(3, SERVO_SHOULDER_FR_STAND);
    set_servo_pulse(4, SERVO_SHOULDER_FL_STAND);
    set_servo_pulse(5, SERVO_SHIN_FL_STAND);
    set_servo_pulse(6, SERVO_SHOULDER_BL_STAND);
    set_servo_pulse(7, SERVO_SHIN_BL_STAND);
    commands_send_text("OK stand\n");
  } else if (strcmp(cmd, "sit") == 0) {
    /* 2026-09-17:同 center,用 8 路标准值(含物理偏移) */
    motion_apply_neutral();
    commands_send_text("OK sit\n");
  } else if (strcmp(cmd, "mode text") == 0) {
    g_mode = MODE_TEXT;
    commands_send_text("OK mode=text\n");
  } else if (strcmp(cmd, "mode bin") == 0) {
    g_mode = MODE_BIN;
    commands_send_text("OK mode=bin\n");
  } else if (sscanf(cmd, "%u %u", &id, &pulse) == 2) {
    if (id <= 7) {
      set_servo_pulse((uint8_t)id, (uint16_t)pulse);
      char buf[24]; snprintf(buf, sizeof(buf), "OK s%u=%u\n", id, pulse);
      commands_send_text(buf);
    } else {
      commands_send_text("ERR id>7\n");
    }
  } else {
    char buf[64]; snprintf(buf, sizeof(buf), "ERR fmt: '%s'\n", cmd);
    commands_send_text(buf);
  }
}

/* === CRC8:XOR 所有 len+cmd+data === */
static uint8_t csum8(const uint8_t *p, uint8_t n) {
  uint8_t c = 0;
  for (uint8_t i = 0; i < n; i++) c ^= p[i];
  return c;
}

/* === 发送帧(二进制 ACK)===
 * 阻塞发送,USART1 115200 一帧 7~20 字节约 0.6~1.8ms,不影响实时性
 */
static void send_frame(uint8_t cmd, const uint8_t *data, uint8_t len) {
  uint8_t hdr[4] = { 0xAA, 0x55, len, cmd };
  uint8_t cs    = csum8(hdr + 2, 2);  /* XOR len + cmd */
  if (data && len) cs ^= csum8(data, len);
  HAL_UART_Transmit(&huart1, hdr, 4, HAL_MAX_DELAY);
  if (data && len) HAL_UART_Transmit(&huart1, (uint8_t*)data, len, HAL_MAX_DELAY);
  HAL_UART_Transmit(&huart1, &cs, 1, HAL_MAX_DELAY);
}

void commands_send_ack(uint8_t cmd, uint8_t status, const uint8_t *data, uint8_t len) {
  /* ACK 的 cmd = 原 cmd | 0x80,data 第 0 字节是 status */
  uint8_t ack_cmd = cmd | 0x80;
  uint8_t buf[32];
  if (len + 1 > sizeof(buf)) { len = sizeof(buf) - 1; }
  buf[0] = status;
  if (data && len) memcpy(buf + 1, data, len);
  send_frame(ack_cmd, buf, len + 1);
}

void commands_send_text(const char *s) {
  if (!s) return;
  HAL_UART_Transmit(&huart1, (uint8_t*)s, (uint16_t)strlen(s), HAL_MAX_DELAY);
}

/* === 处理一帧二进制数据 === */
static void dispatch_frame(const uint8_t *f, uint8_t total_len) {
  /* total_len 至少 5 (AA 55 len cmd csum) */
  if (total_len < 5) return;
  uint8_t len = f[2];
  uint8_t cmd = f[3];
  uint8_t expected_total = (uint8_t)(4 + len + 1);
  if (expected_total != total_len) {
    commands_send_ack(cmd, 4, NULL, 0);  /* BAD_LEN */
    return;
  }
  /* CRC 校验:对 len+cmd+data 算 XOR */
  uint8_t cs = csum8(f + 2, (uint8_t)(2 + len));
  if (cs != f[4 + len]) {
    commands_send_ack(cmd, 1, NULL, 0);  /* CRC_ERR */
    return;
  }

  /* 2026-09-15:收到合法帧立刻回 "OK\n" 给 X3 监听脚本看(诊断用)
   * 2026-09-17 修 L2:默认关闭 —— 每帧多 3 字节 + 一次阻塞 HAL_UART_Transmit,
   *   X3 端 read_ack 靠帧头 0xAA55 定位,本来也不依赖这段文本。
   *   调试时在 CubeIDE 的 Preprocessor 里定义 COMMANDS_DEBUG_ACK_TEXT 即可恢复。
   */
#ifdef COMMANDS_DEBUG_ACK_TEXT
  HAL_UART_Transmit(&huart1, (uint8_t*)"OK\n", 3, 100);
#endif

  const uint8_t *d = f + 4;
  switch (cmd) {
    case 0x01: {  /* MOTION_PLAY: [id u8, duration_ms u32 LE] = 5 bytes */
      if (len != 5) { commands_send_ack(cmd, 3, NULL, 0); return; }
      uint8_t  id = d[0];
      uint32_t dur = (uint32_t)d[1] | ((uint32_t)d[2] << 8)
                    | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
      /* id:1=STAND 2=TROT 3=BOB 4=SHIN_TEST(对 motions.c 表) */
      if (id < 1 || id > 4) { commands_send_ack(cmd, 3, NULL, 0); return; }
      /* 直接调对应 motion 的 setup,避开编译时 MOTION_ID */
      motion_play_by_id(id, dur);
      commands_send_ack(cmd, 0, NULL, 0);
      watchdog_reset();
      break;
    }
    case 0x03: {  /* SET_PWM: [(id u8, pulse u16 LE) * N],N = len/3 */
      if (len == 0 || (len % 3) != 0) { commands_send_ack(cmd, 3, NULL, 0); return; }
      uint8_t n = len / 3;
      if (n > 8) n = 8;
      for (uint8_t i = 0; i < n; i++) {
        uint8_t  id = d[i*3 + 0];
        uint16_t pulse = (uint16_t)d[i*3 + 1] | ((uint16_t)d[i*3 + 2] << 8);
        if (id > 7 || pulse < 500 || pulse > 2500) {
          commands_send_ack(cmd, 3, NULL, 0); return;
        }
        set_servo_pulse(id, pulse);
      }
      commands_send_ack(cmd, 0, NULL, 0);
      watchdog_reset();
      break;
    }
    case 0x05: {  /* HEARTBEAT */
      watchdog_reset();
      commands_send_ack(cmd, 0, NULL, 0);
      break;
    }
    case 0x06: {  /* EMERGENCY_STOP */
      stepping_stop();
      motion_play_by_id(1, 0);  /* STAND,无限 */
      commands_send_ack(cmd, 0, NULL, 0);
      watchdog_reset();
      break;
    }
    /* 2026-09-16:0x07/0x08 蜂鸣器命令作废,代码移除 */
    case 0x09: {  /* ACTION_PLAY: [action_id u8, repeat u8, hold_ms u16 LE]
                  * action_id: 5=SIT_DOWN 6=STAND_UP 7=SIT_TO_STAND 8=STAND_TO_SIT
                  * repeat:     忽略(协议层不实现,由 X3 端循环调用)
                  * hold_ms:    ramp 完成后保持时长(2026-09-17 加)
                  *             0 = 无限保持(不回 STAND);>0 = 保持后渐进回 STAND
                  *             旧格式(len==2)无此字段,按 0 处理
                  */
      if (len < 2) { commands_send_ack(cmd, 3, NULL, 0); return; }
      uint8_t action_id = d[0];
      uint8_t repeat    = d[1];
      uint16_t hold_ms  = 0;
      if (len >= 4) {
        hold_ms = (uint16_t)d[2] | ((uint16_t)d[3] << 8);
      }
      if (action_id < 1 || action_id > 8) {
        commands_send_ack(cmd, 3, NULL, 0);
        return;
      }
      if (repeat < 1) repeat = 1;
      (void)repeat;
      motion_play_action(action_id, hold_ms);
      commands_send_ack(cmd, 0, NULL, 0);
      watchdog_reset();
      break;
    }
    default:
      commands_send_ack(cmd, 2, NULL, 0);  /* BAD_CMD */
      break;
  }
}

/* === 处理一帧文本(以 \n 结尾的 ASCII 行)==*/
static char     text_buf[64];
static uint8_t  text_idx = 0;
static void dispatch_text_byte(uint8_t c) {
  if (c == '\n' || c == '\r') {
    if (text_idx > 0) {
      text_buf[text_idx] = 0;
      parse_text_command(text_buf);
      text_idx = 0;
    }
  } else if (text_idx < sizeof(text_buf) - 1) {
    text_buf[text_idx++] = c;
  }
}

/* === IDLE 中断回调 ===
 * 在 stm32g4xx_it.c 的 USART1 中断里调,把 DMA 已收字节数算出来交给主循环
 */
void commands_on_rx_idle(uint16_t n) {
  if (n == 0 || n >= COMMANDS_RX_BUF_SIZE) {
    /* 异常:重启接收 */
    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_buf, COMMANDS_RX_BUF_SIZE);
    return;
  }
  /* 把字节流按模式分发 */
  if (g_mode == MODE_BIN) {
    /* 二进制:在主循环里串行解析,这里只复制并标长度 */
    /* 但 IDLE 时 rx_len 表示当前已收字节;为简化,直接让主循环看 rx_buf+rx_len */
    rx_len = n;
  } else {
    /* 文本模式:逐字节 dispatch */
    for (uint16_t i = 0; i < n; i++) dispatch_text_byte(rx_buf[i]);
    rx_len = 0;
    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_buf, COMMANDS_RX_BUF_SIZE);
  }
}

/* === 主循环:解析已收到的二进制帧 === */
void commands_poll(void) {
  if (g_mode != MODE_BIN) return;
  if (rx_len == 0) return;

  uint16_t n = rx_len;
  rx_len = 0;

  uint16_t i = 0;
  while (i + 4 < n) {
    if (rx_buf[i] != 0xAA || rx_buf[i+1] != 0x55) { i++; continue; }
    uint8_t len = rx_buf[i+2];
    uint16_t frame_total = (uint16_t)(4 + len + 1);
    if (i + frame_total > n) break;
    dispatch_frame(&rx_buf[i], (uint8_t)frame_total);
    i = (uint16_t)(i + frame_total);
  }

  HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_buf, COMMANDS_RX_BUF_SIZE);
}

/* === 初始化 === */
void commands_init(void) {
  rx_len = 0;
  text_idx = 0;
  g_mode = MODE_BIN;

  /* 2026-09-15:CubeMX 把 USART1 NVIC 优先级设成 0 跟 TIM6 冲突,强制改到 1 */
  HAL_NVIC_SetPriority(USART1_IRQn, 1, 0);

  /* 启动 USART1 DMA + IDLE 中断接收 */
  if (HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_buf, COMMANDS_RX_BUF_SIZE) != HAL_OK) {
    Error_Handler();
  }
}

/* === HAL UART 接收事件回调(USART1 IDLE / RX 完成时调)===
 *
 * HAL_UARTEx_ReceiveToIdle_DMA 收完 / 触发 IDLE 后调这个回调。
 * Size 表示这次实际收到的字节数(包括之前没处理完的部分)。
 *
 * 关键陷阱:HAL 默认会把缓冲重新启动接收,否则只能收一次。
 * 这里我们关闭自动重启,自己在 commands_poll() 末尾重启 ——
 * 因为我们要在主循环里串行解析,避免 ISR 和 main loop 抢 rx_buf。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
  if (huart->Instance != USART1) return;
  /* 关键:先关闭 DMA 自动重启,让本函数返回后不再触发新的接收 */
  HAL_UART_DMAStop(huart);
  /* 把字节数和缓冲交给主循环处理 */
  commands_on_rx_idle(Size);
  /* 注意:这里不重启 DMA,在 commands_poll() 里串行处理完再重启 */
}
