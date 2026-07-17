#include "knob.h"
#include "tim.h"
#define COUNTER_INIT_VALUE 65535/2

/**
 * @brief 设置计数器的值
 * 
 * @param counter 要设置的值 
 */
void Knob_SetCounter(uint32_t counter)
{
    __HAL_TIM_SetCounter(&htim1,counter);
}

/**
 * @brief 获取旋转编码器所在定时器计数器的值
 * 
 * @return uint32_t 计数器的值
 */
uint32_t Knob_GetCounter(void)
{
    return __HAL_TIM_GetCounter(&htim1);
}

/**
 * @brief 旋转编码器初始化
 * 
 */
void Knob_Init(void)
{
    HAL_TIM_Encoder_Start(&htim1,TIM_CHANNEL_ALL);//启动TIM1编码器接口。
    Knob_SetCounter(COUNTER_INIT_VALUE);//设置TIM1计数器初始值
}

/**
 * @brief 判断旋转编码器旋转的方向
 * 
 * @return KnobDirection 旋转编码器旋转的方向
 */
KnobDirection Knob_Direction(void)
{
    KnobDirection direction=KNOB_DIR_NONE;//记录方向的变量,默认为未转（局部变量的作用域只在定义它的语句中）
    uint32_t counter = Knob_GetCounter();
    if(counter < COUNTER_INIT_VALUE)//左转
    {
        direction=KNOB_DIR_LEFT;
    }
    else if(counter > COUNTER_INIT_VALUE)//右转
    {
        direction=KNOB_DIR_RIGHT;
    }
    Knob_SetCounter(COUNTER_INIT_VALUE);//将计数器恢复初始值（只需要判断旋转编码器旋转方向，不需要记录旋转编码器转了几次）
    return direction;
}
