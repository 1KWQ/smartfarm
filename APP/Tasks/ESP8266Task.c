/**
 * @file    ESP8266Task.c
 * @brief   ESP8266 数据上报任务 — 采集传感器数据 → OneNET MQTT 上报
 * @note
 *   通信方式: 纯AT指令, 不使用透传固件
 *   平台:     中国移动 OneNET MQTT (183.230.40.39:1883)
 *   上报周期: 5秒 (ESP8266_REPORT_INTERVAL_MS)
 *   容错:     WiFi/MQTT断线后无限自动重连, 重连间隔5秒
 *   报警:     本地阈值判断 + 云端规则引擎双重告警, alarm_state 随数据上报
 *
 *   依赖:
 *     - bsp_esp8266.h  (驱动层)
 *     - esp8266config.h (WiFi/OneNET宏)
 *     - FarmState.h    (全局传感器数据)
 *     - Pump.h         (水泵状态)
 */

#include "bsp_esp8266.h"
#include "esp8266config.h"
#include "FarmState.h"
#include "Pump.h"
#include "cmsis_os2.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

/* ==========================================================================
 * 外部引用
 * ========================================================================== */
extern UART_HandleTypeDef huart3;  /* 调试串口 */

/* ==========================================================================
 * 调试日志 (通过USART3输出, 可通过宏开关)
 * ========================================================================== */
#define ESP8266_DEBUG_LOG   /* 注释此行可关闭调试日志 */

#ifdef ESP8266_DEBUG_LOG
static void dbg_print(const char *msg)
{
    HAL_UART_Transmit(&huart3, (uint8_t *)msg, strlen(msg), 100);
}
static void dbg_println(const char *msg)    { dbg_print(msg); dbg_print("\r\n"); }
static void dbg_print_state(void)
{
    char buf[64];
    ESP8266_ConnState st = ESP8266_GetState();
    const char *s = "UNKNOWN";
    switch (st) {
        case ESP8266_STATE_UNINIT:   s = "UNINIT";   break;
        case ESP8266_STATE_AT_READY: s = "AT_READY"; break;
        case ESP8266_STATE_WIFI_OK:  s = "WIFI_OK";  break;
        case ESP8266_STATE_MQTT_OK:  s = "MQTT_OK";  break;
    }
    snprintf(buf, sizeof(buf), "[ESP8266] State: %s", s);
    dbg_println(buf);
}
#else
#define dbg_print(msg)       (void)(msg)
#define dbg_println(msg)     (void)(msg)
#define dbg_print_state()    do{}while(0)
#endif

/* ==========================================================================
 * 私有函数 — 单次数据上报
 * ========================================================================== */

/**
 * @brief 从全局 FarmState 采集数据并上报到 OneNET
 * @return ESP8266_OK 上报成功, 其他值表示失败
 */
static ESP8266_Status ESP8266_ReportOnce(void)
{
    ESP8266_Status ret;

    ret = ESP8266_MQTT_PublishProperty(
        farmState.temperature,
        farmState.humidity,
        farmState.soilMoisture,
        farmState.rainGauge,
        farmState.lightIntensity,
        farmState.waterPumpState,
        alarm_state
    );

    if (ret == ESP8266_OK) {
        dbg_println("[ESP8266] Report OK");
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "[ESP8266] Report FAILED, code=%d, err=%s",
                 (int)ret, ESP8266_GetLastError());
        dbg_println(buf);
    }

    return ret;
}

/* ==========================================================================
 * 任务入口
 * ========================================================================== */

/**
 * @brief ESP8266数据上报任务
 * @note
 *   启动阶段: 无限重试初始化直到链路建立 (间隔5s)
 *   运行阶段: 周期性上报 + 断线自动重连
 *   所有延时使用 osDelay, 不占用CPU死循环
 */
void StartESP8266Task(void *argument)
{
    (void)argument;

    dbg_println("[ESP8266] Task started");

    /* ======================================================================
     * 阶段1: 初始化连接 (无限重试)
     * ====================================================================== */
    dbg_println("[ESP8266] Phase 1: Initial connection...");

    for (;;) {
        ESP8266_Status ret = ESP8266_ServiceInit();

        if (ret == ESP8266_OK) {
            dbg_println("[ESP8266] Connection established, entering report loop");
            break;
        }

        /* 初始化失败, 记录错误并等待5秒后重试 */
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[ESP8266] Init failed (code=%d): %s",
                 (int)ret, ESP8266_GetLastError());
        dbg_println(buf);
        dbg_println("[ESP8266] Retrying in 5 seconds...");

        osDelay(ESP8266_RECONNECT_INTERVAL_MS);
    }

    /* ======================================================================
     * 阶段2: 周期性数据上报 (主循环)
     * ====================================================================== */
    for (;;) {
        /* --- 2a. 上报一次数据 --- */
        ESP8266_Status ret = ESP8266_ReportOnce();

        if (ret == ESP8266_OK) {
            /* 上报成功 → 等待下一个周期 */
            osDelay(ESP8266_REPORT_INTERVAL_MS);
            continue;
        }

        /* --- 2b. 上报失败 → 标记断线 → 重建连接 --- */
        dbg_println("[ESP8266] Report failed, starting reconnect...");
        dbg_print_state();

        /* 逐层重连 (WiFi → MQTT), 无限重试 */
        for (;;) {
            osDelay(ESP8266_RECONNECT_INTERVAL_MS);
            dbg_println("[ESP8266] Reconnecting...");
            ESP8266_Status recon_ret = ESP8266_EnsureConnected();

            if (recon_ret == ESP8266_OK) {
                dbg_println("[ESP8266] Reconnect OK, re-reporting...");
                dbg_print_state();

                /* 重连成功 → 立即补发一次数据, 然后回到主循环 */
                ESP8266_Status pub_ret = ESP8266_ReportOnce();
                if (pub_ret == ESP8266_OK) {
                    break;  /* 跳出重连循环, 回到主循环 */
                }
                /* 补发失败, 继续重连循环 */
                dbg_println("[ESP8266] Re-report failed, retrying...");
            } else {
                char buf[64];
                snprintf(buf, sizeof(buf),
                         "[ESP8266] Reconnect failed (code=%d): %s",
                         (int)recon_ret, ESP8266_GetLastError());
                dbg_println(buf);
            }
        }

        /* 恢复正常 → 等待下一个上报周期 */
        osDelay(ESP8266_REPORT_INTERVAL_MS);
    }
}
