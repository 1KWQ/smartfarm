#include "FarmState.h"

FarmState farmState;//农场环境状态 由Sensortask定期更新
FarmSafeRange farmSafeRange;//环境安全范围  可通过OLED界面修改
uint8_t alarm_state = 0;   //本地报警标志: 1=报警中, 0=正常  由Sensortask根据阈值更新

/**
 * @brief 初始化环境安全范围阈值
 * 
 * 设置默认的安全范围值，在系统启动时调用一次
 * 这些默认值适用于大多数农作物的生长环境
 * 
 * 默认值说明：
 * - 温度范围：15-30℃（适合大多数温室作物）
 * - 湿度范围：20-70%（避免过干或过湿）
 * - 土壤湿度：20-80%（保持适宜的土壤水分）
 * - 光照强度：30-800 lx（适合大多数植物）
 * - 最大降雨量：80%（超过此值触发报警）
 */
void EnvSafeRange_Init() {
  // 温度范围：15-30摄氏度
  farmSafeRange.minTemperature = 15;
  farmSafeRange.maxTemperature = 35;
  
  // 湿度范围：20-70%
  farmSafeRange.minHumidity = 20;
  farmSafeRange.maxHumidity = 75;
  
  // 土壤湿度范围：20-80%
  farmSafeRange.minSoilMoisture = 0;
  farmSafeRange.maxSoilMoisture = 65;
  
  // 光照强度范围：30-800勒克斯
  farmSafeRange.minLightIntensity = 30;
  farmSafeRange.maxLightIntensity = 800;
  
  // 最大降雨量阈值：80%
  farmSafeRange.maxRainGauge = 80;
}
