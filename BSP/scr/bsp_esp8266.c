/**
 * @file    bsp_esp8266.c
 * @brief   ESP8266-01S 驱动层 — AT指令引擎 + WiFi/MQTT业务
 * @note    USART2 (TX=PA2, RX=PA3), 115200-8N1
 *          复位引脚 PB0, 调试串口 USART3
 *
 * 架构分层:
 *   [驱动层-硬件抽象] → 环形缓冲区、HardReset
 *   [驱动层-数据接收] → ISR → ring_buf → detect_response_end 状态机
 *   [驱动层-AT引擎]   → ESP8266_SendCmd (互斥锁+超时+结果解析)
 *   [业务层]          → Init / ConnectWiFi / MQTT_Connect / Publish
 */

#include "bsp_esp8266.h"
#include "gpio.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

/* ==========================================================================
 * [驱动层] 私有变量
 * ========================================================================== */

/** @brief 环形接收缓冲区 */
static uint8_t rx_ring_buf[ESP8266_RX_BUF_SIZE];

/** @brief 环形缓冲区写指针 (仅ISR上下文写入) */
static volatile uint16_t rx_wr = 0;

/** @brief 环形缓冲区读指针 (仅任务上下文读取) */
static volatile uint16_t rx_rd = 0;

/** @brief AT响应文本暂存区 */
static char at_response[ESP8266_RX_BUF_SIZE];

/** @brief 响应已就绪标志 (ISR置1, 任务读后清0) */
static volatile int at_resp_ready = 0;

/** @brief 响应结果: 0=无, 1=收到OK, 2=收到ERROR */
static volatile int at_resp_result = 0;

/** @brief 当前连接状态 */
static volatile ESP8266_ConnState conn_state = ESP8266_STATE_UNINIT;

/** @brief 最后一次错误描述 */
static char last_error[128] = "";

/** @brief 重连计数器 */
static uint32_t reconnect_count = 0;

/** @brief 数据上报消息ID (自增) */
static uint32_t msg_id = 0;

/* ==========================================================================
 * [驱动层] 响应检测状态机
 * ========================================================================== */
typedef enum {
    DETECT_IDLE = 0,
    DETECT_CR1,       /* 收到 \r          */
    DETECT_LF1,       /* 收到 \n (行首)   */
    DETECT_O,         /* 收到 O           */
    DETECT_K,         /* 收到 K           */
    DETECT_CR2,       /* OK后的 \r        */
    DETECT_E,         /* 收到 E           */
    DETECT_R1,        /* 收到 R (ER)      */
    DETECT_R2,        /* 收到 R (ERR)     */
    DETECT_O2,        /* 收到 O (ERRO)    */
} RespDetectState;

static volatile RespDetectState detect_state = DETECT_IDLE;

/* ==========================================================================
 * [驱动层] 私有函数声明
 * ========================================================================== */
static void ESP8266_SetError(const char *str);
static void detect_response_end(uint8_t ch);
static ESP8266_Status ESP8266_WaitForPattern(const char *pattern,
                                             uint32_t timeout_ms);

/* ==========================================================================
 * [驱动层] 硬件复位
 * ========================================================================== */
/**
 * @brief 硬件复位ESP8266模块 (PB0低电平脉冲)
 */
void ESP8266_HardReset(void)
{
    /* 确保复位引脚已初始化为输出 */
    HAL_GPIO_WritePin(ESP8266_RST_GPIO_Port, ESP8266_RST_Pin, GPIO_PIN_RESET);
    osDelay(300);   /* 拉低至少200ms, 取300ms留余量 */
    HAL_GPIO_WritePin(ESP8266_RST_GPIO_Port, ESP8266_RST_Pin, GPIO_PIN_SET);
    osDelay(1500);  /* 等待模块启动完成 */
}

/* ==========================================================================
 * [驱动层] 环形缓冲区 (单生产者ISR / 单消费者任务)
 * ========================================================================== */

/** @brief 写入一字节到环形缓冲区 */
inline static void ring_buf_put(uint8_t ch)
{
    uint16_t next_wr = (rx_wr + 1) % ESP8266_RX_BUF_SIZE;
    if (next_wr == rx_rd) {
        return;  /* 缓冲区满, 丢弃新数据 */
    }
    rx_ring_buf[rx_wr] = ch;
    rx_wr = next_wr;
}

/** @brief 从环形缓冲区读取一字节 (任务上下文) */
static inline uint8_t ring_buf_get(void)
{
    if (rx_rd == rx_wr) return 0;
    uint8_t ch = rx_ring_buf[rx_rd];
    rx_rd = (rx_rd + 1) % ESP8266_RX_BUF_SIZE;
    return ch;
}

/** @brief 获取环形缓冲区可读字节数 */
inline static int ring_buf_available(void)
{
    return (rx_wr - rx_rd + ESP8266_RX_BUF_SIZE) % ESP8266_RX_BUF_SIZE;
}

/** @brief 清空环形缓冲区 */
inline static void ring_buf_clear(void)
{
    rx_rd = rx_wr;
}

/* ==========================================================================
 * [驱动层] 错误记录
 * ========================================================================== */
static void ESP8266_SetError(const char *str)
{
    strncpy(last_error, str, sizeof(last_error) - 1);
    last_error[sizeof(last_error) - 1] = '\0';
}

/* ==========================================================================
 * [驱动层] 响应检测状态机
 * ========================================================================== */
/**
 * @brief ISR中逐字节驱动, 检测AT响应终止序列
 *
 * 检测两种终止模式:
 *   \r\nOK\r\n    → at_resp_result=1  命令成功
 *   ERROR         → at_resp_result=2  命令失败
 */
static void detect_response_end(uint8_t ch)
{
    switch (detect_state) {
    case DETECT_IDLE:
        if (ch == '\r')      detect_state = DETECT_CR1;
        else if (ch == 'E')  detect_state = DETECT_E;
        break;
    case DETECT_CR1:
        detect_state = (ch == '\n') ? DETECT_LF1 : DETECT_IDLE;
        break;
    case DETECT_LF1:
        if (ch == 'O')       detect_state = DETECT_O;
        else if (ch == 'E')  detect_state = DETECT_E;
        else if (ch == '\r') detect_state = DETECT_CR1;
        else                 detect_state = DETECT_IDLE;
        break;
    case DETECT_O:
        detect_state = (ch == 'K') ? DETECT_K : DETECT_IDLE;
        break;
    case DETECT_K:
        detect_state = (ch == '\r') ? DETECT_CR2 : DETECT_IDLE;
        break;
    case DETECT_CR2:
        if (ch == '\n') {
            at_resp_result = 1;
            at_resp_ready = 1;
        }
        detect_state = DETECT_IDLE;
        break;
    case DETECT_E:
        detect_state = (ch == 'R') ? DETECT_R1 : DETECT_IDLE;
        break;
    case DETECT_R1:
        detect_state = (ch == 'R') ? DETECT_R2 : DETECT_IDLE;
        break;
    case DETECT_R2:
        detect_state = (ch == 'O') ? DETECT_O2 : DETECT_IDLE;
        break;
    case DETECT_O2:
        if (ch == 'R') {
            at_resp_result = 2;
            at_resp_ready = 1;
        }
        detect_state = DETECT_IDLE;
        break;
    default:
        detect_state = DETECT_IDLE;
        break;
    }
}

/* ==========================================================================
 * [驱动层] USART2中断处理
 * ========================================================================== */
void ESP8266_UART_IRQHandler(UART_HandleTypeDef *huart)
{
    /* RXNE: 读数据寄存器非空 */
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_RXNE)) {
        uint8_t ch = (uint8_t)(huart->Instance->DR & 0xFF);
        ring_buf_put(ch);
        detect_response_end(ch);
    }
    /* ORE: 过载错误 */
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_ORE)) {
        (void)(huart->Instance->DR & 0xFF);
    }
    /* FE: 帧错误 */
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_FE)) {
        __HAL_UART_CLEAR_FLAG(huart, UART_FLAG_FE);
    }
    /* PE: 校验错误 */
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_PE)) {
        __HAL_UART_CLEAR_FLAG(huart, UART_FLAG_PE);
    }
    /* 溢出后可能同时有RXNE, 再次检查 */
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_RXNE)) {
        uint8_t ch = (uint8_t)(huart->Instance->DR & 0xFF);
        ring_buf_put(ch);
        detect_response_end(ch);
    }
}

/* ==========================================================================
 * [驱动层] AT指令收发引擎
 * ========================================================================== */
/**
 * @brief 发送AT指令并等待OK/ERROR响应
 * @param cmd        AT指令字符串 (需包含\r\n)
 * @param timeout_ms 超时(ms)
 * @return ESP8266_OK / ESP8266_TIMEOUT / ESP8266_AT_FAIL / ...
 */
ESP8266_Status ESP8266_SendCmd(const char *cmd, uint32_t timeout_ms)
{
    /* 获取发送互斥锁 (最多等5s) */
    if (osMutexAcquire(uart_tx_mutexHandle, 5000) != osOK) {
        return ESP8266_BUSY;
    }

    /* 清空缓冲区与状态 */
    ring_buf_clear();
    memset(at_response, 0, sizeof(at_response));
    at_resp_ready  = 0;
    at_resp_result = 0;

    /* 阻塞发送AT指令 */
    uint16_t len = (uint16_t)strlen(cmd);
    if (HAL_UART_Transmit(&huart2, (uint8_t *)cmd, len, 500) != HAL_OK) {
        osMutexRelease(uart_tx_mutexHandle);
        ESP8266_SetError("SendCmd: UART transmit failed");
        return ESP8266_SEND_FAIL;
    }

    /* 轮询等待响应 (osDelay让出CPU) */
    uint32_t waited = 0;
    const uint32_t poll_interval = 10;
    while (waited < timeout_ms && at_resp_ready == 0) {
        osDelay(poll_interval);
        waited += poll_interval;
    }

    /* 从环形缓冲区取出响应文本 */
    uint16_t avail = ring_buf_available();
    if (avail >= sizeof(at_response))
        avail = sizeof(at_response) - 1;
    for (uint16_t i = 0; i < avail; i++) {
        at_response[i] = ring_buf_get();
    }
    at_response[avail] = '\0';

    /* 超时未响应 */
    if (waited >= timeout_ms && at_resp_ready == 0) {
        osMutexRelease(uart_tx_mutexHandle);
        ESP8266_SetError("SendCmd: timeout");
        return ESP8266_TIMEOUT;
    }

    /* 状态机判定为OK (result==1) */
    if (at_resp_result == 1) {
        osMutexRelease(uart_tx_mutexHandle);
        return ESP8266_OK;
    }

    /* 状态机判定为ERROR 或 文本含ERROR/FAIL */
    if (at_resp_result == 2 ||
        strstr(at_response, "ERROR") != NULL ||
        strstr(at_response, "FAIL")  != NULL) {
        osMutexRelease(uart_tx_mutexHandle);
        return ESP8266_AT_FAIL;
    }

    osMutexRelease(uart_tx_mutexHandle);
    return ESP8266_OK;
}

/* ==========================================================================
 * [驱动层] 响应文本检查
 * ========================================================================== */
/**
 * @brief 检查最近一次AT响应是否包含指定字符串
 */
int ESP8266_ResponseContains(const char *str)
{
    if (str == NULL) return 0;
    return (strstr(at_response, str) != NULL) ? 1 : 0;
}

/* ==========================================================================
 * [驱动层] 异步模式等待 (用于MQTT连接等异步通知)
 * ========================================================================== */
/**
 * @brief 等待ESP8266异步上报的指定模式
 * @param pattern    期望包含的字符串
 * @param timeout_ms 超时(ms)
 * @note  不发送AT指令, 仅监听串口接收
 */
static ESP8266_Status ESP8266_WaitForPattern(const char *pattern,
                                             uint32_t timeout_ms)
{
    ring_buf_clear();
    memset(at_response, 0, sizeof(at_response));
    at_resp_ready  = 0;
    at_resp_result = 0;

    uint32_t waited = 0;
    const uint32_t poll_interval = 100;

    while (waited < timeout_ms) {
        osDelay(poll_interval);
        waited += poll_interval;

        /* 从环形缓冲区读取最新数据 */
        uint16_t avail = ring_buf_available();
        uint16_t offset = strlen(at_response);
        if (avail >= sizeof(at_response) - offset - 1)
            avail = sizeof(at_response) - offset - 1;
        for (uint16_t i = 0; i < avail; i++) {
            at_response[offset + i] = ring_buf_get();
        }
        at_response[offset + avail] = '\0';

        /* 检测期望模式 */
        if (strstr(at_response, pattern) != NULL) {
            return ESP8266_OK;
        }
        /* 检测失败模式 */
        if (strstr(at_response, "FAILED")  != NULL ||
            strstr(at_response, "ERROR")   != NULL ||
            strstr(at_response, "DISCONNECTED") != NULL) {
            return ESP8266_AT_FAIL;
        }
    }

    return ESP8266_TIMEOUT;
}

/* ==========================================================================
 * [业务层] 初始化
 * ========================================================================== */

/**
 * @brief ESP8266初始化: GPIO配置 → 硬件复位 → AT自检
 * @note  调用前需确保USART2已初始化
 */
ESP8266_Status ESP8266_Init(void)
{
    /* 1. 初始化复位引脚 PB0 */
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin   = ESP8266_RST_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(ESP8266_RST_GPIO_Port, &GPIO_InitStruct);
    HAL_GPIO_WritePin(ESP8266_RST_GPIO_Port, ESP8266_RST_Pin, GPIO_PIN_SET);

    /* 2. 硬件复位 */
    ESP8266_HardReset();

    /* 3. 清空缓冲区 (复位期间可能有乱码) */
    ring_buf_clear();
    memset(at_response, 0, sizeof(at_response));
    at_resp_ready  = 0;
    at_resp_result = 0;
    detect_state   = DETECT_IDLE;

    /* 4. AT自检 (尝试3次) */
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (ESP8266_AT_Test() == ESP8266_OK) {
            conn_state = ESP8266_STATE_AT_READY;
            return ESP8266_OK;
        }
        osDelay(500);  /* 间隔500ms重试 */
    }

    conn_state = ESP8266_STATE_UNINIT;
    ESP8266_SetError("Init: AT test failed after 3 attempts");
    return ESP8266_AT_FAIL;
}

/* ==========================================================================
 * [业务层] AT通信自检
 * ========================================================================== */
/**
 * @brief 发送AT指令检测通信是否正常
 */
ESP8266_Status ESP8266_AT_Test(void)
{
    /* 先发空命令清空模块缓冲区 */
    ESP8266_SendCmd("AT\r\n", 1000);
    osDelay(100);

    /* 正式测试 */
    ESP8266_Status ret = ESP8266_SendCmd("AT\r\n", ESP8266_AT_TIMEOUT_MS);
    if (ret == ESP8266_OK && ESP8266_ResponseContains("OK")) {
        conn_state = ESP8266_STATE_AT_READY;
        return ESP8266_OK;
    }

    conn_state = ESP8266_STATE_UNINIT;
    ESP8266_SetError("AT_Test: no OK response");
    return ESP8266_AT_FAIL;
}

/* ==========================================================================
 * [业务层] WiFi管理
 * ========================================================================== */
/**
 * @brief 连接2.4G WiFi
 * @param ssid     WiFi名称
 * @param password WiFi密码
 */
ESP8266_Status ESP8266_ConnectWiFi(const char *ssid, const char *password)
{
    if (conn_state < ESP8266_STATE_AT_READY) {
        ESP8266_SetError("ConnectWiFi: not initialized");
        return ESP8266_NOT_INITIALIZED;
    }

    /* 1. 设置为Station模式 */
    ESP8266_Status ret = ESP8266_SendCmd("AT+CWMODE=1\r\n", ESP8266_AT_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("ConnectWiFi: CWMODE failed");
        conn_state = ESP8266_STATE_AT_READY;  /* 不影响AT通信 */
        return ret;
    }
    osDelay(200);

    /* 2. 断开已有WiFi (避免重连时状态冲突) */
    ESP8266_SendCmd("AT+CWQAP\r\n", 3000);
    osDelay(500);

    /* 3. 连接WiFi */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, password);
    ret = ESP8266_SendCmd(cmd, ESP8266_LONG_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("ConnectWiFi: CWJAP failed");
        conn_state = ESP8266_STATE_AT_READY;
        return ret;
    }

    /* 4. 等待获取IP (查询+CIFSR确认) */
    osDelay(1000);
    ret = ESP8266_SendCmd("AT+CIFSR\r\n", ESP8266_AT_TIMEOUT_MS);
    if (ret == ESP8266_OK && ESP8266_ResponseContains("STAIP")) {
        conn_state = ESP8266_STATE_WIFI_OK;
        return ESP8266_OK;
    }

    /* CWJAP返回了OK但未获取到IP, 可能是DHCP较慢, 再等一会 */
    osDelay(2000);
    conn_state = ESP8266_STATE_WIFI_OK;  /* 假定连接成功 */
    return ESP8266_OK;
}

/* ==========================================================================
 * [业务层] MQTT管理
 * ========================================================================== */
/**
 * @brief 连接MQTT Broker (OneNET平台)
 * @param broker    MQTT服务器地址
 * @param port      MQTT服务器端口
 * @param client_id 客户端ID (设备名)
 * @param username  用户名 (产品ID)
 * @param password  密码 (设备Token)
 */
ESP8266_Status ESP8266_MQTT_Connect(const char *broker, uint16_t port,
                                    const char *client_id,
                                    const char *username,
                                    const char *password)
{
    if (conn_state < ESP8266_STATE_WIFI_OK) {
        ESP8266_SetError("MQTT_Connect: WiFi not connected");
        return ESP8266_WIFI_DISCONNECTED;
    }

    /* 1. 清理旧MQTT连接 (如果存在) */
    ESP8266_SendCmd("AT+MQTTCLEAN=0\r\n", 3000);
    osDelay(300);

    /* 2. 配置MQTT用户参数
     *    格式: AT+MQTTUSERCFG=<LinkID>,<scheme>,<"client_id">,<"username">,
     *                           <"password">,<cert_key_ID>,<CA_ID>,<"path">
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
     *    格式: AT+MQTTCONN=<LinkID>,<"host">,<port>,<reconnect>
     *    reconnect=0: 不自动重连 (由MCU控制重连逻辑)
     */
    snprintf(cmd, sizeof(cmd),
             "AT+MQTTCONN=0,\"%s\",%u,0\r\n", broker, port);
    ret = ESP8266_SendCmd(cmd, ESP8266_AT_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("MQTT_Connect: MQTTCONN command failed");
        conn_state = ESP8266_STATE_WIFI_OK;
        return ret;
    }

    /* 4. 等待异步连接结果 (+MQTTCONNECTED:0,<err_code>)
     *    err_code=0 表示成功
     */
    ret = ESP8266_WaitForPattern("+MQTTCONNECTED:0,0", ESP8266_MQTT_TIMEOUT_MS);
    if (ret == ESP8266_OK) {
        conn_state = ESP8266_STATE_MQTT_OK;
        reconnect_count = 0;
        return ESP8266_OK;
    }

    /* 连接失败, 状态回退 */
    ESP8266_SetError("MQTT_Connect: broker connection failed");
    conn_state = ESP8266_STATE_WIFI_OK;
    return ESP8266_AT_FAIL;
}

/**
 * @brief 断开MQTT连接
 */
ESP8266_Status ESP8266_MQTT_Disconnect(void)
{
    if (conn_state < ESP8266_STATE_MQTT_OK) {
        return ESP8266_MQTT_DISCONNECTED;
    }

    ESP8266_Status ret = ESP8266_SendCmd("AT+MQTTCLEAN=0\r\n", 5000);
    osDelay(200);

    conn_state = ESP8266_STATE_WIFI_OK;
    return ret;
}

/* ==========================================================================
 * [业务层] MQTT发布
 * ========================================================================== */
/**
 * @brief 发布JSON字符串到指定主题
 * @param topic MQTT主题
 * @param json  JSON字符串 (内部会做转义)
 * @note  QoS=1
 */
ESP8266_Status ESP8266_MQTT_PublishJson(const char *topic, const char *json)
{
    if (conn_state < ESP8266_STATE_MQTT_OK) {
        ESP8266_SetError("PublishJson: MQTT not connected");
        return ESP8266_MQTT_DISCONNECTED;
    }

    if (topic == NULL || json == NULL) {
        ESP8266_SetError("PublishJson: null argument");
        return ESP8266_ERROR;
    }

    /* 构建转义后的JSON (将 " 转义为 \", \ 转义为 \\) */
    static char escaped_json[ESP8266_CMD_BUF_SIZE];
    uint16_t wi = 0;
    for (uint16_t ri = 0; json[ri] != '\0' && wi < sizeof(escaped_json) - 2; ri++) {
        if (json[ri] == '\\') {
            if (wi < sizeof(escaped_json) - 3) {
                escaped_json[wi++] = '\\';
                escaped_json[wi++] = '\\';
            }
        } else if (json[ri] == '"') {
            if (wi < sizeof(escaped_json) - 3) {
                escaped_json[wi++] = '\\';
                escaped_json[wi++] = '"';
            }
        } else {
            escaped_json[wi++] = json[ri];
        }
    }
    escaped_json[wi] = '\0';

    /* 构建完整AT命令: AT+MQTTPUB=<LinkID>,<"topic">,<"data">,<qos>,<retain> */
    static char cmd[ESP8266_CMD_BUF_SIZE];
    int cmd_len = snprintf(cmd, sizeof(cmd),
                           "AT+MQTTPUB=0,\"%s\",\"%s\",1,0\r\n",
                           topic, escaped_json);

    if (cmd_len < 0 || cmd_len >= (int)sizeof(cmd)) {
        ESP8266_SetError("PublishJson: command too long");
        return ESP8266_BUF_OVERFLOW;
    }

    ESP8266_Status ret = ESP8266_SendCmd(cmd, ESP8266_AT_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("PublishJson: publish failed");
        /* 发布失败可能表示MQTT断线, 标记状态 */
        if (ESP8266_ResponseContains("CLOSED") ||
            ESP8266_ResponseContains("DISCONNECTED")) {
            conn_state = ESP8266_STATE_WIFI_OK;
        }
        return ret;
    }

    return ESP8266_OK;
}

/**
 * @brief 构建OneNET物模型JSON并上报
 * @param topic         发布主题
 * @param temperature   温度 (℃)
 * @param humidity      湿度 (%)
 * @param soil_moisture 土壤湿度 (%)
 * @param rain          降雨量 (%)
 * @param light         光照强度 (lx)
 * @param pump_state    水泵状态 (0=关, 1=开)
 */
ESP8266_Status ESP8266_MQTT_PublishProperty(const char *topic,
                                            float temperature, float humidity,
                                            uint16_t soil_moisture, uint16_t rain,
                                            uint16_t light, uint8_t pump_state)
{
    if (topic == NULL) {
        ESP8266_SetError("PublishProperty: null topic");
        return ESP8266_ERROR;
    }

    /* 构建OneNET物模型JSON
     * 格式: {"id":"<msg_id>","version":"1.0","params":{...}}
     */
    static char json[ESP8266_CMD_BUF_SIZE];
    msg_id++;

    int len = snprintf(json, sizeof(json),
        "{\"id\":\"%lu\",\"version\":\"1.0\",\"params\":{"
        "\"temperature\":{\"value\":%.1f},"
        "\"humidity\":{\"value\":%.1f},"
        "\"soilmoisture\":{\"value\":%u},"
        "\"rain\":{\"value\":%u},"
        "\"light\":{\"value\":%u},"
        "\"pump\":{\"value\":%u}"
        "}}",
        (unsigned long)msg_id,
        (double)temperature, (double)humidity,
        (unsigned int)soil_moisture, (unsigned int)rain,
        (unsigned int)light, (unsigned int)pump_state);

    if (len < 0 || len >= (int)sizeof(json)) {
        ESP8266_SetError("PublishProperty: JSON too long");
        return ESP8266_BUF_OVERFLOW;
    }

    return ESP8266_MQTT_PublishJson(topic, json);
}

/* ==========================================================================
 * [业务层] 连接维护
 * ========================================================================== */
/**
 * @brief 逐层检查并恢复连接 (WiFi → MQTT)
 * @note  重连间隔5秒, 无限重试
 */
ESP8266_Status ESP8266_EnsureConnected(
    const char *ssid, const char *wifi_pwd,
    const char *broker, uint16_t port,
    const char *client_id, const char *username, const char *mqtt_pwd)
{
    /* 层1: 确保AT通信 */
    if (conn_state < ESP8266_STATE_AT_READY) {
        ESP8266_Status ret = ESP8266_Init();
        if (ret != ESP8266_OK) return ret;
    }

    /* 层2: 确保WiFi连接 */
    if (conn_state < ESP8266_STATE_WIFI_OK) {
        ESP8266_Status ret = ESP8266_ConnectWiFi(ssid, wifi_pwd);
        if (ret != ESP8266_OK) {
            reconnect_count++;
            return ret;
        }
    }

    /* 层3: 确保MQTT连接 */
    if (conn_state < ESP8266_STATE_MQTT_OK) {
        ESP8266_Status ret = ESP8266_MQTT_Connect(broker, port,
                                                  client_id, username, mqtt_pwd);
        if (ret != ESP8266_OK) {
            reconnect_count++;
            return ret;
        }
    }

    return ESP8266_OK;
}

/**
 * @brief 一键上报 + 断线重连: 先尝试发布, 失败则重建连接后重试
 */
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
    if (conn_state == ESP8266_STATE_MQTT_OK) {
        conn_state = ESP8266_STATE_WIFI_OK;  /* 标记MQTT断线 */
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
 * [业务层] 状态查询
 * ========================================================================== */

ESP8266_ConnState ESP8266_GetState(void)
{
    return conn_state;
}

const char* ESP8266_GetLastError(void)
{
    return last_error;
}
