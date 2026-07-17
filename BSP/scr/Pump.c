#include "Pump.h"
#include "gpio.h"

/**
 * @brief 开启水泵
 * 
 */
void Pump_On(void)
{
    HAL_GPIO_WritePin(Pump_GPIO_Port,Pump_Pin,GPIO_PIN_SET);
}

/**
 * @brief 关闭水泵
 * 
 */
void Pump_Off(void)
{
    HAL_GPIO_WritePin(Pump_GPIO_Port,Pump_Pin,GPIO_PIN_RESET);
}