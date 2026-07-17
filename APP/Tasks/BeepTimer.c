#include "BeepTimer.h"
#include "tim.h"
#include "cmsis_os2.h"
#include "main.h"
/**
 * @brief 启动蜂鸣器
 * 开启软件定时器，实现蜂鸣器周期性响铃，周期为500ms
 * 
 */
void Beep_On(void)
{
    osTimerStart(BeepTimerHandle,500);//开启软件定时器
}

/**
 * @brief 关闭蜂鸣器
 * 停止软件定时器，关闭PWM输出，设置占空比为0
 * 
 */
void Beep_Off(void)
{
    osTimerStop(BeepTimerHandle);//关闭软件定时器
   // HAL_TIM_PWM_Stop(&htim4,TIM_CHANNEL_4);//关闭TIM4通道4的PWM输出
    __HAL_TIM_SET_COMPARE(&htim4,TIM_CHANNEL_4,0);//设置PWM占空比为0
}

/**
 * @brief 软件定时器回调函数
 * @note 实现500ms间隔蜂鸣器响铃 500ms进一次回调函数，一次开，一次关，交替
 * @param argument 
 */
void BeepTimerCallback(void *argument)
{
    static uint8_t Beep_state=1;//0：蜂鸣器关闭状态  1：蜂鸣器开启状态
    if(Beep_state==0)
    {
        //HAL_TIM_PWM_Start(&htim4,TIM_CHANNEL_4);//开启PWM输出
        __HAL_TIM_SET_COMPARE(&htim4,TIM_CHANNEL_4,500);//设置PWM占空比
        Beep_state=1;
    }
    else
    {
        //HAL_TIM_PWM_Stop(&htim4,TIM_CHANNEL_4);//关闭PWM输出
        __HAL_TIM_SET_COMPARE(&htim4,TIM_CHANNEL_4,0);//设置PWM占空比
        Beep_state=0;
    }
}