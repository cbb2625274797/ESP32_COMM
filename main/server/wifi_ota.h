/*
 * wifi_ota.h - WiFi OTA 服务器
 * T4.1 - T4.5
 *
 * WifiOta 类封装 WiFi 初始化、HTTP OTA 服务器和网络状态查询。
 */
#ifndef WIFI_OTA_H
#define WIFI_OTA_H

#include "ota_relay.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_event.h"

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

    // 以下状态为 public，供 C 回调函数访问
    ota_wifi_mode_t m_wifiMode = OTA_WIFI_MODE_AP;
    bool m_coldBoot = false;
    bool m_relayTriggered = false;
    bool m_staConnected = false;
    int m_staRetryCount = 0;
    bool m_staRetryExhausted = false;
    SemaphoreHandle_t m_staConnectedSem = NULL;
    SemaphoreHandle_t m_staGotIpSem = NULL;

    bool m_esp32OtaActive = false;
    esp_ota_handle_t m_esp32OtaHandle = 0;
    const esp_partition_t *m_esp32UpdatePartition = NULL;
    size_t m_esp32Received = 0;
    bool m_gd32StorageDone = false;
    httpd_handle_t m_httpServer = NULL;

    void initAp();
    esp_err_t initSta();
    void setStaticIp();
    static void wifiEventHandler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data);
    esp_err_t httpServerStart();
    esp_err_t waitStaConnected(char *ip_str, size_t ip_len);
};

#endif // WIFI_OTA_H