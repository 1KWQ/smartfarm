/**
 * @file    esp8266config.c
 * @brief   ESP8266 业务层 — WiFi/MQTT连接管理 + 数据上报
 * @note    依赖驱动层 bsp_esp8266.h (SendCmd, WaitForPattern, FlushRx 等)
 *
 * 职责:
 *   - ESP8266初始化 (AT自检, 3次重试)
 *   - WiFi连接 (CWMODE → CWJAP → DHCP)
 *   - MQTT连接 (MQTTUSERCFG → MQTTCONN → 异步等待)
 *   - MQTT发布 (JSON转义 → MQTTPUB)
 *   - 连接维护 (逐层重连)
 *   - OneNET物模型JSON组包
 */

#include "esp8266config.h"
#include "bsp_esp8266.h"
#include "cmsis_os2.h"
#include <stdio.h>
#include <string.h>

/* ==========================================================================
 * [业务层] 私有变量
 * ========================================================================== */

/** @brief 重连计数器 */
static uint32_t reconnect_count = 0;

/** @brief 数据上报消息ID (自增, 用于OneNET JSON) */
static uint32_t msg_id = 0;

/** @brief 共享工作缓冲区 (PublishJson / PublishProperty 共用, 同任务上下文无竞争) */
static char work_buf[ESP8266_CMD_BUF_SIZE];

/* ==========================================================================
 * 调试日志 (USART3输出, 可选)
 * ========================================================================== */
extern UART_HandleTypeDef huart3;

#ifdef ESP8266_DEBUG_LOG
static void dbg_print(const char *msg)
{
    HAL_UART_Transmit(&huart3, (uint8_t *)msg, strlen(msg), 100);
}
static void dbg_println(const char *msg)
{
    dbg_print(msg);
    dbg_print("\r\n");
}
#else
#define dbg_print(msg)      (void)(msg)
#define dbg_println(msg)    (void)(msg)
#endif

/* ==========================================================================
 * [业务层] 初始化 — GPIO + 硬件复位 + AT自检 (3次重试)
 * ========================================================================== */
ESP8266_Status ESP8266_Init(void)
{
    /* 1. 硬件复位 (内部自动初始化PB0) */
    ESP8266_HardReset();

    /* 2. 清空复位期间的乱码 */
    ESP8266_FlushRx();

    /* 3. AT自检 (尝试3次) */
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (ESP8266_AT_Test() == ESP8266_OK) {
            ESP8266_SetState(ESP8266_STATE_AT_READY);
            return ESP8266_OK;
        }
        osDelay(500);
    }

    ESP8266_SetState(ESP8266_STATE_UNINIT);
    ESP8266_SetError("Init: AT test failed after 3 attempts");
    return ESP8266_AT_FAIL;
}

/* ==========================================================================
 * [业务层] AT通信自检
 * ========================================================================== */
ESP8266_Status ESP8266_AT_Test(void)
{
    /* 先发空命令清空模块缓冲区 */
    ESP8266_SendCmd("AT\r\n", 1000);
    osDelay(100);

    /* 正式测试 */
    ESP8266_Status ret = ESP8266_SendCmd("AT\r\n", ESP8266_AT_TIMEOUT_MS);
    if (ret == ESP8266_OK && ESP8266_ResponseContains("OK")) {
        ESP8266_SetState(ESP8266_STATE_AT_READY);
        return ESP8266_OK;
    }

    ESP8266_SetState(ESP8266_STATE_UNINIT);
    ESP8266_SetError("AT_Test: no OK response");
    return ESP8266_AT_FAIL;
}

/* ==========================================================================
 * [业务层] WiFi连接
 * ========================================================================== */
ESP8266_Status ESP8266_ConnectWiFi(const char *ssid, const char *password)
{
    if (ESP8266_GetState() < ESP8266_STATE_AT_READY) {
        ESP8266_SetError("ConnectWiFi: not initialized");
        return ESP8266_NOT_INITIALIZED;
    }

    /* 1. 设置为Station模式 */
    ESP8266_Status ret = ESP8266_SendCmd("AT+CWMODE=1\r\n", ESP8266_AT_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("ConnectWiFi: CWMODE failed");
        return ret;
    }
    osDelay(200);

    /* 2. 断开已有WiFi (避免重连时状态冲突) */
    ESP8266_SendCmd("AT+CWQAP\r\n", 3000);
    osDelay(500);

    /* 3. 连接WiFi */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, password);
    ret = ESP8266_SendCmd(cmd, ESP8266_WIFI_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("ConnectWiFi: CWJAP failed");
        return ret;
    }

    /* 4. 等待获取IP (查询+CIFSR确认) */
    osDelay(1000);
    ret = ESP8266_SendCmd("AT+CIFSR\r\n", ESP8266_AT_TIMEOUT_MS);
    if (ret == ESP8266_OK && ESP8266_ResponseContains("STAIP")) {
        ESP8266_SetState(ESP8266_STATE_WIFI_OK);
        return ESP8266_OK;
    }

    /* CWJAP返回了OK但未获取到IP, 可能是DHCP较慢, 再等一会 */
    osDelay(2000);
    ESP8266_SetState(ESP8266_STATE_WIFI_OK);
    return ESP8266_OK;
}

/* ==========================================================================
 * [业务层] MQTT连接
 * ========================================================================== */
ESP8266_Status ESP8266_MQTT_Connect(const char *broker, uint16_t port,
                                    const char *client_id,
                                    const char *username,
                                    const char *password)
{
    if (ESP8266_GetState() < ESP8266_STATE_WIFI_OK) {
        ESP8266_SetError("MQTT_Connect: WiFi not connected");
        return ESP8266_WIFI_DISCONNECTED;
    }

    /* 1. 清理旧MQTT连接 (如果存在) */
    ESP8266_SendCmd("AT+MQTTCLEAN=0\r\n", 3000);
    osDelay(300);

    /* 2. 配置MQTT用户参数
     *    AT+MQTTUSERCFG=<LinkID>,<scheme>,<"client_id">,<"username">,
     *                    <"password">,<cert_key_ID>,<CA_ID>,<"path">
     *    scheme=1: MQTT over TCP (非TLS)
     */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"%s\",0,0,\"\"\r\n",
             client_id, username, password);
    ESP8266_Status ret = ESP8266_SendCmd(cmd, ESP8266_AT_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("MQTT_Connect: MQTTUSERCFG failed");
        return ret;
    }
    osDelay(200);

    /* 3. 连接MQTT Broker
     *    AT+MQTTCONN=<LinkID>,<"host">,<port>,<reconnect>
     *    reconnect=0: 不自动重连 (由MCU控制)
     */
    snprintf(cmd, sizeof(cmd),
             "AT+MQTTCONN=0,\"%s\",%u,0\r\n", broker, port);
    ret = ESP8266_SendCmd(cmd, ESP8266_AT_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("MQTT_Connect: MQTTCONN command failed");
        ESP8266_SetState(ESP8266_STATE_WIFI_OK);
        return ret;
    }

    /* 4. 等待异步连接结果 +MQTTCONNECTED:0,<err_code>
     *    err_code=0 表示成功
     */
    ret = ESP8266_WaitForPattern("+MQTTCONNECTED:0,0", ESP8266_MQTT_TIMEOUT_MS);
    if (ret == ESP8266_OK) {
        ESP8266_SetState(ESP8266_STATE_MQTT_OK);
        reconnect_count = 0;
        return ESP8266_OK;
    }

    /* 连接失败, 状态回退 */
    ESP8266_SetError("MQTT_Connect: broker connection failed");
    ESP8266_SetState(ESP8266_STATE_WIFI_OK);
    return ESP8266_AT_FAIL;
}

/* ==========================================================================
 * [业务层] MQTT断开
 * ========================================================================== */
ESP8266_Status ESP8266_MQTT_Disconnect(void)
{
    if (ESP8266_GetState() < ESP8266_STATE_MQTT_OK) {
        return ESP8266_MQTT_DISCONNECTED;
    }

    ESP8266_Status ret = ESP8266_SendCmd("AT+MQTTCLEAN=0\r\n", 5000);
    osDelay(200);

    ESP8266_SetState(ESP8266_STATE_WIFI_OK);
    return ret;
}

/* ==========================================================================
 * [业务层] MQTT发布
 * ========================================================================== */
ESP8266_Status ESP8266_MQTT_PublishJson(const char *topic, const char *json)
{
    if (ESP8266_GetState() < ESP8266_STATE_MQTT_OK) {
        ESP8266_SetError("PublishJson: MQTT not connected");
        return ESP8266_MQTT_DISCONNECTED;
    }

    if (topic == NULL || json == NULL) {
        ESP8266_SetError("PublishJson: null argument");
        return ESP8266_ERROR;
    }

    /* 在共享缓冲区中直接构建完整AT命令 (前缀 + 转义JSON + 后缀, 一锅出) */
    int wi = snprintf(work_buf, sizeof(work_buf),
                      "AT+MQTTPUB=0,\"%s\",\"", topic);
    if (wi < 0 || wi >= (int)sizeof(work_buf) - 4) {
        ESP8266_SetError("PublishJson: command too long");
        return ESP8266_BUF_OVERFLOW;
    }

    /* 逐字符追加JSON, 同步转义 " → \" 和 \ → \\ */
    for (int ri = 0; json[ri] != '\0' && wi < (int)sizeof(work_buf) - 3; ri++) {
        if (json[ri] == '"')       { work_buf[wi++] = '\\'; work_buf[wi++] = '"'; }
        else if (json[ri] == '\\') { work_buf[wi++] = '\\'; work_buf[wi++] = '\\'; }
        else                       { work_buf[wi++] = json[ri]; }
    }

    /* 追加命令后缀: ",1,0\r\n */
    wi += snprintf(work_buf + wi, sizeof(work_buf) - wi, "\",1,0\r\n");
    if (wi >= (int)sizeof(work_buf)) {
        ESP8266_SetError("PublishJson: command too long");
        return ESP8266_BUF_OVERFLOW;
    }

    ESP8266_Status ret = ESP8266_SendCmd(work_buf, ESP8266_AT_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("PublishJson: publish failed");
        if (ESP8266_ResponseContains("CLOSED") ||
            ESP8266_ResponseContains("DISCONNECTED")) {
            ESP8266_SetState(ESP8266_STATE_WIFI_OK);
        }
        return ret;
    }

    return ESP8266_OK;
}

/* ==========================================================================
 * [业务层] OneNET物模型JSON组包 + 发布
 * ========================================================================== */
ESP8266_Status ESP8266_MQTT_PublishProperty(const char *topic,
                                            float temperature, float humidity,
                                            uint16_t soil_moisture, uint16_t rain,
                                            uint16_t light, uint8_t pump_state,
                                            uint8_t alarm_state)
{
    if (topic == NULL) {
        ESP8266_SetError("PublishProperty: null topic");
        return ESP8266_ERROR;
    }

    if (ESP8266_GetState() < ESP8266_STATE_MQTT_OK) {
        ESP8266_SetError("PublishProperty: MQTT not connected");
        return ESP8266_MQTT_DISCONNECTED;
    }

    /* 直接在 work_buf 中构建完整的 AT+MQTTPUB 命令 (JSON内嵌, 一次snprintf)
     * 原始JSON: {"id":"123","version":"1.0","params":{...}}
     * AT命令:   AT+MQTTPUB=0,"topic","{\"id\":\"123\",...}",1,0\r\n
     * 格式串中的 \\\" = 源码 \\ + 源码 \" = 实际字符 \" (转义后的JSON引号)
     */
    msg_id++;
    int len = snprintf(work_buf, sizeof(work_buf),
        "AT+MQTTPUB=0,\"%s\",\""
        "{\\\"id\\\":\\\"%lu\\\",\\\"version\\\":\\\"1.0\\\",\\\"params\\\":{"
        "\\\"temperature\\\":{\\\"value\\\":%.1f},"
        "\\\"humidity\\\":{\\\"value\\\":%.1f},"
        "\\\"soilmoisture\\\":{\\\"value\\\":%u},"
        "\\\"rain\\\":{\\\"value\\\":%u},"
        "\\\"light\\\":{\\\"value\\\":%u},"
        "\\\"pump\\\":{\\\"value\\\":%u},"
        "\\\"alarm\\\":{\\\"value\\\":%u}"
        "}}\",1,0\r\n",
        topic,
        (unsigned long)msg_id,
        (double)temperature, (double)humidity,
        (unsigned int)soil_moisture, (unsigned int)rain,
        (unsigned int)light, (unsigned int)pump_state,
        (unsigned int)alarm_state);

    if (len < 0 || len >= (int)sizeof(work_buf)) {
        ESP8266_SetError("PublishProperty: command too long");
        return ESP8266_BUF_OVERFLOW;
    }

    ESP8266_Status ret = ESP8266_SendCmd(work_buf, ESP8266_AT_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("PublishProperty: publish failed");
        if (ESP8266_ResponseContains("CLOSED") ||
            ESP8266_ResponseContains("DISCONNECTED")) {
            ESP8266_SetState(ESP8266_STATE_WIFI_OK);
        }
        return ret;
    }

    return ESP8266_OK;
}

/* ==========================================================================
 * [业务层] 连接维护 — 逐层检查并恢复
 * ========================================================================== */
ESP8266_Status ESP8266_EnsureConnected(
    const char *ssid, const char *wifi_pwd,
    const char *broker, uint16_t port,
    const char *client_id, const char *username, const char *mqtt_pwd)
{
    /* 层1: 确保AT通信 */
    if (ESP8266_GetState() < ESP8266_STATE_AT_READY) {
        ESP8266_Status ret = ESP8266_Init();
        if (ret != ESP8266_OK) return ret;
    }

    /* 层2: 确保WiFi连接 */
    if (ESP8266_GetState() < ESP8266_STATE_WIFI_OK) {
        ESP8266_Status ret = ESP8266_ConnectWiFi(ssid, wifi_pwd);
        if (ret != ESP8266_OK) {
            reconnect_count++;
            return ret;
        }
    }

    /* 层3: 确保MQTT连接 */
    if (ESP8266_GetState() < ESP8266_STATE_MQTT_OK) {
        ESP8266_Status ret = ESP8266_MQTT_Connect(broker, port,
                                                  client_id, username, mqtt_pwd);
        if (ret != ESP8266_OK) {
            reconnect_count++;
            return ret;
        }
    }

    return ESP8266_OK;
}

/* ==========================================================================
 * [业务层] 一键上报 + 断线重连
 * ========================================================================== */
ESP8266_Status ESP8266_ReportAndReconnect(
    const char *topic, const char *json,
    const char *ssid, const char *wifi_pwd,
    const char *broker, uint16_t port,
    const char *client_id, const char *username, const char *mqtt_pwd)
{
    /* 尝试直接发布 */
    ESP8266_Status ret = ESP8266_MQTT_PublishJson(topic, json);
    if (ret == ESP8266_OK) {
        return ESP8266_OK;
    }

    /* 发布失败 → 标记断线 → 重建连接 */
    if (ESP8266_GetState() == ESP8266_STATE_MQTT_OK) {
        ESP8266_SetState(ESP8266_STATE_WIFI_OK);
    }

    ESP8266_SetError("ReportAndReconnect: reconnecting...");

    /* 重建连接链 */
    ret = ESP8266_EnsureConnected(ssid, wifi_pwd, broker, port,
                                  client_id, username, mqtt_pwd);
    if (ret != ESP8266_OK) {
        return ret;
    }

    /* 连接恢复, 重试发布 */
    return ESP8266_MQTT_PublishJson(topic, json);
}

/* ==========================================================================
 * [业务层] 一键服务封装
 * ========================================================================== */

/**
 * @brief ESP8266联网初始化: AT自检 → WiFi → MQTT (含日志)
 */
ESP8266_Status ESP8266_ServiceInit(void)
{
    ESP8266_Status ret;

    dbg_println("[ESP8266] Service init start...");

    /* 阶段1: 硬件初始化 + AT自检 */
    dbg_println("[ESP8266] Step 1: Hardware init & AT test...");
    ret = ESP8266_Init();
    if (ret != ESP8266_OK) {
        dbg_println("[ESP8266] Init FAILED");
        return ret;
    }
    dbg_println("[ESP8266] Init OK");

    /* 阶段2: 连接WiFi */
    dbg_println("[ESP8266] Step 2: Connecting WiFi...");
    ret = ESP8266_ConnectWiFi(ESP8266_WIFI_SSID, ESP8266_WIFI_PASSWORD);
    if (ret != ESP8266_OK) {
        dbg_println("[ESP8266] WiFi connect FAILED");
        return ret;
    }
    dbg_println("[ESP8266] WiFi OK");

    /* 阶段3: 连接OneNET MQTT */
    dbg_println("[ESP8266] Step 3: Connecting MQTT...");
    ret = ESP8266_MQTT_Connect(
        ONENET_MQTT_BROKER,
        ONENET_MQTT_PORT,
        ONENET_DEVICE_NAME,
        ONENET_PRODUCT_ID,
        ONENET_DEVICE_TOKEN
    );
    if (ret != ESP8266_OK) {
        dbg_println("[ESP8266] MQTT connect FAILED");
        return ret;
    }
    dbg_println("[ESP8266] MQTT OK");

    dbg_println("[ESP8266] Service init SUCCESS");
    return ESP8266_OK;
}

/**
 * @brief 检查并恢复连接 (供Task周期性调用)
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
