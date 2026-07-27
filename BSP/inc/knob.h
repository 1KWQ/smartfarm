//Created by kk on 2026/7/8 10:40

#ifndef __KNOB_H__
#define __KNOB_H__

#include <stdint.h>

typedef enum {
    KNOB_DIR_NONE  = 0,
    KNOB_DIR_LEFT  = 1,
    KNOB_DIR_RIGHT = 2,
} KnobDirection;

/**
 * @brief 编码器初始化
 *        启动 TIM1 编码器接口，计数器置中，清空内部状态
 */
void Knob_Init(void);

/**
 * @brief  同步内部状态到当前硬件计数器（丢弃未读取的增量）
 * @note   在编码器"不活跃→活跃"的边界调用，如页面切换到编辑页时
 */
void Knob_Sync(void);

/**
 * @brief  获取自上次调用以来的累计旋转步数（不复位，不丢步）
 * @note   TI12 模式下 2 个硬件脉冲 = 1 个业务档位，余数暂存下次合并输出
 * @return 正数 = 右旋步数，负数 = 左旋步数，0 = 无旋转
 */
int32_t Knob_GetDelta(void);

/**
 * @brief  获取旋转方向（每次调用后复位计数器）
 * @deprecated  会丢失步数信息，新代码请使用 Knob_GetDelta()
 * @note   保留此接口仅为向后兼容，调用后会同步清理增量状态
 * @return 本次调用时的旋转方向
 */
KnobDirection Knob_Direction(void);

#endif
