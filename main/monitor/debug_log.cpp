/*
 * debug_log.c - WiFi 调试日志输出实现
 *
 * 通过 UDP socket 发送调试日志到上位机
 * 上位机需监听 <DEBUG_LOG_IP>:<DEBUG_LOG_PORT>
 * 
 * 支持双通道输出：UDP + UART0 串口
 * UART0: TX=GPIO21, RX=GPIO20, 115200 波特率
 */
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/uart.h"

#include "debug_log.h"

static const char *TAG = "DEBUG_LOG";

debug_log_level_t g_debug_log_level = DEBUG_LOG_ALL;

static int s_udp_sock = -1;
static struct sockaddr_in s_debug_addr;
static bool s_initialized = false;

// UART0 状态
static bool s_uart_initialized = false;
static uart_port_t s_uart_port = UART_NUM_0;

// ============================================================
// UART0 串口调试
// ============================================================

esp_err_t uart_debug_init(void)
{
    if (s_uart_initialized) {
        return ESP_OK;
    }

    uart_config_t uart_config;
    memset(&uart_config, 0, sizeof(uart_config));
    uart_config.baud_rate = UART_DEBUG_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;
    uart_config.rx_flow_ctrl_thresh = 0;

    esp_err_t ret = uart_driver_install(s_uart_port, 1024, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART0 驱动安装失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(s_uart_port, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART0 参数配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(s_uart_port, UART_DEBUG_TX_GPIO, UART_DEBUG_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART0 引脚配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_uart_initialized = true;
    ESP_LOGI(TAG, "UART0 调试输出已初始化 (TX=GPIO%d, RX=GPIO%d, %d baud)",
             UART_DEBUG_TX_GPIO, UART_DEBUG_RX_GPIO, UART_DEBUG_BAUD_RATE);

    return ESP_OK;
}

esp_err_t uart_debug_send(const uint8_t *data, size_t len)
{
    if (!s_uart_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int ret = uart_write_bytes(s_uart_port, data, len);
    if (ret < 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

// ============================================================
// UDP 调试日志
// ============================================================

// 初始化 UDP socket（注意：socket 创建延迟到 debug_log_send 首次调用时）
// 因为 esp_netif_init() 会在 wifi_ota_task 里调用，提前创建 socket 会导致 tcpip 线程未就绪而崩溃
esp_err_t debug_log_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // 初始化 UART0（不需要网络）
    uart_debug_init();

    // 延迟创建 socket：先只初始化目标地址，socket 在 debug_log_send 首次发 UDP 时创建
    memset(&s_debug_addr, 0, sizeof(s_debug_addr));
    s_debug_addr.sin_family = AF_INET;
    s_debug_addr.sin_port = htons(DEBUG_LOG_PORT);
    s_debug_addr.sin_addr.s_addr = inet_addr(DEBUG_LOG_IP);

    s_initialized = true;
    ESP_LOGI(TAG, "调试日志已初始化（UDP socket 延迟创建），上位机 IP=%s:%d", DEBUG_LOG_IP, DEBUG_LOG_PORT);

    return ESP_OK;
}

// 延迟创建 UDP socket（仅在首次发送 UDP 时调用）
static esp_err_t ensure_udp_socket(void)
{
    if (s_udp_sock >= 0) {
        return ESP_OK;  // 已创建
    }

    // 创建 UDP socket
    s_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "创建 UDP socket 失败");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UDP socket 创建成功");
    return ESP_OK;
}

// 发送原始数据到上位机（UDP + UART 双通道）
esp_err_t debug_log_send(const uint8_t *data, size_t len)
{
    esp_err_t udp_ret = ESP_FAIL;
    esp_err_t uart_ret = ESP_FAIL;

    // UART 通道发送
    if (s_uart_initialized) {
        uart_ret = uart_debug_send(data, len);
    }

    // UDP 通道发送（延迟创建 socket）
    if (s_initialized) {
        // 首次 UDP 发送前才创建 socket
        if (s_udp_sock < 0) {
            if (ensure_udp_socket() != ESP_OK) {
                // socket 创建失败，走 UART 通道
            } else {
                udp_ret = ESP_OK;  // 标记为已就绪
            }
        }

        if (s_udp_sock >= 0) {
            if (len > 1024) {
                len = 1024;  // 限制单次发送大小
            }

            int ret = sendto(s_udp_sock, data, len, 0,
                             (struct sockaddr *)&s_debug_addr, sizeof(s_debug_addr));

            if (ret < 0) {
                udp_ret = ESP_FAIL;
            } else {
                udp_ret = ESP_OK;
            }
        }
    }

    // 至少一个通道成功即返回成功
    if (udp_ret == ESP_OK || uart_ret == ESP_OK) {
        return ESP_OK;
    }

    return ESP_FAIL;
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