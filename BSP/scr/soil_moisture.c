#include "soil_moisture.h"
#include "adc.h"

/**
 * @brief 开启ADC检测
 * 
 */
void SoilMoisture_Init(void)
{
    HAL_ADC_Start(&hadc2);
}

uint16_t SoilMoisture_Get(void)
{
    uint16_t adc=HAL_ADC_GetValue(&hadc2);
    if(adc>4000)
    {
        adc=4000;
    }
    return 100 - adc / 40;
}