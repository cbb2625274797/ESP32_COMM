/*
 * wifi_ota.h - WiFi OTA 服务器
 * T4.1 - T4.5
 *
 * WifiOta 类封装 WiFi 初始化、HTTP OTA 服务器和网络状态查询。
 * ESP32 本机 OTA 的写入/收尾逻辑已抽离到 Esp32OtaCore（WiFi/BLE 共用）。
 */
#ifndef WIFI_OTA_H
#define WIFI_OTA_H

#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include <atomic>

// SHA-256 验证所需类型前向声明
typedef enum {
    OTA_WIFI_MODE_AP,
    OTA_WIFI_MODE_STA
} ota_wifi_mode_t;

class WifiOta {
public:
    /**
      * @brief 获取 WifiOta 单例实例。
     * @return WifiOta 单例引用。
     */
    static WifiOta& instance();

    /**
      * @brief 在调度器启动前初始化 WiFi 并尝试 STA 连接。
     * @return true 表示 STA 初始化中，false 需回退至 AP 模式。
     */
    bool earlyInit();
    /**
      * @brief WiFi OTA FreeRTOS 任务入口，提供 HTTP 上传和中继触发。
     * @param pvParameters FreeRTOS 任务参数。
     */
    static void taskEntry(void *pvParameters);
    /**
      * @brief 通知 OTA 中继开始向 GD32 转发固件。
     */
    void signalRelay();

    /**
      * @brief 获取当前 WiFi 工作模式。
     * @return 当前 WiFi 模式（AP 或 STA）。
     */
    static ota_wifi_mode_t getMode();
    /**
      * @brief 获取当前 WiFi 信号强度。
     * @return 信号强度值（dBm）。
     */
    static int8_t getRssi();
    /**
      * @brief 判断 STA 是否已连接到上级 WiFi。
     * @return true 已连接，false 未连接。
     */
    static bool isConnected();

    // ── WiFi 连接状态（WiFi 事件回调 / HTTP / reconnect task 共享，需 mutex 保护）──
    /** @brief 获取 STA 连接状态。 */
    bool getStaConnected() const          { return m_staConnected.load(); }
    /** @brief 设置 STA 连接状态。 */
    void setStaConnected(bool v)          { m_staConnected.store(v); }
    /** @brief 获取 STA 重试次数。 */
    int  getStaRetryCount() const         { return m_staRetryCount.load(); }
    /** @brief 设置 STA 重试次数。 */
    void setStaRetryCount(int v)          { m_staRetryCount.store(v); }
    /** @brief 获取 STA 重试是否已耗尽。 */
    bool getStaRetryExhausted() const     { return m_staRetryExhausted.load(); }
    /** @brief 设置 STA 重试耗尽标志。 */
    void setStaRetryExhausted(bool v)     { m_staRetryExhausted.store(v); }
    /** @brief 获取 GD32 固件存储完成标志。 */
    bool getGd32StorageDone() const       { return m_gd32StorageDone.load(); }
    /** @brief 设置 GD32 固件存储完成标志。 */
    void setGd32StorageDone(bool v)       { m_gd32StorageDone.store(v); }
    /** @brief 获取中继触发标志。 */
    bool getRelayTriggered() const        { return m_relayTriggered.load(); }
    /** @brief 设置中继触发标志。 */
    void setRelayTriggered(bool v)        { m_relayTriggered.store(v); }

    // ── 状态互斥锁（保护复合读-改-写操作）──
    /** @brief 获取状态互斥锁句柄。 */
    SemaphoreHandle_t getStateMutex() const { return m_stateMutex; }
    /** @brief 锁定状态互斥锁。 */
    void stateLock()   { if (m_stateMutex) xSemaphoreTake(m_stateMutex, portMAX_DELAY); }
    /** @brief 解锁状态互斥锁。 */
    void stateUnlock() { if (m_stateMutex) xSemaphoreGive(m_stateMutex); }

    // ── 初始化/运行时状态（仅 OTA 任务访问，无需额外锁）──
    ota_wifi_mode_t m_wifiMode = OTA_WIFI_MODE_AP;
    bool m_coldBoot = false;
    SemaphoreHandle_t m_staConnectedSem = NULL;
    SemaphoreHandle_t m_staGotIpSem = NULL;
    TaskHandle_t m_reconnectTask = nullptr;

    httpd_handle_t m_httpServer = NULL;

    // ★ BLE OTA 传输期间暂停 WiFi 射频活动（减少共存干扰）
    std::atomic<bool> m_bleOtaActive{false};
    /** @brief BLE OTA 开始传输时调用，暂停 WiFi RSSI 轮询等射频活动。 */
    void setBleOtaActive(bool v) { m_bleOtaActive.store(v); }

private:
    // ── 跨任务共享状态（atomic + m_stateMutex 保护）──
    std::atomic<bool> m_staConnected{false};
    std::atomic<int>  m_staRetryCount{0};
    std::atomic<bool> m_staRetryExhausted{false};
    std::atomic<bool> m_gd32StorageDone{false};
    std::atomic<bool> m_relayTriggered{false};
    SemaphoreHandle_t m_stateMutex = NULL;

    /**
      * @brief 初始化 SoftAP 模式，供局域网 OTA 使用。
     */
    void initAp();
    /**
      * @brief 初始化 STA 模式并连接预配置路由器。
     * @return ESP_OK 成功，其他失败。
     */
    esp_err_t initSta();
    /**
      * @brief 设置 STA 静态 IP、网关和子网掩码。
     */
    void setStaticIp();
    /**
      * @brief WiFi/IP 事件回调，处理连接、断开和 IP 获取。
     * @param arg 回调用户参数。
     * @param event_base 事件基类。
     * @param event_id 具体事件 ID。
     * @param event_data 事件附带数据。
     */
    static void wifiEventHandler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data);
    /**
      * @brief STA 后台重连任务，处理退避延迟和重连。
     * @param pvParameters FreeRTOS 任务参数。
     */
    static void reconnectTask(void *pvParameters);
    /**
      * @brief 启动 HTTP OTA 服务器，注册各接口路由。
     * @return ESP_OK 成功，其他失败。
     */
    esp_err_t httpServerStart();
    /**
      * @brief 等待 STA 连接成功并获取 IP 地址。
     * @param ip_str 输出缓冲区，存放 IP 字符串。
     * @param ip_len 缓冲区长度。
     * @return ESP_OK 连接成功，ESP_ERR_TIMEOUT 超时。
     */
    esp_err_t waitStaConnected(char *ip_str, size_t ip_len);
};

#endif // WIFI_OTA_H
