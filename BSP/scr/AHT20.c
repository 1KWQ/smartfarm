#include "AHT20.h"

#define AHT20_ADDRESS 0x70 

/**
 * @brief AHT20初始化函数
 * 
 */
void AHT20_Init(void)
{
    //上电后等待150ms
    vTaskDelay(150);
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
 * @brief AHT20测量温湿度函数
 * 
 * @param temperature 测量的温度
 * @param humidity 测量的湿度
 */
void AHT20_READ(float*temperature,float*humidity)
{
    //用于存储要发送的数据
    uint8_t SendBuffer[3]={0xAC , 0x33 , 0x00};
    uint8_t ReadBuffer[7];
    //1、测量

    //等待10ms
    HAL_Delay(10);
    //发送0xAC命令，触发测量(等待直到发送完毕)
    HAL_I2C_Master_Transmit(&hi2c1,AHT20_ADDRESS,SendBuffer,3,HAL_MAX_DELAY);
    //等待80ms
    HAL_Delay(90);
    //读取测量后的数据
    HAL_I2C_Master_Receive(&hi2c1,AHT20_ADDRESS,ReadBuffer,7,HAL_MAX_DELAY);
    //通过读取状态字判断是否测量完成（bit[7]=0，测量完成）
    //数组的第一项是状态字
    //一直等待直到测量完成
    while((ReadBuffer[0] & 0x80) != 0x00)
    {
        //等待80ms
        HAL_Delay(90);
        //再次读取测量后的数据
        HAL_I2C_Master_Receive(&hi2c1,AHT20_ADDRESS,ReadBuffer,7,HAL_MAX_DELAY);
    }
    //2、计算温湿度值

    //湿度
    uint32_t data=((uint32_t)ReadBuffer[3] >> 4 )+((uint32_t)ReadBuffer[2] << 4)+((uint32_t)ReadBuffer[1] << 12);
    *humidity=(data * 100.0f)/(1 << 20);
    //温度
    data=(((uint32_t)ReadBuffer[3]&0x0F) << 16)+((uint32_t)ReadBuffer[4] << 8)+((uint32_t)ReadBuffer[5]);
    *temperature=(data * 200.0f)/(1 << 20) - 50;
}