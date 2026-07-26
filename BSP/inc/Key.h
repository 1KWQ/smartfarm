//Created by kk on 2026/7/7

#ifndef __KEY_H__
#define __KEY_H__
#include "main.h"
#include "cmsis_os.h"

/* 任务通知位定义 */
#define KEY1_NOTIFY_BIT  (1UL << 0)  /* KEY1 按下事件 */
#define KEY3_NOTIFY_BIT  (1UL << 1)  /* KEY3 按下事件 */
/* 自定义EXTI中断线掩码，专门操作EXTI->IMR / EXTI->PR */
#define EXTI_LINE_KEY1    (1UL << 12U)
#define EXTI_LINE_KEY3    (1UL << 15U)
/**
 * @brief 按键模块初始化
 * 配置 GPIO EXTI 下降沿中断 + 创建 20ms 消抖软件定时器
 */
void Key_Init(void);

#endif
