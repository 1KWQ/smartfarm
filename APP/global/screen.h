//create by kk on 2026/7/9 21:50
//页面索引枚举
typedef enum{
    Page_HOME=0,    //首页
    Page_RANGE=1,   //编辑页
    Page_END=2,     //结束标志，用于循环切换
} ScreenPage;
extern ScreenPage pageIndex;
void ScreenPage_NextPage();

//环境安全范围索引枚举
typedef enum{
    RANGE_EDIT_TEMPERATURE_MIN=0,        //最小温度报警阈值
    RANGE_EDIT_TEMPERATURE_MAX=1,        //最大温度报警阈值
    RANGE_EDIT_HUMIDITY_MIN = 2,         // 最小湿度报警阈值
    RANGE_EDIT_HUMIDITY_MAX = 3,         // 最大湿度报警阈值
    RANGE_EDIT_LIGHT_INTENSITY_MIN = 4,  // 最小光照强度报警阈值
    RANGE_EDIT_LIGHT_INTENSITY_MAX = 5,  // 最大光照强度报警阈值
    RANGE_EDIT_SOIL_MOISTURE_MIN = 6,    // 最小土壤湿度报警阈值
    RANGE_EDIT_SOIL_MOISTURE_MAX = 7,    // 最大土壤湿度报警阈值
    RANGE_EDIT_RAIN_GAUGE_MAX = 8,       // 最大降雨量报警阈值
    RANGE_EDIT_END,                      // 结束标志：用于循环切换
}RangeEditIndex;
extern RangeEditIndex rangeEditIndex;
void RangeEditIndex_Next();
void RangeEditIndex_Prev();


//交互模式索引枚举
typedef enum{
    RANGE_EDIT_STATE_NORMAL=0,     //浏览模式
    RANGE_EDIT_STATE_EDITING=1,    //编辑模式
}RangeEditState;
extern RangeEditState rangeEditState;
void RangeEditState_EnterEditing();
void RangeEditState_QuitEditing();
void RangeEditState_Toggle();