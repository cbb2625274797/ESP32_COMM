/*
 * debug_log.c - 调试日志输出实现
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
 *   本模块在首次 debug_log_send() 时才创建 socket（延迟创建）
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

/** @brief 当前调试日志过滤级别，运行时可动态调整。 */
debug_log_level_t g_debug_log_level = DEBUG_LOG_ALL;

/** @brief UDP 调试日志 socket 句柄，首次发送时延迟创建。 */
static int s_udp_sock = -1;
/** @brief UDP 调试日志目标地址。 */
static struct sockaddr_in s_debug_addr;
/** @brief 日志模块初始化标志。 */
static bool s_initialized = false;

// ============================================================
// 日志环形缓冲区（供 HTTP /logs 拉取）
// ============================================================
#define LOG_RING_SLOTS  64
#define LOG_RING_SIZE   256
static char  s_log_ring[LOG_RING_SLOTS][LOG_RING_SIZE];
static int   s_log_ring_lens[LOG_RING_SLOTS];
static int   s_log_ring_head  = 0;  // 下次写入位置
static int   s_log_ring_count = 0;  // 已存条数（超过 LOG_RING_SLOTS 后不再增长，覆盖旧数据）
/** @brief 环形缓冲区互斥锁，保护多线程并发读写。 */
static SemaphoreHandle_t s_log_ring_mutex = NULL;

/**
 * @brief 获取日志缓冲区中的条数
 * @return 当前日志条数。
 */
int debug_log_ring_count(void)
{
    if (!s_log_ring_mutex) return 0;
    xSemaphoreTake(s_log_ring_mutex, portMAX_DELAY);
    int count = s_log_ring_count;
    xSemaphoreGive(s_log_ring_mutex);
    return count;
}

// 读取并清空日志缓冲区，返回 JSON 数组字符串
// 调用者负责 free 返回值
char *debug_log_ring_drain_json(int *out_len)
{
    if (!s_log_ring_mutex) {
        *out_len = 0;
        return NULL;
    }

    xSemaphoreTake(s_log_ring_mutex, portMAX_DELAY);

    int count = s_log_ring_count;
    if (count == 0) {
        xSemaphoreGive(s_log_ring_mutex);
        *out_len = 0;
        return NULL;
    }

    // 估算 JSON 大小：每条 ~280 字节（转义 + JSON 包装）
    size_t buf_size = (size_t)count * 280 + 16;
    char *json = (char *)malloc(buf_size);
    if (!json) {
        xSemaphoreGive(s_log_ring_mutex);
        *out_len = 0;
        return NULL;
    }

    int pos = 0;
    json[pos++] = '[';

    // 从最旧的开始读
    int start = (count < LOG_RING_SLOTS) ? 0 : s_log_ring_head;
    for (int i = 0; i < count && pos < (int)buf_size - 200; i++) {
        int idx = (start + i) % LOG_RING_SLOTS;
        int len = s_log_ring_lens[idx];
        if (len <= 0) continue;

        if (i > 0) json[pos++] = ',';

        pos += snprintf(json + pos, buf_size - pos, "\"");
        for (int j = 0; j < len && pos < (int)buf_size - 10; j++) {
            char c = s_log_ring[idx][j];
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
    s_log_ring_head = 0;
    /** @brief 环形缓冲区中已存储的日志条数。 */
    s_log_ring_count = 0;

    xSemaphoreGive(s_log_ring_mutex);

    *out_len = pos;
    return json;
}

/**
 * @brief 写入一条日志到环形缓冲区
 * @param data
 * @param len
 */
static void log_ring_push(const char *data, int len)
{
    if (!s_log_ring_mutex || len <= 0) return;

    xSemaphoreTake(s_log_ring_mutex, portMAX_DELAY);

    int copy_len = len;
    if (copy_len >= LOG_RING_SIZE) copy_len = LOG_RING_SIZE - 1;

    memcpy(s_log_ring[s_log_ring_head], data, copy_len);
    s_log_ring[s_log_ring_head][copy_len] = '\0';
    s_log_ring_lens[s_log_ring_head] = copy_len;

    /** @brief 环形缓冲区下一个写入位置。 */
    s_log_ring_head = (s_log_ring_head + 1) % LOG_RING_SLOTS;
    if (s_log_ring_count < LOG_RING_SLOTS) s_log_ring_count++;

    xSemaphoreGive(s_log_ring_mutex);
}

// UART0 状态
static bool s_uart_initialized = false;
/** @brief 调试串口使用的 UART 端口号。 */
static uart_port_t s_uart_port = UART_NUM_0;

// ============================================================
// UART0 串口调试
// ============================================================
/**
  * @brief 初始化 UART0 调试串口，配置波特率和引脚映射。
 * @return ESP_OK 成功，其他失败。
 */
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

    /** @brief UART0 初始化标志。 */
    s_uart_initialized = true;
    ESP_LOGI(TAG, "UART0 调试输出已初始化 (TX=GPIO%d, RX=GPIO%d, %d baud)",
             UART_DEBUG_TX_GPIO, UART_DEBUG_RX_GPIO, UART_DEBUG_BAUD_RATE);

    return ESP_OK;
}

/**
  * @brief 通过 UART0 发送一段原始字节数据。
 * @param data 待发送的数据指针。
 * @param len 数据长度。
 * @return ESP_OK 成功，其他失败。
 */
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

/**
 * @brief 初始化（UART 立即可用，UDP socket 延迟到首次发送时创建）
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t debug_log_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    uart_debug_init();

    /** @brief 环形缓冲区互斥锁，保护多线程并发读写。 */
    s_log_ring_mutex = xSemaphoreCreateMutex();

    // 目标地址：单播到上位机
    memset(&s_debug_addr, 0, sizeof(s_debug_addr));
    s_debug_addr.sin_family = AF_INET;
    s_debug_addr.sin_port = htons(DEBUG_LOG_PORT);
    s_debug_addr.sin_addr.s_addr = inet_addr(DEBUG_LOG_IP);

    /** @brief 日志模块初始化标志。 */
    s_initialized = true;
    ESP_LOGI(TAG, "调试日志已初始化，上位机 %s:%d", DEBUG_LOG_IP, DEBUG_LOG_PORT);

    return ESP_OK;
}

/**
 * @brief 延迟创建 UDP socket（首次发送时调用）
 * @return
 */
static esp_err_t ensure_udp_socket(void)
{
    if (s_udp_sock >= 0) {
        return ESP_OK;
    }

    /** @brief UDP 调试日志 socket 句柄，首次发送时延迟创建。 */
    s_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "创建 UDP socket 失败");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UDP socket 创建成功");
    return ESP_OK;
}

/**
 * @brief 发送原始数据（UDP 单播 + UART 双通道）
 * @param data 日志数据指针。
 * @param len 数据长度。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t debug_log_send(const uint8_t *data, size_t len)
{
    esp_err_t udp_ret = ESP_FAIL;
    esp_err_t uart_ret = ESP_FAIL;

    // UART 通道
    if (s_uart_initialized) {
        uart_ret = uart_debug_send(data, len);
    }

    // UDP 通道（WiFi 未连接时 sendto 失败，静默跳过）
    if (s_initialized) {
        if (s_udp_sock < 0) {
            ensure_udp_socket();
        }

        if (s_udp_sock >= 0) {
            size_t send_len = len > 1024 ? 1024 : len;
            int ret = sendto(s_udp_sock, data, send_len, 0,
                             (struct sockaddr *)&s_debug_addr, sizeof(s_debug_addr));
            if (ret >= 0) {
                udp_ret = ESP_OK;
            }
        }
    }

    // 同时写入环形缓冲区（供 HTTP /logs 拉取）
    log_ring_push((const char *)data, (int)len);

    if (udp_ret == ESP_OK || uart_ret == ESP_OK) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

/**
 * @brief 格式化日志输出
 * @param level 日志级别。
 * @param tag 日志标签。
 * @param fmt 格式化字符串。
 */
void debug_log(debug_log_level_t level, const char *tag, const char *fmt, ...)
{
    if (!s_initialized) {
        debug_log_init();
    }

    if (level > g_debug_log_level) {
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

    debug_log_send((const uint8_t *)log_buf, total);
}
