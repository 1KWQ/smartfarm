/**
 * @file    task_watchdog_monitor.c
 * @brief   看门狗监控任务 — 系统级健康检查 + 硬件 IWDG 喂狗
 * @note
 *   架构约束:
 *     - 本任务是全系统**唯一**允许调用 HAL_IWDG_Refresh() 的地方
 *     - 本项目严格限制仅最低优先级监控任务执行，规避分散喂狗造成故障掩盖的风险。
 *     - 禁止在 ISR、其他业务任务中直接喂硬件狗
 *
 *   故障分类:
 *     - 类型A: 四大核心任务 (Input/Sensor/ESP/Screen) 任一心跳超时
 *              → 系统级故障 → NVIC_SystemReset() 自愈
 *     - 类型B: ESP8266 仅通信链路断开, 但 ESP8266Task 心跳正常
 *              → 仅网络层故障 → 不复位, 由 ESP8266Task 自行恢复
 *
 *   看门狗超时链:
 *     监控任务 500ms 喂狗 → 软件检测最快 500ms 发现故障 → 主动复位
 *     若监控任务本身卡死 → IWDG 硬件 2.5s 后自动复位 (硬件兜底)
 */

#include "task_watchdog_monitor.h"
#include "cmsis_os2.h"
#include "service_watchdog.h"
#include "iwdg.h"              /* hiwdg, HAL_IWDG_Refresh() */
#include "stm32f1xx_hal.h"     /* NVIC_SystemReset() */

/**
 * @brief 监控任务入口
 * @note  CubeMX 已创建此任务: osPriorityLow, stack 128×4
 */
void StartWatchdogMonitorTask(void *argument)
{
    (void)argument;
    Wdg_Status status;

    /* 初始化心跳时间戳 (以当前 tick 为基准, 给各任务启动窗口) */
    Service_Wdg_Init();

    for (;;) {
        /*
         * Step 1: 短暂让出 CPU，确保其他任务有机会运行并上报
         *         首次心跳。避免监控任务立即喂狗导致误判。
         */
        osDelay(10);

        /*
         * Step 2: 统一检查所有任务心跳状态
         */
        Service_Wdg_CheckAll(&status);

        /*
         * Step 3: 故障分类处理
         */
        if (!status.all_core_ok) {
            /*
             * 类型A: 核心任务心跳丢失 → 系统级故障
             *
             * 不喂狗，直接调用 NVIC_SystemReset() 自愈。
             * 即使软件复位失败 (极端硬件故障), IWDG 在 2.5s
             * 后也会硬件复位——双重保险。
             */
            NVIC_SystemReset();
            /* never reach here */
        }

        /* 所有核心任务正常 → 刷新硬件狗倒计时 */
        HAL_IWDG_Refresh(&hiwdg);

        /*
         * 类型B: ESP8266 通信链路可能断开, 但 ESP8266Task 心跳正常
         *
         * status.esp_comm_alive 仅供调试参考, 此处不做任何处理。
         * ESP8266Task 内部的逐层重连逻辑 (AT→WiFi→MQTT) 会自行
         * 恢复网络连接, 无需系统级复位。
         */

        osDelay(500);  /* 500ms 检查周期 */
    }
}
