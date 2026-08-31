/*
 * debug_log.cpp - 调试日志输出实现
 *
 * 日志会同时写入三处：
 * 1. UART0 串口（本地串口调试）
 * 2. UDP 单播到指定上位机（独立 UDP 调试接收端可选）
 * 3. 内部环形缓冲区（供 HTTP /logs 拉取，WebUI 实际使用这一条）
 *
 * UART0: TX=GPIO21, RX=GPIO20, 3000000 波特率
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
#include <stdlib.h>
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
// 环形缓冲（堆上；Locked 变体假定已持 m_logRingMutex）
// ============================================================
bool DebugLog::allocRing(int slots)
{
    if (slots <= 0) {
        return false;
    }
    char *rows = static_cast<char *>(calloc((size_t)slots, (size_t)LOG_RING_SIZE));
    int *lens = static_cast<int *>(calloc((size_t)slots, sizeof(int)));
    if (!rows || !lens) {
        free(rows);
        free(lens);
        return false;
    }
    free(m_logRing);
    free(m_logRingLens);
    m_logRing = rows;
    m_logRingLens = lens;
    m_logRingSlots = slots;
    m_logRingHead = 0;
    m_logRingCount = 0;
    return true;
}

void DebugLog::shrinkRingAfterBoot()
{
    if (m_logRingShrunk || !m_logRingMutex) {
        return;
    }
    if (m_logRingSlots > 0 && m_logRingSlots <= LOG_RING_SLOTS_STEADY) {
        m_logRingShrunk = true;
        return;
    }

    xSemaphoreTake(m_logRingMutex, portMAX_DELAY);

    const int old_slots = m_logRingSlots;
    const int old_count = m_logRingCount;
    const int old_head = m_logRingHead;
    char *old_ring = m_logRing;
    int *old_lens = m_logRingLens;

    const int new_slots = LOG_RING_SLOTS_STEADY;
    char *rows = static_cast<char *>(calloc((size_t)new_slots, (size_t)LOG_RING_SIZE));
    int *lens = static_cast<int *>(calloc((size_t)new_slots, sizeof(int)));
    if (!rows || !lens) {
        free(rows);
        free(lens);
        xSemaphoreGive(m_logRingMutex);
        ESP_LOGW(TAG, "log ring shrink failed, keep boot ring (%d slots)", old_slots);
        return;
    }

    /* 保留时间序上最新的 min(count, new_slots) 条 */
    int keep = old_count;
    if (keep > new_slots) {
        keep = new_slots;
    }
    int new_head = 0;
    if (old_ring && old_lens && keep > 0 && old_slots > 0) {
        for (int i = 0; i < keep; ++i) {
            /* 从最旧保留起点：跳过被挤掉的旧条 */
            const int src_i = old_count - keep + i;
            const int src = (old_head - old_count + src_i + old_slots * 4) % old_slots;
            int len = old_lens[src];
            if (len < 0) {
                len = 0;
            }
            if (len >= LOG_RING_SIZE) {
                len = LOG_RING_SIZE - 1;
            }
            char *dst = rows + (size_t)new_head * (size_t)LOG_RING_SIZE;
            if (len > 0) {
                memcpy(dst, old_ring + (size_t)src * (size_t)LOG_RING_SIZE, (size_t)len);
            }
            dst[len] = '\0';
            lens[new_head] = len;
            new_head = (new_head + 1) % new_slots;
        }
    }

    m_logRing = rows;
    m_logRingLens = lens;
    m_logRingSlots = new_slots;
    m_logRingCount = keep;
    m_logRingHead = (keep >= new_slots) ? new_head : keep;
    m_logRingShrunk = true;

    xSemaphoreGive(m_logRingMutex);

    free(old_ring);
    free(old_lens);

    ESP_LOGI(TAG, "log ring shrunk: %d → %d slots (free ~%d B)",
             old_slots, new_slots,
             (old_slots - new_slots) * (LOG_RING_SIZE + (int)sizeof(int)));
}

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
    if (!out || out_size == 0 || m_logRingCount <= 0 || !m_logRing || m_logRingSlots <= 0) {
        return false;
    }

    const int idx = (m_logRingHead - m_logRingCount + m_logRingSlots) % m_logRingSlots;
    int len = m_logRingLens[idx];
    if (len < 0) {
        len = 0;
    }

    int copy_len = len;
    if ((size_t)copy_len >= out_size) {
        copy_len = (int)out_size - 1;
    }

    if (copy_len > 0) {
        memcpy(out, ringSlot(idx), (size_t)copy_len);
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
    if (!m_logRing || m_logRingSlots <= 0) {
        return 0;
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
        const int idx = (m_logRingHead - m_logRingCount + i + m_logRingSlots) % m_logRingSlots;
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
    if (!data || len <= 0 || !m_logRing || !m_logRingLens || m_logRingSlots <= 0) {
        return;
    }

    int copy_len = len;
    if (copy_len >= LOG_RING_SIZE) {
        copy_len = LOG_RING_SIZE - 1;
    }

    /* 满环时优先覆盖 INFO/DBG，保护 ERR/WARN。 */
    if (m_logRingCount >= m_logRingSlots) {
        int victim_idx = m_logRingHead;
        for (int i = 0; i < m_logRingSlots; ++i) {
            const int idx = (m_logRingHead + i) % m_logRingSlots;
            const char *s = ringSlot(idx);
            if (s[0] == '[' && (s[1] == 'I' || s[1] == 'D')) {
                victim_idx = idx;
                break;
            }
        }
        m_logRingHead = victim_idx;
    }

    char *slot = ringSlot(m_logRingHead);
    memcpy(slot, data, (size_t)copy_len);
    slot[copy_len] = '\0';
    m_logRingLens[m_logRingHead] = copy_len;

    m_logRingHead = (m_logRingHead + 1) % m_logRingSlots;
    if (m_logRingCount < m_logRingSlots) {
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
 * 流式导出：peek 快照条数 → 逐条 pop（仅 pop 期间持锁）转义拼 JSON →
 * 攒满 512B 栈缓冲即经 sink 发一个 chunk。替代原 ~4KB 一次性 malloc，
 * 把 /logs 发送路径的应用层瞬时峰值从几 KB 降到栈缓冲级别。
 */
int DebugLog::ringDrainJsonStream(LogJsonSink sink, void *ctx)
{
    if (!sink) {
        return 0;
    }

    const uint32_t free_now = esp_get_free_heap_size();
    if (free_now < kDrainMinFree || !m_logRingMutex) {
        return 0;  /* 低堆/未就绪：caller 一次性发 "[]" */
    }

    /* 先 peek 快照条数：无内容则让 caller 走 "[]" 一次性响应，不进 chunked 编码。 */
    xSemaphoreTake(m_logRingMutex, portMAX_DELAY);
    int total_raw = 0;
    int drain_n = ringPeekDrainMetaLocked(LOG_DRAIN_MAX, &total_raw);
    xSemaphoreGive(m_logRingMutex);
    if (drain_n <= 0) {
        return 0;
    }

    /* 512B 栈缓冲替代原 ~4KB malloc。单条转义最坏 2×LOG_RING_SIZE，
     * flush 后空缓冲须能容纳单条 + 括号余量（见 static_assert）。 */
    static constexpr int STREAM_BUF_CAP = 512;
    static_assert(STREAM_BUF_CAP >= LOG_RING_SIZE * 2 + 8,
                  "stream buf must hold one worst-case escaped entry");
    char buf[STREAM_BUF_CAP];
    char entry[LOG_RING_SIZE];

    int pos = 0;
    buf[pos++] = '[';
    bool first = true;

    for (int k = 0; k < drain_n; ++k) {
        /* 逐条 pop：仅在 pop 期间持锁，绝不在 sink（网络 I/O）时持锁，
         * 避免阻塞其它任务写日志。 */
        int entry_len = 0;
        xSemaphoreTake(m_logRingMutex, portMAX_DELAY);
        bool popped = ringPopOldestLocked(entry, sizeof(entry), &entry_len) && entry_len > 0;
        xSemaphoreGive(m_logRingMutex);
        if (!popped) {
            break;  /* 环已空（并发被别处 drain） */
        }

        /* 预留单条转义最坏占用再决定是否 flush：jsonEscapeAppend 内部把单条封顶在
         * LOG_RING_SIZE*2（控制符 \u00XX 会 6× 膨胀），故按该上限 + 逗号/引号/收尾预留，
         * 与 entry_len 无关；放不下先 flush，flush 后空缓冲必能完整容纳单条不被截断。 */
        const int need = LOG_RING_SIZE * 2 + 8;
        if (pos + need > STREAM_BUF_CAP) {
            if (!sink(ctx, buf, pos)) {
                return -1;
            }
            pos = 0;
        }

        if (!first) {
            buf[pos++] = ',';
        }
        first = false;
        buf[pos++] = '"';
        pos = jsonEscapeAppend(buf, pos, STREAM_BUF_CAP, entry, entry_len);
        if (pos < STREAM_BUF_CAP - 1) {
            buf[pos++] = '"';
        } else {
            buf[pos - 1] = '"'; /* 极端兜底：覆盖末字节闭合 */
        }
    }

    /* 收尾 ']'：缓冲放不下先 flush，再单独发闭合。 */
    if (pos + 1 > STREAM_BUF_CAP) {
        if (!sink(ctx, buf, pos)) {
            return -1;
        }
        pos = 0;
    }
    buf[pos++] = ']';
    if (!sink(ctx, buf, pos)) {
        return -1;
    }
    return 1;
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

// ============================================================
// UART0 RX 泵：把 RX(GPIO20) 收到的串口行转发进 ring buffer，
// 经 HTTP /logs 出现在网页日志面板（tag=UART）。
// 接线：H1/H2 排针上任意串口源 TX → ESP_RX(GPIO20)，3M 8N1。
// ============================================================
static void uartRxPumpTask(void *arg)
{
    (void)arg;
    uint8_t chunk[64];
    char line[128];
    int pos = 0;

    for (;;) {
        const int n = uart_read_bytes(UART_NUM_0, chunk, sizeof(chunk), portMAX_DELAY);
        for (int i = 0; i < n; i++) {
            const char c = (char)chunk[i];
            if (c == '\n' || pos >= (int)sizeof(line) - 1) {
                if (pos > 0) {
                    line[pos] = '\0';
                    DebugLog::instance().log(DEBUG_LOG_INFO, "UART", "%s", line);
                    pos = 0;
                }
            } else if (c != '\r') {
                line[pos++] = c;
            }
        }
    }
}

esp_err_t DebugLog::uartSend(const uint8_t *data, size_t len)
{
    if (!m_uartInitialized || !m_uartAvailable) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 非阻塞直写 FIFO：满了丢弃（日志必进 ring，web /logs 仍完整）。
     * 旧实现 uart_write_bytes 在 TX buffer=0 时同步阻塞到整行发完
     * （115200 下 ~5-8ms/条），OTA 期间日志密集会把 ota_relay(prio 23)
     * / spi_worker(prio 6) 阻塞数 ms~数十 ms——50ms ACK 窗口被拖爆，
     * 表现为改短重试后超时重发频繁（线上 12/777）。 */
    const int ret = uart_tx_chars(UART_NUM_0, (const char *)data, (int)len);
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

    if (!allocRing(LOG_RING_SLOTS_BOOT)) {
        ESP_LOGE(TAG, "boot log ring alloc 失败 (%d slots)，/logs 不可用", LOG_RING_SLOTS_BOOT);
        /* UART/UDP 仍可用 */
    } else {
        ESP_LOGI(TAG, "boot log ring %d×%d (~%d B heap)",
                 LOG_RING_SLOTS_BOOT, LOG_RING_SIZE,
                 LOG_RING_SLOTS_BOOT * (LOG_RING_SIZE + (int)sizeof(int)));
    }

    memset(&m_debugAddr, 0, sizeof(m_debugAddr));
    m_debugAddr.sin_family = AF_INET;
    m_debugAddr.sin_port = htons(DEBUG_LOG_PORT);
    m_debugAddr.sin_addr.s_addr = inet_addr(DEBUG_LOG_IP);

    /* UART0 RX 泵：串口行 → ring → /logs 网页面板 */
    if (m_uartAvailable) {
        if (xTaskCreate(uartRxPumpTask, "uart_rx", 3072, nullptr, 1, nullptr) == pdPASS) {
            ESP_LOGI(TAG, "UART0 RX 泵已启动 (GPIO%d → /logs)", UART_DEBUG_RX_GPIO);
        } else {
            ESP_LOGW(TAG, "UART0 RX 泵任务创建失败（堆不足？）");
        }
    }

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
            /* MSG_DONTWAIT：非阻塞发送。lwIP 在 ARP 未解析/路由未就绪时
             * sendto 会排队等待（可达 SO_SNDTIMEO 200ms），会阻塞打日志的
             * 业务任务（OTA relay/worker）——日志转发必须尽力而为。 */
            const int ret = ::sendto(sock, data, send_len, MSG_DONTWAIT,
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
