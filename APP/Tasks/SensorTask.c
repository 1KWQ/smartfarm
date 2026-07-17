/**
 * @file SensorTask.c
 * @brief 传感器数据采集和报警检测任务
 * Created by KK on 2026/7/9 14:37
 * 本任务负责：
 * 1. 初始化所有传感器（AHT20温湿度、土壤湿度、光照、降雨量）
 * 2. 定期读取传感器数据并更新全局状态
 * 3. 检查环境参数是否超出安全范围
 * 4. 当检测到异常时，发送报警消息到BLE队列并启动蜂鸣器
 * 
 * 任务优先级：osPriorityNormal
 * 任务周期：1000ms（1秒）
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
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "utils.h"
#include "BeepTimer.h"

/**
 * @brief 
 * 
 * @param value 
 * @param reason 
 */
//发送浮点数报警消息
static void SendWarningFloat(float value,char* reason)
{   
    // 在FreeRTOS堆中分配内存用于存储消息
    char *msg = pvPortMalloc(100);
    if(msg == NULL)
    {
        return;//内存分配失败，直接返回
    }
    //将浮点数的整数部分和小数部分分别存入变量中
    int Int,Dec;
    floatToIntDec(value,&Int,&Dec);
    //将报警的数据和原因存入msg当中
    snprintf(msg,100,"{\"type\":\"warning\", \"reason\":\"%s\", \"value\":%d.%d}\n",reason,Int,Dec);
    //在队列中发送消息
    osMessageQueuePut(BLEQueueHandle,&msg,0,0);
}
/**
 * @brief 
 * 
 * @param value 
 * @param reason 
 */
//发送整型数据报警消息
static void SendWarningInt(uint16_t value,char* reason)
{
    //在freetos堆中分配内存以存储消息
    char *msg=pvPortMalloc(100);
    if(msg == NULL)
    {
        return;//分配内存失败，立即返回
    }
    //将报警原因和数据存入msg当中
    snprintf(msg,100,"{\"type\":\"warning\", \"reason\":\"%s\", \"value\":%d}\n",reason,value);
    //在队列中发送消息
    osMessageQueuePut(BLEQueueHandle,&msg,0,0);
}
/**
 * @brief 
 * 
 * @param value 
 * @param min 
 * @param max 
 * @param minreason 
 * @param maxreason 
 * @return uint8_t 
 */
//检测浮点数据是否超出阈值
static uint8_t CheckRangeFloat(float value,float min,float max,char* minreason,char* maxreason)
{
    uint8_t warning=0;//用于报警的标志位
    //小于最小安全阈值
    if(value<min)
    {
        //发送报警消息
        SendWarningFloat(value,minreason);
        warning=1;
    }
    //大于最大安全阈值
    else if(value>max)
    {
        //发送报警消息
        SendWarningFloat(value,maxreason);
        warning=1;
    }
    return warning;
}
/**
 * @brief 
 * 
 * @param value 
 * @param min 
 * @param max 
 * @param minreason 
 * @param maxreason 
 * @return uint8_t 
 */
//检测整型数据是否超出阈值
static uint8_t CheckRangeInt(uint16_t value,uint16_t min,uint16_t max,char* minreason,char* maxreason)
{
    uint8_t warning=0;//用于报警的标志位
    //小于最小安全阈值
    if(value<min)
    {
        //发送报警消息
        SendWarningInt(value,minreason);
        warning=1;
    }
    //大于最大安全阈值
    else if(value>max)
    {
        //发送报警消息
        SendWarningInt(value,maxreason);
        warning=1;
    }
    return warning;
}

/**
 * @brief 
 * 
 * @param argument 
 */
void StartSensorTask(void *argument)
{
  /* USER CODE BEGIN StartSensorTask */
    //初始化农场安全指标范围
    EnvSafeRange_Init();
    //初始化传感器
    osMutexAcquire(i2c1MutexHandle,osWaitForever);//获取互斥量
    AHT20_Init();//初始化温湿度传感器
    osMutexRelease(i2c1MutexHandle);//释放互斥量
    Rain_Init();//初始化降雨量传感器
    SoilMoisture_Init();//初始化土壤湿度传感器    
  /* Infinite loop */
  for(;;)
  {
    //获取传感器的值，存入对应的变量中
    farmState.lightIntensity=Light_Get();//获取光照强度
    osMutexAcquire(i2c1MutexHandle,osWaitForever);//获取互斥量
    AHT20_READ(&farmState.temperature,&farmState.humidity);//获取温湿度
    osMutexRelease(i2c1MutexHandle);//释放互斥量
    farmState.rainGauge=Rain_Get();//获取降雨量
    farmState.soilMoisture=SoilMoisture_Get();//获取土壤湿度

    //检查土壤湿度是否低于安全阈值，低于则自动灌溉
    if(farmState.soilMoisture < farmSafeRange.minSoilMoisture)
    {
        Pump_On();//开启水泵
    }
    else
    {
        Pump_Off();
    }
    //检查环境是否超出安全阈值
    uint8_t warning=0;
    //检查温湿度
    warning+=CheckRangeFloat(farmState.temperature,farmSafeRange.minTemperature,
                            farmSafeRange.maxTemperature,"Temperature_Low","Temperature_High");
    //检查湿度
    warning+=CheckRangeFloat(farmState.humidity,farmSafeRange.minHumidity,
                            farmSafeRange.maxHumidity,"Humidity_Low","Humidity_High");
    //检查光照强度
    warning+=CheckRangeInt(farmState.lightIntensity,farmSafeRange.minLightIntensity,
                            farmSafeRange.maxLightIntensity,"LightIntensity_Low","LightIntensity_High");
    //检查降雨量，只检查上限
    if(farmState.rainGauge > farmSafeRange.maxRainGauge)
    {
        warning+=1;
        SendWarningInt(farmState.rainGauge,"RainGauge_High");
    } 
    //检查土壤湿度
    warning+=CheckRangeInt(farmState.soilMoisture,farmSafeRange.minSoilMoisture,
                            farmSafeRange.maxSoilMoisture,"SoilMoisture_Low","SoilMoisture_High");                                           
    //warning大于0报警---蜂鸣器响铃
    if(warning > 0)
    {
        Beep_On();//蜂鸣器开启
    }
    else
    {
        Beep_Off();//等于0不报警
    }
    
    osDelay(1000);//间隔1s周期性获取并检测农场各项环境指标
  }
  /* USER CODE END StartSensorTask */
}