#include "Key.h"

#define IS_KEY1_PRESSED() HAL_GPIO_ReadPin(KEY1_GPIO_Port,KEY1_Pin)==GPIO_PIN_RESET
#define IS_KEY3_PRESSED() HAL_GPIO_ReadPin(KEY3_GPIO_Port,KEY3_Pin)==GPIO_PIN_RESET
/**
 * @brief 检测按键1是否按下
 * 
 * @return uint8_t  0:未按下  1:按下
 * @note 适合用于判断语句中
 */
uint8_t isKey1Clicked()
{
    static uint8_t pressed=0;//0：未按下  1：已按下
    //判断是否按下
    if(IS_KEY1_PRESSED() && !pressed)
    {
        osDelay(15);//延迟消抖
        if(IS_KEY1_PRESSED())
        {
            pressed=1;
        }
    }
    //判断是否松开
    if(!IS_KEY1_PRESSED() && pressed)
    {
        pressed=0;
        return 1;
    }
    return 0;
}
/**
 * @brief 检测按键3是否按下
 * 
 * @return uint8_t  0:未按下  1:按下
 * @note 适合用于判断语句中
 */
uint8_t isKey3Clicked()
{
    static uint8_t pressed=0;//0：未按下  1：已按下
    //判断是否按下
    if(IS_KEY3_PRESSED() && !pressed)
    {
        osDelay(15);//延迟消抖
        if(IS_KEY3_PRESSED())
        {
            pressed=1;
        }
    }
    //判断是否松开
    if(!IS_KEY3_PRESSED() && pressed)
    {
        pressed=0;
        return 1;
    }
    return 0;
}