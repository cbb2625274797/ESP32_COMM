/*
 * debug_log.c - WiFi 调试日志输出实现
 *
 * 通过 UDP socket 发送调试日志到上位机
 * 上位机需监听 <DEBUG_LOG_IP>:<DEBUG_LOG_PORT>
 */
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "debug_log.h"

static const char *TAG = "DEBUG_LOG";

debug_log_level_t g_debug_log_level = DEBUG_LOG_ALL;

static int s_udp_sock = -1;
static struct sockaddr_in s_debug_addr;
static bool s_initialized = false;

// 初始化 UDP socket
esp_err_t debug_log_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // 创建 UDP socket
    s_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "创建 UDP socket 失败");
        return ESP_FAIL;
    }

    // 设置目标地址
    memset(&s_debug_addr, 0, sizeof(s_debug_addr));
    s_debug_addr.sin_family = AF_INET;
    s_debug_addr.sin_port = htons(DEBUG_LOG_PORT);
    s_debug_addr.sin_addr.s_addr = inet_addr(DEBUG_LOG_IP);

    s_initialized = true;
    ESP_LOGI(TAG, "调试日志已初始化，上位机 IP=%s:%d", DEBUG_LOG_IP, DEBUG_LOG_PORT);

    return ESP_OK;
}

// 发送原始数据到上位机
esp_err_t debug_log_send(const uint8_t *data, size_t len)
{
    if (!s_initialized || s_udp_sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (len > 1024) {
        len = 1024;  // 限制单次发送大小
    }

    int ret = sendto(s_udp_sock, data, len, 0,
                     (struct sockaddr *)&s_debug_addr, sizeof(s_debug_addr));

    if (ret < 0) {
        // 静默失败，避免影响正常流程
        return ESP_FAIL;
    }

    return ESP_OK;
}

// 格式化日志输出
void debug_log(debug_log_level_t level, const char *tag, const char *fmt, ...)
{
    if (!s_initialized) {
        debug_log_init();
    }

    if (level > g_debug_log_level) {
        return;
    }

    // 格式化日志头
    char prefix[32];
    const char *level_str;
    switch (level) {
        case DEBUG_LOG_ERR:  level_str = "E"; break;
        case DEBUG_LOG_WARN: level_str = "W"; break;
        case DEBUG_LOG_INFO: level_str = "I"; break;
        case DEBUG_LOG_DBG:  level_str = "D"; break;
        default:             level_str = "?"; break;
    }
    snprintf(prefix, sizeof(prefix), "[%s][%s]", level_str, tag);

    // 格式化消息体
    va_list args;
    va_start(args, fmt);
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    // 组装完整日志帧
    char log_buf[600];
    int total = snprintf(log_buf, sizeof(log_buf), "%s %s\n", prefix, msg);

    // 发送（末尾 \n 会被保留）
    debug_log_send((const uint8_t *)log_buf, total);
}