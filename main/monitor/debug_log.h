/*
 * debug_log.h - 调试日志输出
 *
 * 当前日志会并行写入：
 * - UART0 串口
 * - UDP 单播（独立调试接收端可选）
 * - 环形缓冲区，供 HTTP /logs 拉取
 *
 * 注意：当前 tools/WebUI 日志面板走的是 HTTP /logs，
 * 不是直接监听 UDP 6666。
 * 
 * UART0: TX=GPIO21, RX=GPIO20, 115200 波特率
 */
#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdint.h>
#include <stdarg.h>
#include "esp_err.h"
#include "app_config.h"

// 调试日志配置（DEBUG_LOG_IP/PORT/ENABLED 已在 app_config.h 中定义）

// UART0 调试配置
#define UART_DEBUG_TX_GPIO   21
#define UART_DEBUG_RX_GPIO   20
#define UART_DEBUG_BAUD_RATE 115200

// 日志级别
typedef enum {
    DEBUG_LOG_NONE  = 0,
    DEBUG_LOG_ERR   = 1,    // 错误
    DEBUG_LOG_WARN  = 2,    // 警告
    DEBUG_LOG_INFO  = 3,    // 信息
    DEBUG_LOG_DBG   = 4,    // 调试
    DEBUG_LOG_ALL   = 5     // 全部
} debug_log_level_t;

// 当前日志级别（运行时可改）
extern debug_log_level_t g_debug_log_level;

/**
 * @brief 初始化日志系统，包括 UART 输出、UDP 目标和内部缓冲区。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t debug_log_init(void);

/**
 * @brief 初始化 UART0 调试串口。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t uart_debug_init(void);

/**
 * @brief 通过 UART0 直接发送一段原始字节数据。
 * @param data 待发送的数据指针。
 * @param len 数据长度。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t uart_debug_send(const uint8_t *data, size_t len);

/**
 * @brief 发送原始日志数据到底层输出通道。
 * @param data 日志数据指针。
 * @param len 数据长度。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t debug_log_send(const uint8_t *data, size_t len);

/**
 * @brief 按日志级别与标签格式化输出一条日志。
 * @param level 日志级别。
 * @param tag 日志标签。
 * @param fmt 格式化字符串。
 */
void debug_log(debug_log_level_t level, const char *tag, const char *fmt, ...);

/**
 * @brief 获取当前环形日志缓冲区中的日志条数。
 * @return 当前日志条数。
 */
int debug_log_ring_count(void);

// 读取并清空环形日志缓冲区，返回 JSON 数组字符串，需由调用者 free。
char *debug_log_ring_drain_json(int *out_len);  // 调用者 free

#define DEBUG_ERR(tag, fmt, ...)   debug_log(DEBUG_LOG_ERR,   tag, fmt, ##__VA_ARGS__)
#define DEBUG_WARN(tag, fmt, ...)  debug_log(DEBUG_LOG_WARN,  tag, fmt, ##__VA_ARGS__)
#define DEBUG_INFO(tag, fmt, ...)   debug_log(DEBUG_LOG_INFO,  tag, fmt, ##__VA_ARGS__)
#define DEBUG_DBG(tag, fmt, ...)   debug_log(DEBUG_LOG_DBG,   tag, fmt, ##__VA_ARGS__)

#endif // DEBUG_LOG_H