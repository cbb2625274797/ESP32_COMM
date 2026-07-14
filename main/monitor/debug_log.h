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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app_config.h"
#include <sys/socket.h>
#include <netinet/in.h>

// 调试日志配置（DEBUG_LOG_IP/PORT/ENABLED 已在 app_config.h 中定义）

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
    /**
      * @brief 获取 DebugLog 单例实例。
     * @return DebugLog 引用。
     */
    static DebugLog& instance();

    /** @brief 初始化日志系统，包括 UART 输出、UDP 目标和内部缓冲区。 */
    esp_err_t init();

    /** @brief 初始化 UART0 调试串口。 */
    esp_err_t uartInit();

    /** @brief 通过 UART0 直接发送一段原始字节数据。 */
    esp_err_t uartSend(const uint8_t *data, size_t len);

    /** @brief 发送原始日志数据到底层输出通道。 */
    esp_err_t send(const uint8_t *data, size_t len);

    /** @brief 按日志级别与标签格式化输出一条日志。 */
    void log(debug_log_level_t level, const char *tag, const char *fmt, ...);

    /** @brief 获取当前环形日志缓冲区中的日志条数。 */
    int ringCount();

    /** @brief 弹出一条最旧日志并从环形缓冲区移除。 */
    bool ringPopOldest(char *out, size_t out_size, int *out_len);

    /**
     * @brief 将环形缓冲区所有日志导出为 JSON 数组字符串。
     * @param out_len 输出 JSON 字符串长度。
     * @return 动态分配的 JSON 字符串，调用者须 free()。
     */
    char* ringDrainJson(int *out_len);

    /** @brief 当前日志级别（运行时可改）。 */
    debug_log_level_t logLevel = DEBUG_LOG_INFO;

private:
    DebugLog() = default;
    ~DebugLog() = default;
    DebugLog(const DebugLog&) = delete;
    DebugLog& operator=(const DebugLog&) = delete;

    int m_udpSock = -1;
    struct sockaddr_in m_debugAddr;
    bool m_initialized = false;
    bool m_uartInitialized = false;
    // 记录 UART 是否真正可用（uartInit 失败时为 false，跳过 uart_write_bytes）。
    //   m_uartInitialized 仅表示曾尝试初始化，m_uartAvailable 才是写日志的判据。
    bool m_uartAvailable = false;

    /* 环形日志：OTA 中继期需保留关键 INFO，64×160≈10KB BSS。 */
    static constexpr int LOG_RING_SLOTS = 64;
    static constexpr int LOG_RING_SIZE  = 160;
    char  m_logRing[LOG_RING_SLOTS][LOG_RING_SIZE];
    int   m_logRingLens[LOG_RING_SLOTS];
    int   m_logRingHead  = 0;
    int   m_logRingCount = 0;
    SemaphoreHandle_t m_logRingMutex = NULL;

    /* 堆门控（防 OTA 时 UDP/格式化抢 LWIP 堆）：
     * free < kDropWarnFree → 非 OTA/SPI 只保留 ERR
     * free < kDropInfoFree → 非 OTA/SPI 丢 INFO/DBG（OTA/SPI 的 INFO 仍进 ring）
     * free < kSkipUdpFree  → 不 sendto（仍 UART+ring）
     * free < kDrainMinFree → /logs 尽量仍返回 ring（降低门槛，避免“沉默失败”） */
    static constexpr uint32_t kDropWarnFree = 6U * 1024U;
    static constexpr uint32_t kDropInfoFree = 10U * 1024U;
    static constexpr uint32_t kSkipUdpFree  = 10U * 1024U;
    static constexpr uint32_t kDrainMinFree = 8U * 1024U;

    esp_err_t ensureUdpSocket();
    void ringPush(const char *data, int len);
};

// 便捷宏 — 直接调用单例方法
#define DEBUG_ERR(tag, fmt, ...)   DebugLog::instance().log(DEBUG_LOG_ERR,   tag, fmt, ##__VA_ARGS__) ///< 错误级别日志宏
#define DEBUG_WARN(tag, fmt, ...)  DebugLog::instance().log(DEBUG_LOG_WARN,  tag, fmt, ##__VA_ARGS__) ///< 警告级别日志宏
#define DEBUG_INFO(tag, fmt, ...)  DebugLog::instance().log(DEBUG_LOG_INFO,  tag, fmt, ##__VA_ARGS__) ///< 信息级别日志宏
#define DEBUG_DBG(tag, fmt, ...)   DebugLog::instance().log(DEBUG_LOG_DBG,   tag, fmt, ##__VA_ARGS__) ///< 调试级别日志宏

#endif // DEBUG_LOG_H
