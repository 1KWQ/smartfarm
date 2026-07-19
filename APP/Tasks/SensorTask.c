/**
 * @file    SensorTask.c
 * @brief   传感器数据采集任务 — 定期读取环境参数, 自动灌溉控制
 * @note
 *   本任务负责:
 *     1. 初始化所有传感器 (AHT20温湿度、光照、土壤湿度、降雨量)
 *     2. 每秒读取传感器数据并更新全局 FarmState
 *     3. 土壤湿度低于阈值时自动开启水泵灌溉
 *
 *   报警处理: 本地阈值判断 + 蜂鸣器声光报警 (与 OneNET 云端规则引擎双重告警)
 *   任务优先级: osPriorityAboveNormal
 *   任务周期:   1000ms
 */

#include "AHT20.h"
#include "cmsis_os2.h"
#include "freertos.h"
#include "FarmState.h"
#include "Light.h"
#include "main.h"
#include "rain.h"
#include "soil_moisture.h"
#include "pump.h"
#include "beep.h"
#include <stdint.h>

/* ==========================================================================
 * 传感器数据采集任务
 * ========================================================================== */
void StartSensorTask(void *argument)
{
    (void)argument;

    /* ---- 初始化安全阈值默认值 ---- */
    EnvSafeRange_Init();

    /* ---- 初始化传感器 ---- */
    osMutexAcquire(i2c1MutexHandle, osWaitForever);
    AHT20_Init();
    osMutexRelease(i2c1MutexHandle);

    Rain_Init();
    SoilMoisture_Init();

    /* ---- 主循环: 1秒周期 ---- */
    for (;;) {
        /* 1. 采集所有传感器数据 */
        farmState.lightIntensity = Light_Get();

        osMutexAcquire(i2c1MutexHandle, osWaitForever);
        AHT20_READ(&farmState.temperature, &farmState.humidity);
        osMutexRelease(i2c1MutexHandle);

        farmState.rainGauge     = Rain_Get();
        farmState.soilMoisture  = SoilMoisture_Get();

        /* 2. 环境阈值判断 → 控制蜂鸣器本地报警
         *    任一参数超出安全范围即触发报警, 全部正常后恢复
         *    与 OneNET 云端规则引擎独立工作, 形成双重告警 */
        {
            uint8_t is_alarm = 0;

            /* 温度越界检查 */
            if (farmState.temperature < farmSafeRange.minTemperature ||
                farmState.temperature > farmSafeRange.maxTemperature) {
                is_alarm = 1;
            }
            /* 湿度越界检查 */
            else if (farmState.humidity < farmSafeRange.minHumidity ||
                     farmState.humidity > farmSafeRange.maxHumidity) {
                is_alarm = 1;
            }
            /* 土壤湿度越界检查 */
            else if (farmState.soilMoisture < farmSafeRange.minSoilMoisture ||
                     farmState.soilMoisture > farmSafeRange.maxSoilMoisture) {
                is_alarm = 1;
            }
            /* 光照强度越界检查 */
            else if (farmState.lightIntensity < farmSafeRange.minLightIntensity ||
                     farmState.lightIntensity > farmSafeRange.maxLightIntensity) {
                is_alarm = 1;
            }
            /* 降雨量越界检查 */
            else if (farmState.rainGauge > farmSafeRange.maxRainGauge) {
                is_alarm = 1;
            }

            /* 更新全局报警标志并控制蜂鸣器 */
            if (is_alarm) {
                alarm_state = 1;
                Beep_On();
            } else {
                alarm_state = 0;
                Beep_Off();
            }
        }

        /* 3. 自动灌溉: 土壤湿度低于阈值 → 开水泵 */
        if (farmState.soilMoisture < farmSafeRange.minSoilMoisture) {
            Pump_On();
            farmState.waterPumpState = 1;
        } else {
            Pump_Off();
            farmState.waterPumpState = 0;
        }

        osDelay(1000);
    }
}
