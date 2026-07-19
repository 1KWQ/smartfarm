/**
 * @file    esp8266config.h
 * @brief   ESP8266 业务层 — WiFi/OneNET配置 + 业务API声明
 * @note    驱动层函数 (SendCmd等) 见 BSP/inc/bsp_esp8266.h
 *
 * 职责边界:
 *   [驱动层] → bsp_esp8266.h:  环形缓冲区、ISR、AT指令收发
 *   [业务层] → 本文件:          WiFi/MQTT连接管理、数据上报
 */

#ifndef ESP8266_CONFIG_H__
#define ESP8266_CONFIG_H__

#include "bsp_esp8266.h"  /* ESP8266_Status, ESP8266_ConnState */

/* ==========================================================================
 * [1] WiFi 凭据 (2.4G)
 * ========================================================================== */
#define ESP8266_WIFI_SSID        "nubia"
#define ESP8266_WIFI_PASSWORD    "zxcvbnm123"

/* ==========================================================================
 * [2] OneNET 平台凭据
 * ========================================================================== */
#define ONENET_PRODUCT_ID        "bo9vLVFhge"
#define ONENET_DEVICE_NAME       "AHT20Test"
#define ONENET_DEVICE_TOKEN      "version=2018-10-31&res=products%2Fbo9vLVFhge%2Fdevices%2FAHT20Test&et=1805693871&method=md5&sign=UAS0VIS1IY0rZx0GAAJoPg%3D%3D"

/* ==========================================================================
 * [3] OneNET MQTT Broker (非SSL直连)
 * ========================================================================== */
#define ONENET_MQTT_BROKER       "183.230.40.39"
#define ONENET_MQTT_PORT         1883

/* ==========================================================================
 * [4] MQTT 主题
 * ========================================================================== */
/** 物模型属性上报主题: $sys/{产品ID}/{设备名}/thing/property/post */
#define ONENET_TOPIC_PROPERTY_POST \
    "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/post"

/* ==========================================================================
 * [5] 超时与重连 (业务层)
 * ========================================================================== */
#define ESP8266_WIFI_TIMEOUT_MS      15000   /* WiFi连接超时          */
#define ESP8266_MQTT_TIMEOUT_MS      10000   /* MQTT连接超时          */
#define ESP8266_RECONNECT_INTERVAL_MS  5000  /* 断线重连间隔 (5秒)    */
#define ESP8266_MAX_RECONNECT            0   /* 最大重连次数, 0=无限  */

/* ==========================================================================
 * [6] 数据上报周期
 * ========================================================================== */
#define ESP8266_REPORT_INTERVAL_MS    5000   /* 上报间隔 (5秒)        */

/* ==========================================================================
 * [7] 业务层API — 初始化与状态
 * ========================================================================== */
ESP8266_Status   ESP8266_Init(void);
ESP8266_Status   ESP8266_AT_Test(void);

/* ==========================================================================
 * [8] 业务层API — WiFi管理
 * ========================================================================== */
ESP8266_Status ESP8266_ConnectWiFi(const char *ssid, const char *password);

/* ==========================================================================
 * [9] 业务层API — MQTT管理
 * ========================================================================== */
ESP8266_Status ESP8266_MQTT_Connect(const char *broker, uint16_t port,
                                    const char *client_id,
                                    const char *username,
                                    const char *password);
ESP8266_Status ESP8266_MQTT_Disconnect(void);
ESP8266_Status ESP8266_MQTT_PublishJson(const char *topic, const char *json);
ESP8266_Status ESP8266_MQTT_PublishProperty(const char *topic,
    float temperature, float humidity,
    uint16_t soil_moisture, uint16_t rain,
    uint16_t light, uint8_t pump_state);

/* ==========================================================================
 * [10] 业务层API — 连接维护
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

/* ==========================================================================
 * [11] 业务层API — 一键服务封装
 * ========================================================================== */
ESP8266_Status ESP8266_ServiceInit(void);
ESP8266_Status ESP8266_ServiceEnsureLink(void);

#endif /* ESP8266_CONFIG_H__ */
