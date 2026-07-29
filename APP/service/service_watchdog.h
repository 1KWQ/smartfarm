/**
 * @file    service_watchdog.h
 * @brief   软件看门狗服务层 — 任务心跳管理 + 通信链路监测
 * @author  SmartFarm Project
 * @date    2026-07-27
 *
 * @note    职责:
 *          - 注册 4 个业务任务心跳 ID
 *          - 提供心跳上报接口 (FeedTask / FeedESPComm)
 *          - 提供统一监测接口 (CheckAll)，由监控任务周期性调用
 *
 *          硬件 IWDG 仅由 WatchdogMonitor 任务单点喂狗,
 *          业务任务和 ISR 禁止直接调用 HAL_IWDG_Refresh().
 *
 *          临界区保护: 所有心跳数组读写使用 taskENTER_CRITICAL /
 *          taskEXIT_CRITICAL, 保证多任务并发安全.
 */

#ifndef SERVICE_WATCHDOG_H__
#define SERVICE_WATCHDOG_H__

#include <stdint.h>

/* ==========================================================================
 * 任务心跳 ID
 * ========================================================================== */
typedef enum {
    WD_TASK_INPUT   = 0,
    WD_TASK_SENSOR  = 1,
    WD_TASK_ESP     = 2,
    WD_TASK_SCREEN  = 3,
    WD_TASK_COUNT   = 4
} Wdg_TaskID;

/* ==========================================================================
 * 心跳超时阈值 (ms)
 *
 * 阈值选取原则: 任务周期的 3~5 倍, 既留足偶尔卡顿的容错空间,
 * 又能在任务真死锁时快速检测.
 * ========================================================================== */
#define WD_TIMEOUT_INPUT_MS      200    /**< InputTask:   周期 ~20ms × 10   */
#define WD_TIMEOUT_SENSOR_MS     5000   /**< SensorTask:  周期 1000ms × 5   */
#define WD_TIMEOUT_ESP_MS        20000  /**< ESP8266Task: 周期 5000ms × 4   */
#define WD_TIMEOUT_SCREEN_MS     100    /**< ScreenTask:  周期 10ms × 10    */
#define WD_TIMEOUT_ESP_COMM_MS   30000  /**< ESP 通信链路超时 (仅参考,不触发复位) */

/* ==========================================================================
 * 监控结果结构
 * ========================================================================== */
typedef struct {
    uint8_t input_alive;       /**< InputTask    是否存活 */
    uint8_t sensor_alive;      /**< SensorTask   是否存活 */
    uint8_t esp_alive;         /**< ESP8266Task  是否存活 */
    uint8_t screen_alive;      /**< ScreenTask   是否存活 */
    uint8_t esp_comm_alive;    /**< ESP 通信链路是否正常 (仅参考) */
    uint8_t all_core_ok;       /**< 四大核心任务是否全部存活 */
} Wdg_Status;

/* ==========================================================================
 * API
 * ========================================================================== */

/**
 * @brief 初始化所有心跳时间戳为当前 tick
 * @note  必须在监控任务主循环之前调用一次
 */
void Service_Wdg_Init(void);

/**
 * @brief 刷新指定任务的心跳时间戳
 * @param task_id  任务 ID (Wdg_TaskID 枚举值)
 * @note  由各业务任务在主循环内调用, 每次循环调用一次即可
 */
void Service_Wdg_FeedTask(Wdg_TaskID task_id);

/**
 * @brief 刷新 ESP8266 通信链路心跳
 * @note  由 ESP8266Task 在数据上报成功或重连成功后调用
 */
void Service_Wdg_FeedESPComm(void);

/**
 * @brief 检查所有任务心跳状态
 * @param status_out  输出当前各任务存活状态
 * @note  由 WatchdogMonitor 任务周期性调用 (500ms 周期)
 *        仅在临界区内快照心跳值, 时间比较在临界区外完成
 */
void Service_Wdg_CheckAll(Wdg_Status *status_out);

#endif /* SERVICE_WATCHDOG_H__ */
