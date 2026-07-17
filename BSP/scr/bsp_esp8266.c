#include "bsp_esp8266.h"
#include "gpio.h"

/* ==========================================================================
 * [驱动层] 私有变量
 * ========================================================================== */
/** @brief USART2 句柄 —— 非static, 供stm32f1xx_it.c的USART2_IRQHandler引用 */
//UART_HandleTypeDef huart2_esp;

/** @brief 串口发送互斥锁 (防止多任务同时发送AT指令) */
static osMutexId_t uart_tx_mutex = NULL;

/** @brief 环形接收缓冲区 */
static uint8_t rx_ring_buf[ESP8266_RX_BUF_SIZE];

/** @brief 环形缓冲区写指针 (仅ISR上下文写入) */
static volatile uint16_t rx_wr = 0;

/** @brief 环形缓冲区读指针 (仅任务上下文读取) */
static volatile uint16_t rx_rd = 0;

/** @brief AT响应文本暂存区 (单次命令的响应) */
static char at_response[ESP8266_RX_BUF_SIZE];

/** @brief 响应已就绪标志 (ISR置1, 任务读后清0) */
static volatile int at_resp_ready = 0;

/** @brief 响应结果: 0=无, 1=收到OK, 2=收到ERROR */
static volatile int at_resp_result = 0;

/** @brief 当前连接状态 */
static volatile ESP8266_ConnState conn_state = ESP8266_STATE_UNINIT;

/** @brief 最后一次错误描述 */
static char last_error[64] = "";

/** @brief 重连计数器 */
static uint32_t reconnect_count = 0;

/** @brief 数据上报消息ID (自增) */
static uint32_t msg_id = 0;
/**
 * @brief 响应检测状态机
 *
 * ESP8266 AT固件响应终止模式:
 *   成功: ...\r\nOK\r\n
 *   失败: ...\r\nERROR\r\n
 *
 * 在ISR中逐字节驱动此状态机, 检测到终止序列后
 * 设置 at_resp_ready = 1 通知任务上下文
 */
typedef enum {
    DETECT_IDLE = 0,
    DETECT_CR1,       /* 收到 \r */
    DETECT_LF1,       /* 收到 \n (行首) */
    DETECT_O,         /* 收到 O */
    DETECT_K,         /* 收到 K */
    DETECT_CR2,       /* OK后的 \r */
    DETECT_E,         /* 收到 E */
    DETECT_R1,        /* 收到 R (ER) */
    DETECT_R2,        /* 收到 R (ERR) */
    DETECT_O2,        /* 收到 O (ERRO) */
} RespDetectState;

static volatile RespDetectState detect_state = DETECT_IDLE;

/* ==========================================================================
 * [驱动层] 私有函数声明
 * ========================================================================== */
static void ESP8266_HardReset(void);
static ESP8266_Status ESP8266_SendCmd(const char *cmd, uint32_t timeout_ms);
static int ESP8266_ResponseContains(const char *str);
static void ESP8266_SetError(const char *str);

/* ==========================================================================
 * [驱动层] 硬件复位
 * ========================================================================== */
/**
 * @brief 硬件复位ESP8266模块
 * @note  通过拉低复位引脚实现硬件复位，未配置复位引脚则软件复位
 */
static void ESP8266_HardReset(void)
{
#ifdef ESP8266_RESET_PIN
    HAL_GPIO_WritePin(ESP8266_RESET_GPIO_Port, ESP8266_RESET_Pin, GPIO_PIN_RESET);
    osDelay(200);  // 拉低至少200ms
    HAL_GPIO_WritePin(ESP8266_RESET_GPIO_Port, ESP8266_RESET_Pin, GPIO_PIN_SET);
    osDelay(1000);  // 等待模块启动
#else
    // 如果没有配置复位引脚，则尝试软件复位
    ESP8266_SendCmd("AT+RST\r\n", 5000);
    osDelay(2000);  // 等待模块重启
#endif
}

/* ==========================================================================
 * [驱动层] 环形缓冲区（单生产者ISR/单消费者任务）
 * ========================================================================== */
/**
 * @brief 将一个字节写入环形缓冲区
 * @param byte 要写入的字节
 */
inline static void ring_buf_put(uint8_t ch)
{
    if(rx_wr+1 == rx_rd)
    {
        // 缓冲区已满，丢弃新数据
        return;
    }
    //未满则写入
    ring_buf[rx_wr] = byte;
    rx_wr=(rx_wr+1)%ESP8266_RX_BUF_SIZE;
}

inline static uint8_t ring_buf_get(uint8_t ch)
{

}

inline static int ring_buf_available(void)
{
    return (rx_wr - rx_rd + ESP8266_RX_BUF_SIZE) % ESP8266_RX_BUF_SIZE;
}

/**
 * @brief 清空环形缓冲区
 * 
 */
inline static void ring_buf_clear(void)
{
    rx_rd = rx_wr;
}

/* ==========================================================================
 * [驱动层] USART2中断处理接收
 * ========================================================================== */
/**
 * @brief USART2中断处理函数
 * @param huart USART句柄
 * @note 此函数在stm32f1xx_it.c中由USART2_IRQHandlerd调用
 * 流程:先判断是否为接收中断,如果是则读取数据并放入环形缓冲区-->调用响应检测状态机-->清除相应中断标志位
 *      再判断其他异常情况，并清除这些异常情况对应的标志位
 */
void ESP8266_UART_IRQHandler(UART_HandleTypeDef *huart)
{
    /* RXNE读数据寄存器非空(读DR会将RXNE位清零) */
    if(__HAL_UART_GET_FLAG(huart,UART_FLAG_RXNE)){
        uint8_t ch=(uint8_t)(huart->Instance->DR & 0xFF);//读取数据寄存器DR中的数据
        ring_buf_put(ch);
		detect_response_end(ch);
    }
    /* ORE过载错误(读SR再读DR会将ORE位清零) */
    if(__HAL_UART_GET_FLAG(huart,UART_FLAG_ORE)){
        (void)(huart->Instance->DR & 0xFF);
    }
    /* FE帧错误(直接清零) */
    if(__HAL_UART_GET_FLAG(huart,UART_FLAG_FE)){
        __HAL_UART_CLEAR_FLAG(huart,UART_FLAG_FE);
    }
    /* PE校验错误(直接清零) */
    if(__HAL_UART_GET_FLAG(huart,UART_FLAG_FE)){
        __HAL_UART_CLEAR_FLAG(huart,UART_FLAG_FE);
    }
    /* 溢出时可能同时有RXNE, 再次检查 */
    if(__HAL_UART_GET_FLAG(huart,UART_FLAG_RXNE)){
        uint8_t ch=(uint8_t)(huart->Instance->DR & 0xFF);//读取数据寄存器DR中的数据
        ring_buf_put(ch);
		detect_response_end(ch);
    }
}

/* ==========================================================================
 * [驱动层] 响应检测状态机
 * ========================================================================== */
/**
 * @brief 响应检测状态机
 * 
 * @param ch 要检测的单字节
 * @note 在usart2中断处理函数中调用
 * 检测两种终止模式:  \r\nOK\r\n    → at_resp_result=1  命令成功
 *                   \r\nERROR     → at_resp_result=2  命令失败
 * 检测到后设置 at_resp_ready=1 , 任务上下文轮询该标志
 */
static void detect_response_end(uint8_t ch)
{
    switch (detect_state){
    case DETECT_IDLE:
		if(ch == '\r') detect_state = DETECT_CR1;
		else if (ch == 'E')  detect_state = DETECT_E; //虽然绝大多数 AT 响应都以\r\n开头,但有些情况，ERROR可能不是跟在\r\n之后（比如乱码中的ERROR）
        break;
    case DETECT_CR1:
		detect_state = (ch == '\n') ? DETECT_LF1 : DETECT_IDLE;
        break;
    case DETECT_LF1:
		if(ch == 'O')         detect_state = DETECT_O;
		else if(ch == 'E')    detect_state = DETECT_E;
		else if(ch == '\r')   detect_state = DETECT_CR1;
		else      			  detect_state = DETECT_IDLE;
        break;
    case DETECT_O:
        detect_state = (ch =='K') ? DETECT_K : DETECT_IDLE;
        break;
    case DETECT_K:
        detect_state = (ch =='\r') ? DETECT_CR2 : DETECT_IDLE;
        break;
    case DETECT_CR2:
        if(ch == '\n'){
			/* 检测到\r\nOK\r\n->命令成功*/
			at_resp_result = 1;
			at_resp_ready = 1;
		}
		detect_state = DETECT_IDLE;
        break;
    case DETECT_E:
        detect_state = (ch =='R') ? DETECT_R1 : DETECT_IDLE;
        break;
    case DETECT_R1:
        detect_state = (ch =='R') ? DETECT_R2 : DETECT_IDLE;
        break;
    case DETECT_R2:
        detect_state = (ch =='O') ? DETECT_O2 : DETECT_IDLE;
        break; 
	case DETECT_O2:
        if(ch == 'R'){
			/* 检测到ERROR->命令失败(不必等后续\r\n) */
			at_resp_result=2;
			at_resp_ready=1;
		}
		detect_state = DETECT_IDLE;
        break;                       
    default:
		detect_state = DETECT_IDLE;
        break;
    }
}

/* ==========================================================================
 * [驱动层] AT指令收发引擎
 * ========================================================================== */

/**
 * @brief 发送AT指令并等待响应
 * 
 * @param cmd AT指令字符串(需包含\r\n)
 * @param timeout_ms 等待响应的时间(单位:ms)
 * @return ESP8266_Status 返回AT指令的响应结果: ESP8266_OK 命令成功，ESP8266_TIMEOUT 超时，ESP8266_AT_FAIL 返回ERROR
 * 执行流程：
 * 1、获取发送互斥锁（最多等5s）
 * 2、清空循环缓冲区和响应暂存区
 * 3、阻塞发送AT指令字符串（最多等500ms）
 * 4、等待响应，记录等待时间，超时则退出等待；轮询at_resp_ready标志位（ISR中调用响应检测状态机会置位），每次轮询osdelay(10)，让出cpu 
 * 5、将环形缓冲区内容取出，放到响应暂存区			
 */
static ESP8266_Status ESP8266_SendCmd(const char *cmd, uint32_t timeout_ms)
{
	
}