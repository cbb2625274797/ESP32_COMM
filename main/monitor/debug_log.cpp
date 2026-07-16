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
 *   WiFi 未获 IP 时跳过 sendto；日志仍写 UART 与 HTTP ring
 *
 * 线程：仅任务上下文；禁止 ISR 调用 DEBUG_*（内部 portMAX_DELAY mutex）。
 */
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>

#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "driver/uart.h"

#include "debug_log.h"

static const char *TAG = "DEBUG_LOG";

static TickType_t warnIntervalTicks()
{
    return pdMS_TO_TICKS(10000);
}
static constexpr int kUdpSndTimeoutMs = 200;
static constexpr uint8_t kUdpFailRecreate = 8;

// ============================================================
// 单例 / 工具
// ============================================================
DebugLog& DebugLog::instance()
{
    static DebugLog inst;
    return inst;
}

bool DebugLog::isKeepAliveTag(const char *tag)
{
    if (!tag) {
        return false;
    }
    /* OTA / OTA_COORD 等；上传路径 WIFI；总线 SPI */
    if (strncmp(tag, "OTA", 3) == 0) {
        return true;
    }
    if (strcmp(tag, "SPI") == 0 || strcmp(tag, "WIFI") == 0) {
        return true;
    }
    return false;
}

char *DebugLog::allocEmptyJson(int *out_len)
{
    char *empty = (char *)malloc(3);
    if (!empty) {
        return nullptr;
    }
    empty[0] = '[';
    empty[1] = ']';
    empty[2] = '\0';
    if (out_len) {
        *out_len = 2;
    }
    return empty;
}

/**
 * JSON 字符串内容转义写入 buf[pos..]；返回新 pos。
 * 控制字符除 \n\r\t 外写成 \u00XX；空间不足时截断并尽量写 "..."。
 */
int DebugLog::jsonEscapeAppend(char *buf, int pos, int capacity, const char *src, int src_len)
{
    const int content_start = pos;
    const int hard_limit = capacity - 6; /* 预留 "..." + '"' + ']' + '\0' 量级 */
    bool truncated = false;

    for (int i = 0; i < src_len; ++i) {
        if (pos >= hard_limit) {
            truncated = true;
            break;
        }
        /* 单条转义上限，避免异常长行占满 drain */
        if (pos - content_start >= LOG_RING_SIZE * 2) {
            truncated = true;
            break;
        }

        const unsigned char c = (unsigned char)src[i];
        if (c == '"') {
            if (pos + 2 > hard_limit) { truncated = true; break; }
            buf[pos++] = '\\';
            buf[pos++] = '"';
        } else if (c == '\\') {
            if (pos + 2 > hard_limit) { truncated = true; break; }
            buf[pos++] = '\\';
            buf[pos++] = '\\';
        } else if (c == '\n') {
            if (pos + 2 > hard_limit) { truncated = true; break; }
            buf[pos++] = '\\';
            buf[pos++] = 'n';
        } else if (c == '\r') {
            if (pos + 2 > hard_limit) { truncated = true; break; }
            buf[pos++] = '\\';
            buf[pos++] = 'r';
        } else if (c == '\t') {
            if (pos + 2 > hard_limit) { truncated = true; break; }
            buf[pos++] = '\\';
            buf[pos++] = 't';
        } else if (c < 0x20) {
            /* 其它控制符 → \u00XX（6 字符） */
            if (pos + 6 > hard_limit) { truncated = true; break; }
            static const char hex[] = "0123456789abcdef";
            buf[pos++] = '\\';
            buf[pos++] = 'u';
            buf[pos++] = '0';
            buf[pos++] = '0';
            buf[pos++] = hex[(c >> 4) & 0xF];
            buf[pos++] = hex[c & 0xF];
        } else {
            buf[pos++] = (char)c;
        }
    }

    if (truncated && pos + 3 <= hard_limit) {
        buf[pos++] = '.';
        buf[pos++] = '.';
        buf[pos++] = '.';
    }
    return pos;
}

// ============================================================
// 环形缓冲（Locked 变体假定已持 m_logRingMutex）
// ============================================================
int DebugLog::ringCount()
{
    if (!m_logRingMutex) {
        return 0;
    }
    xSemaphoreTake(m_logRingMutex, portMAX_DELAY);
    const int count = m_logRingCount;
    xSemaphoreGive(m_logRingMutex);
    return count;
}

bool DebugLog::ringPopOldestLocked(char *out, size_t out_size, int *out_len)
{
    if (out_len) {
        *out_len = 0;
    }
    if (!out || out_size == 0 || m_logRingCount <= 0) {
        return false;
    }

    const int idx = (m_logRingHead - m_logRingCount + LOG_RING_SLOTS) % LOG_RING_SLOTS;
    int len = m_logRingLens[idx];
    if (len < 0) {
        len = 0;
    }

    int copy_len = len;
    if ((size_t)copy_len >= out_size) {
        copy_len = (int)out_size - 1;
    }

    if (copy_len > 0) {
        memcpy(out, m_logRing[idx], (size_t)copy_len);
    }
    out[copy_len] = '\0';
    if (out_len) {
        *out_len = copy_len;
    }

    m_logRingLens[idx] = 0;
    m_logRingCount--;
    return true;
}

bool DebugLog::ringPopOldest(char *out, size_t out_size, int *out_len)
{
    if (out_len) {
        *out_len = 0;
    }
    if (!m_logRingMutex || !out || out_size == 0) {
        return false;
    }

    xSemaphoreTake(m_logRingMutex, portMAX_DELAY);
    const bool ok = ringPopOldestLocked(out, out_size, out_len);
    xSemaphoreGive(m_logRingMutex);
    return ok;
}

/** 预览即将 drain 的条数与原始字节总和（用于估 capacity）。 */
int DebugLog::ringPeekDrainMetaLocked(int max_n, int *total_raw_len) const
{
    if (total_raw_len) {
        *total_raw_len = 0;
    }
    int n = m_logRingCount;
    if (n > max_n) {
        n = max_n;
    }
    if (n <= 0) {
        return 0;
    }

    int sum = 0;
    for (int i = 0; i < n; ++i) {
        const int idx = (m_logRingHead - m_logRingCount + i + LOG_RING_SLOTS) % LOG_RING_SLOTS;
        int len = m_logRingLens[idx];
        if (len < 0) {
            len = 0;
        }
        if (len >= LOG_RING_SIZE) {
            len = LOG_RING_SIZE - 1;
        }
        sum += len;
    }
    if (total_raw_len) {
        *total_raw_len = sum;
    }
    return n;
}

void DebugLog::ringPushLocked(const char *data, int len)
{
    if (!data || len <= 0) {
        return;
    }

    int copy_len = len;
    if (copy_len >= LOG_RING_SIZE) {
        copy_len = LOG_RING_SIZE - 1;
    }

    memcpy(m_logRing[m_logRingHead], data, (size_t)copy_len);
    m_logRing[m_logRingHead][copy_len] = '\0';
    m_logRingLens[m_logRingHead] = copy_len;

    m_logRingHead = (m_logRingHead + 1) % LOG_RING_SLOTS;
    if (m_logRingCount < LOG_RING_SLOTS) {
        m_logRingCount++;
    }
}

void DebugLog::ringPush(const char *data, int len)
{
    if (!m_logRingMutex || len <= 0) {
        return;
    }
    xSemaphoreTake(m_logRingMutex, portMAX_DELAY);
    ringPushLocked(data, len);
    xSemaphoreGive(m_logRingMutex);
}

/**
 * 单次持锁：peek 元数据 →（可选）按堆缩小条数 → pop+转义拼 JSON。
 * capacity 按真实 raw 长度估算，避免 48×320 虚高导致 OTA 期返空。
 */
char *DebugLog::ringDrainJson(int *out_len)
{
    if (!out_len) {
        return nullptr;
    }
    *out_len = 0;

    const uint32_t free_now = esp_get_free_heap_size();
    if (free_now < kDrainMinFree || !m_logRingMutex) {
        return allocEmptyJson(out_len);
    }

    xSemaphoreTake(m_logRingMutex, portMAX_DELAY);

    int total_raw = 0;
    int drain_n = ringPeekDrainMetaLocked(LOG_DRAIN_MAX, &total_raw);
    if (drain_n <= 0) {
        xSemaphoreGive(m_logRingMutex);
        return allocEmptyJson(out_len);
    }

    /* 转义最坏 ~2x + 每条 "" 与逗号 + 数组括号 */
    auto calc_capacity = [](int n, int raw) -> int {
        int cap = raw * 2 + n * 4 + 16;
        if (cap < 16) {
            cap = 16;
        }
        return cap;
    };

    int capacity = calc_capacity(drain_n, total_raw);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    /* 堆不够则减条数重估（仍持锁，peek 只读；减 n 时从最旧开始仍一致） */
    while (drain_n > 0 && largest < (size_t)capacity) {
        drain_n = drain_n / 2;
        if (drain_n == 0) {
            break;
        }
        total_raw = 0;
        drain_n = ringPeekDrainMetaLocked(drain_n, &total_raw);
        capacity = calc_capacity(drain_n, total_raw);
    }

    if (drain_n <= 0 || largest < (size_t)capacity) {
        xSemaphoreGive(m_logRingMutex);
        return allocEmptyJson(out_len);
    }

    /* 持锁期间尽量短：只 pop 到栈上临时区会爆栈；改为直接写入 malloc 缓冲。
     * 先释放锁再 malloc，再二次持锁 pop——中间可能插入新日志，只 drain 最旧 N 条即可。 */
    xSemaphoreGive(m_logRingMutex);

    char *buf = (char *)malloc((size_t)capacity);
    if (!buf) {
        return allocEmptyJson(out_len);
    }

    xSemaphoreTake(m_logRingMutex, portMAX_DELAY);

    /* 重新核对：ring 可能变少 */
    if (m_logRingCount <= 0) {
        xSemaphoreGive(m_logRingMutex);
        free(buf);
        return allocEmptyJson(out_len);
    }
    if (drain_n > m_logRingCount) {
        drain_n = m_logRingCount;
    }
    if (drain_n > LOG_DRAIN_MAX) {
        drain_n = LOG_DRAIN_MAX;
    }

    int pos = 0;
    buf[pos++] = '[';
    bool first = true;
    char entry[LOG_RING_SIZE];

    for (int k = 0; k < drain_n; ++k) {
        /* 先估空间再 pop，避免 pop 后写不下导致丢条 */
        const int need_min = first ? 4 : 5; /* ,"x" 或 "x" 量级 + 闭合余量 */
        if (pos + need_min >= capacity - 2) {
            break;
        }

        int entry_len = 0;
        if (!ringPopOldestLocked(entry, sizeof(entry), &entry_len) || entry_len <= 0) {
            break;
        }

        if (!first) {
            buf[pos++] = ',';
        }
        first = false;
        buf[pos++] = '"';
        pos = jsonEscapeAppend(buf, pos, capacity, entry, entry_len);
        if (pos < capacity - 1) {
            buf[pos++] = '"';
        } else if (pos < capacity) {
            buf[pos - 1] = '"'; /* 极端：覆盖末字节闭合 */
        }
    }

    if (pos + 1 < capacity) {
        buf[pos++] = ']';
        buf[pos] = '\0';
    } else if (capacity >= 3) {
        buf[capacity - 2] = ']';
        buf[capacity - 1] = '\0';
        pos = capacity - 1;
    }

    xSemaphoreGive(m_logRingMutex);

    *out_len = pos;
    return buf;
}

// ============================================================
// UART
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
        /* 可能 console 已占用 UART0——降级为 ring-only */
        ESP_LOGW(TAG, "UART0 驱动安装失败（可能 console 占用）: %s", esp_err_to_name(ret));
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
    m_uartAvailable = true;
    ESP_LOGD(TAG, "UART0 调试输出已初始化 (TX=GPIO%d, RX=GPIO%d, %d baud)",
             UART_DEBUG_TX_GPIO, UART_DEBUG_RX_GPIO, UART_DEBUG_BAUD_RATE);
    return ESP_OK;
}

esp_err_t DebugLog::uartSend(const uint8_t *data, size_t len)
{
    if (!m_uartInitialized || !m_uartAvailable) {
        return ESP_ERR_INVALID_STATE;
    }

    const int ret = uart_write_bytes(UART_NUM_0, data, len);
    if (ret < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ============================================================
// 初始化 / UDP
// ============================================================
esp_err_t DebugLog::init()
{
    if (m_initialized) {
        return ESP_OK;
    }

    esp_err_t uart_ret = uartInit();
    if (uart_ret != ESP_OK) {
        ESP_LOGW(TAG, "UART 不可用，仅 ring buffer 模式 (err=%s)", esp_err_to_name(uart_ret));
        m_uartAvailable = false;
    }

    m_logRingMutex = xSemaphoreCreateMutex();
    if (!m_logRingMutex) {
        ESP_LOGE(TAG, "ring mutex 创建失败，HTTP /logs 将不可用");
        /* 仍标记 initialized，避免 log() 反复 init；UART 路径可工作 */
        m_initialized = true;
        return ESP_ERR_NO_MEM;
    }

    memset(&m_debugAddr, 0, sizeof(m_debugAddr));
    m_debugAddr.sin_family = AF_INET;
    m_debugAddr.sin_port = htons(DEBUG_LOG_PORT);
    m_debugAddr.sin_addr.s_addr = inet_addr(DEBUG_LOG_IP);

    m_initialized = true;
    ESP_LOGI(TAG, "调试日志已初始化，上位机 %s:%d", DEBUG_LOG_IP, DEBUG_LOG_PORT);
    return ESP_OK;
}

bool DebugLog::udpNetifHasIp() const
{
    esp_netif_ip_info_t info = {};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0) {
        return true;
    }
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap && esp_netif_get_ip_info(ap, &info) == ESP_OK && info.ip.addr != 0) {
        return true;
    }
    return false;
}

void DebugLog::closeUdpSocket()
{
    int s = -1;
    portENTER_CRITICAL(&m_udpMux);
    s = m_udpSock;
    m_udpSock = -1;
    portEXIT_CRITICAL(&m_udpMux);
    if (s >= 0) {
        close(s);
    }
}

esp_err_t DebugLog::ensureUdpSocket()
{
    portENTER_CRITICAL(&m_udpMux);
    if (m_udpSock >= 0) {
        portEXIT_CRITICAL(&m_udpMux);
        return ESP_OK;
    }
    portEXIT_CRITICAL(&m_udpMux);

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        ESP_LOGE(TAG, "创建 UDP socket 失败 errno=%d", errno);
        return ESP_FAIL;
    }

    /* 避免 sendto 在无路由/对端不可达时长时间阻塞日志路径 */
    struct timeval tv = {};
    tv.tv_sec = kUdpSndTimeoutMs / 1000;
    tv.tv_usec = (kUdpSndTimeoutMs % 1000) * 1000;
    (void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    portENTER_CRITICAL(&m_udpMux);
    if (m_udpSock >= 0) {
        /* 竞态：另一任务已建好 */
        portEXIT_CRITICAL(&m_udpMux);
        close(s);
        return ESP_OK;
    }
    m_udpSock = s;
    portEXIT_CRITICAL(&m_udpMux);

    ESP_LOGI(TAG, "UDP socket 创建成功 (SNDTIMEO=%dms)", kUdpSndTimeoutMs);
    return ESP_OK;
}

esp_err_t DebugLog::send(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t udp_ret = ESP_FAIL;
    esp_err_t uart_ret = ESP_FAIL;
    const uint32_t free_now = esp_get_free_heap_size();

    if (m_uartAvailable) {
        uart_ret = uartSend(data, len);
    }

    /* UDP：堆紧 / 无 IP 时跳过 sendto */
    if (m_initialized && free_now >= kSkipUdpFree && udpNetifHasIp()) {
        if (m_udpSock < 0) {
            (void)ensureUdpSocket();
        }

        int sock = -1;
        portENTER_CRITICAL(&m_udpMux);
        sock = m_udpSock;
        portEXIT_CRITICAL(&m_udpMux);

        if (sock >= 0) {
            const size_t send_len = len > 1024 ? 1024 : len;
            const int ret = ::sendto(sock, data, send_len, 0,
                                     (struct sockaddr *)&m_debugAddr, sizeof(m_debugAddr));
            if (ret >= 0) {
                udp_ret = ESP_OK;
                m_udpFailStreak = 0;
            } else {
                if (m_udpFailStreak < 255) {
                    m_udpFailStreak++;
                }
                if (m_udpFailStreak >= kUdpFailRecreate) {
                    closeUdpSocket();
                    m_udpFailStreak = 0;
                    const TickType_t now = xTaskGetTickCount();
                    if (m_lastUdpFailWarnTick == 0 ||
                        (now - m_lastUdpFailWarnTick) >= warnIntervalTicks()) {
                        m_lastUdpFailWarnTick = now;
                        static const char kMsg[] = "[W][LOG] UDP 连续失败，已重建 socket\n";
                        ringPush(kMsg, (int)sizeof(kMsg) - 1);
                    }
                }
            }
        }
    } else if (m_initialized && free_now < kSkipUdpFree) {
        const TickType_t now = xTaskGetTickCount();
        if (m_lastUdpSkipWarnTick == 0 ||
            (now - m_lastUdpSkipWarnTick) >= warnIntervalTicks()) {
            m_lastUdpSkipWarnTick = now;
            static const char kUdpSkipMsg[] = "[W][LOG] UDP 发送已暂停: 堆内存不足\n";
            ringPush(kUdpSkipMsg, (int)sizeof(kUdpSkipMsg) - 1);
        }
    }

    ringPush((const char *)data, (int)len);

    if (udp_ret == ESP_OK || uart_ret == ESP_OK) {
        return ESP_OK;
    }
    if (m_logRingMutex) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

void DebugLog::log(debug_log_level_t level, const char *tag, const char *fmt, ...)
{
    if (!m_initialized) {
        (void)init();
    }

    if (level > m_logLevel) {
        return;
    }

    const uint32_t free_now = esp_get_free_heap_size();
    const bool keep = isKeepAliveTag(tag);

    /*
     * 堆门控：
     * - ERR/WARN：永不因堆丢弃（排障必需）
     * - keep-alive（OTA 前缀 / SPI / WIFI）：INFO 保留；极紧时仍丢 DBG
     * - 其它：free < kDropInfoFree 丢 INFO/DBG
     */
    if (level >= DEBUG_LOG_INFO) {
        if (level >= DEBUG_LOG_DBG && free_now < kDropWarnFree) {
            return;
        }
        if (!keep) {
            if (free_now < kDropInfoFree) {
                return;
            }
        } else if (level >= DEBUG_LOG_DBG && free_now < kDropInfoFree) {
            /* keep-alive DBG：中等堆紧也丢，减刷屏 */
            return;
        }
    }

    const char *level_str;
    switch (level) {
        case DEBUG_LOG_ERR:  level_str = "E"; break;
        case DEBUG_LOG_WARN: level_str = "W"; break;
        case DEBUG_LOG_INFO: level_str = "I"; break;
        case DEBUG_LOG_DBG:  level_str = "D"; break;
        default:             level_str = "?"; break;
    }

    /* 单缓冲：与 LOG_RING_SIZE 对齐，避免 msg[]+log_buf[] 双栈 */
    char log_buf[LOG_RING_SIZE];
    const char *safe_tag = tag ? tag : "?";
    int prefix_len = snprintf(log_buf, sizeof(log_buf), "[%s][%s] ", level_str, safe_tag);
    if (prefix_len < 0) {
        return;
    }
    if ((size_t)prefix_len >= sizeof(log_buf)) {
        /* 前缀已截断：保证可发送的以 \\n 结尾的串 */
        log_buf[sizeof(log_buf) - 2] = '\n';
        log_buf[sizeof(log_buf) - 1] = '\0';
        (void)send((const uint8_t *)log_buf, sizeof(log_buf) - 1);
        return;
    }

    va_list args;
    va_start(args, fmt);
    int body = vsnprintf(log_buf + prefix_len, sizeof(log_buf) - (size_t)prefix_len, fmt, args);
    va_end(args);
    if (body < 0) {
        return;
    }

    size_t used = (size_t)prefix_len + (size_t)body;
    if (used >= sizeof(log_buf)) {
        used = sizeof(log_buf) - 1;
        log_buf[used] = '\0';
    }

    /* 保证换行结尾（截断时覆盖末字节） */
    if (used == 0 || log_buf[used - 1] != '\n') {
        if (used + 1 < sizeof(log_buf)) {
            log_buf[used++] = '\n';
            log_buf[used] = '\0';
        } else {
            log_buf[sizeof(log_buf) - 2] = '\n';
            log_buf[sizeof(log_buf) - 1] = '\0';
            used = sizeof(log_buf) - 1;
        }
    }

    (void)send((const uint8_t *)log_buf, used);
}
