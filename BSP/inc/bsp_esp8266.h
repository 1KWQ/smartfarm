/**
 * @file    bsp_esp8266.h
 * @brief   ESP8266-01S 驱动层头文件 — 纯硬件抽象 + AT引擎
 * @note    业务层函数 (Init / ConnectWiFi / MQTT_* / Publish) 见 esp8266config.h
 *
 * 职责边界:
 *   [驱动层] → 本文件: 环形缓冲区、ISR、AT指令收发、状态查询
 *   [业务层] → esp8266config.h: WiFi/MQTT连接管理、数据上报
 */

#ifndef __BSP_ESP8266_H__
#define __BSP_ESP8266_H__

#include "main.h"
#include "cmsis_os2.h"
#include <stdint.h>


/* ==========================================================================
 * 缓冲区与超时 (驱动层)
 * ========================================================================== */
#define ESP8266_RX_BUF_SIZE           512   /* 环形缓冲区字节数       */
#define ESP8266_AT_TIMEOUT_MS        2000   /* AT命令通用超时(ms)     */
#define ESP8266_CMD_BUF_SIZE          512   /* AT命令构建缓冲区       */

/* ==========================================================================
 * 函数返回值
 * ========================================================================== */
typedef enum {
    ESP8266_OK                  =  0,
    ESP8266_ERROR               = -1,
    ESP8266_TIMEOUT             = -2,
    ESP8266_BUSY                = -3,
    ESP8266_AT_FAIL             = -4,
    ESP8266_WIFI_DISCONNECTED   = -5,
    ESP8266_MQTT_DISCONNECTED   = -6,
    ESP8266_NOT_INITIALIZED     = -7,
    ESP8266_SEND_FAIL           = -8,
    ESP8266_BUF_OVERFLOW        = -9,
} ESP8266_Status;

/* ==========================================================================
 * 模块连接状态 (分层状态机 — 驱动/业务共用)
 * ========================================================================== */
typedef enum {
    ESP8266_STATE_UNINIT    = 0,  /* 未初始化        */
    ESP8266_STATE_AT_READY  = 1,  /* AT通信OK        */
    ESP8266_STATE_WIFI_OK   = 2,  /* WiFi已连        */
    ESP8266_STATE_MQTT_OK   = 3,  /* MQTT已连,可发布 */
} ESP8266_ConnState;

/* ==========================================================================
 * 外部引用 — 由 freertos.c / usart.c 创建
 * ========================================================================== */
extern osMutexId_t uart_tx_mutexHandle;
extern UART_HandleTypeDef huart2;

/* ==========================================================================
 * [驱动层 API] 硬件抽象
 * ========================================================================== */
void ESP8266_HardReset(void);                           /* PB0低电平复位       */
void ESP8266_UART_IRQHandler(UART_HandleTypeDef *huart); /* USART2中断入口      */

/* ==========================================================================
 * [驱动层 API] AT引擎
 * ========================================================================== */
ESP8266_Status ESP8266_SendCmd(const char *cmd, uint32_t timeout_ms);
int            ESP8266_ResponseContains(const char *str);
ESP8266_Status ESP8266_WaitForPattern(const char *pattern, uint32_t timeout_ms);

/* ==========================================================================
 * [驱动层 API] 缓冲区与状态管理 (供业务层调用)
 * ========================================================================== */
void               ESP8266_FlushRx(void);               /* 清空接收缓冲+状态机  */
void               ESP8266_SetState(ESP8266_ConnState s);
ESP8266_ConnState  ESP8266_GetState(void);
void               ESP8266_SetError(const char *str);
const char*        ESP8266_GetLastError(void);

#endif /* __BSP_ESP8266_H__ */
