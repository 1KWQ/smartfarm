#include "bsp_esp8266.h"

/* ==========================================================================
 * [驱动层] 私有变量
 * ========================================================================== */
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
static void ESP8266_HardReset(void){
    
}