/**
 * @file    task_watchdog_monitor.h
 * @brief   看门狗监控任务 — 系统健康检查 + 硬件 IWDG 喂狗
 * @note
 *   这是整个看门狗体系中唯一有权调用 HAL_IWDG_Refresh() 的任务.
 *   优先级: osPriorityLow (最低), 保证不抢占任何业务任务.
 *   周期:   500ms
 */
#ifndef SMARTFARM_TASK_WATCHDOG_MONITOR_H
#define SMARTFARM_TASK_WATCHDOG_MONITOR_H

void StartWatchdogMonitorTask(void *argument);

#endif /* SMARTFARM_TASK_WATCHDOG_MONITOR_H */
