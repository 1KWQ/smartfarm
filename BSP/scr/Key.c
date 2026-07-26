//Created by kk on 2026/7/7

#include "Key.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "timers.h"

/* 消抖时长 (ms) */
#define KEY_DEBOUNCE_MS  20

/* freertos.c 中定义的 InputTask 句柄 */
extern osThreadId_t InputTaskHandle;

/* ---- 静态变量 ---- */
static TimerHandle_t key1DebounceTimer = NULL;
static TimerHandle_t key3DebounceTimer = NULL;

/*
 * ISR 安全标志：用于替代 xTimerIsTimerActive（该函数不可在 ISR 中调用）
 * ISR 中设 1，定时器回调中清 0
 */
static volatile uint8_t key1Debouncing = 0;
static volatile uint8_t key3Debouncing = 0;

/**
 * @brief KEY1 消抖定时器回调（定时器守护任务上下文，非 ISR）
 */
static void key1DebounceCallback(TimerHandle_t xTimer)
{
    (void)xTimer;
    key1Debouncing = 0;  /* 消抖完成，清除标志 */

    /* 读取 GPIO 确认按键是否仍按下（消抖确认） */
    if (HAL_GPIO_ReadPin(KEY1_GPIO_Port, KEY1_Pin) == GPIO_PIN_RESET)
    {
        osThreadFlagsSet(InputTaskHandle, KEY1_NOTIFY_BIT);
    }

    /* 清除挂起中断 + 重新开启 EXTI 线 12（顺序重要：先清 PR 再开 IMR） */
    __HAL_GPIO_EXTI_CLEAR_IT(KEY1_Pin);
    EXTI->IMR |= (EXTI_LINE_KEY1);
}

/**
 * @brief KEY3 消抖定时器回调（定时器守护任务上下文，非 ISR）
 */
static void key3DebounceCallback(TimerHandle_t xTimer)
{
    (void)xTimer;
    key3Debouncing = 0;

    if (HAL_GPIO_ReadPin(KEY3_GPIO_Port, KEY3_Pin) == GPIO_PIN_RESET)
    {
        osThreadFlagsSet(InputTaskHandle, KEY3_NOTIFY_BIT);
    }

    __HAL_GPIO_EXTI_CLEAR_IT(KEY3_Pin);
    EXTI->IMR |= (EXTI_LINE_KEY3);
}

/**
 * @brief HAL GPIO EXTI 回调（ISR 上下文）
 * 关闭对应 EXTI 线，启动 20ms 消抖定时器
 *
 * 注意：此函数在 ISR 中执行，只能调用 FromISR 后缀的 FreeRTOS API 和
 *       简单的寄存器访问。严禁调用阻塞函数或非 ISR 安全函数。
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == KEY1_Pin)
    {
        /* 用 volatile 标志防重入，替代 xTimerIsTimerActive（非 ISR 安全） */
        if (key1Debouncing)
        {
            EXTI->PR = (EXTI_LINE_KEY1);  /* 清除抖动挂起的 PR 位 */
            return;
        }
        key1Debouncing = 1;
        EXTI->IMR &= ~(EXTI_LINE_KEY1);   /* 屏蔽 EXTI 线，防止抖动重复触发 */

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        if (xTimerStartFromISR(key1DebounceTimer, &xHigherPriorityTaskWoken) != pdPASS)
        {
            /* 定时器命令队列满，回滚状态 */
            key1Debouncing = 0;
            EXTI->IMR |= (EXTI_LINE_KEY1);
        }
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
    else if (GPIO_Pin == KEY3_Pin)
    {
        if (key3Debouncing)
        {
            EXTI->PR = (EXTI_LINE_KEY3);
            return;
        }
        key3Debouncing = 1;
        EXTI->IMR &= ~(EXTI_LINE_KEY3);

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        if (xTimerStartFromISR(key3DebounceTimer, &xHigherPriorityTaskWoken) != pdPASS)
        {
            key3Debouncing = 0;
            EXTI->IMR |= (EXTI_LINE_KEY3);
        }
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/**
 * @brief 按键模块初始化
 * 创建两个 20ms 单次消抖定时器
 * （GPIO EXTI 和 NVIC 已由 CubeMX 在 MX_GPIO_Init() 中配置）
 */
void Key_Init(void)
{
    key1DebounceTimer = xTimerCreate(
        "key1_db",
        pdMS_TO_TICKS(KEY_DEBOUNCE_MS),
        pdFALSE,                  /* 单次触发 */
        NULL,
        key1DebounceCallback
    );
    key3DebounceTimer = xTimerCreate(
        "key3_db",
        pdMS_TO_TICKS(KEY_DEBOUNCE_MS),
        pdFALSE,
        NULL,
        key3DebounceCallback
    );

    /* 定时器创建失败则断言（通常不会，除非堆内存耗尽） */
    configASSERT(key1DebounceTimer != NULL);
    configASSERT(key3DebounceTimer != NULL);
}
