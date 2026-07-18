/**
 * @file    esp8266config.c
 * @brief   ESP8266 业务初始化 — WiFi + MQTT 连接封装
 * @note    在 ESP8266Task 启动时调用, 完成联网链路建立
 */

#include "esp8266config.h"
#include "bsp_esp8266.h"
#include "cmsis_os2.h"
#include <stdio.h>
#include <string.h>

/* 调试串口 (USART3) 重定向 — 可选, 用于日志输出 */
extern UART_HandleTypeDef huart3;

/* ==========================================================================
 * 调试日志 (通过USART3输出)
 * ========================================================================== */
#ifdef ESP8266_DEBUG_LOG
static void debug_print(const char *msg)
{
    HAL_UART_Transmit(&huart3, (uint8_t *)msg, strlen(msg), 100);
}

static void debug_println(const char *msg)
{
    debug_print(msg);
    debug_print("\r\n");
}
#else
#define debug_print(msg)      (void)(msg)
#define debug_println(msg)    (void)(msg)
#endif

/* ==========================================================================
 * 业务初始化 — 建立完整连接链路
 * ========================================================================== */

/**
 * @brief ESP8266联网初始化: AT自检 → WiFi连接 → MQTT连接
 * @return ESP8266_OK 链路建立成功, 其他值表示失败
 * @note  失败后调用者应等待 5s 后重试 (由 ESP8266Task 处理)
 */
ESP8266_Status ESP8266_ServiceInit(void)
{
    ESP8266_Status ret;

    debug_println("[ESP8266] Service init start...");

    /* 阶段1: ESP8266初始化 + AT自检 */
    debug_println("[ESP8266] Step 1: Hardware init & AT test...");
    ret = ESP8266_Init();
    if (ret != ESP8266_OK) {
        debug_println("[ESP8266] Init FAILED");
        return ret;
    }
    debug_println("[ESP8266] Init OK");

    /* 阶段2: 连接WiFi */
    debug_println("[ESP8266] Step 2: Connecting WiFi...");
    ret = ESP8266_ConnectWiFi(ESP8266_WIFI_SSID, ESP8266_WIFI_PASSWORD);
    if (ret != ESP8266_OK) {
        debug_println("[ESP8266] WiFi connect FAILED");
        return ret;
    }
    debug_println("[ESP8266] WiFi OK");

    /* 阶段3: 连接OneNET MQTT */
    debug_println("[ESP8266] Step 3: Connecting MQTT...");
    ret = ESP8266_MQTT_Connect(
        ONENET_MQTT_BROKER,
        ONENET_MQTT_PORT,
        ONENET_DEVICE_NAME,      /* ClientID = 设备名 */
        ONENET_PRODUCT_ID,       /* Username = 产品ID */
        ONENET_DEVICE_TOKEN      /* Password = 设备Token */
    );
    if (ret != ESP8266_OK) {
        debug_println("[ESP8266] MQTT connect FAILED");
        return ret;
    }
    debug_println("[ESP8266] MQTT OK");

    debug_println("[ESP8266] Service init SUCCESS");
    return ESP8266_OK;
}

/**
 * @brief 检查并恢复连接 (供Task周期性调用)
 * @return ESP8266_OK 链路正常, 其他值表示当前断线
 */
ESP8266_Status ESP8266_ServiceEnsureLink(void)
{
    return ESP8266_EnsureConnected(
        ESP8266_WIFI_SSID, ESP8266_WIFI_PASSWORD,
        ONENET_MQTT_BROKER, ONENET_MQTT_PORT,
        ONENET_DEVICE_NAME, ONENET_PRODUCT_ID,
        ONENET_DEVICE_TOKEN
    );
}
