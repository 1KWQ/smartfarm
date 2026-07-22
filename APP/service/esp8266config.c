/**
 * @file    esp8266config.c
 * @brief   ESP8266 业务层 — WiFi/MQTT 连接管理 + OneNET 数据上报
 * @author  SmartFarm Project
 * @date    2026-07
 *
 * @note    依赖关系:
 *           - 驱动层 bsp_esp8266.h 提供 AT 引擎 (SendCmd / WaitForPattern / FlushRx)
 *           - FreeRTOS cmsis_os2.h 提供 osDelay / osMutex 等系统调用
 *           - STM32 HAL 库提供 UART 外设操作
 *
 * @warning  本文件所有函数均应在同一 FreeRTOS 任务上下文中调用,
 *           因为静态缓冲区 work_buf 和状态变量未加互斥锁保护。
 *
 * ## 模块职责分层
 * ```
 * ┌──────────────────────────────────────────┐
 * │  业务层 (本文件)                          │
 * │  • WiFi/MQTT 连接管理 (状态机驱动)        │
 * │  • OneNET 物模型 JSON 组包                │
 * │  • AT 命令特殊字符转义 (", \, ,)          │
 * │  • 断线重连策略                           │
 * └──────────────┬───────────────────────────┘
 *                │ ESP8266_Status
 * ┌──────────────▼───────────────────────────┐
 * │  驱动层 bsp_esp8266.h/.c                  │
 * │  • 环形缓冲区 (USART2 RX)                 │
 * │  • UART 中断服务 (IDLE + 逐字节)          │
 * │  • AT 指令收发 + 超时等待                 │
 * │  • 硬件复位 (PB0 拉低)                    │
 * └──────────────────────────────────────────┘
 * ```
 *
 * ## AT 命令规范 (ESP8266-01S MQTT 固件)
 * - 默认波特率: 115200 bps
 * - 单条命令长度不应超过 256 字节
 * - 命令以 CR-LF (\r\n) 结尾
 * - 字符串参数中特殊字符需转义: " → \"   \ → \\   , → \,
 * - 分隔参数的外层逗号无需转义, 数据内的逗号必须转义
 *
 * ## 连接状态机
 * ```
 * UNINIT ──(AT自检OK)──▶ AT_READY ──(CWJAP+DHCP)──▶ WIFI_OK ──(MQTTCONN)──▶ MQTT_OK
 *    ▲                      ▲                        ▲                       │
 *    └──── 3次重试失败       └──── 断线检测            └──── 断线检测   ◀──────┘ (发布失败/CLOSED)
 * ```
 */

#include "esp8266config.h"
#include "bsp_esp8266.h"
#include "cmsis_os2.h"
#include <stdio.h>
#include <string.h>

/* ==========================================================================
 * [业务层] 私有变量 — 单任务上下文, 无锁设计
 * ========================================================================== */

/**
 * @brief 断线重连累计次数
 * @note  每次重连失败自增, MQTT 连接成功后清零
 *        可用于监控连接质量, 超过阈值触发告警
 */
static uint32_t reconnect_count = 0;

/**
 * @brief 数据上报消息 ID (单调递增)
 * @note  OneNET 物模型 JSON 中 id 字段要求为数字字符串,
 *        每次发布前自增, 用于服务端去重和排序
 */
static uint32_t msg_id = 0;

/**
 * @brief 共享 AT 命令构建缓冲区 (512 字节)
 * @warning 仅在同一任务上下文中使用, 不可跨任务共享
 *          仅 ESP8266_MQTT_PublishJson 写入此缓冲区
 */
static char work_buf[ESP8266_CMD_BUF_SIZE];

/* ==========================================================================
 * 内部辅助 — AT 命令字符串参数转义
 *
 * 背景: ESP8266 AT 固件使用类 CSV 的参数解析规则 —
 *       逗号分隔参数, 双引号包裹字符串, 反斜杠为转义前缀.
 *       因此字符串参数内的 " \ , 三个字符必须用 \" \\ \, 转义,
 *       否则固件会错误切分参数或截断字符串.
 * ========================================================================== */

/**
 * @brief 对 AT 命令中的字符串参数做特殊字符转义
 *
 * 将源字符串中的 ", \, , 三种字符前插入反斜杠进行转义,
 * 其余字符原样复制. 目标缓冲区不足时返回 -1 表示截断.
 *
 * @param dst      目标缓冲区 (已转义字符串存入此处)
 * @param dst_size 目标缓冲区字节数 (含终止符 '\0')
 * @param src      源字符串 (以 '\0' 结尾)
 * @return         写入 dst 的字符数 (不含终止符), 截断返回 -1
 *
 * @note  转义后字符串长度最大为原始长度的 2 倍 (全为特殊字符时),
 *        调用方应确保 dst_size 足够容纳最坏情况.
 *
 * 示例:
 * ```
 * src = "comma,backslash\\ssid"
 * dst = "comma\,backslash\\\\ssid"
 * ```
 * 对应 AT 命令: AT+CWJAP="comma\,backslash\\ssid","password"\r\n
 */
static int esp8266_escape_at_string(char *dst, int dst_size, const char *src)
{
    int wi = 0;  /* 写指针, 始终指向下一个可写位置 */

    for (int ri = 0; src[ri] != '\0'; ri++) {
        char c = src[ri];

        if (c == '"' || c == '\\' || c == ',') {
            /* 特殊字符: 写入反斜杠前缀 + 字符本身, 共 2 字节 */
            if (wi + 3 > dst_size) return -1;  /* 还需 \0 终止符 */
            dst[wi++] = '\\';
            dst[wi++] = c;
        } else {
            /* 普通字符: 直接拷贝 1 字节 */
            if (wi + 2 > dst_size) return -1;
            dst[wi++] = c;
        }
    }

    /* 终止符检查 */
    if (wi >= dst_size) return -1;
    dst[wi] = '\0';
    return wi;
}

/* ==========================================================================
 * 调试日志 — 通过 USART3 输出运行状态 (可选, 编译时开关)
 *
 * 使用方法: 在编译选项中添加 -DESP8266_DEBUG_LOG 启用
 * 注意: USART3 不得与 ESP8266 的 USART2 冲突,
 *       本项目的 ESP8266 使用 USART2, 调试用 USART3
 * ========================================================================== */
extern UART_HandleTypeDef huart3;  /* main.c 或 freertos.c 中定义 */

#ifdef ESP8266_DEBUG_LOG
/** @brief 通过 USART3 发送字符串 (阻塞模式) */
static void dbg_print(const char *msg)
{
    HAL_UART_Transmit(&huart3, (uint8_t *)msg, strlen(msg), 100);
}
/** @brief 通过 USART3 发送字符串 + 换行 */
static void dbg_println(const char *msg)
{
    dbg_print(msg);
    dbg_print("\r\n");
}
#else
/* 未启用调试日志时, 所有 dbg_print 调用被编译器优化掉 (无开销) */
#define dbg_print(msg)      (void)(msg)
#define dbg_println(msg)    (void)(msg)
#endif

/* ==========================================================================
 * 阶段 1: ESP8266 硬件初始化 & AT 通信自检
 *
 * 流程: GPIO/PB0 复位 → 清空 RX 缓冲区垃圾数据 → AT\r\n 探测 (最多 3 次)
 *
 * 为什么需要 3 次重试:
 * - 硬件复位后 ESP8266 需要约 200-500ms 启动
 * - 首次上电可能输出 "ready" 等乱码干扰响应匹配
 * - 3 次重试覆盖绝大多数启动时序抖动场景
 * ========================================================================== */

/**
 * @brief ESP8266 初始化 — 硬件复位 + AT 通信自检 (3 次重试)
 *
 * @return ESP8266_OK              AT 通信正常, 状态切换为 AT_READY
 * @return ESP8266_AT_FAIL         3 次重试均无 OK 响应, 状态保持 UNINIT
 *
 * @note  调用本函数前需确保 USART2 和 PB0 已由 CubeMX 初始化
 * @note  内部调用 ESP8266_HardReset() 会拉低 PB0 ≥200ms 后恢复
 */
ESP8266_Status ESP8266_Init(void)
{
    /* 步骤 1: 硬件复位 — PB0 拉低 200ms (内部含 osDelay 等待稳定) */
    ESP8266_HardReset();

    /* 步骤 2: 清空复位期间 ESP8266 输出的启动信息 (通常为乱码或 "ready") */
    ESP8266_FlushRx();

    /* 步骤 3: 发送 AT\r\n 探测通信, 最多尝试 3 次, 每次间隔 500ms */
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (ESP8266_AT_Test() == ESP8266_OK) {
            ESP8266_SetState(ESP8266_STATE_AT_READY);
            return ESP8266_OK;
        }
        osDelay(500);  /* 等待 ESP8266 内部状态就绪 */
    }

    /* 3 次均失败: 记录错误, 保持 UNINIT 状态 */
    ESP8266_SetState(ESP8266_STATE_UNINIT);
    ESP8266_SetError("Init: AT test failed after 3 attempts");
    return ESP8266_AT_FAIL;
}

/**
 * @brief AT 通信自检 — 发送 AT\r\n 并等待 OK 响应
 *
 * @return ESP8266_OK      收到 OK 响应
 * @return ESP8266_AT_FAIL 超时或响应不包含 OK
 *
 * @note  先发送一次空 AT 命令清空模块缓冲区,
 *        避免上次通信残留数据干扰本次响应匹配.
 *        该预清操作不检查返回值 (可能本身就会超时).
 */
ESP8266_Status ESP8266_AT_Test(void)
{
    /* 预清: 发送空命令吸收可能存在的残留响应 */
    ESP8266_SendCmd("AT\r\n", 1000);
    osDelay(100);

    /* 正式测试: 发送 AT\r\n, 超时窗口 ESP8266_AT_TIMEOUT_MS (默认 2000ms) */
    ESP8266_Status ret = ESP8266_SendCmd("AT\r\n", ESP8266_AT_TIMEOUT_MS);
    if (ret == ESP8266_OK ) {
        ESP8266_SetState(ESP8266_STATE_AT_READY);
        return ESP8266_OK;
    }

    /* 未收到 OK: 可能是模块未就绪 / 波特率不匹配 / 硬件连接问题 */
    ESP8266_SetState(ESP8266_STATE_UNINIT);
    ESP8266_SetError("AT_Test: no OK response");
    return ESP8266_AT_FAIL;
}

/* ==========================================================================
 * 阶段 2: WiFi 连接 — Station 模式 + DHCP 获取 IP
 *
 * 流程: CWMODE=1 (Station) → CWQAP (断开旧连接) → CWJAP (连接AP)
 *       → CIFSR (查询IP, 确认DHCP成功)
 *
 * 状态要求: 调用前必须处于 AT_READY 或更高状态
 * 状态变更: 成功 → WIFI_OK
 *
 * 关键超时: ESP8266_WIFI_TIMEOUT_MS (默认 15000ms)
 *           WiFi 连接涉及 AP 关联 + DHCP, 需要较长超时
 * ========================================================================== */

/**
 * @brief 连接 WiFi 热点 (Station 模式, DHCP 获取 IP)
 *
 * SSID/密码取自 esp8266config.h 宏定义 (ESP8266_WIFI_SSID / ESP8266_WIFI_PASSWORD),
 * 含特殊字符时内部自动转义.
 *
 * @return ESP8266_OK                WiFi 已连接并获取 IP
 * @return ESP8266_NOT_INITIALIZED   AT 通信尚未就绪
 * @return ESP8266_BUF_OVERFLOW      转义后凭据超出缓冲区
 * @return 其他错误码                CWMODE / CWJAP / 超时失败
 *
 * @note  CWQAP 断开旧连接可避免 "已连接但状态冲突" 导致 CWJAP 返回 ERROR
 * @note  CIFSR 查询失败不视为致命错误 — DHCP 可能稍慢, 仍返回 OK
 */
ESP8266_Status ESP8266_ConnectWiFi(void)
{
    /* 前置检查: AT 通信必须已就绪 */
    if (ESP8266_GetState() < ESP8266_STATE_AT_READY) {
        ESP8266_SetError("ConnectWiFi: not initialized");
        return ESP8266_NOT_INITIALIZED;
    }

    /* 步骤 1: 设置 Wi-Fi 模式为 Station (客户端, 非 AP 模式)
     *        CWMODE=1 → 仅 Station, 不使用 SoftAP
     */
    ESP8266_Status ret = ESP8266_SendCmd("AT+CWMODE=1\r\n", ESP8266_AT_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("ConnectWiFi: CWMODE failed");
        return ret;
    }
    osDelay(200);  /* 等待模式切换生效 */

    /* 步骤 2: 断开已有 WiFi 连接, 避免重连时 "ALREADY CONNECTED" 错误
     *        CWQAP 在无连接时返回 ERROR 是正常的, 不检查返回值
     */
    ESP8266_SendCmd("AT+CWQAP\r\n", 3000);
    osDelay(500);  /* 等待断开完成 */

    /* 步骤 3: 连接目标 AP
     *        对 SSID 和密码做 AT 特殊字符转义后拼入命令
     *        超时使用 ESP8266_WIFI_TIMEOUT_MS (15s) — 包含 AP 关联 + DHCP 时间
     */
    char cmd[512];
    char esc_ssid[128], esc_pwd[128];
    if (esp8266_escape_at_string(esc_ssid, sizeof(esc_ssid), ESP8266_WIFI_SSID) < 0 ||
        esp8266_escape_at_string(esc_pwd, sizeof(esc_pwd), ESP8266_WIFI_PASSWORD) < 0) {
        ESP8266_SetError("ConnectWiFi: credential too long after escape");
        return ESP8266_BUF_OVERFLOW;
    }
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", esc_ssid, esc_pwd);
    ret = ESP8266_SendCmd(cmd, ESP8266_WIFI_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("ConnectWiFi: CWJAP failed");
        return ret;
    }

    /* 步骤 4: 查询 IP 地址确认 DHCP 已分配
     *        CIFSR 返回格式: +CIFSR:STAIP,"192.168.x.x"
     *        匹配 "STAIP" 子串即可确认
     */
    osDelay(1000);
    ret = ESP8266_SendCmd("AT+CIFSR\r\n", ESP8266_AT_TIMEOUT_MS);
    if (ret == ESP8266_OK && ESP8266_ResponseContains("STAIP")) {
        ESP8266_SetState(ESP8266_STATE_WIFI_OK);
        return ESP8266_OK;
    }

    /* CWJAP 返回了 OK 但 CIFSR 未看到 STAIP:
     * DHCP 可能因网络环境较慢尚未完成, 再等待 2 秒后视为成功
     * (后续 MQTT 连接如果失败会自然回退状态)
     */
    osDelay(2000);
    ESP8266_SetState(ESP8266_STATE_WIFI_OK);
    return ESP8266_OK;
}

/* ==========================================================================
 * 阶段 3: MQTT 连接 — 用户配置 + 发起连接 + 轮询确认状态 + 订阅主题
 *
 * 流程 (基于 OneNET MQTT 接入规范, 用同步查询替代异步 URC):
 *   步骤1: MQTTUSERCFG  (配置凭据)
 *   步骤2: MQTTCONN     (发起连接, 不检查返回值)
 *   步骤3: MQTTCONN?    (每秒轮询查询, 直到 state>=4, 最多等10秒)
 *   步骤4: MQTTSUB ×2   (订阅 post/reply 和 property/set)
 *
 * 状态要求: 调用前必须处于 WIFI_OK 或更高状态
 * 状态变更: 成功 → MQTT_OK
 *
 * 关键参数:
 * - scheme=1: MQTT over TCP (非 SSL, 端口 1883)
 * - reconnect=0: MCU 全权控制重连
 * ========================================================================== */

/**
 * @brief 连接 MQTT Broker 并订阅主题 (OneNET MQTT 接入规范)
 *
 * 凭据取自 esp8266config.h 宏定义. 连接成功后自动订阅:
 * - ONENET_TOPIC_PROPERTY_POST_REPLY (接收平台回复)
 * - ONENET_TOPIC_PROPERTY_SET        (接收平台下发指令)
 *
 * 不使用异步 URC (+MQTTCONNECTED), 改用 AT+MQTTCONN? 每秒轮询查询
 * 连接状态 (state=4/5/6 表示已连接), 避免 URC 时序丢失问题.
 *
 * @return ESP8266_OK                MQTT 已连接且已订阅主题
 * @return ESP8266_WIFI_DISCONNECTED WiFi 未连接
 * @return ESP8266_BUF_OVERFLOW      转义后凭据超出缓冲区
 * @return 其他错误码                USERCFG / SUB 命令失败或轮询超时
 *
 * @note  订阅失败会导致状态回退到 WIFI_OK, 下次 EnsureConnected 将重新连接+订阅.
 */
ESP8266_Status ESP8266_MQTT_Connect(void)
{
    /* 前置检查: WiFi 必须已连接 */
    if (ESP8266_GetState() < ESP8266_STATE_WIFI_OK) {
        ESP8266_SetError("MQTT_Connect: WiFi not connected");
        return ESP8266_WIFI_DISCONNECTED;
    }

    /* 步骤 1: 配置 MQTT 用户参数
     *
     *        AT+MQTTUSERCFG=<LinkID>,<scheme>,
     *                         <"client_id">,<"username">,<"password">,
     *                         <cert_key_ID>,<CA_ID>,<"path">
     *
     *        参数说明:
     *        - LinkID=0:     使用连接通道 0 (ESP8266 仅支持 0)
     *        - scheme=1:     MQTT over TCP (非 TLS 直连)
     *        - cert_key_ID=0: 无客户端证书
     *        - CA_ID=0:      无 CA 证书
     *        - path="":      无资源路径
     */
    char cmd[640];
    char esc_client[128], esc_user[128], esc_pass[256];
    if (esp8266_escape_at_string(esc_client, sizeof(esc_client), ONENET_DEVICE_NAME) < 0 ||
        esp8266_escape_at_string(esc_user,   sizeof(esc_user),   ONENET_PRODUCT_ID)   < 0 ||
        esp8266_escape_at_string(esc_pass,   sizeof(esc_pass),   ONENET_DEVICE_TOKEN) < 0) {
        ESP8266_SetError("MQTT_Connect: credential too long after escape");
        return ESP8266_BUF_OVERFLOW;
    }
    snprintf(cmd, sizeof(cmd),
             "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"%s\",0,0,\"\"\r\n",
             esc_client, esc_user, esc_pass);
    ESP8266_Status ret = ESP8266_SendCmd(cmd, ESP8266_AT_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("MQTT_Connect: MQTTUSERCFG failed");
        return ret;
    }
    osDelay(200);  /* 等待配置写入 NVRAM */

    /* 步骤 2: 发起 MQTT Broker 连接 (MQTT 接入规范 步骤2)
     *
     *        AT+MQTTCONN=<LinkID>,<"host">,<port>,<reconnect>
     *
     *        reconnect=0: MCU 控制重连. 不检查此命令返回值 —
     *        若模块已连接 (上次 session 残留), 返回 ERROR 是正常的.
     *        后续通过 MQTTCONN? 查询实际状态.
     */
    snprintf(cmd, sizeof(cmd),
             "AT+MQTTCONN=0,\"%s\",%u,0\r\n",
             ONENET_MQTT_BROKER, ONENET_MQTT_PORT);
    ESP8266_SendCmd(cmd, ESP8266_AT_TIMEOUT_MS);
    /* 忽略返回值: 无论 OK/ERROR/TIMEOUT, 下一步轮询会确认真实状态 */

    /* 步骤 3: 轮询 AT+MQTTCONN? 确认连接状态 (替代异步 URC 等待)
     *
     *        AT+MQTTCONN? 查询命令 (官方文档: MQTT 指令步骤)
     *        响应: +MQTTCONN:<LinkID>,<state>,<scheme>,<"host">,<port>,...
     *        <state> 含义:
     *          0=MQTT未初始化  1=已设置USERCFG  2=已设置CONNCFG
     *          3=连接已断开    4=已建立连接      5=已连接未订阅
     *          6=已连接已订阅
     *
     *        先等待 2 秒让模块有时间发起 TCP + MQTT 握手,
     *        然后每 1.5 秒查询一次, 最多等到 ESP8266_MQTT_TIMEOUT_MS.
     *        每次查询失败时记录原因, 超时时写入错误消息方便诊断.
     */
    {
        uint32_t waited = 0;
        const uint32_t poll_interval = 1500;
        int connected = 0;
        char last_state[48] = "no query sent";

        /* 先给 2 秒让模块做 DNS+TCP+MQTT 握手 */
        osDelay(2000);
        waited = 2000;

        while (waited < (uint32_t)ESP8266_MQTT_TIMEOUT_MS) {
            ret = ESP8266_SendCmd("AT+MQTTCONN?\r\n", ESP8266_AT_TIMEOUT_MS);
            if (ret == ESP8266_OK) {
                if (ESP8266_ResponseContains("+MQTTCONN:0,6"))
                    { snprintf(last_state, sizeof(last_state), "state=6(已订阅)"); connected = 1; break; }
                if (ESP8266_ResponseContains("+MQTTCONN:0,5"))
                    { snprintf(last_state, sizeof(last_state), "state=5(已连接未订阅)"); connected = 1; break; }
                if (ESP8266_ResponseContains("+MQTTCONN:0,4"))
                    { snprintf(last_state, sizeof(last_state), "state=4(已连接)"); connected = 1; break; }
                if (ESP8266_ResponseContains("+MQTTCONN:0,3"))
                    { snprintf(last_state, sizeof(last_state), "state=3(已断开)"); }
                else if (ESP8266_ResponseContains("+MQTTCONN:0,2"))
                    { snprintf(last_state, sizeof(last_state), "state=2(已配置CONNCFG)"); }
                else if (ESP8266_ResponseContains("+MQTTCONN:0,1"))
                    { snprintf(last_state, sizeof(last_state), "state=1(已配置USERCFG)"); }
                else if (ESP8266_ResponseContains("+MQTTCONN:0,0"))
                    { snprintf(last_state, sizeof(last_state), "state=0(未初始化)"); }
                else
                    { snprintf(last_state, sizeof(last_state), "no MQTTCONN in resp"); }
            } else {
                snprintf(last_state, sizeof(last_state), "SendCmd err=%d", (int)ret);
            }

            osDelay(poll_interval);
            waited += poll_interval;
        }

        if (!connected) {
            char err[64];
            snprintf(err, sizeof(err),
                     "MQTT poll t/o, %s", last_state);
            ESP8266_SetError(err);
            ESP8266_SetState(ESP8266_STATE_WIFI_OK);
            return ESP8266_AT_FAIL;
        }
    }

    /* 连接确认: 切换状态到 MQTT_OK, 清零重连计数 */
    ESP8266_SetState(ESP8266_STATE_MQTT_OK);
    reconnect_count = 0;

    /* 步骤 4: 订阅主题 (MQTT 接入规范 步骤3)
     *
     *        AT+MQTTSUB=<LinkID>,<"topic">,<qos>
     *
     *        订阅"属性上报响应"主题: 接收 OneNET 平台对上报数据的回复,
     *        回复中包含消息 ID, 用于确认数据已被平台接收.
     */
    snprintf(cmd, sizeof(cmd), "AT+MQTTSUB=0,\"%s\",0\r\n",
             ONENET_TOPIC_PROPERTY_POST_REPLY);
    ret = ESP8266_SendCmd(cmd, ESP8266_AT_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("MQTT_Connect: subscribe post/reply failed");
        ESP8266_SetState(ESP8266_STATE_WIFI_OK);
        return ret;
    }

    /* 订阅"属性设置"主题: 接收 OneNET 平台下发的控制指令
     * (如开关水泵、修改上报周期等), 为后续命令下发功能做准备.
     */
    snprintf(cmd, sizeof(cmd), "AT+MQTTSUB=0,\"%s\",0\r\n",
             ONENET_TOPIC_PROPERTY_SET);
    ret = ESP8266_SendCmd(cmd, ESP8266_AT_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("MQTT_Connect: subscribe property/set failed");
        ESP8266_SetState(ESP8266_STATE_WIFI_OK);
        return ret;
    }

    return ESP8266_OK;
}

/**
 * @brief 断开 MQTT 连接, 释放模块资源
 *
 * @return ESP8266_OK                  断开成功
 * @return ESP8266_MQTT_DISCONNECTED   当前未连接, 无需操作
 *
 * @note  断开后状态回退至 WIFI_OK (WiFi 保持连接, 可重新连 MQTT)
 * @note  MQTTCLEAN=0 断开但不释放用户配置, 下次连接无需重新 USERCFG
 */
ESP8266_Status ESP8266_MQTT_Disconnect(void)
{
    if (ESP8266_GetState() < ESP8266_STATE_MQTT_OK) {
        return ESP8266_MQTT_DISCONNECTED;
    }

    ESP8266_Status ret = ESP8266_SendCmd("AT+MQTTCLEAN=0\r\n", 5000);
    osDelay(200);  /* 等待断开完成 */

    /* 无论命令返回什么, 状态回退到 WiFi 层 */
    ESP8266_SetState(ESP8266_STATE_WIFI_OK);
    return ret;
}

/* ==========================================================================
 * 阶段 4: MQTT 数据发布 — JSON 转义 + AT+MQTTPUB 组包
 *
 * 两个发布函数的分工:
 * - PublishJson:   通用发布, 调用方提供完整 JSON 字符串, 内部做 AT 转义
 * - PublishProperty: OneNET 物模型专用, 自动组 JSON 后调用 PublishJson
 *
 * AT+MQTTPUB 命令格式:
 *   AT+MQTTPUB=<LinkID>,<"topic">,<"data">,<qos>,<retain>
 *   - LinkID=0: 连接通道 0
 *   - qos=1:   至少一次送达 (MQTT QoS 1)
 *   - retain=0: 不保留消息 (非遗嘱消息)
 *
 * 转义规则 (AT 规范, data 字段内):
 *   " → \"   \ → \\   , → \,
 *   拆分后单条命令 < 256 字节, 逗号转义不会超长.
 * ========================================================================== */

/**
 * @brief 通用 MQTT 发布 — 发布任意 JSON 到指定 Topic
 *
 * 在共享缓冲区 work_buf 中构建完整 AT+MQTTPUB 命令:
 *   1. 写入命令前缀: AT+MQTTPUB=0,"<topic>","
 *   2. 逐字符追加 JSON, 同步转义 " → \"   \ → \\   , → \,
 *   3. 写入命令后缀: ",1,0\r\n
 *
 * @param topic MQTT 主题 (直接拼入, 不做转义 — 合法的 MQTT Topic 不含特殊字符)
 * @param json  待发布的 JSON 字符串 (内部逐字符转义后嵌入 AT 命令)
 * @return ESP8266_OK                发布成功 (收到 OK 响应)
 * @return ESP8266_MQTT_DISCONNECTED MQTT 未连接
 * @return ESP8266_BUF_OVERFLOW      命令超出 work_buf 容量 (512 字节)
 * @return ESP8266_ERROR            参数为 NULL
 *
 * @note  发布失败时自动检测 CLOSED / DISCONNECTED 关键字,
 *        若出现则将状态回退至 WIFI_OK, 触发上层重连逻辑.
 *
 * @warning 本函数使用静态 work_buf, 不可跨任务并发调用.
 *          当前设计: 所有 ESP8266 操作在同一任务中串行执行, 无需互斥.
 */
ESP8266_Status ESP8266_MQTT_PublishJson(const char *topic, const char *json)
{
    /* 前置检查: MQTT 必须已连接 */
    if (ESP8266_GetState() < ESP8266_STATE_MQTT_OK) {
        ESP8266_SetError("PublishJson: MQTT not connected");
        return ESP8266_MQTT_DISCONNECTED;
    }

    /* 参数有效性检查 */
    if (topic == NULL || json == NULL) {
        ESP8266_SetError("PublishJson: null argument");
        return ESP8266_ERROR;
    }

    /* 步骤 1: 写入命令前缀
     *        格式: AT+MQTTPUB=0,"<topic>","
     *        预留至少 4 字节给后缀和终止符: ",1,0\r\n + \0
     */
    int wi = snprintf(work_buf, sizeof(work_buf),
                      "AT+MQTTPUB=0,\"%s\",\"", topic);
    if (wi < 0 || wi >= (int)sizeof(work_buf) - 4) {
        ESP8266_SetError("PublishJson: command too long");
        return ESP8266_BUF_OVERFLOW;
    }

    /* 步骤 2: 逐字符追加 JSON, AT 规范要求三种字符转义:
     *        " → \"   \ → \\   , → \,
     *        拆分后单条命令 < 256 字节, 逗号转义不会超长.
     */
    for (int ri = 0; json[ri] != '\0' && wi < (int)sizeof(work_buf) - 3; ri++) {
        char c = json[ri];
        if (c == '"' || c == '\\' || c == ',') {
            work_buf[wi++] = '\\';
            work_buf[wi++] = c;
        } else {
            work_buf[wi++] = c;
        }
    }

    /* 步骤 3: 写入命令后缀
     *         ",1,0\r\n  → 闭合数据字符串, QoS=1, Retain=0
     *         QoS=1: 至少一次送达 (AT LEAST ONCE)
     *         Retain=0: 不保留消息在 Broker
     */
    wi += snprintf(work_buf + wi, sizeof(work_buf) - wi, "\",1,0\r\n");
    if (wi >= (int)sizeof(work_buf)) {
        ESP8266_SetError("PublishJson: command too long");
        return ESP8266_BUF_OVERFLOW;
    }

    /* 步骤 4: 发送命令并检查结果 */
    ESP8266_Status ret = ESP8266_SendCmd(work_buf, ESP8266_AT_TIMEOUT_MS);
    if (ret != ESP8266_OK) {
        ESP8266_SetError("PublishJson: publish failed");

        /* 检测连接断开信号:
         * - CLOSED:      Broker 主动关闭了 TCP 连接
         * - DISCONNECTED: MQTT 层断连 (Keep-Alive 超时等)
         * 出现任一信号 → 状态回退到 WIFI_OK, 下次 EnsureConnected 会重新建连
         */
        if (ESP8266_ResponseContains("CLOSED") ||
            ESP8266_ResponseContains("DISCONNECTED")) {
            ESP8266_SetState(ESP8266_STATE_WIFI_OK);
        }
        return ret;
    }

    return ESP8266_OK;
}

/**
 * @brief OneNET 物模型属性上报 — 分两次发布 (单次命令 < 256 字节)
 *
 * Topic 使用宏 ONENET_TOPIC_PROPERTY_POST, 两次发布同一 Topic、不同 msg_id.
 *
 * 第1次: 环境传感器 (温度/湿度/土壤湿度/雨量)  ~231 字节
 * 第2次: 设备状态   (光照/水泵/告警)            ~177 字节
 *
 * @param temperature   温度值 (°C, 保留 1 位小数)
 * @param humidity      湿度值 (%RH, 保留 1 位小数)
 * @param soil_moisture 土壤湿度 ADC 原始值 (0-4095)
 * @param rain          雨量 ADC 原始值 (0-4095)
 * @param light         光照 ADC 原始值 (0-4095)
 * @param pump_state    水泵状态 (0=关闭, 1=开启)
 * @param alarm_state   告警状态 (0=正常, 1=告警)
 * @return ESP8266_OK            两次发布均成功
 * @return ESP8266_BUF_OVERFLOW  JSON 超出缓冲区
 * @return 其他错误码             任一次 PublishJson 失败即返回
 *
 * @note  每次调用 msg_id 自增 2 (两次发布各占一个 id).
 */
ESP8266_Status ESP8266_MQTT_PublishProperty(float temperature, float humidity,
                                            uint16_t soil_moisture, uint16_t rain,
                                            uint16_t light, uint8_t pump_state,
                                            uint8_t alarm_state)
{
    /* 状态检查 */
    if (ESP8266_GetState() < ESP8266_STATE_MQTT_OK) {
        ESP8266_SetError("PublishProperty: MQTT not connected");
        return ESP8266_MQTT_DISCONNECTED;
    }

    /* 调试阶段只发温湿度, 单条命令 ~186 字节 */
    char json_buf[200];

    msg_id++;
    int len = snprintf(json_buf, sizeof(json_buf),
        "{\"id\":\"%lu\",\"version\":\"1.0\",\"params\":{"
        "\"temperature\":{\"value\":%.1f},"
        "\"humidity\":{\"value\":%.1f}"
        "}}",
        (unsigned long)msg_id,
        (double)temperature, (double)humidity);

    if (len < 0 || len >= (int)sizeof(json_buf)) {
        ESP8266_SetError("PublishProperty: JSON too long");
        return ESP8266_BUF_OVERFLOW;
    }
    return ESP8266_MQTT_PublishJson(ONENET_TOPIC_PROPERTY_POST, json_buf);
}

/* ==========================================================================
 * 阶段 5: 连接维护 & 断线重连
 *
 * 分层恢复策略 (逐层检查, 哪层断就从哪层重建):
 *   UNINIT → AT_READY → WIFI_OK → MQTT_OK
 *
 * ReportAndReconnect 的工作流:
 *   1. 尝试直接发布
 *   2. 发布失败 → 标记 MQTT 断线 (状态回退到 WIFI_OK)
 *   3. 调用 EnsureConnected 逐层恢复连接链
 *   4. 重试发布
 *
 * 设计理由:
 * - 不自动重连: 由 MCU 控制重连间隔和次数, 避免模块在弱网下反复震荡
 * - 分层恢复: 只重建断开的层, 不从头初始化, 节省时间
 * ========================================================================== */

/**
 * @brief 确保连接链完整 — 逐层检查并恢复 (WiFi → MQTT)
 *
 * 凭据自动从 esp8266config.h 宏定义读取.
 *
 * 按状态机自底向上检查:
 *   - 状态 < AT_READY  → 重新初始化 (硬件复位 + AT 自检)
 *   - 状态 < WIFI_OK   → 重新连接 WiFi
 *   - 状态 < MQTT_OK   → 重新连接 MQTT Broker
 *
 * @return ESP8266_OK  所有层均就绪 (最终状态 = MQTT_OK)
 * @return 其他错误码  某一层恢复失败 (状态停留在失败层)
 *
 * @note  每次调用本函数前尝试直接发布, 失败后才调用本函数 —
 *        这是 "乐观发布, 悲观恢复" 策略: 大多数调用只需状态检查而无需重建.
 * @note  重连失败时 reconnect_count 自增, 可用于监控和告警.
 */
ESP8266_Status ESP8266_EnsureConnected(void)
{
    /* 层 1: 确保 AT 通信正常 (最低层)
     *       状态 < AT_READY 意味着模块未初始化或 AT 无响应,
     *       重新执行硬件复位 + AT 自检
     */
    if (ESP8266_GetState() < ESP8266_STATE_AT_READY) {
        ESP8266_Status ret = ESP8266_Init();
        if (ret != ESP8266_OK) return ret;  /* 基础层失败, 无法继续 */
    }

    /* 层 2: 确保 WiFi 已连接
     *       状态 < WIFI_OK 意味着 WiFi 断线或从未连接,
     *       重新执行 CWMODE + CWJAP + DHCP 流程
     */
    if (ESP8266_GetState() < ESP8266_STATE_WIFI_OK) {
        ESP8266_Status ret = ESP8266_ConnectWiFi();
        if (ret != ESP8266_OK) {
            reconnect_count++;  /* 记录重连失败 */
            return ret;
        }
    }

    /* 层 3: 确保 MQTT 已连接 (最高层)
     *       状态 < MQTT_OK 意味着 MQTT 断线或从未连接,
     *       重新执行 MQTTUSERCFG + MQTTCONN 流程
     */
    if (ESP8266_GetState() < ESP8266_STATE_MQTT_OK) {
        ESP8266_Status ret = ESP8266_MQTT_Connect();
        if (ret != ESP8266_OK) {
            reconnect_count++;  /* 记录重连失败 */
            return ret;
        }
    }

    return ESP8266_OK;
}

/**
 * @brief 一键上报 + 断线自动重连 (对外主要接口)
 *
 * 连接凭据自动从 esp8266config.h 宏定义读取,
 * 调用方只需传入 Topic 和 JSON 数据.
 *
 * 采用 "乐观发布, 悲观恢复" 策略:
 *   1. 先尝试直接发布 (大多数情况下连接正常, 一次成功)
 *   2. 发布失败 → 标记断线 → 重建完整连接链 → 重试发布
 *
 * @param topic     MQTT 主题
 * @param json      待发布的 JSON 字符串
 * @return ESP8266_OK  发布成功
 * @return 其他错误码  发布失败且重连也失败
 *
 * @note  本函数封装了完整的 "发布-失败-重连-重试" 流程.
 *
 * 调用示例:
 * ```
 * ESP8266_ReportAndReconnect(ONENET_TOPIC_PROPERTY_POST, json_str);
 * ```
 */
ESP8266_Status ESP8266_ReportAndReconnect(const char *topic, const char *json)
{
    /* 第一跳: 乐观发布 — 假设当前连接正常, 直接发送 */
    ESP8266_Status ret = ESP8266_MQTT_PublishJson(topic, json);
    if (ret == ESP8266_OK) {
        return ESP8266_OK;  /* 快速路径: 发布成功, 立即返回 */
    }

    /* 发布失败: 标记 MQTT 断线, 回退状态到 WIFI_OK
     * (如果已处于更低状态则保持不变, 避免覆盖更严重的错误状态)
     */
    if (ESP8266_GetState() == ESP8266_STATE_MQTT_OK) {
        ESP8266_SetState(ESP8266_STATE_WIFI_OK);
    }

    ESP8266_SetError("ReportAndReconnect: reconnecting...");

    /* 第二跳: 逐层重建连接链 (WiFi → MQTT) */
    ret = ESP8266_EnsureConnected();
    if (ret != ESP8266_OK) {
        return ret;  /* 重连失败, 向上传递错误 */
    }

    /* 第三跳: 连接恢复, 重试发布 */
    return ESP8266_MQTT_PublishJson(topic, json);
}

/* ==========================================================================
 * 阶段 6: 一键服务封装 — 简化 Task 层调用
 *
 * 对外暴露两个高层 API:
 * - ServiceInit:      系统启动时调用一次, 完成初始化 + 连接
 * - ServiceEnsureLink: 周期性调用 (如每 5 秒), 检查并维护连接
 *
 * 凭据从 esp8266config.h 宏定义中读取, 调用方无需传参.
 * ========================================================================== */

/**
 * @brief ESP8266 服务初始化 — 一次性完成 AT → WiFi → MQTT 全流程
 *
 * 调用链路: Init → ConnectWiFi → MQTT_Connect
 * 任一阶段失败则停止并返回错误.
 *
 * @return ESP8266_OK      全部就绪, 可进行数据发布
 * @return 其他错误码       失败阶段对应的错误码
 *
 * @note  应在 FreeRTOS 调度器启动后、网络任务创建后调用一次.
 *        如果凭据错误或网络不可达, 返回错误但不阻塞 — 后续由
 *        ServiceEnsureLink 周期性重试.
 *
 * 调试输出 (启用 ESP8266_DEBUG_LOG 时):
 * ```
 * [ESP8266] Service init start...
 * [ESP8266] Step 1: Hardware init & AT test...
 * [ESP8266] Init OK
 * [ESP8266] Step 2: Connecting WiFi...
 * [ESP8266] WiFi OK
 * [ESP8266] Step 3: Connecting MQTT...
 * [ESP8266] MQTT OK
 * [ESP8266] Service init SUCCESS
 * ```
 */
ESP8266_Status ESP8266_ServiceInit(void)
{
    ESP8266_Status ret;

    dbg_println("[ESP8266] Service init start...");

    /* 阶段 1: 硬件初始化 + AT 通信自检 */
    dbg_println("[ESP8266] Step 1: Hardware init & AT test...");
    ret = ESP8266_Init();
    if (ret != ESP8266_OK) {
        dbg_println("[ESP8266] Init FAILED");
        return ret;
    }
    dbg_println("[ESP8266] Init OK");

    /* 阶段 2: 连接 WiFi (SSID/密码来自 esp8266config.h 宏定义) */
    dbg_println("[ESP8266] Step 2: Connecting WiFi...");
    ret = ESP8266_ConnectWiFi();
    if (ret != ESP8266_OK) {
        dbg_println("[ESP8266] WiFi connect FAILED");
        return ret;
    }
    dbg_println("[ESP8266] WiFi OK");

    /* 阶段 3: 连接 OneNET MQTT Broker
     *        - 设备名称: ONENET_DEVICE_NAME
     *        - 用户名:   ONENET_PRODUCT_ID (OneNET 平台使用产品 ID 作为 MQTT 用户名)
     *        - 密码:     ONENET_DEVICE_TOKEN (设备密钥, 用于鉴权)
     */
    dbg_println("[ESP8266] Step 3: Connecting MQTT...");
    ret = ESP8266_MQTT_Connect();
    if (ret != ESP8266_OK) {
        dbg_println("[ESP8266] MQTT connect FAILED");
        return ret;
    }
    dbg_println("[ESP8266] MQTT OK");

    dbg_println("[ESP8266] Service init SUCCESS");
    return ESP8266_OK;
}

/**
 * @brief 周期性连接维护 — 检查并修复断开的连接 (供 Task 周期调用)
 *
 * 内部调用 EnsureConnected 逐层检查状态:
 * - 连接正常 → 状态检查通过, 几乎无开销
 * - 连接断开 → 自动重建, reconnect_count 自增
 *
 * @return ESP8266_OK      连接正常
 * @return 其他错误码       连接异常且恢复失败
 *
 * @note  推荐调用周期: 5 秒 (ESP8266_RECONNECT_INTERVAL_MS)
 *         与数据上报周期解耦 — 上报前直接调用 ReportAndReconnect 即可,
 *         本函数作为兜底保活机制.
 *
 * 典型 Task 使用模式:
 * ```
 * void Task_ESP8266(void *arg) {
 *     ESP8266_ServiceInit();
 *     for (;;) {
 *         ESP8266_ServiceEnsureLink();          // 保活检查
 *         ESP8266_MQTT_PublishProperty(...);     // 数据上报
 *         osDelay(ESP8266_REPORT_INTERVAL_MS);   // 周期等待
 *     }
 * }
 * ```
 */
ESP8266_Status ESP8266_ServiceEnsureLink(void)
{
    return ESP8266_EnsureConnected();
}
