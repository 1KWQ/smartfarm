/**
 * @file    beep.c
 * @brief   蜂鸣器驱动 — 通过 PA7 GPIO 控制有源蜂鸣器
 * @note
 *   有源蜂鸣器: 低电平响, 高电平停
 *   PA7 初始化在 Core/Src/gpio.c 中完成 (CubeMX 生成)
 */

#include "beep.h"
#include "gpio.h"

/**
 * @brief 开启蜂鸣器 (PA7 输出低电平)
 */
void Beep_On(void)
{
    HAL_GPIO_WritePin(Beep_GPIO_Port, Beep_Pin, GPIO_PIN_RESET);
}

/**
 * @brief 关闭蜂鸣器 (PA7 输出高电平)
 */
void Beep_Off(void)
{
    HAL_GPIO_WritePin(Beep_GPIO_Port, Beep_Pin, GPIO_PIN_SET);
}
