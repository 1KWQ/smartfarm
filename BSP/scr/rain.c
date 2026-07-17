#include "rain.h"
#include "adc.h"

/**
 * @brief 开启ADC
 * 
 */
void Rain_Init(void)
{
    HAL_ADC_Start(&hadc1);
}

/**
 * @brief 获取下雨量大小
 * 
 * @return uint16_t 0~100的降雨值，0表示无雨，100表示大雨
 */
uint16_t Rain_Get(void)
{
    uint16_t adc=HAL_ADC_GetValue(&hadc1);
    if(adc>4000)
    {
        adc=4000;
    }
    return 100 - adc / 40;
}