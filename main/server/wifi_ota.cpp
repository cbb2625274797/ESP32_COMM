/*
 * wifi_ota.cpp - WiFi OTA 服务器实现
 * WifiOta 类封装 WiFi 初始化、HTTP OTA 服务器和网络状态查询。
 * T4.1 - T4.5
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "lwip/netdb.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_event.h"
#include "esp_mac.h"

#include "app_config.h"
#include "wifi_ota.h"
#include "esp32_ota_core.h"
#include "ota_coordinator.h"
#include "ota_relay.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"
#include "spi_master.h"
#include "ble_ota.h"
#include "http_handlers.h"



// ============================================================
/**
  * @brief 获取 WifiOta 单例实例（静态局部变量实现）。
 * @return WifiOta 单例引用。
 */
WifiOta& WifiOta::instance()
{
    static WifiOta inst;
    return inst;
}

/**
 * @brief 预先声明：为 STA 模式配置静态 IP、网关和子网掩码。
 */

// ----------------------------------------
// STA 后台重连任务（避免在系统事件回调中调用 esp_wifi_connect 死锁）
// ----------------------------------------
/**
 * @brief 独立 FreeRTOS 任务，处理 STA 连接/重连的退避延迟和 esp_wifi_connect() 调用。
 *        事件回调仅通过 xTaskNotifyGive 唤醒本任务，不直接在系统事件任务中阻塞或调用 WiFi API。
 * @param pvParameters FreeRTOS 任务参数（未使用）。
 */
void WifiOta::reconnectTask(void* pvParameters)
{
    (void)pvParameters;
    while (1) {
        // 等待事件回调通知（阻塞，不消耗 CPU）
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    
        WifiOta& self = WifiOta::instance();
    
        // 已连接或已放弃，无需处理
        if (self.getStaConnected() || self.getStaRetryExhausted()) {
            continue;
        }
    
        self.stateLock();
    
        // 达到最大重试次数
        if (self.getStaRetryCount() >= WIFI_STA_MAX_RETRY) {
            DEBUG_ERR("WIFI", "STA 重连失败 %d 次，放弃", self.getStaRetryCount());
            self.setStaRetryExhausted(true);
            self.stateUnlock();
            continue;
        }
    
        self.setStaRetryCount(self.getStaRetryCount() + 1);
        int retryCount = self.getStaRetryCount();
    
        // 计算退避延迟：冷启动前 3 次递增退避，其余固定 1s
        int delay_ms = 0;
        if (self.m_coldBoot && retryCount <= 3) {
            delay_ms = 2000 * retryCount;  // 2s, 4s, 6s
            DEBUG_INFO("WIFI", "冷启动退避重连第 %d 次 (等 %dms)",
                       retryCount, delay_ms);
        } else if (retryCount > 1) {
            delay_ms = 1000;  // 普通重连 1s 延迟
            DEBUG_INFO("WIFI", "重连第 %d 次...", retryCount);
        } else {
            DEBUG_INFO("WIFI", "首次连接...");
        }
    
        self.stateUnlock();
    
        if (delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    
        // 延迟后再次检查状态，防止期间已连接或已放弃
        if (!self.getStaConnected() && !self.getStaRetryExhausted()) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                DEBUG_ERR("WIFI", "esp_wifi_connect() 失败: %s", esp_err_to_name(err));
            }
        }
    }
}

// ----------------------------------------
// WiFi/IP 事件回调（事件驱动连接）
// ----------------------------------------
/**
 * @brief WiFi/IP 事件回调，处理 STA 启停、重连和获取 IP 等状态变化。
 * @param arg 回调用户参数。
 * @param event_base 事件基类（WIFI_EVENT 或 IP_EVENT）。
 * @param event_id 具体事件 ID。
 * @param event_data 事件附带数据。
 */
void WifiOta::wifiEventHandler(void* arg, esp_event_base_t event_base,
                                      int32_t event_id, void* event_data)
{
    WifiOta& inst = WifiOta::instance();

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            DEBUG_INFO("WIFI", "STA 已启动，开始连接...");
            inst.stateLock();
            inst.setStaRetryCount(0);
            inst.setStaRetryExhausted(false);
            inst.stateUnlock();
            if (inst.m_reconnectTask) {
                xTaskNotifyGive(inst.m_reconnectTask);
            }
            break;

        case WIFI_EVENT_STA_CONNECTED:
            DEBUG_INFO("WIFI", "STA 已关联到 AP");
            inst.stateLock();
            inst.setStaConnected(true);
            inst.setStaRetryCount(0);
            inst.setStaRetryExhausted(false);  // ★ 连接成功，重置耗尽标志
            inst.stateUnlock();
            // ★ M2: 推送 WiFi 连接状态给 HealthMonitor（解耦监控层主动查询）
            HealthMonitor::instance().setWifiConnected(true);
            if (inst.m_staConnectedSem) {
                xSemaphoreGive(inst.m_staConnectedSem);
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*)event_data;
            inst.stateLock();
            inst.setStaConnected(false);
            bool exhausted = inst.getStaRetryExhausted();
            int retryCount = inst.getStaRetryCount();
            inst.stateUnlock();
            // ★ M2: 推送 WiFi 断开状态给 HealthMonitor
            HealthMonitor::instance().setWifiConnected(false);

            if (!exhausted) {
                DEBUG_WARN("WIFI", "STA 断开, reason=%d, retry=%d",
                           disconn->reason, retryCount);
                if (inst.m_reconnectTask) {
                    xTaskNotifyGive(inst.m_reconnectTask);
                }
            } else {
                DEBUG_WARN("WIFI", "STA 断开, 重试次数已耗尽");
            }
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*)event_data;
            DEBUG_INFO("WIFI", "AP: 客户端 " MACSTR " 连入, AID=%d", MAC2STR(event->mac), event->aid);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*)event_data;
            DEBUG_INFO("WIFI", "AP: 客户端 " MACSTR " 离开, AID=%d, reason=%d",
                       MAC2STR(event->mac), event->aid, event->reason);
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP: {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
            DEBUG_INFO("WIFI", "STA 获取 IP: " IPSTR, IP2STR(&event->ip_info.ip));
            if (WifiOta::instance().m_staGotIpSem) {
                xSemaphoreGive(WifiOta::instance().m_staGotIpSem);
            }
            break;
        }

        default:
            break;
        }
    }
}

// ----------------------------------------
// WiFi AP 模式初始化
/**
  * @brief 初始化 SoftAP 模式，供局域网 OTA 使用。
 */
void WifiOta::initAp()
{
    DEBUG_INFO("WIFI", "=== 初始化 WiFi AP 模式 ===");

    WifiOta::instance().m_wifiMode = OTA_WIFI_MODE_AP;

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (ap_netif != NULL) {
        esp_netif_dhcps_stop(ap_netif);

        esp_netif_ip_info_t ip_info;
        memset(&ip_info, 0, sizeof(ip_info));
        // 方案 A：SoftAP 使用 192.168.31.0/24 网段
        inet_pton(AF_INET, "192.168.31.1", &ip_info.ip);
        inet_pton(AF_INET, "192.168.31.1", &ip_info.gw);
        inet_pton(AF_INET, "255.255.255.0", &ip_info.netmask);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));

        esp_netif_dhcps_start(ap_netif);
        DEBUG_INFO("WIFI", "AP IP 已设置: 192.168.31.1");
    }

    wifi_config_t wifi_config = {};
    memcpy(wifi_config.ap.ssid, WIFI_AP_SSID, strlen(WIFI_AP_SSID) + 1);
    wifi_config.ap.ssid_len = strlen(WIFI_AP_SSID);
    memcpy(wifi_config.ap.password, WIFI_AP_PASSWORD, strlen(WIFI_AP_PASSWORD) + 1);
    wifi_config.ap.max_connection = WIFI_AP_MAX_CONNECT;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    wifi_config.ap.channel = WIFI_AP_CHANNEL;

    DEBUG_INFO("WIFI", "AP SSID: %s, Channel: %d", WIFI_AP_SSID, WIFI_AP_CHANNEL);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    DEBUG_INFO("WIFI", "WiFi AP 已启动: %s", WIFI_AP_SSID);
    DEBUG_INFO("WIFI", "AP IP 地址: 192.168.31.1");
    DEBUG_INFO("WIFI", "AP 网关地址: 192.168.31.1");
    DEBUG_INFO("WIFI", "最大连接数: %d", WIFI_AP_MAX_CONNECT);
}

// ----------------------------------------
// WiFi STA 模式初始化
/**
  * @brief 初始化 STA 模式并连接预配置的路由器。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t WifiOta::initSta()
{
    DEBUG_INFO("WIFI", "=== 初始化 WiFi STA 模式 ===");

    WifiOta::instance().m_wifiMode = OTA_WIFI_MODE_STA;
    WifiOta::instance().setStaRetryCount(0);
    WifiOta::instance().setStaRetryExhausted(false);

    // ★ 设置国家代码：中国，允许信道 1-13
    // ESP32 默认国家是 US（只允许 1-11），中国路由可能用 12/13
    wifi_country_t country = {};
    memcpy(country.cc, "CN", 3);
    country.schan = 1;
    country.nchan = 13;
    country.policy = WIFI_COUNTRY_POLICY_MANUAL;

    wifi_config_t wifi_config = {};
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.listen_interval = 3;
    memcpy(wifi_config.sta.ssid, WIFI_STA_SSID, strlen(WIFI_STA_SSID) + 1);
    memcpy(wifi_config.sta.password, WIFI_STA_PASSWORD, strlen(WIFI_STA_PASSWORD) + 1);

    DEBUG_INFO("WIFI", "STA SSID: %s (全信道扫描, CN 国家代码)", WIFI_STA_SSID);

    ESP_ERROR_CHECK(esp_wifi_set_country(&country));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    setStaticIp();

    // ★ 冷启动时在 wifi_start 前增加延迟
    // 长时间断电后首次上电，WiFi PHY 校准需要额外时间
    // esp_wifi_start() 内部会触发 PHY 初始化和校准
    // 如果校准未完成就启动射频，可能导致连接失败
    if (WifiOta::instance().m_coldBoot) {
        DEBUG_INFO("WIFI", "冷启动: 等待 PHY 就绪...");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    DEBUG_INFO("WIFI", "WiFi STA 启动, SSID: %s, 事件驱动连接...", WIFI_STA_SSID);

    return ESP_OK;
}

/**
 * @brief 等待 STA 连接成功并获取 IP（静态 IP 等 CONNECTED，DHCP 等 GOT_IP）
 * @param ip_str 输出缓冲区，存放获取到的 IP 字符串。
 * @param ip_len 缓冲区长度。
 * @return ESP_OK 连接成功，其他失败。
 */
esp_err_t WifiOta::waitStaConnected(char *ip_str, size_t ip_len)
{
    SemaphoreHandle_t wait_sem = WIFI_STA_USE_STATIC_IP ? WifiOta::instance().m_staConnectedSem : WifiOta::instance().m_staGotIpSem;
    const char *wait_name = WIFI_STA_USE_STATIC_IP ? "STA_CONNECTED" : "STA_GOT_IP";

    if (wait_sem == NULL) {
        DEBUG_ERR("WIFI", "%s 信号量未创建", wait_name);
        return ESP_ERR_INVALID_STATE;
    }

    // ★ 使用轮询等待，检查 WifiOta::instance().m_staRetryExhausted 标志加速退出
    // 不再一次性等完整超时，而是每秒检查一次重试耗尽状态
    TickType_t timeout_ticks = pdMS_TO_TICKS(WIFI_STA_CONNECT_TIMEOUT_MS);
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t deadline = start_tick + timeout_ticks;

    while (xTaskGetTickCount() < deadline) {
        // 每次最多等 1 秒
        TickType_t wait_until = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
        if (wait_until > deadline) wait_until = deadline;

        if (xSemaphoreTake(wait_sem, wait_until - xTaskGetTickCount()) == pdTRUE) {
            esp_netif_ip_info_t ip_info;
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                if (ip_str) {
                    snprintf(ip_str, ip_len, IPSTR, IP2STR(&ip_info.ip));
                }
                DEBUG_INFO("WIFI", "%s 等待成功, 当前 IP: " IPSTR, wait_name, IP2STR(&ip_info.ip));
            }
            WifiOta::instance().setStaConnected(true);
            return ESP_OK;
        }

        // ★ 重试已耗尽，不再等待
        if (WifiOta::instance().getStaRetryExhausted()) {
            DEBUG_ERR("WIFI", "STA 重试已耗尽 (%d 次)，立即切换 AP", WifiOta::instance().getStaRetryCount());
            return ESP_ERR_TIMEOUT;
        }
    }

    DEBUG_ERR("WIFI", "等待 %s 超时 (%d ms), 重连次数: %d", wait_name, WIFI_STA_CONNECT_TIMEOUT_MS, WifiOta::instance().getStaRetryCount());
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 设置 STA 静态 IP
 */
void WifiOta::setStaticIp()
{
    if (WIFI_STA_USE_STATIC_IP) {
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif) {
            DEBUG_INFO("WIFI", "STA 使用静态 IP 模式，停止 DHCP Client");
            ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta_netif));

            esp_netif_ip_info_t info;
            memset(&info, 0, sizeof(info));
            inet_pton(AF_INET, WIFI_STA_STATIC_IP, &info.ip);
            inet_pton(AF_INET, WIFI_STA_STATIC_GW, &info.gw);
            inet_pton(AF_INET, WIFI_STA_STATIC_NETMASK, &info.netmask);
            ESP_ERROR_CHECK(esp_netif_set_ip_info(sta_netif, &info));
            DEBUG_INFO("WIFI", "静态 IP 已设置: %s", WIFI_STA_STATIC_IP);

            // 设置 DNS
            esp_netif_dns_info_t dns_info;
            memset(&dns_info, 0, sizeof(dns_info));
            inet_pton(AF_INET, WIFI_STA_STATIC_DNS, &dns_info.ip.u_addr.ip4.addr);
            ESP_ERROR_CHECK(esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info));
            DEBUG_INFO("WIFI", "DNS 已设置: %s", WIFI_STA_STATIC_DNS);
        }
    } else {
        DEBUG_INFO("WIFI", "使用 DHCP 获取 IP");
    }
}

// ----------------------------------------
// ESP32 OTA 内部函数
// 注：begin/write/finalize/abort 已抽离到 Esp32OtaCore（main/ota/esp32_ota_core.{h,cpp}）
//     WiFi 与 BLE 通道共用同一核心，本文件仅负责 HTTP 传输层。



// ----------------------------------------
// 启动 HTTP OTA 服务器
/**
   * @brief 启动 HTTP OTA 服务器，注册各接口路由。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t WifiOta::httpServerStart()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_SERVER_PORT;
    config.stack_size = HTTP_SERVER_STACK_SIZE;
    config.max_uri_handlers = 32;  // 现有业务端点 + 对应 OPTIONS 预检
    config.max_open_sockets = 4;   // LWIP_MAX_SOCKETS 默认 7，HTTP 内部占 3，最多 4
    config.lru_purge_enable = true;  // socket 满时自动清理最久未用的连接
    config.recv_wait_timeout = 30;  // GD32 上传前会擦除 Flash，5 秒默认值过紧
    config.send_wait_timeout = 30;

    DEBUG_INFO("WIFI", "=== 启动 HTTP OTA 服务器 ===");
    DEBUG_INFO("WIFI", "HTTP 服务器端口: %d", config.server_port);

    esp_err_t err = httpd_start(&WifiOta::instance().m_httpServer, &config);
    if (err != ESP_OK) {
        DEBUG_ERR("WIFI", "HTTP 服务器启动失败: %s", esp_err_to_name(err));
        return err;
    }

    // 从 NVS 恢复固件存储状态（重启后保留）
    {
        ota_meta_t boot_meta;
        if (OtaCoordinator::instance().loadGd32Meta(&boot_meta) == ESP_OK &&
            boot_meta.total_size > 0 && boot_meta.stored_size >= boot_meta.total_size) {
            WifiOta::instance().setGd32StorageDone(true);
            DEBUG_INFO("WIFI", "从 NVS 恢复: 固件已就绪 (%u bytes)", boot_meta.total_size);
        }
    }

    struct UriEntry {
        const char *uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t *);
    };
    static const UriEntry kHttpRoutes[] = {
        {"/ota/upload",    HTTP_POST, ota_upload_handler},
        {"/ota/relay",     HTTP_POST, ota_relay_trigger_handler},
        {"/ota/update",    HTTP_POST, esp32_ota_update_handler},
        {"/restart",       HTTP_POST, restart_handler},
        {"/logs",          HTTP_GET,  logs_handler},
        {"/system/status", HTTP_GET,  system_status_handler},
        {"/system/probe",  HTTP_GET,  system_probe_handler},
        {"/can/send",      HTTP_POST, can_send_handler},
        {"/spi/send",      HTTP_POST, spi_send_handler},
    };

    for (const auto &r : kHttpRoutes) {
        httpd_uri_t main_uri = {};
        main_uri.uri = r.uri;
        main_uri.method = r.method;
        main_uri.handler = r.handler;
        main_uri.user_ctx = NULL;
        httpd_register_uri_handler(WifiOta::instance().m_httpServer, &main_uri);

        httpd_uri_t opts_uri = {};
        opts_uri.uri = r.uri;
        opts_uri.method = HTTP_OPTIONS;
        opts_uri.handler = http_options_handler;
        opts_uri.user_ctx = NULL;
        httpd_register_uri_handler(WifiOta::instance().m_httpServer, &opts_uri);
    }

    DEBUG_INFO("WIFI", "注册端点: /ota/upload, /ota/relay, /ota/update, /restart, /logs, /system/status, /system/probe, /can/send, /spi/send");

    return ESP_OK;
}

// ----------------------------------------
// 公开 API
/**
  * @brief 获取当前 WiFi 工作模式（AP 或 STA）。
 * @return 当前 WiFi 模式枚举值。
 */
ota_wifi_mode_t WifiOta::getMode()
{
    return WifiOta::instance().m_wifiMode;
}

/**
  * @brief 获取当前 WiFi 信号强度。
 * @return 信号强度（dBm），未连接时返回无效值。
 */
int8_t WifiOta::getRssi()
{
    if (WifiOta::instance().m_wifiMode != OTA_WIFI_MODE_STA || !WifiOta::instance().getStaConnected()) {
        return -127;
    }
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return -127;
}

/**
  * @brief 判断 STA 是否已连接到上级 WiFi。
 * @return true 已连接，false 未连接。
 */
bool WifiOta::isConnected()
{
    return WifiOta::instance().getStaConnected();
}

// ============================================================
// IBleCoexListener 接口实现（L1: 通过协调器接收 BLE 共存通知）
// ============================================================
void WifiOta::onBleOtaStart()
{
    setBleOtaActive(true);
}

void WifiOta::onBleOtaEnd()
{
    setBleOtaActive(false);
}

// ----------------------------------------
// ★ WiFi 早期初始化（在 app_main 调度器启动前调用）
// 包括：NVS → netif → event handler → WiFi driver → 配置 → connect
// 返回：true = STA 模式已启动（正在连接），false = 需要切 AP
/**
  * @brief 在调度器启动前初始化 WiFi 并尝试 STA 连接。
 * @return true STA 初始化中，false 需回退 AP。
 */
bool WifiOta::earlyInit()
{
    DEBUG_INFO("WIFI", "=== WiFi 早期初始化 (调度器启动前) ===");

    // ★ 冷启动检测：判断是否为上电复位
    esp_reset_reason_t reset_reason = esp_reset_reason();
    WifiOta::instance().m_coldBoot = (reset_reason == ESP_RST_POWERON || reset_reason == ESP_RST_BROWNOUT);
    
    DEBUG_INFO("WIFI", "复位原因: %d (%s), 冷启动: %s",
               reset_reason,
               (reset_reason == ESP_RST_POWERON) ? "POWER_ON" :
               (reset_reason == ESP_RST_BROWNOUT) ? "BROWNOUT" :
               (reset_reason == ESP_RST_SW) ? "SOFTWARE" :
               (reset_reason == ESP_RST_PANIC) ? "PANIC" :
               (reset_reason == ESP_RST_INT_WDT) ? "INT_WDT" :
               (reset_reason == ESP_RST_TASK_WDT) ? "TASK_WDT" :
               (reset_reason == ESP_RST_DEEPSLEEP) ? "DEEP_SLEEP" :
               "OTHER",
               WifiOta::instance().m_coldBoot ? "是" : "否");

    esp_err_t err = nvs_flash_init();
    bool nvs_erased = false;
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 分区损坏或版本不匹配，擦除后重试
        DEBUG_WARN("WIFI", "NVS 分区异常 (%s)，擦除后重试", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_erased = true;
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        DEBUG_ERR("WIFI", "NVS 初始化失败: %s", esp_err_to_name(err));
    } else {
        if (nvs_erased) {
            DEBUG_WARN("WIFI", "NVS 已擦除重建，WiFi PHY 校准数据已清除，将重新校准");
        }
        DEBUG_INFO("WIFI", "NVS 初始化成功");
    }

    // ★ 冷启动时强制清除 WiFi NVS 缓存，避免残留脏数据
    // 长时间断电后 NVS 中的 WiFi 配置缓存可能不一致
    if (WifiOta::instance().m_coldBoot) {
        nvs_handle_t nvs_handle;
        err = nvs_open("nvs.net80211", NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK) {
            // 清除 WiFi 驱动内部 NVS 缓存键
            nvs_erase_all(nvs_handle);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            DEBUG_DBG("WIFI", "冷启动: 已清除 WiFi NVS 缓存 (nvs.net80211)");
        } else {
            DEBUG_INFO("WIFI", "冷启动: 无 WiFi NVS 缓存分区，跳过清除");
        }
        // 同样清除 PHY 校准数据分区，强制下次全量校准
        err = nvs_open("phy", NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK) {
            nvs_erase_all(nvs_handle);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            DEBUG_DBG("WIFI", "冷启动: 已清除 PHY 校准缓存，将重新校准");
        }
    }

    // ★ 创建状态互斥锁（保护 WiFi 连接状态、OTA 中继标志等跨任务共享变量）
    if (WifiOta::instance().getStateMutex() == NULL) {
        SemaphoreHandle_t mtx = xSemaphoreCreateMutex();
        if (mtx) {
            // 通过临时获取再赋值（私有成员在同类内可访问）
            WifiOta::instance().m_stateMutex = mtx;
            DEBUG_DBG("WIFI", "状态互斥锁已创建");
        } else {
            DEBUG_ERR("WIFI", "创建状态互斥锁失败");
        }
    }

    WifiOta::instance().m_staConnectedSem = xSemaphoreCreateBinary();
    WifiOta::instance().m_staGotIpSem = xSemaphoreCreateBinary();
    if (!WifiOta::instance().m_staConnectedSem || !WifiOta::instance().m_staGotIpSem) {
        DEBUG_ERR("WIFI", "创建信号量失败");
    }

    // 创建后台重连任务：避免在系统事件任务中调用 esp_wifi_connect()/vTaskDelay 导致死锁
    if (WifiOta::instance().m_reconnectTask == nullptr) {
        BaseType_t ret = xTaskCreate(&WifiOta::reconnectTask, "wifi_reconnect",
                                     4096, NULL, 5,
                                     &WifiOta::instance().m_reconnectTask);
        if (ret != pdPASS) {
            DEBUG_ERR("WIFI", "创建重连任务失败");
        } else {
            DEBUG_DBG("WIFI", "WiFi 重连任务已创建");
        }
    }

    // 注册 WiFi/IP 事件回调（必须在 esp_wifi_init 之前）
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiOta::wifiEventHandler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &WifiOta::wifiEventHandler, NULL));
    DEBUG_DBG("WIFI", "WiFi/IP 事件回调已注册");

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == NULL) {
        sta_netif = esp_netif_create_default_wifi_sta();
        if (sta_netif == NULL) {
            DEBUG_ERR("WIFI", "创建 STA netif 失败");
        } else {
            DEBUG_DBG("WIFI", "STA netif 已创建");
        }
    } else {
        DEBUG_INFO("WIFI", "STA netif 已存在，跳过创建");
    }

    // ★ 压制 WiFi 框架内部 INFO 日志（pp/net80211/wifi/wifi_init 等 tag 大量启动打印）
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("pp", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    DEBUG_INFO("WIFI", "WiFi driver 初始化成功");

    // STA 优先，失败时返回 false 让 task 切 AP
    if (CONFIG_WIFI_MODE == 1) {
        DEBUG_INFO("WIFI", "工作模式: STA (优先)");
        err = initSta();
        if (err == ESP_OK) {
            // 连接已触发（STA_START 事件驱动），返回 true
            // 实际连接状态由 WifiOta::taskEntry 等待确认
            return true;
        }
        DEBUG_WARN("WIFI", "STA 初始化失败，将切换 AP");
        return false;
    } else {
        DEBUG_INFO("WIFI", "工作模式: AP");
        initAp();
        return false;  // AP 模式，不需要等待连接
    }
}

// ----------------------------------------
// WiFi OTA FreeRTOS 任务
// WiFi 初始化已在 app_main 中通过 WifiOta::instance().earlyInit() 完成
// 此任务负责：等待连接结果 → 启动 HTTP 服务器 → 状态监控
/**
  * @brief WiFi OTA FreeRTOS 任务，提供 HTTP 上传和中继触发。
 * @param pvParameters FreeRTOS 任务参数，未使用。
 */
void WifiOta::taskEntry(void *pvParameters)
{
    (void)pvParameters;
    WifiOta::instance().m_taskHandle = xTaskGetCurrentTaskHandle();

    DEBUG_INFO("WIFI", "=== WiFi OTA 任务启动 ===");

    // 等待 STA 连接结果（wifi_early_init 已触发连接）
    if (WifiOta::getMode() == OTA_WIFI_MODE_STA) {
        char ip_str[16] = {0};
        esp_err_t err = instance().waitStaConnected(ip_str, sizeof(ip_str));
        if (err == ESP_OK) {
            DEBUG_INFO("WIFI", "STA 已连接, IP: %s", ip_str);
        } else {
            DEBUG_WARN("WIFI", "STA 连接超时，切换到 AP 模式...");
            // ★ 完整的 STA→AP 切换流程：
            // 1. 停止 WiFi
            // 2. 反初始化 WiFi 驱动（释放内部资源，包括 NVS 缓存）
            // 3. 重新初始化 WiFi 驱动
            // 4. 重新注册事件回调
            // 5. 以 AP 模式启动
            esp_wifi_stop();
            esp_err_t deinit_err = esp_wifi_deinit();
            if (deinit_err != ESP_OK) {
                DEBUG_WARN("WIFI", "WiFi deinit 失败: %s，仅 stop 后切 AP", esp_err_to_name(deinit_err));
            } else {
                DEBUG_INFO("WIFI", "WiFi 驱动已反初始化，重新初始化...");
                wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
                deinit_err = esp_wifi_init(&wifi_cfg);
                if (deinit_err != ESP_OK) {
                    DEBUG_ERR("WIFI", "WiFi 驱动重新初始化失败: %s", esp_err_to_name(deinit_err));
                } else {
                    // ★ deinit 后事件回调会丢失，必须重新注册
                    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiOta::wifiEventHandler, NULL);
                    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &WifiOta::wifiEventHandler, NULL);
                    DEBUG_INFO("WIFI", "WiFi/IP 事件回调已重新注册");
                }
            }
            WifiOta::instance().m_wifiMode = OTA_WIFI_MODE_AP;
            instance().initAp();
        }
    }
    // AP 模式：wifi_early_init 已完成，无需额外操作

    esp_err_t err = instance().httpServerStart();
    if (err != ESP_OK) {
        DEBUG_ERR("WIFI", "HTTP 服务器启动失败");
    }

    // ★ WiFi 已连接，UDP 从这里开始能通
    // 等待网络栈稳定（ARP 表建立、路由就绪），避免首批 UDP 包丢失
    vTaskDelay(pdMS_TO_TICKS(500));

    // 启动摘要，发两遍确保上位机收到（第一包可能触发 ARP 被丢）
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    for (int i = 0; i < 2; i++) {
        DEBUG_INFO("WIFI", "========================================");
        DEBUG_INFO("WIFI", "[启动摘要] ESP32-C3 OTA Relay v%s", FW_VERSION);
        DEBUG_INFO("WIFI", "[启动摘要] Chip: %s, Rev %d", CONFIG_IDF_TARGET, chip.revision);
        DEBUG_INFO("WIFI", "[启动摘要] Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
        DEBUG_INFO("WIFI", "[启动摘要] WiFi: %s", WifiOta::getMode() == OTA_WIFI_MODE_STA ? "STA" : "AP");
        DEBUG_INFO("WIFI", "[启动摘要] NVS + WiFi driver + netif + event handler 初始化完成");
        DEBUG_INFO("WIFI", "[启动摘要] HTTP 服务器端口: %d", HTTP_SERVER_PORT);
        DEBUG_INFO("WIFI", "========================================");
        DEBUG_INFO("WIFI", "=== WiFi OTA 服务器就绪 ===");
        if (i == 0) vTaskDelay(pdMS_TO_TICKS(200));
    }

    // WiFi 状态监控
    // 每秒一次循环：每 10 秒检查 RSSI
    uint32_t tick = 0;
    while (1) {
        tick++;

        // ★ BLE OTA 传输期间，暂停 WiFi RSSI 轮询，避免 esp_wifi_sta_get_ap_info()
        //   触发射频活动干扰 BLE 连接事件
        if (WifiOta::instance().m_bleOtaActive.load()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // 每 10 秒检查一次 RSSI
        if (tick % 10 == 0) {
            int8_t rssi = WifiOta::getRssi();
            if (rssi != -127) {
                HealthMonitor::instance().updateFromWifiRssi(rssi);

                if (rssi < WIFI_RSSI_CRITICAL) {
                    DEBUG_WARN("WIFI", "WiFi RSSI 危险: %d dBm (阈值: %d)", rssi, WIFI_RSSI_CRITICAL);
                } else if (rssi < WIFI_RSSI_WARNING) {
                    DEBUG_WARN("WIFI", "WiFi RSSI 较弱: %d dBm", rssi);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

