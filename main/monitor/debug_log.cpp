/*
 * debug_log.cpp - 调试日志输出实现
 *
 * 日志会同时写入三处：
 * 1. UART0 串口（本地串口调试）
 * 2. UDP 单播到指定上位机（独立 UDP 调试接收端可选）
 * 3. 内部环形缓冲区（供 HTTP /logs 拉取，WebUI 实际使用这一条）
 *
 * UART0: TX=GPIO21, RX=GPIO20, 115200 波特率
 * UDP:   <DEBUG_LOG_IP>:<DEBUG_LOG_PORT>
 *
 * 时序说明：
 *   esp_netif_init() 创建 tcpip 线程后才可安全使用 socket
 *   本模块在首次 send() 时才创建 socket（延迟创建）
 *   WiFi 连接前 sendto() 会失败（无路由），日志仍可写入 UART 与 HTTP 日志缓冲
 *   WiFi 连接后 UDP 立即生效，无需额外操作
 */
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/uart.h"

#include "debug_log.h"

static const char *TAG = "DEBUG_LOG";

// ============================================================
// 单例
// ============================================================
DebugLog& DebugLog::instance()
{
    static DebugLog inst;
    return inst;
}

// ============================================================
// 日志环形缓冲区（供 HTTP /logs 拉取）
// ============================================================
int DebugLog::ringCount()
{
    if (!m_logRingMutex) return 0;
    xSemaphoreTake(m_logRingMutex, portMAX_DELAY);
    int count = m_logRingCount;
    xSemaphoreGive(m_logRingMutex);
    return count;
}

char *DebugLog::ringDrainJson(int *out_len)
{
    if (!m_logRingMutex) {
        *out_len = 0;
        return NULL;
    }

    xSemaphoreTake(m_logRingMutex, portMAX_DELAY);

    int count = m_logRingCount;
    if (count == 0) {
        xSemaphoreGive(m_logRingMutex);
        *out_len = 0;
        return NULL;
    }

    // 估算 JSON 大小：每条 ~280 字节（转义 + JSON 包装）
    size_t buf_size = (size_t)count * 280 + 16;
    char *json = (char *)malloc(buf_size);
    if (!json) {
        xSemaphoreGive(m_logRingMutex);
        *out_len = 0;
        return NULL;
    }

    int pos = 0;
    json[pos++] = '[';

    // 从最旧的开始读
    int start = (count < LOG_RING_SLOTS) ? 0 : m_logRingHead;
    for (int i = 0; i < count && pos < (int)buf_size - 200; i++) {
        int idx = (start + i) % LOG_RING_SLOTS;
        int len = m_logRingLens[idx];
        if (len <= 0) continue;

        if (i > 0) json[pos++] = ',';

        pos += snprintf(json + pos, buf_size - pos, "\"");
        for (int j = 0; j < len && pos < (int)buf_size - 10; j++) {
            char c = m_logRing[idx][j];
            if (c == '"' || c == '\\') {
                json[pos++] = '\\';
                json[pos++] = c;
            } else if (c == '\n') {
                json[pos++] = '\\';
                json[pos++] = 'n';
            } else if (c >= 32) {
                json[pos++] = c;
            }
        }
        pos += snprintf(json + pos, buf_size - pos, "\"");
    }

    json[pos++] = ']';
    json[pos] = '\0';

    // 清空缓冲区
    m_logRingHead = 0;
    m_logRingCount = 0;

    xSemaphoreGive(m_logRingMutex);

    *out_len = pos;
    return json;
}

void DebugLog::ringPush(const char *data, int len)
{
    if (!m_logRingMutex || len <= 0) return;

    xSemaphoreTake(m_logRingMutex, portMAX_DELAY);

    int copy_len = len;
    if (copy_len >= LOG_RING_SIZE) copy_len = LOG_RING_SIZE - 1;

    memcpy(m_logRing[m_logRingHead], data, copy_len);
    m_logRing[m_logRingHead][copy_len] = '\0';
    m_logRingLens[m_logRingHead] = copy_len;

    m_logRingHead = (m_logRingHead + 1) % LOG_RING_SLOTS;
    if (m_logRingCount < LOG_RING_SLOTS) m_logRingCount++;

    xSemaphoreGive(m_logRingMutex);
}

// ============================================================
// UART 调试日志
// ============================================================
esp_err_t DebugLog::uartInit()
{
    if (m_uartInitialized) {
        return ESP_OK;
    }

    const uart_port_t uart_port = UART_NUM_0;
    uart_config_t uart_config = {};
    uart_config.baud_rate = UART_DEBUG_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity    = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t ret = uart_driver_install(uart_port, 1024, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART0 驱动安装失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(uart_port, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART0 参数配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(uart_port, UART_DEBUG_TX_GPIO, UART_DEBUG_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART0 引脚配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    m_uartInitialized = true;
    ESP_LOGI(TAG, "UART0 调试输出已初始化 (TX=GPIO%d, RX=GPIO%d, %d baud)",
             UART_DEBUG_TX_GPIO, UART_DEBUG_RX_GPIO, UART_DEBUG_BAUD_RATE);

    return ESP_OK;
}

esp_err_t DebugLog::uartSend(const uint8_t *data, size_t len)
{
    if (!m_uartInitialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int ret = uart_write_bytes(UART_NUM_0, data, len);
    if (ret < 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

// ============================================================
// 初始化（UART 立即可用，UDP socket 延迟到首次发送时创建）
// ============================================================
esp_err_t DebugLog::init()
{
    if (m_initialized) {
        return ESP_OK;
    }

    uartInit();

    m_logRingMutex = xSemaphoreCreateMutex();

    // 目标地址：单播到上位机
    memset(&m_debugAddr, 0, sizeof(m_debugAddr));
    m_debugAddr.sin_family = AF_INET;
    m_debugAddr.sin_port = htons(DEBUG_LOG_PORT);
    m_debugAddr.sin_addr.s_addr = inet_addr(DEBUG_LOG_IP);

    m_initialized = true;
    ESP_LOGI(TAG, "调试日志已初始化，上位机 %s:%d", DEBUG_LOG_IP, DEBUG_LOG_PORT);

    return ESP_OK;
}

esp_err_t DebugLog::ensureUdpSocket()
{
    if (m_udpSock >= 0) {
        return ESP_OK;
    }

    m_udpSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_udpSock < 0) {
        ESP_LOGE(TAG, "创建 UDP socket 失败");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UDP socket 创建成功");
    return ESP_OK;
}

esp_err_t DebugLog::send(const uint8_t *data, size_t len)
{
    esp_err_t udp_ret = ESP_FAIL;
    esp_err_t uart_ret = ESP_FAIL;

    // UART 通道
    if (m_uartInitialized) {
        uart_ret = uartSend(data, len);
    }

    // UDP 通道（WiFi 未连接时 sendto 失败，静默跳过）
    if (m_initialized) {
        if (m_udpSock < 0) {
            ensureUdpSocket();
        }

        if (m_udpSock >= 0) {
            size_t send_len = len > 1024 ? 1024 : len;
            int ret = ::sendto(m_udpSock, data, send_len, 0,
                               (struct sockaddr *)&m_debugAddr, sizeof(m_debugAddr));
            if (ret >= 0) {
                udp_ret = ESP_OK;
            }
        }
    }

    // 同时写入环形缓冲区（供 HTTP /logs 拉取）
    ringPush((const char *)data, (int)len);

    if (udp_ret == ESP_OK || uart_ret == ESP_OK) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

void DebugLog::log(debug_log_level_t level, const char *tag, const char *fmt, ...)
{
    if (!m_initialized) {
        init();
    }

    if (level > logLevel) {
        return;
    }

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

    va_list args;
    va_start(args, fmt);
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    char log_buf[600];
    int total = snprintf(log_buf, sizeof(log_buf), "%s %s\n", prefix, msg);

    send((const uint8_t *)log_buf, total);
}