//Created by KK on 2026/7/9 13:50
#include "main.h"

/**
 * @brief 农场环境状态结构体
 * 存储从传感器读取到的实时环境数据
 * 在sensortask定期更新，供其他任务读取使用
 */
typedef struct {
	float temperature;		//环境温度  （单位：℃）
	float humidity;			//空气湿度  （单位：百分比％ 0-100）
	uint16_t rainGauge;		//降雨量 	（单位：百分比％ 0-100）
	uint16_t soilMoisture;	//土壤湿度  （单位：百分比％ 0-100）
	uint16_t lightIntensity;//光照强度  （单位：lx）

	uint8_t waterPumpState; //水泵状态  （0关闭  1开启）
}FarmState;

/**
 * @brief 农场环境状态阈值结构体 
 * 定义了环境状态的安全阈值
 * 用于环境状态超阈值时报警处理
 * 用户可以通过OLED界面修改这些值
 */
typedef struct {
	float minTemperature; 
	float maxTemperature;
	float minHumidity;
	float maxHumidity;
	uint16_t maxRainGauge;
	uint16_t minSoilMoisture;
	uint16_t maxSoilMoisture;
	uint16_t minLightIntensity;
	uint16_t maxLightIntensity;
}FarmSafeRange;

extern FarmState farmState;
extern FarmSafeRange farmSafeRange;
extern uint8_t alarm_state;  /* 本地报警标志: 1=报警中, 0=正常 */
void EnvSafeRange_Init();
