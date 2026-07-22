/**
 * @file    esp8266config.h
 * @brief   ESP8266 业务层 — WiFi/OneNET 凭据配置 + 业务 API 声明
 * @author  SmartFarm Project
 * @date    2026-07
 *
 * @note    职责边界:
 *          - [驱动层] bsp_esp8266.h: 环形缓冲区、UART ISR、AT 引擎
 *          - [业务层] 本文件:       WiFi/MQTT 连接管理、OneNET 数据上报
 *
 * ## 凭据管理
 * 所有 WiFi 和 OneNET 凭据以宏定义形式集中管理,
 * 业务层函数自动读取宏, 调用方无需传参.
 *
 * ## 超时参数调优指南
 * - ESP8266_WIFI_TIMEOUT_MS:  取决于 AP 响应速度和 DHCP 服务器速度,
 *   信号弱或 DHCP 慢时可适当增大
 * - ESP8266_MQTT_TIMEOUT_MS:  包含 DNS 解析 + TCP 握手 + MQTT CONNECT,
 *   公网环境建议 10-15 秒
 * - ESP8266_REPORT_INTERVAL_MS: OneNET 免费版限制最少 3 秒/条, 建议 ≥5 秒
 */

#ifndef ESP8266_CONFIG_H__
#define ESP8266_CONFIG_H__

#include "bsp_esp8266.h"  /* ESP8266_Status, ESP8266_ConnState */

/* ==========================================================================
 * [1] WiFi 凭据 (2.4GHz, 不支持 5GHz)
 * ========================================================================== */
#define ESP8266_WIFI_SSID        "nubia"
#define ESP8266_WIFI_PASSWORD    "zxcvbnm123"

/* ==========================================================================
 * [2] OneNET 平台凭据
 *
 * - ONENET_PRODUCT_ID:   产品 ID, 在 OneNET 控制台创建产品后获取
 * - ONENET_DEVICE_NAME:  设备名称, 需在 OneNET 平台预先注册
 * - ONENET_DEVICE_TOKEN: 设备密钥 (鉴权 Token)
 *   格式: version=2018-10-31&res=products%2F{产品ID}%2Fdevices%2F{设备名}
 *         &et={过期时间戳}&method=md5&sign={MD5签名}
 *   注意: Token 中包含 %2F (= '/' 的 URL 编码) 和 %3D (= '=' 的 URL 编码),
 *         这些字符属于 AT 命令普通字符, 无需转义
 * ========================================================================== */
#define ONENET_PRODUCT_ID        "bo9vLVFhge"
#define ONENET_DEVICE_NAME       "AHT20Test"
#define ONENET_DEVICE_TOKEN      "version=2018-10-31&res=products%2Fbo9vLVFhge%2Fdevices%2FAHT20Test&et=1805693871&method=md5&sign=UAS0VIS1IY0rZx0GAAJoPg%3D%3D"

/* ==========================================================================
 * [3] OneNET MQTT Broker (非 SSL 直连)
 * ========================================================================== */
#define ONENET_MQTT_BROKER       "mqtts.heclouds.com"
#define ONENET_MQTT_PORT         1883

/* ==========================================================================
 * [4] MQTT 主题 (Topic)
 *
 * OneNET 物模型话题格式:
 *   $sys/{产品ID}/{设备名}/thing/property/post       — 属性上报
 *   $sys/{产品ID}/{设备名}/thing/property/post/reply — 上报响应 (服务端下发)
 *   $sys/{产品ID}/{设备名}/thing/property/set        — 属性设置 (订阅)
 *   $sys/{产品ID}/{设备名}/thing/property/set_reply  — 设置响应 (发布)
 *
 * 已使用:
 * - property/post       发布传感器数据
 * - property/post/reply 订阅平台回复 (确认送达)
 * - property/set        订阅平台下发指令
 *
 * 预留:
 * - property/set_reply  回复平台指令执行结果
 * ========================================================================== */

/** 属性上报: 发布传感器数据到 OneNET 平台 */
#define ONENET_TOPIC_PROPERTY_POST \
    "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/post"

/** 属性上报响应: 订阅此主题接收 OneNET 平台的回复 (含消息ID, 用于确认送达) */
#define ONENET_TOPIC_PROPERTY_POST_REPLY \
    "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/post/reply"

/** 属性设置: 订阅此主题接收 OneNET 平台下发的控制指令 (如开关水泵) */
#define ONENET_TOPIC_PROPERTY_SET \
    "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/set"

/* ==========================================================================
 * [5] 超时与重连 (业务层参数)
 * ========================================================================== */

#define ESP8266_WIFI_TIMEOUT_MS      15000   /**< WiFi 连接总超时 (含 4 步握手 + DHCP) */
#define ESP8266_MQTT_TIMEOUT_MS      10000   /**< MQTT 连接总超时 (含 DNS + TCP + CONNACK) */
#define ESP8266_RECONNECT_INTERVAL_MS  5000  /**< 断线重连检查间隔 (5 秒)                 */
#define ESP8266_MAX_RECONNECT            0   /**< 最大重连次数, 0 = 无限重连               */

/* ==========================================================================
 * [6] 数据上报周期
 * ========================================================================== */
#define ESP8266_REPORT_INTERVAL_MS    5000   /**< 传感器数据上报间隔 (5 秒) */

/* ==========================================================================
 * [7] 业务层 API — 初始化与状态
 * ========================================================================== */

ESP8266_Status ESP8266_Init(void);
ESP8266_Status ESP8266_AT_Test(void);

/* ==========================================================================
 * [8] 业务层 API — WiFi 管理
 * ========================================================================== */

/**
 * @brief 连接 WiFi 热点 (Station 模式, DHCP)
 *
 * SSID/密码自动读取 ESP8266_WIFI_SSID / ESP8266_WIFI_PASSWORD.
 *
 * @return ESP8266_OK / ESP8266_NOT_INITIALIZED / ESP8266_BUF_OVERFLOW / 其他错误
 */
ESP8266_Status ESP8266_ConnectWiFi(void);

/* ==========================================================================
 * [9] 业务层 API — MQTT 管理
 * ========================================================================== */

/**
 * @brief 连接 MQTT Broker (OneNET 平台)
 *
 * Broker 地址/端口/凭据自动读取 ONENET_* 宏定义.
 *
 * @return ESP8266_OK / ESP8266_WIFI_DISCONNECTED / ESP8266_BUF_OVERFLOW / 其他错误
 */
ESP8266_Status ESP8266_MQTT_Connect(void);

/**
 * @brief 断开 MQTT 连接
 * @return ESP8266_OK / ESP8266_MQTT_DISCONNECTED
 */
ESP8266_Status ESP8266_MQTT_Disconnect(void);

/**
 * @brief 通用 MQTT 发布 — 发布 JSON 到指定 Topic (内部 AT 转义)
 * @param topic MQTT 主题
 * @param json  待发布 JSON 字符串
 * @return ESP8266_OK / ESP8266_MQTT_DISCONNECTED / ESP8266_BUF_OVERFLOW / ESP8266_ERROR
 */
ESP8266_Status ESP8266_MQTT_PublishJson(const char *topic, const char *json);

/**
 * @brief OneNET 物模型属性上报 — 自动组 JSON 并发布
 *
 * Topic 使用宏 ONENET_TOPIC_PROPERTY_POST,
 * 构建 OneNET 规范 JSON: {"id":"...","version":"1.0","params":{...}}
 *
 * @param temperature   温度 (°C, 保留 1 位小数)
 * @param humidity      湿度 (%RH, 保留 1 位小数)
 * @param soil_moisture 土壤湿度 ADC 值
 * @param rain          雨量 ADC 值
 * @param light         光照 ADC 值
 * @param pump_state    水泵状态 (0 关 / 1 开)
 * @param alarm_state   告警状态 (0 正常 / 1 告警)
 * @return ESP8266_OK / ESP8266_BUF_OVERFLOW / 其他错误
 */
ESP8266_Status ESP8266_MQTT_PublishProperty(
    float temperature, float humidity,
    uint16_t soil_moisture, uint16_t rain,
    uint16_t light, uint8_t pump_state,
    uint8_t alarm_state);

/* ==========================================================================
 * [10] 业务层 API — 连接维护
 * ========================================================================== */

/**
 * @brief 逐层检查并恢复连接 (WiFi → MQTT)
 *
 * 凭据自动读取宏定义, 状态机自底向上恢复:
 * UNINIT → AT_READY → WIFI_OK → MQTT_OK.
 *
 * @return ESP8266_OK / 错误码
 */
ESP8266_Status ESP8266_EnsureConnected(void);

/* ==========================================================================
 * [11] 业务层 API — 一键服务封装 (简化 Task 调用)
 * ========================================================================== */

/**
 * @brief 服务初始化 — AT → WiFi → MQTT 全流程 (启动时调用一次)
 *
 * 凭据自动读取宏定义, 任一阶段失败即停止并返回错误.
 *
 * @return ESP8266_OK / 失败阶段错误码
 */
ESP8266_Status ESP8266_ServiceInit(void);

#endif /* ESP8266_CONFIG_H__ */
