/*
 * debug_log.h - WiFi 调试日志输出
 *
 * 通过 UDP socket 将调试信息发送到上位机
 * 上位机监听 <udp_ip>/<udp_port> 接收日志
 */
#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdint.h>
#include <stdarg.h>
#include "esp_err.h"
#include "app_config.h"

// 调试日志配置（DEBUG_LOG_IP/PORT/ENABLED 已在 app_config.h 中定义）

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

// 初始化调试日志系统（启动 UDP 客户端）
esp_err_t debug_log_init(void);

// 发送原始数据到上位机
esp_err_t debug_log_send(const uint8_t *data, size_t len);

// 格式化日志输出（类似 printf）
void debug_log(debug_log_level_t level, const char *tag, const char *fmt, ...);

// 宏定义方便调用
#define DEBUG_ERR(tag, fmt, ...)   debug_log(DEBUG_LOG_ERR,   tag, fmt, ##__VA_ARGS__)
#define DEBUG_WARN(tag, fmt, ...)  debug_log(DEBUG_LOG_WARN,  tag, fmt, ##__VA_ARGS__)
#define DEBUG_INFO(tag, fmt, ...)   debug_log(DEBUG_LOG_INFO,  tag, fmt, ##__VA_ARGS__)
#define DEBUG_DBG(tag, fmt, ...)   debug_log(DEBUG_LOG_DBG,   tag, fmt, ##__VA_ARGS__)

#endif // DEBUG_LOG_H