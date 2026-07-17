//2026/7/10 9:36
#include "screen.h"

//定义页面索引
ScreenPage pageIndex = Page_HOME;//默认为首页
/**
 * @brief 页面切换
 * 实现[首页<--->编辑页]的切换
 * 按下KEY1按键时调用，实现按下按键切换界面
 * 
 */
void ScreenPage_NextPage()
{
    pageIndex++;
    if(pageIndex == Page_END)//到了编辑页的结束页，则切换为首页
    {
        pageIndex = Page_HOME;
    }
}

//定义编辑项索引
RangeEditIndex rangeEditIndex = RANGE_EDIT_TEMPERATURE_MIN;//默认为编辑第一项
/**
 * @brief 切换到下一个编辑项
 * 旋转编码器右转时调用
 * 实现编码器右转选中下一个编辑项，到最后一个编辑项时，再右转，切换到第一个编辑项
 * （页面同步切换，根据编辑项索引位置，改变编辑页的渲染效果，在screentask中实现）
 * 
 */
void RangeEditIndex_Next()
{
    rangeEditIndex++;
    if(rangeEditIndex == RANGE_EDIT_END)
    {
        rangeEditIndex = RANGE_EDIT_TEMPERATURE_MIN;
    }
}
/**
 * @brief 切换到上一个编辑项
 * 旋转编码器左转时调用
 * 实现编码器左转切换到上一个编辑项，索引到第一个编辑项时，再左转，切换到最后一个编辑项
 * 
 */
void RangeEditIndex_Prev()
{
    if(rangeEditIndex == RANGE_EDIT_TEMPERATURE_MIN)
    {
        rangeEditIndex = RANGE_EDIT_RAIN_GAUGE_MAX;
    }
    else
    rangeEditIndex--;
}

//定义交互模式索引
RangeEditState rangeEditState = RANGE_EDIT_STATE_NORMAL;//默认为浏览模式
/**
 * @brief 进入编辑模式
 * 在索引为浏览模式且按下KEY3时调用
 * 索引为编辑模式时，下划线在选中的编辑项下方闪烁
 * 
 */
void RangeEditState_EnterEditing()
{
    rangeEditState = RANGE_EDIT_STATE_EDITING;
}

/**
 * @brief 退出编辑模式
 * 在索引为编辑模式且按下KEY3时调用
 * 索引为浏览模式时，下划线在选中的编辑项下方停止
 */
void RangeEditState_QuitEditing()
{   
    rangeEditState = RANGE_EDIT_STATE_NORMAL;    
}

/**
 * @brief   切换交互模式
 * 按下KEY3时调用
 * 实现按下KEY3切换交互模式[浏览模式<--->编辑模式]
 * 
 */
void RangeEditState_Toggle()
{
    if(rangeEditState == RANGE_EDIT_STATE_NORMAL)
    {
        RangeEditState_EnterEditing();
    }
    else if(rangeEditState == RANGE_EDIT_STATE_EDITING)
    {
        RangeEditState_QuitEditing();
    }
}
