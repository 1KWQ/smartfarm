#include "knob.h"
#include "tim.h"

/* ==========================================================================
 * 硬件参数
 * ========================================================================== */
#define COUNTER_INIT_VALUE       32767   /* 16 位计数器中点 (65535/2)        */
#define KNOB_HW_PULSE_PER_STEP   2       /* TI12 模式: 每个物理档位=2 个脉冲        */
#define KNOB_MAX_DELTA_PER_POLL  200     /* 20ms 内人手物理上限, 超出=失步自动重同步 */

/* ==========================================================================
 * 文件静态变量
 * ========================================================================== */
static uint16_t last_cnt       = COUNTER_INIT_VALUE;
static int32_t  acc_remainder  = 0;

/* ==========================================================================
 * 内部函数
 * ========================================================================== */

/**
 * @brief  获取原始硬件脉冲增量（自动处理 16 位计数器回绕）
 * @note   uint16_t 无符号减法天然回绕，转为 int16_t 即得正确有符号增量
 *         前提: 两次调用间隔内的增量不超过 ±32767（人手指 max ~100）
 * @retval 原始脉冲差值，正数=右旋，负数=左旋
 */
static int16_t Knob_GetRawDelta(void)
{
    uint16_t cur   = (uint16_t)__HAL_TIM_GetCounter(&htim1);
    int16_t  delta = (int16_t)(cur - last_cnt);

    /*
     * 失步检测: 若 delta 超过人手物理上限，说明上次轮询至今已间隔很久
     * （如用户在首页旋转了大量格数导致计数器漂移），此时 int16_t 符号会反转。
     * 直接将 last_cnt 同步到当前值，返回 0，下次轮询恢复正常。
     */
    if (delta > KNOB_MAX_DELTA_PER_POLL || delta < -KNOB_MAX_DELTA_PER_POLL) {
        last_cnt = cur;  /* 仅同步，不返回异常值 */
        return 0;
    }

    last_cnt = cur;
    return delta;
}

/* ==========================================================================
 * 公共 API
 * ========================================================================== */

 /**
 * @brief 设置定时器计数器的值（内部使用）
 */
static void Knob_SetCounter(uint32_t counter)
{
    __HAL_TIM_SetCounter(&htim1, counter);
}


/**
 * @brief 编码器初始化
 */
void Knob_Init(void)
{
    HAL_TIM_Encoder_Start(&htim1, TIM_CHANNEL_ALL);
    Knob_SetCounter(COUNTER_INIT_VALUE);
    last_cnt      = COUNTER_INIT_VALUE;
    acc_remainder = 0;
}

/**
 * @brief  同步内部状态到当前硬件计数器（丢弃未读取的增量）
 * @note   页面切换到编辑页时调用，防止非活跃期间的噪声/漂移被消费
 */
void Knob_Sync(void)
{
    last_cnt      = (uint16_t)__HAL_TIM_GetCounter(&htim1);
    acc_remainder = 0;
}

/**
 * @brief  获取自上次调用以来的累计旋转步数
 * @note   累加器暂存余数，奇数脉冲不丢失
 *         例如: 连续两次 raw=+1 → steps=0 再 steps=1
 * @retval 正数=右旋步数，负数=左旋步数，0=无旋转
 */
int32_t Knob_GetDelta(void)
{
    int16_t raw = Knob_GetRawDelta();
    acc_remainder += raw;

    int32_t steps = acc_remainder / KNOB_HW_PULSE_PER_STEP;
    acc_remainder %= KNOB_HW_PULSE_PER_STEP;

    return steps;
}

/**
 * @brief [废弃] 获取旋转方向，每次调用后复位计数器
 * @deprecated  会丢失步数，新代码请使用 Knob_GetDelta()
 * @note  兼容旧接口的同时同步清理 last_cnt 和累加器，
 *        防止新旧 API 混用导致状态错乱
 */
KnobDirection Knob_Direction(void)
{
    KnobDirection dir = KNOB_DIR_NONE;
    uint16_t cur = (uint16_t)__HAL_TIM_GetCounter(&htim1);

    if (cur > COUNTER_INIT_VALUE)
        dir = KNOB_DIR_RIGHT;
    else if (cur < COUNTER_INIT_VALUE)
        dir = KNOB_DIR_LEFT;

    Knob_SetCounter(COUNTER_INIT_VALUE);
    last_cnt      = COUNTER_INIT_VALUE;    /* 同步状态，防止混用 */
    acc_remainder = 0;

    return dir;
}
