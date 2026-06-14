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
    static WifiOta& instance();

    bool earlyInit();
    static void taskEntry(void *pvParameters);
    void signalRelay();

    static ota_wifi_mode_t getMode();
    static int8_t getRssi();
    static bool isConnected();

    // ── WiFi 连接状态（WiFi 事件回调 / HTTP / reconnect task 共享，需 mutex 保护）──
    bool getStaConnected() const          { return m_staConnected.load(); }
    void setStaConnected(bool v)          { m_staConnected.store(v); }
    int  getStaRetryCount() const         { return m_staRetryCount.load(); }
    void setStaRetryCount(int v)          { m_staRetryCount.store(v); }
    bool getStaRetryExhausted() const     { return m_staRetryExhausted.load(); }
    void setStaRetryExhausted(bool v)     { m_staRetryExhausted.store(v); }
    bool getGd32StorageDone() const       { return m_gd32StorageDone.load(); }
    void setGd32StorageDone(bool v)       { m_gd32StorageDone.store(v); }
    bool getRelayTriggered() const        { return m_relayTriggered.load(); }
    void setRelayTriggered(bool v)        { m_relayTriggered.store(v); }

    // ── 状态互斥锁（保护复合读-改-写操作）──
    SemaphoreHandle_t getStateMutex() const { return m_stateMutex; }
    void stateLock()   { if (m_stateMutex) xSemaphoreTake(m_stateMutex, portMAX_DELAY); }
    void stateUnlock() { if (m_stateMutex) xSemaphoreGive(m_stateMutex); }

    // ── 初始化/运行时状态（仅 OTA 任务访问，无需额外锁）──
    ota_wifi_mode_t m_wifiMode = OTA_WIFI_MODE_AP;
    bool m_coldBoot = false;
    SemaphoreHandle_t m_staConnectedSem = NULL;
    SemaphoreHandle_t m_staGotIpSem = NULL;
    TaskHandle_t m_reconnectTask = nullptr;

    httpd_handle_t m_httpServer = NULL;

private:
    // ── 跨任务共享状态（atomic + m_stateMutex 保护）──
    std::atomic<bool> m_staConnected{false};
    std::atomic<int>  m_staRetryCount{0};
    std::atomic<bool> m_staRetryExhausted{false};
    std::atomic<bool> m_gd32StorageDone{false};
    std::atomic<bool> m_relayTriggered{false};
    SemaphoreHandle_t m_stateMutex = NULL;

    void initAp();
    esp_err_t initSta();
    void setStaticIp();
    static void wifiEventHandler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data);
    static void reconnectTask(void *pvParameters);
    esp_err_t httpServerStart();
    esp_err_t waitStaConnected(char *ip_str, size_t ip_len);
};

#endif // WIFI_OTA_H
