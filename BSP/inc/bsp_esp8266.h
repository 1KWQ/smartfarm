#ifndef __BSP_ESP8266_H__
#define __BSP_ESP8266_H__

#include "main.h"
#include "cmsis_os2.h"
#include <stdint.h>

/* ==========================================================================
 * 硬件引脚配置 — ESP8266-01S
 * ========================================================================== */
#define ESP8266_RST_Pin         GPIO_PIN_0   /* PB0 — 复位引脚 */
#define ESP8266_RST_GPIO_Port   GPIOB

/* ==========================================================================
 * 缓冲区与超时
 * ========================================================================== */
#define ESP8266_RX_BUF_SIZE          1024   /* 环形缓冲区字节数 */
#define ESP8266_AT_TIMEOUT_MS        2000   /* AT命令通用超时 */
#define ESP8266_LONG_TIMEOUT_MS     15000   /* WiFi连接超时(需扫描信道) */
#define ESP8266_MQTT_TIMEOUT_MS     10000   /* MQTT连接超时(TCP握手) */
#define ESP8266_CMD_BUF_SIZE         1024   /* AT命令构建缓冲区 */

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
 * 模块连接状态 (分层状态机核心)
 * ========================================================================== */
typedef enum {
    ESP8266_STATE_UNINIT    = 0,  /* 未初始化        */
    ESP8266_STATE_AT_READY  = 1,  /* AT通信OK        */
    ESP8266_STATE_WIFI_OK   = 2,  /* WiFi已连        */
    ESP8266_STATE_MQTT_OK   = 3,  /* MQTT已连,可发布 */
} ESP8266_ConnState;

/* ==========================================================================
 * 外部引用 — 由 freertos.c 创建
 * ========================================================================== */
extern osMutexId_t uart_tx_mutexHandle;
extern UART_HandleTypeDef huart2;

/* ==========================================================================
 * [区域A] 驱动层 — 硬件抽象
 * ========================================================================== */
void    ESP8266_HardReset(void);
void    ESP8266_UART_IRQHandler(UART_HandleTypeDef *huart);

/* ==========================================================================
 * [区域B] 驱动层 — AT引擎
 * ========================================================================== */
ESP8266_Status ESP8266_SendCmd(const char *cmd, uint32_t timeout_ms);
int            ESP8266_ResponseContains(const char *str);

/* ==========================================================================
 * [区域C] 业务层 — 初始化与状态
 * ========================================================================== */
ESP8266_Status   ESP8266_Init(void);
ESP8266_Status   ESP8266_AT_Test(void);
ESP8266_ConnState ESP8266_GetState(void);
const char*      ESP8266_GetLastError(void);

/* ==========================================================================
 * [区域D] 业务层 — WiFi管理
 * ========================================================================== */
ESP8266_Status ESP8266_ConnectWiFi(const char *ssid, const char *password);

/* ==========================================================================
 * [区域E] 业务层 — MQTT管理
 * ========================================================================== */
ESP8266_Status ESP8266_MQTT_Connect(const char *broker, uint16_t port,
                                    const char *client_id,
                                    const char *username,
                                    const char *password);
ESP8266_Status ESP8266_MQTT_Disconnect(void);
ESP8266_Status ESP8266_MQTT_PublishJson(const char *topic, const char *json);
ESP8266_Status ESP8266_MQTT_PublishProperty(
    float temperature, float humidity,
    uint16_t soil_moisture, uint16_t rain,
    uint16_t light, uint8_t pump_state);

/* ==========================================================================
 * [区域F] 业务层 — 连接维护
 * ========================================================================== */
ESP8266_Status ESP8266_EnsureConnected(
    const char *ssid, const char *wifi_pwd,
    const char *broker, uint16_t port,
    const char *client_id, const char *username, const char *mqtt_pwd);
ESP8266_Status ESP8266_ReportAndReconnect(
    const char *topic, const char *json,
    const char *ssid, const char *wifi_pwd,
    const char *broker, uint16_t port,
    const char *client_id, const char *username, const char *mqtt_pwd);

#endif /* __BSP_ESP8266_H__ */
