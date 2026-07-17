//create by kk on 2026/7/9 16:17
#include "cmsis_os2.h"
#include "main.h"
#include "stm32f1xx_hal_uart.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>
#include "freertos.h"

void StartBLETask(void *argument)
{
    for(;;)
    {
        char *msg;//定义用于接收字符串首地址的指针变量
        //从队列中获取消息
        osMessageQueueGet(BLEQueueHandle,&msg,NULL,osWaitForever);
        if(msg != NULL)//消息不为空
        {
            //通过串口3使用DMA发送消息到蓝牙模块
            HAL_UART_Transmit_DMA(&huart3,(uint8_t*)msg,strlen(msg));
            //等待DMA发送完成
            while(HAL_UART_GetState(&huart3)!=HAL_UART_STATE_READY)
            {
                osDelay(1);
            }
    }
        vPortFree(msg);
    }
}