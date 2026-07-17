#ifndef __BSP_ESP8266_H__
#define __BSP_ESP8266_H__


#include "main.h"           // → stm32f1xx_hal.h, cmsis_os2.h
#include "cmsis_os2.h"
#include <stdint.h>

#ifndef ESP8266_RST_Pin
#define ESP8266_RST_Pin        GPIO_PIN_8  //ESP8266复位引脚
#endif 

#ifndef ESP8266_RST_GPIO_Port       
#define ESP8266_RST_GPIO_Port  GPIOB       //ESP8266复位引脚端口
#endif

#define ESP8266_RX_BUF_SIZE         512    // 环形缓冲区字节数
#define ESP8266_AT_TIMEOUT_MS       2000   // AT命令通用超时

// 函数返回值
typedef enum {
    ESP8266_OK                  =  0,  //成功
    ESP8266_ERROR               = -1,  //错误
    ESP8266_TIMEOUT             = -2,  //超时
    ESP8266_BUSY                = -3,  //模块忙
    ESP8266_AT_FAIL             = -4,  //AT命令执行失败
    ESP8266_WIFI_DISCONNECTED   = -5,  //WiFi未连接
    ESP8266_MQTT_DISCONNECTED   = -6,  //MQTT未连接
    ESP8266_NOT_INITIALIZED     = -7,  //未初始化
    ESP8266_SEND_FAIL           = -8,  //发送失败
    ESP8266_BUF_OVERFLOW        = -9,  //缓冲区溢出
} ESP8266_Status;

// 模块连接状态 (分层状态机核心)
typedef enum {
    ESP8266_STATE_UNINIT    = 0,  // 未初始化
    ESP8266_STATE_AT_READY  = 1,  // AT通信OK
    ESP8266_STATE_WIFI_OK   = 2,  // WiFi已连
    ESP8266_STATE_MQTT_OK   = 3,  // MQTT已连, 可publish
} ESP8266_ConnState;

/**
// ===== 驱动层 API =====
static void ESP8266_HardReset(void);
static ESP8266_Status ESP8266_SendCmd(const char *cmd, uint32_t timeout_ms);
static int ESP8266_ResponseContains(const char *str);
static void ESP8266_SetError(const char *str);
// ===== 业务层 API =====
ESP8266_Status ESP8266_Init(void);                // 硬件+AT自检一条龙
ESP8266_Status ESP8266_AT_Test(void);             // 单独AT测试
ESP8266_ConnState ESP8266_GetState(void);         // 查询连接状态
ESP8266_Status ESP8266_ConnectWiFi(void);         // 连接WiFi
ESP8266_Status ESP8266_MQTT_Connect(void);        // 连接MQTT Broker
ESP8266_Status ESP8266_MQTT_Disconnect(void);     // 断开MQTT
ESP8266_Status ESP8266_MQTT_PublishProperty(      // 上报物模型
    float temperature, float humidity, uint16_t illumination);
ESP8266_Status ESP8266_MQTT_PublishJson(const char *json);  // 上报原始JSON
ESP8266_Status ESP8266_EnsureConnected(void);     // 链路维护(重连)
ESP8266_Status ESP8266_ReportAndReconnect(        // 一键上报+重连
    float temperature, float humidity, uint16_t illumination);
const char* ESP8266_GetLastError(void);           // 获取错误描述

// ===== 中断接口 =====
extern UART_HandleTypeDef huart2_esp;             // USART2句柄(供it.c引用)
void ESP8266_UART_IRQHandler(UART_HandleTypeDef *huart);  // 中断入口
*/

/**
[区域A] 私有变量        — 环形缓冲区、at_response、状态标志、互斥锁
[区域B] 驱动层-硬件抽象  — USART2_Init、HardReset
[区域C] 驱动层-数据接收  — 环形缓冲区操作、detect_response_end状态机
[区域D] 驱动层-AT引擎    — ESP8266_SendCmd（含互斥锁、超时、结果解析）
[区域E] 业务层-初始化    — ESP8266_Init、AT_Test
[区域F] 业务层-WiFi管理  — ConnectWiFi
[区域G] 业务层-MQTT管理  — MQTT_Connect/Disconnect/Publish
[区域H] 业务层-连接维护  — EnsureConnected、ReportAndReconnect
[区域I] 中断服务        — ESP8266_UART_IRQHandler
 * 
 */
#endif