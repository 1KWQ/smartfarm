#ifndef __AHT20_H__
#define __AHT20_H__
//#include "AHT20-21_DEMO_V1_3.h"
#include "i2c.h"
#include "cmsis_os.h"
//初始化函数 (调用者需持有 i2c1MutexHandle)
void AHT20_Init(void);
//发送触发测量命令 (调用者需持有 i2c1MutexHandle)
void AHT20_Trigger(void);
//等待测量完成并读取温湿度 (调用者需持有 i2c1MutexHandle, 调用前需已等待 ≥80ms)
void AHT20_ReadResult(float*temperature,float*humidity);

#endif 