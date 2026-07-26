//create by kk on2026/7/10 12:00
#include "cmsis_os2.h"
#include "FarmState.h"
#include "screen.h"
#include "Key.h"
#include "knob.h"

/**
 * @brief 修改选中编辑项的值
 * 先判断当中选中了哪一个编辑项
 * 再根据旋转的方向对其进行修改
 * 修改注意事项：修改的最小值不能大于最大安全阈值，修改的最大值不能小于最小安全阈值
 * @param index  当前选中编辑项的索引（RangeEditIndex中的值）
 * @param direction   旋转编码器的方向
 * @note  浮点数修改跨度：0.1  整数修改跨度：1
 */
void EditRangeValue(RangeEditIndex index,int8_t direction)
{
    switch(index)
    {
        //选中最低温度
        case RANGE_EDIT_TEMPERATURE_MIN:
            //最小温度不能大于最大温度
            if(farmSafeRange.minTemperature+0.1*direction < farmSafeRange.maxTemperature)
            {
                farmSafeRange.minTemperature+=0.1*direction;//修改最低温度
            }       
            break;
        //选中最高温度
        case RANGE_EDIT_TEMPERATURE_MAX:
            //最高温度不能小于最低温度
            if(farmSafeRange.maxTemperature+0.1*direction > farmSafeRange.minTemperature)
            {
                farmSafeRange.maxTemperature+=0.1*direction;//修改最高温度
            }
            break;
        //选中最低湿度
        case RANGE_EDIT_HUMIDITY_MIN:
            //最低湿度不能大于最高湿度
            if(farmSafeRange.minHumidity+0.1*direction < farmSafeRange.maxHumidity)
            {
                farmSafeRange.minHumidity+=0.1*direction;//修改最低湿度
            }
            break;
        //选中最高湿度
        case RANGE_EDIT_HUMIDITY_MAX:
            //最高湿度不能低于最低湿度
            if(farmSafeRange.maxHumidity+0.1*direction > farmSafeRange.minHumidity)
            {
                farmSafeRange.maxHumidity+=0.1*direction;
            }
            break;
        //选中最小光照强度
        case RANGE_EDIT_LIGHT_INTENSITY_MIN:
            if(farmSafeRange.minLightIntensity+direction < farmSafeRange.maxLightIntensity)
            {
               farmSafeRange.minLightIntensity+=direction;
            }
            break;
        //选中最大光照强度
        case RANGE_EDIT_LIGHT_INTENSITY_MAX:
            if(farmSafeRange.maxLightIntensity+direction > farmSafeRange.minLightIntensity)
            {
                farmSafeRange.maxLightIntensity+=direction;
            }
            break;
        //选中最小土壤湿度
        case RANGE_EDIT_SOIL_MOISTURE_MIN:
            if(farmSafeRange.minSoilMoisture+direction < farmSafeRange.maxSoilMoisture)
            {
                farmSafeRange.minSoilMoisture+=direction;
            }
            break;
        //选中最大土壤湿度
        case RANGE_EDIT_SOIL_MOISTURE_MAX:
            if(farmSafeRange.maxSoilMoisture+direction > farmSafeRange.minSoilMoisture)
            {
                farmSafeRange.maxSoilMoisture+=direction;
            }
            break;
        //选中最大降水量
        case RANGE_EDIT_RAIN_GAUGE_MAX:
            if(farmSafeRange.maxRainGauge+direction>0 && farmSafeRange.maxRainGauge+direction<100)
            {
                farmSafeRange.maxRainGauge+=direction;
            }
        break;
        default:
        break;
    }
}
  
/**
 * @brief 输入任务主函数
 * 处理输入任务
 * 按键按下    1、KEY1按下，实现首页和编辑页切换，通过调用ScreenPage_NextPage()（通过修改索引实现） 
 *            2、KEY3按下（页面索引为编辑页时有效），实现编辑页交互模式切换，通过调用RangeEditState_Toggle()
 * 旋转编码器旋转（页面索引为编辑页时有效）
 *              1、浏览模式：旋转实现切换选中的编辑项（左旋：切换选中上一个、右旋：切换选中下一个）
 *                 左旋：RangeEditIndex_Prev()，右旋：RangeEditIndex_Next()
 *              2、编辑模式：旋转实现选中编辑项的数值修改 （左旋：编辑项数值减少，右旋：编辑项数值增加）
 *                   首先要判断当前选中的编辑项是哪个
 *                  左旋：farmSafeRange.min+=0.1/1*direction  
 * 
 * @param argument 
 */
void StartInputTask(void *argument)
{
    //旋转编码器初始化
    Knob_Init();
    //按键模块初始化（GPIO EXTI + 消抖定时器）
    Key_Init();

    for(;;)
    {
        /*
         * 阻塞等待按键通知，超时 20ms 用于编码器轮询
         *   - 按键事件到达：flags 携带 KEY1/KEY3 通知位
         *   - 超时返回：flags 最高位置位（osFlagsErrorTimeout），忽略
         */
        uint32_t flags = osThreadFlagsWait(
            KEY1_NOTIFY_BIT | KEY3_NOTIFY_BIT,  /* 等待的位 */
            osFlagsWaitAny,                     /* 任意一位触发即返回 */
            20U                                 /* 20ms 超时，保证编码器轮询 */
        );

        /* 超时/错误返回的特征是 bit31 置位，此时不得处理按键事件 */
        if(flags & osFlagsError) {
            /* 超时返回，跳过按键处理 */
        }
        //KEY1按下
        else if(flags & KEY1_NOTIFY_BIT)
        {
            ScreenPage_NextPage();//首页和编辑页切换
        }
        //KEY3按下（仅编辑页有效）
        else if(flags & KEY3_NOTIFY_BIT)
        {
            if(pageIndex == Page_RANGE)
            {
                RangeEditState_Toggle();//编辑页交互模式切换[浏览模式<--->编辑模式]
            }
        }

        //页面为编辑页时 → 处理编码器
        if(pageIndex == Page_RANGE)
        {
            KnobDirection direction=Knob_Direction();//储存旋转编码器方向
            //交互模式为浏览模式
            if(rangeEditState == RANGE_EDIT_STATE_NORMAL)
            {
                //左旋
                if(direction == KNOB_DIR_LEFT)
                {
                    RangeEditIndex_Prev();//切换到上一个编辑项
                }
                //右旋
                else if(direction == KNOB_DIR_RIGHT)
                {
                    RangeEditIndex_Next();//切换到下一个编辑项
                }
            }
            //交互模式为编辑模式(先判断选中了哪个编辑项再编辑其数值)
            else if(rangeEditState == RANGE_EDIT_STATE_EDITING)
            {
                //左旋-1
                if(direction == KNOB_DIR_LEFT)
                {
                    EditRangeValue(rangeEditIndex,-1);
                }
                //右旋+1
                else if(direction == KNOB_DIR_RIGHT)
                {
                    EditRangeValue(rangeEditIndex,1);
                }
            }
        }
    }
}