#ifndef __AHT20_H__
#define __AHT20_H__
//#include "AHT20-21_DEMO_V1_3.h"
#include "i2c.h"
#include "cmsis_os.h"
//初始化函数
void AHT20_Init(void);
//测量温湿度函数
void AHT20_READ(float*temperature,float*humidity);

#endif 