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
 *
 * 线程模型：DEBUG_* / log() / send() 仅允许在任务上下文调用
 * （内部使用 portMAX_DELAY 互斥锁，禁止 ISR）。
 */
#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdint.h>
#include <stdarg.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app_config.h"
#include <sys/socket.h>
#include <netinet/in.h>

// 调试日志配置（DEBUG_LOG_IP/PORT 在 app_config.h）

// UART0 调试配置
#define UART_DEBUG_TX_GPIO   21     ///< UART0 TX GPIO 引脚号
#define UART_DEBUG_RX_GPIO   20     ///< UART0 RX GPIO 引脚号
#define UART_DEBUG_BAUD_RATE 115200 ///< UART0 波特率

// 日志级别
typedef enum {
    DEBUG_LOG_ERR   = 1,    // 错误
    DEBUG_LOG_WARN  = 2,    // 警告
    DEBUG_LOG_INFO  = 3,    // 信息
    DEBUG_LOG_DBG   = 4     // 调试
} debug_log_level_t;

/**
 * @brief 调试日志类（单例）
 *
 * 封装 UART0、UDP 和环形缓冲区三路日志输出通道，
 * 所有内部状态（socket、缓冲区、互斥锁等）均为私有成员。
 */
class DebugLog {
public:
    static DebugLog& instance();

    /** @brief 初始化日志系统（UART + 开机大环；UDP socket 延迟创建）。 */
    esp_err_t init();

    /**
     * @brief 启动 burst 结束后把环从开机容量缩到稳态小环，释放大块堆。
     *        幂等；失败则保留大环。宜在 WiFi/HTTP 就绪后、日志变少时调用。
     */
    void shrinkRingAfterBoot();

    /** @brief 初始化 UART0 调试串口。 */
    esp_err_t uartInit();

    /** @brief 通过 UART0 直接发送一段原始字节数据。 */
    esp_err_t uartSend(const uint8_t *data, size_t len);

    /** @brief 发送原始日志数据到底层输出通道（任务上下文）。 */
    esp_err_t send(const uint8_t *data, size_t len);

    /** @brief 按日志级别与标签格式化输出一条日志（任务上下文）。 */
    void log(debug_log_level_t level, const char *tag, const char *fmt, ...);

    /** @brief 获取当前环形日志缓冲区中的日志条数。 */
    int ringCount();

    /** @brief 弹出一条最旧日志并从环形缓冲区移除。 */
    bool ringPopOldest(char *out, size_t out_size, int *out_len);

    /**
     * @brief 将环形缓冲区日志导出为 JSON 数组字符串。
     * @param out_len 输出 JSON 字符串长度。
     * @return 动态分配的 JSON 字符串，调用者须 free()；失败返回 NULL。
     */
    char* ringDrainJson(int *out_len);

    void setLogLevel(debug_log_level_t level) { m_logLevel = level; }
    debug_log_level_t getLogLevel() const { return m_logLevel; }

private:
    DebugLog() = default;
    ~DebugLog() = default;
    DebugLog(const DebugLog&) = delete;
    DebugLog& operator=(const DebugLog&) = delete;

    int m_udpSock = -1;
    struct sockaddr_in m_debugAddr = {};
    bool m_initialized = false;
    bool m_uartInitialized = false;
    /** uartInit 成功后才为 true；send() 据此跳过 uart_write_bytes */
    bool m_uartAvailable = false;
    debug_log_level_t m_logLevel = DEBUG_LOG_INFO;

    /* 环形日志（堆上分配，非 .data）：
     *  开机 LOG_RING_SLOTS_BOOT×128 容纳启动刷屏；
     *  shrinkRingAfterBoot() 后改为 LOG_RING_SLOTS_STEADY×128，释放大环。
     *  业务 DEBUG_* 目标：整行（[L][TAG]+正文+\\n）≤128B；槽 128 含 \\0 → 有效≤127。 */
    static constexpr int LOG_RING_SLOTS_BOOT    = 64;   ///< 开机 burst：~8.4KB 堆
    static constexpr int LOG_RING_SLOTS_STEADY  = 16;   ///< 稳态：~2.1KB 堆（对齐 drain）
    static constexpr int LOG_RING_SIZE         = 128;  ///< 单条槽（含 \\0）；有效≤127（含换行）
    static constexpr int LOG_DRAIN_MAX         = 16;   ///< 单次 /logs 最多条数
    char *m_logRing = nullptr;       ///< [slots * LOG_RING_SIZE] 扁平行
    int  *m_logRingLens = nullptr;   ///< [slots]
    int   m_logRingSlots = 0;        ///< 当前容量
    int   m_logRingHead  = 0;
    int   m_logRingCount = 0;
    bool  m_logRingShrunk = false;
    SemaphoreHandle_t m_logRingMutex = nullptr;

    char *ringSlot(int idx) { return m_logRing + (size_t)idx * (size_t)LOG_RING_SIZE; }
    const char *ringSlot(int idx) const {
        return m_logRing + (size_t)idx * (size_t)LOG_RING_SIZE;
    }
    bool allocRing(int slots);

    /* 堆门控（防 OTA 时 UDP/格式化抢 LWIP 堆）：
     * ERR/WARN 永不因堆门控丢弃
     * free < kDropInfoFree → 非 keep-alive 丢 INFO/DBG；keep-alive 保留 INFO
     * free < kDropWarnFree → 全局丢 DBG
     * free < kSkipUdpFree  → 不 sendto（仍 UART+ring）
     * free < kDrainMinFree → /logs 返 "[]" 避免大块 malloc
     * keep-alive tag：OTA 前缀、SPI、WIFI */
    static constexpr uint32_t kDropWarnFree = 6U * 1024U;
    static constexpr uint32_t kDropInfoFree = 10U * 1024U;
    static constexpr uint32_t kSkipUdpFree  = 6U * 1024U;
    static constexpr uint32_t kDrainMinFree = 4U * 1024U;

    /** UDP 跳过/失败告警节流与 socket 重建 */
    TickType_t m_lastUdpSkipWarnTick = 0;
    TickType_t m_lastUdpFailWarnTick = 0;
    uint8_t m_udpFailStreak = 0;
    portMUX_TYPE m_udpMux = portMUX_INITIALIZER_UNLOCKED;

    esp_err_t ensureUdpSocket();
    void closeUdpSocket();
    bool udpNetifHasIp() const;
    void ringPush(const char *data, int len);
    void ringPushLocked(const char *data, int len);
    bool ringPopOldestLocked(char *out, size_t out_size, int *out_len);
    int  ringPeekDrainMetaLocked(int max_n, int *total_raw_len) const;

    static bool isKeepAliveTag(const char *tag);
    static char *allocEmptyJson(int *out_len);
    static int jsonEscapeAppend(char *buf, int pos, int capacity, const char *src, int src_len);
};

// 便捷宏 — 直接调用单例方法（禁止在 ISR 中使用）
#define DEBUG_ERR(tag, fmt, ...)   DebugLog::instance().log(DEBUG_LOG_ERR,   tag, fmt, ##__VA_ARGS__)
#define DEBUG_WARN(tag, fmt, ...)  DebugLog::instance().log(DEBUG_LOG_WARN,  tag, fmt, ##__VA_ARGS__)
#define DEBUG_INFO(tag, fmt, ...)  DebugLog::instance().log(DEBUG_LOG_INFO,  tag, fmt, ##__VA_ARGS__)
#define DEBUG_DBG(tag, fmt, ...)   DebugLog::instance().log(DEBUG_LOG_DBG,   tag, fmt, ##__VA_ARGS__)

#endif // DEBUG_LOG_H
