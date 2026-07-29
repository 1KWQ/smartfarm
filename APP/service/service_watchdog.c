/**
 * @file    service_watchdog.c
 * @brief   软件看门狗服务层实现
 * @note
 *   心跳数组使用 taskENTER_CRITICAL / taskEXIT_CRITICAL 保护,
 *   保证多任务并发读写安全.
 *
 *   CheckAll() 设计: 在临界区内快照全部心跳值, 关闭临界区后
 *   再做时间比较 —— 最大限度缩短关中断时间.
 */

#include "service_watchdog.h"
#include "FreeRTOS.h"
#include "task.h"

/* ==========================================================================
 * 静态变量 — 心跳时间戳数组
 * ========================================================================== */
static uint32_t wd_heartbeat[WD_TASK_COUNT];     /* 每任务最近一次心跳 tick */
static uint32_t wd_esp_comm_heartbeat;           /* ESP 通信链路最近心跳 tick */

/* ==========================================================================
 * API 实现
 * ========================================================================== */

/**
 * @brief 初始化所有心跳时间戳
 */
void Service_Wdg_Init(void)
{
    uint32_t now = xTaskGetTickCount();
    int i;

    for (i = 0; i < WD_TASK_COUNT; i++) {
        wd_heartbeat[i] = now;
    }
    wd_esp_comm_heartbeat = now;
}

/**
 * @brief 业务任务上报心跳
 */
void Service_Wdg_FeedTask(Wdg_TaskID task_id)
{
    if (task_id >= WD_TASK_COUNT) {
        return;
    }

    taskENTER_CRITICAL();
    wd_heartbeat[task_id] = xTaskGetTickCount();
    taskEXIT_CRITICAL();
}

/**
 * @brief ESP8266 通信链路上报心跳
 */
void Service_Wdg_FeedESPComm(void)
{
    taskENTER_CRITICAL();
    wd_esp_comm_heartbeat = xTaskGetTickCount();
    taskEXIT_CRITICAL();
}

/**
 * @brief 统一检查所有任务心跳
 *
 * 临界区内快照心跳 -> 临界区外比较时间, 兼顾正确性与低延迟.
 */
void Service_Wdg_CheckAll(Wdg_Status *out)
{
    TickType_t now = xTaskGetTickCount();
    uint32_t hb[WD_TASK_COUNT];
    uint32_t esp_hb;
    int i;

    /* ---- 临界区: 快照心跳值 ---- */
    taskENTER_CRITICAL();
    for (i = 0; i < WD_TASK_COUNT; i++) {
        hb[i] = wd_heartbeat[i];
    }
    esp_hb = wd_esp_comm_heartbeat;
    taskEXIT_CRITICAL();

    /* ---- 临界区外: 时间比较 (unsigned 减法天然处理 tick 溢出) ---- */
    out->input_alive   = ((now - hb[WD_TASK_INPUT])  < pdMS_TO_TICKS(WD_TIMEOUT_INPUT_MS))  ? 1 : 0;
    out->sensor_alive  = ((now - hb[WD_TASK_SENSOR]) < pdMS_TO_TICKS(WD_TIMEOUT_SENSOR_MS)) ? 1 : 0;
    out->esp_alive     = ((now - hb[WD_TASK_ESP])    < pdMS_TO_TICKS(WD_TIMEOUT_ESP_MS))    ? 1 : 0;
    out->screen_alive  = ((now - hb[WD_TASK_SCREEN]) < pdMS_TO_TICKS(WD_TIMEOUT_SCREEN_MS)) ? 1 : 0;
    out->esp_comm_alive = ((now - esp_hb) < pdMS_TO_TICKS(WD_TIMEOUT_ESP_COMM_MS)) ? 1 : 0;

    /* ---- 汇总: 四大核心任务是否全活 ---- */
    out->all_core_ok = out->input_alive
                    && out->sensor_alive
                    && out->esp_alive
                    && out->screen_alive;
}
