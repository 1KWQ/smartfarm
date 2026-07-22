/**
 * @file    bsp_esp8266.c
 * @brief   ESP8266-01S 驱动层 — 硬件抽象 + AT指令引擎 (纯驱动, 无业务逻辑)
 * @note    USART2 (TX=PA2, RX=PA3), 115200-8N1, 复位引脚 PB0
 *
 * 架构分层:
 *   [驱动层-硬件抽象] → 环形缓冲区、HardReset
 *   [驱动层-数据接收] → ISR → ring_buf → detect_response_end 状态机
 *   [驱动层-AT引擎]   → ESP8266_SendCmd (互斥锁+超时+结果解析)
 *   [驱动层-状态管理] → FlushRx / SetState / GetState / SetError / GetLastError
 *
 *   [业务层] → 见 APP/service/esp8266config.c
 */

#include "bsp_esp8266.h"
#include "gpio.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

/* ==========================================================================
 * [驱动层] 私有变量
 * ========================================================================== */

/** @brief 环形接收缓冲区 */
static uint8_t rx_ring_buf[ESP8266_RX_BUF_SIZE];

/** @brief 环形缓冲区写指针 (仅ISR上下文写入) */
static volatile uint16_t rx_wr = 0;

/** @brief 环形缓冲区读指针 (仅任务上下文读取) */
static volatile uint16_t rx_rd = 0;

/** @brief AT响应文本暂存区 (单次命令响应快照) */
static char at_response[ESP8266_RX_BUF_SIZE];

/** @brief 响应已就绪标志 (ISR置1, 任务读后清0) */
static volatile int at_resp_ready = 0;

/** @brief 响应结果: 0=无, 1=收到OK, 2=收到ERROR */
static volatile int at_resp_result = 0;

/** @brief 当前连接状态 (驱动/业务共用) */
static volatile ESP8266_ConnState conn_state = ESP8266_STATE_UNINIT;

/** @brief 最后一次错误描述 */
static char last_error[64] = "";

/* ==========================================================================
 * [驱动层] 响应检测状态机 (私有)
 * ========================================================================== */
typedef enum {
    DETECT_IDLE = 0,
    DETECT_CR1,       /* 收到 \r          */
    DETECT_LF1,       /* 收到 \n (行首)   */
    DETECT_O,         /* 收到 O           */
    DETECT_K,         /* 收到 K           */
    DETECT_CR2,       /* OK后的 \r        */
    DETECT_E,         /* 收到 E           */
    DETECT_R1,        /* 收到 R (ER)      */
    DETECT_R2,        /* 收到 R (ERR)     */
    DETECT_O2,        /* 收到 O (ERRO)    */
} RespDetectState;

static volatile RespDetectState detect_state = DETECT_IDLE;

/* ==========================================================================
 * [驱动层] 私有函数声明
 * ========================================================================== */
static void detect_response_end(uint8_t ch);

/* ==========================================================================
 * [驱动层] 环形缓冲区 (单生产者ISR / 单消费者任务)
 * ========================================================================== */

inline static void ring_buf_put(uint8_t ch)
{
    uint16_t next_wr = (rx_wr + 1) % ESP8266_RX_BUF_SIZE;
    if (next_wr == rx_rd) {
        return;  /* 缓冲区满, 丢弃新数据 */
    }
    rx_ring_buf[rx_wr] = ch;
    rx_wr = next_wr;
}

static inline uint8_t ring_buf_get(void)
{
    if (rx_rd == rx_wr) return 0;
    uint8_t ch = rx_ring_buf[rx_rd];
    rx_rd = (rx_rd + 1) % ESP8266_RX_BUF_SIZE;
    return ch;
}

inline static int ring_buf_available(void)
{
    return (rx_wr - rx_rd + ESP8266_RX_BUF_SIZE) % ESP8266_RX_BUF_SIZE;
}

inline static void ring_buf_clear(void)
{
    rx_rd = rx_wr;
}

/* ==========================================================================
 * [驱动层] 硬件复位
 * ========================================================================== */
/**
 * @brief 执行硬件复位 (PB0低电平脉冲)
 * 
 */
void ESP8266_HardReset(void)
{
    /* 低电平脉冲复位 */
    HAL_GPIO_WritePin(ESP8266_RST_GPIO_Port, ESP8266_RST_Pin, GPIO_PIN_RESET);
    osDelay(300);
    HAL_GPIO_WritePin(ESP8266_RST_GPIO_Port, ESP8266_RST_Pin, GPIO_PIN_SET);
    osDelay(1500);  /* 等待模块启动 */
}

/* ==========================================================================
 * [驱动层] 缓冲区与状态管理 (公共API)
 * ========================================================================== */

/**
 * @brief 清空接收缓冲区并重置响应检测状态机
 * @note  业务层在复位/重连前调用, 清除残留数据
 */
void ESP8266_FlushRx(void)
{
    ring_buf_clear();
    memset(at_response, 0, sizeof(at_response));
    at_resp_ready  = 0;
    at_resp_result = 0;
    detect_state   = DETECT_IDLE;
}

/**
 * @brief 设置模块连接状态 (业务层调用)
 */
void ESP8266_SetState(ESP8266_ConnState s)
{
    conn_state = s;
}

/**
 * @brief 查询当前连接状态
 */
ESP8266_ConnState ESP8266_GetState(void)
{
    return conn_state;
}

/**
 * @brief 记录错误描述
 */
void ESP8266_SetError(const char *str)
{
    strncpy(last_error, str, sizeof(last_error) - 1);
    last_error[sizeof(last_error) - 1] = '\0';
}

/**
 * @brief 获取最后一次错误描述
 */
const char* ESP8266_GetLastError(void)
{
    return last_error;
}

/* ==========================================================================
 * [驱动层] 响应检测状态机 (私有)
 * ========================================================================== */
/**
 * @brief ISR中逐字节驱动, 检测AT响应终止序列
 *
 * 检测两种终止模式:
 *   \r\nOK\r\n    → at_resp_result=1  命令成功
 *   ERROR         → at_resp_result=2  命令失败
 */
static void detect_response_end(uint8_t ch)
{
    switch (detect_state) {
    case DETECT_IDLE:
        if (ch == '\r')      detect_state = DETECT_CR1;
        else if (ch == 'E')  detect_state = DETECT_E;
        break;
    case DETECT_CR1:
        detect_state = (ch == '\n') ? DETECT_LF1 : DETECT_IDLE;
        break;
    case DETECT_LF1:
        if (ch == 'O')       detect_state = DETECT_O;
        else if (ch == 'E')  detect_state = DETECT_E;
        else if (ch == '\r') detect_state = DETECT_CR1;
        else                 detect_state = DETECT_IDLE;
        break;
    case DETECT_O:
        detect_state = (ch == 'K') ? DETECT_K : DETECT_IDLE;
        break;
    case DETECT_K:
        detect_state = (ch == '\r') ? DETECT_CR2 : DETECT_IDLE;
        break;
    case DETECT_CR2:
        if (ch == '\n') {
            at_resp_result = 1;
            at_resp_ready = 1;
        }
        detect_state = DETECT_IDLE;
        break;
    case DETECT_E:
        detect_state = (ch == 'R') ? DETECT_R1 : DETECT_IDLE;
        break;
    case DETECT_R1:
        detect_state = (ch == 'R') ? DETECT_R2 : DETECT_IDLE;
        break;
    case DETECT_R2:
        detect_state = (ch == 'O') ? DETECT_O2 : DETECT_IDLE;
        break;
    case DETECT_O2:
        if (ch == 'R') {
            at_resp_result = 2;
            at_resp_ready = 1;
        }
        detect_state = DETECT_IDLE;
        break;
    default:
        detect_state = DETECT_IDLE;
        break;
    }
}

/* ==========================================================================
 * [驱动层] USART2中断处理
 * ========================================================================== */
void ESP8266_UART_IRQHandler(UART_HandleTypeDef *huart)
{
    /* RXNE: 读数据寄存器非空 */
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_RXNE)) {
        uint8_t ch = (uint8_t)(huart->Instance->DR & 0xFF);
        ring_buf_put(ch);
        detect_response_end(ch);
    }
    /* ORE: 过载错误 */
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_ORE)) {
        (void)(huart->Instance->DR & 0xFF);
    }
    /* FE: 帧错误 */
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_FE)) {
        __HAL_UART_CLEAR_FLAG(huart, UART_FLAG_FE);
    }
    /* PE: 校验错误 */
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_PE)) {
        __HAL_UART_CLEAR_FLAG(huart, UART_FLAG_PE);
    }
    /* 溢出后可能同时有RXNE, 再次检查 */
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_RXNE)) {
        uint8_t ch = (uint8_t)(huart->Instance->DR & 0xFF);
        ring_buf_put(ch);
        detect_response_end(ch);
    }
}

/* ==========================================================================
 * [驱动层] AT指令收发引擎
 * ========================================================================== */
/**
 * @brief 发送AT指令并等待OK/ERROR响应
 * @param cmd        AT指令字符串 (需包含\r\n)
 * @param timeout_ms 超时(ms)
 * @return ESP8266_OK / ESP8266_TIMEOUT / ESP8266_AT_FAIL / ...
 */
ESP8266_Status ESP8266_SendCmd(const char *cmd, uint32_t timeout_ms)
{
    /* 获取发送互斥锁 (最多等5s) */
    if (osMutexAcquire(uart_tx_mutexHandle, 5000) != osOK) {
        return ESP8266_BUSY;
    }

    /* 清空缓冲区与状态 */
    ring_buf_clear();
    memset(at_response, 0, sizeof(at_response));
    at_resp_ready  = 0;
    at_resp_result = 0;

    /* 阻塞发送AT指令 */
    uint16_t len = (uint16_t)strlen(cmd);
    if (HAL_UART_Transmit(&huart2, (uint8_t *)cmd, len, 500) != HAL_OK) {
        osMutexRelease(uart_tx_mutexHandle);
        ESP8266_SetError("SendCmd: UART transmit failed");
        return ESP8266_SEND_FAIL;
    }

    /* 轮询等待响应 (osDelay让出CPU) */
    uint32_t waited = 0;
    const uint32_t poll_interval = 10;
    while (waited < timeout_ms && at_resp_ready == 0) {
        osDelay(poll_interval);
        waited += poll_interval;
    }

    /* 从环形缓冲区取出响应文本 */
    uint16_t avail = ring_buf_available();
    if (avail >= sizeof(at_response))
        avail = sizeof(at_response) - 1;
    for (uint16_t i = 0; i < avail; i++) {
        at_response[i] = ring_buf_get();
    }
    at_response[avail] = '\0';

    /* 超时未响应 */
    if (waited >= timeout_ms && at_resp_ready == 0) {
        osMutexRelease(uart_tx_mutexHandle);
        ESP8266_SetError("SendCmd: timeout");
        return ESP8266_TIMEOUT;
    }

    /* 状态机判定为OK (result==1) */
    if (at_resp_result == 1) {
        osMutexRelease(uart_tx_mutexHandle);
        return ESP8266_OK;
    }

    /* 状态机判定为ERROR 或 文本含ERROR/FAIL */
    if (at_resp_result == 2 ||
        strstr(at_response, "ERROR") != NULL ||
        strstr(at_response, "FAIL")  != NULL) {
        osMutexRelease(uart_tx_mutexHandle);
        return ESP8266_AT_FAIL;
    }

    osMutexRelease(uart_tx_mutexHandle);
    return ESP8266_OK;
}

/* ==========================================================================
 * [驱动层] 响应文本检查
 * ========================================================================== */
/**
 * @brief 检查最近一次AT响应是否包含指定字符串
 */
int ESP8266_ResponseContains(const char *str)
{
    if (str == NULL) return 0;
    return (strstr(at_response, str) != NULL) ? 1 : 0;
}

/* ==========================================================================
 * [驱动层] 异步模式等待
 * ========================================================================== */
/**
 * @brief 等待ESP8266异步上报的指定模式 (如 +MQTTCONNECTED)
 * @param pattern    期望包含的字符串
 * @param timeout_ms 超时(ms)
 * @note  不发送AT指令, 仅监听串口接收; 不占用互斥锁
 */
ESP8266_Status ESP8266_WaitForPattern(const char *pattern, uint32_t timeout_ms)
{
    /* 不清空 ring_buf 和 at_response —
     * 异步 URC (如 +MQTTCONNECTED) 可能在 SendCmd 返回后、本函数被调用前
     * 已经到达并存入 ring_buf, 清空会导致丢失. 改为追加模式: 先读已有数据,
     * 匹配不到再轮询等待新数据.
     */
    at_resp_ready  = 0;
    at_resp_result = 0;

    uint32_t waited = 0;
    const uint32_t poll_interval = 100;

    /* 先将 ring_buf 中已有数据追加到 at_response (保留 SendCmd 遗留内容) */
    uint16_t avail = ring_buf_available();
    uint16_t offset = strlen(at_response);
    if (avail > sizeof(at_response) - offset - 1)
        avail = sizeof(at_response) - offset - 1;
    for (uint16_t i = 0; i < avail; i++) {
        at_response[offset + i] = ring_buf_get();
    }
    at_response[offset + avail] = '\0';

    /* 先检查已有数据中是否已包含目标模式 */
    if (strstr(at_response, pattern) != NULL) {
        return ESP8266_OK;
    }

    /* 已有数据不匹配, 进入轮询等待新数据 */
    while (waited < timeout_ms) {
        osDelay(poll_interval);
        waited += poll_interval;

        /* 从环形缓冲区读取最新数据 (追加到 at_response 末尾) */
        avail = ring_buf_available();
        offset = strlen(at_response);
        if (avail > sizeof(at_response) - offset - 1)
            avail = sizeof(at_response) - offset - 1;
        for (uint16_t i = 0; i < avail; i++) {
            at_response[offset + i] = ring_buf_get();
        }
        at_response[offset + avail] = '\0';

        /* 检测期望模式 */
        if (strstr(at_response, pattern) != NULL) {
            return ESP8266_OK;
        }
        /* 检测失败模式 */
        if (strstr(at_response, "FAILED")  != NULL ||
            strstr(at_response, "ERROR")   != NULL ||
            strstr(at_response, "DISCONNECTED") != NULL) {
            return ESP8266_AT_FAIL;
        }
    }

    return ESP8266_TIMEOUT;
}
