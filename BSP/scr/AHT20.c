#include "AHT20.h"
#include "cmsis_os2.h"

#define AHT20_ADDRESS 0x70

/**
 * @brief AHT20初始化函数
 * @note  调用者需持有 i2c1MutexHandle
 */
void AHT20_Init(void)
{
    //上电后等待150ms (让出CPU)
    osDelay(150);
    //用来接收状态字的变量
    uint8_t ReadBuffer;
    //获取一字节的状态字
    HAL_I2C_Master_Receive(&hi2c1,AHT20_ADDRESS,&ReadBuffer,1,HAL_MAX_DELAY);
    //判断是否初始化
    if((ReadBuffer & 0x18)!= 0x18)
    {
        //初始化0x1B、0x1C、0x1E寄存器
        uint8_t SendBuffer[3]={0xBE,0x08,0x00};
        HAL_I2C_Master_Transmit(&hi2c1,AHT20_ADDRESS,SendBuffer,3,HAL_MAX_DELAY);
    }
}


/**
 * @brief 发送触发测量命令 (0xAC 0x33 0x00)
 * @note  调用者需持有 i2c1MutexHandle (占用 ~0.3ms)
 */
void AHT20_Trigger(void)
{
    uint8_t SendBuffer[3]={0xAC , 0x33 , 0x00};
    HAL_I2C_Master_Transmit(&hi2c1,AHT20_ADDRESS,SendBuffer,3,HAL_MAX_DELAY);
}

/**
 * @brief 读取温湿度数据
 * @note  调用者需持有 i2c1MutexHandle
 *        调用前需确保已等待 ≥80ms (测量时间)
 *        忙状态最多重试 5 次 (带超时保护)
 * @param temperature 测量的温度
 * @param humidity 测量的湿度
 */
void AHT20_ReadResult(float*temperature,float*humidity)
{
    uint8_t ReadBuffer[7];

    //读取测量后的数据, 带忙状态超时保护 (最多重试5次)
    for(int retry = 0; retry < 5; retry++)
    {
        HAL_I2C_Master_Receive(&hi2c1,AHT20_ADDRESS,ReadBuffer,7,HAL_MAX_DELAY);

        //通过读取状态字判断是否测量完成（bit[7]=0，测量完成）
        if((ReadBuffer[0] & 0x80) == 0x00)
        {
            break;  //测量完成
        }

        //仍忙 → 短暂等待后重试
        osDelay(10);
    }

    //计算温湿度值
    //湿度
    uint32_t data=((uint32_t)ReadBuffer[3] >> 4 )+((uint32_t)ReadBuffer[2] << 4)+((uint32_t)ReadBuffer[1] << 12);
    *humidity=(data * 100.0f)/(1 << 20);
    //温度
    data=(((uint32_t)ReadBuffer[3]&0x0F) << 16)+((uint32_t)ReadBuffer[4] << 8)+((uint32_t)ReadBuffer[5]);
    *temperature=(data * 200.0f)/(1 << 20) - 50;
}
