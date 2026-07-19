/**
 * @file    beep.h
 * @brief   蜂鸣器驱动 — 报警用有源蜂鸣器控制
 * @note
 *   硬件: PA7 (Beep_Pin), 低电平响/高电平停 (有源蜂鸣器)
 *   初始化: PA7 已在 CubeMX GPIO 初始化中配置为推挽输出、初始高电平(停)
 *   使用:   Beep_On() 开启蜂鸣, Beep_Off() 关闭蜂鸣
 */

#ifndef __BEEP_H__
#define __BEEP_H__

void Beep_On(void);
void Beep_Off(void);

#endif /* __BEEP_H__ */
