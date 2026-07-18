/**
 * @file    esp8266config.h
 * @brief   ESP8266 + OneNET 平台配置宏
 * @note    使用前根据实际 OneNET 产品信息修改以下宏定义
 */

#ifndef ESP8266_CONFIG_H__
#define ESP8266_CONFIG_H__

/* ==========================================================================
 * [1] WiFi 凭据 (2.4G)
 * ========================================================================== */
#define ESP8266_WIFI_SSID        "nubia"
#define ESP8266_WIFI_PASSWORD    "zxcvbnm123"

/* ==========================================================================
 * [2] OneNET 平台凭据
 * ========================================================================== */
#define ONENET_PRODUCT_ID        "bo9vLVFhge"          /* 产品ID         */
#define ONENET_DEVICE_NAME       "AHT20Test"            /* 设备名称       */
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
 * [5] 超时与重连
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
 * [7] 业务初始化函数声明 (实现在 esp8266config.c)
 * ========================================================================== */
#include "bsp_esp8266.h"  /* ESP8266_Status, ESP8266_ConnState */

ESP8266_Status ESP8266_ServiceInit(void);
ESP8266_Status ESP8266_ServiceEnsureLink(void);

#endif /* ESP8266_CONFIG_H__ */
