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
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "lwip/netdb.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_event.h"
#include "esp_mac.h"

#include "app_config.h"
#include "ota_mutex.h"
#include "wifi_ota.h"
#include "esp32_ota_core.h"
#include "ota_coordinator.h"
#include "ota_relay.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"
#include "spi_master.h"
#include "ble_ota.h"
#include "cJSON.h"



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

static constexpr uint32_t GD32_OTA_FLASH_SECTOR_SIZE = 4096U;

// ★ OTA 接收共享缓冲区：由 OtaCoordinator 互斥串行化，无并发风险，避免频繁 malloc/free 导致堆碎片化
static uint8_t s_ota_recv_buf[OTA_BLOCK_SIZE];

/**
  * @brief 按指定对齐值向上取整。
 * @param value 待对齐的值。
 * @param alignment 对齐字节数。
 * @return 对齐后的值。
 */
static uint32_t align_up_u32(uint32_t value, uint32_t alignment)
{
    return ((value + alignment - 1U) / alignment) * alignment;
}

/**
 * @brief 预先声明：为 STA 模式配置静态 IP、网关和子网掩码。
 */

/**
  * @brief 为 HTTP 响应添加跨域访问控制头。
 * @param req HTTP 请求对象。
 */
static void http_set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
}

/**
  * @brief 处理 HTTP OPTIONS 预检请求，返回 204。
 * @param req HTTP 请求对象。
 * @return ESP_OK。
 */
static esp_err_t http_options_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ----------------------------------------
// STA 后台重连任务（避免在系统事件回调中调用 esp_wifi_connect 死锁）
// ----------------------------------------
/**
 * @brief 独立 FreeRTOS 任务，处理 STA 连接/重连的退避延迟和 esp_wifi_connect() 调用。
 *        事件回调仅通过 xTaskNotifyGive 唤醒本任务，不直接在系统事件任务中阻塞或调用 WiFi API。
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
 * @return ESP_OK 成功，其他失败。
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
 * @param len 缓冲区长度。
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



/**
 * @brief HTTP POST /ota/update - 完成 ESP32 OTA 并重启
 *   verify_only=true: 仅校验 SHA-256，不设置启动分区，不重启
 *   verify_only=false或缺省: 完整流程（校验 + 设置启动分区 + 重启）
 *
 * 实际的 end/verify/set_boot/restart 逻辑全部委托给 Esp32OtaCore。
 * @param req
 * @return
 */
static esp_err_t esp32_ota_update_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    Esp32OtaCore &core = Esp32OtaCore::instance();
    if (!core.isActive()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA not started");
        return ESP_FAIL;
    }

    // 读取请求体，获取前端 SHA-256 和 verify_only 标志
    char frontend_sha_hex[65] = {0};
    bool has_frontend_sha = false;
    bool verify_only = false;
    if (req->content_len > 0 && req->content_len < 256) {
        char buf[256] = {0};
        int ret = httpd_req_recv(req, buf, req->content_len);
        if (ret > 0) {
            buf[ret] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *sha_item = cJSON_GetObjectItem(root, "sha256");
                if (sha_item && cJSON_IsString(sha_item)) {
                    const char *sha_str = cJSON_GetStringValue(sha_item);
                    if (sha_str && strlen(sha_str) == 64) {
                        strncpy(frontend_sha_hex, sha_str, 64);
                        frontend_sha_hex[64] = '\0';
                        has_frontend_sha = true;
                        DEBUG_INFO("OTA", "Frontend SHA-256: %s", frontend_sha_hex);
                    }
                }
                cJSON *verify_item = cJSON_GetObjectItem(root, "verify_only");
                if (verify_item && cJSON_IsBool(verify_item)) {
                    verify_only = cJSON_IsTrue(verify_item);
                }
                cJSON_Delete(root);
            }
        }
    }

    char device_sha_hex[65] = {0};

    // ── verify_only 模式：只计算并返回设备端 SHA，不 end/set_boot/reboot ──
    if (verify_only) {
        DEBUG_INFO("OTA", "ESP32 OTA verify-only mode, %u bytes", (unsigned)core.getReceived());
        esp_err_t sha_err = core.computeDeviceSha(device_sha_hex);
        if (sha_err != ESP_OK) {
            DEBUG_ERR("OTA", "verify-only: SHA 计算失败: %s", esp_err_to_name(sha_err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(sha_err));
            return ESP_FAIL;
        }
        char ok_resp[256] = {0};
        snprintf(ok_resp, sizeof(ok_resp),
                 "{\"status\": \"ok\", \"message\": \"SHA-256 computed\", \"sha256\": \"%s\"}",
                 device_sha_hex);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, ok_resp, strlen(ok_resp));
        return ESP_OK;
    }

    // ── 完整模式：收尾+重启序列由协调器统一封装 ──
    //   关键：reboot=false 行为由 finalizeForReboot 内部封装，
    //   响应发回前端后再由 restartEsp32() 统一通知+delay+esp_restart。
    DEBUG_INFO("OTA", "ESP32 OTA finalize, %u bytes...", (unsigned)core.getReceived());
    Esp32FinalizeResult fr = OtaCoordinator::instance().finalizeForReboot(
        has_frontend_sha ? frontend_sha_hex : nullptr);

    if (fr.err == ESP_OK) {
        // 1) 把包含设备端 SHA 的响应发回前端
        char resp[320] = {0};
        snprintf(resp, sizeof(resp),
                 "{\"status\": \"ok\", \"message\": \"OTA finished, rebooting\", "
                 "\"sha256\": \"%s\"}",
                 fr.device_sha_hex);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));

        // 2) 响应发完后由协调器统一重启（notify + delay + esp_restart）
        DEBUG_INFO("OTA", "响应已发送，重启...");
        OtaCoordinator::instance().restartEsp32();   // 不返回
        return ESP_OK;
    }

    // 失败：core 已内部清理（abort + mutex release）
    if (fr.err == ESP_ERR_INVALID_VERSION) {
        char err_resp[256] = {0};
        snprintf(err_resp, sizeof(err_resp),
                 "{\"status\": \"error\", \"message\": \"SHA-256 mismatch\", \"frontend\": \"%s\", \"device\": \"%s\"}",
                 frontend_sha_hex, fr.device_sha_hex);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, err_resp, strlen(err_resp));
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(fr.err));
    }
    return ESP_FAIL;
}

/**
 * @brief HTTP POST /restart - 立即重启 ESP32
 * @param req
 * @return
 */
static esp_err_t restart_handler(httpd_req_t *req)
{
    (void)req;
    http_set_cors_headers(req);

    DEBUG_INFO("OTA", "收到重启请求，重启...");

    const char resp[] = "{\"status\": \"ok\", \"message\": \"rebooting\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    OtaCoordinator::instance().restartEsp32();   // 内部 notify + delay + esp_restart，不返回
    return ESP_OK;
}

// ----------------------------------------
// 从 HTTP 流中读取完整的 JSON 头（以 } 结尾）
/**
 * @brief 返回 JSON 字符串长度，失败返回 0
 * @param req
 * @param buf
 * @param buf_size
 * @return
 */
static int recv_json_header(httpd_req_t *req, char *buf, int buf_size)
{
    int total = 0;
    int depth = 1;  // 开头的 '{' 已被 peek 消费，从 1 开始
    while (total < buf_size - 1) {
        int ret = httpd_req_recv(req, buf + total, 1);  // 逐字节读，精确找到 JSON 结尾
        if (ret <= 0) return total;
        total += ret;
        buf[total] = '\0';
        if (buf[total - 1] == '{') depth++;
        else if (buf[total - 1] == '}') {
            depth--;
            if (depth == 0) return total;  // JSON 头读完
        }
    }
    return total;  // fallback: return consumed bytes
}

// ----------------------------------------
// 统一 /ota/upload 处理器（自动识别目标）
/**
   * @brief HTTP 文件上传处理器，接收固件并写入 Flash。
 * @param req
 * @return ESP_OK 成功，其他失败。
 */
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    // ★ 入口追踪：用于确认 HTTP 请求是否真的到达设备（排查"前端点了但设备没反应"）
    DEBUG_WARN("WIFI", "=== HTTP: POST /ota/upload === content_len=%d", req->content_len);
    DEBUG_INFO("WIFI", "收到固件上传请求, 内容长度: %d 字节", req->content_len);

    // ★ WiFi OTA 入口：强制恢复 WIFI_PS_NONE，防止 BLE OTA 残留省电模式导致断连
    esp_wifi_set_ps(WIFI_PS_NONE);
    WifiOta::instance().setBleOtaActive(false);

    // 预读 1 字节判断是 JSON 还是 binary
    char peek;
    int peek_ret = httpd_req_recv(req, &peek, 1);
    if (peek_ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败");
        return ESP_FAIL;
    }

    bool is_esp32_ota = false;
    uint32_t total_size = 0;
    int json_header_len = 0;
    uint8_t pre_read_buf[256] = {0};
    int pre_read_len = 1;
    pre_read_buf[0] = (uint8_t)peek;

    if (peek == '{') {
        // JSON 模式：逐字节读完 JSON 头
        char json_buf[256];
        json_buf[0] = peek;
        json_header_len = recv_json_header(req, json_buf + 1, sizeof(json_buf) - 1);
        if (json_header_len > 0) {
            json_header_len++;  // 加上已读的 peek 字节
            json_buf[json_header_len] = '\0';
            pre_read_len = json_header_len;
            if (pre_read_len > (int)sizeof(pre_read_buf)) pre_read_len = (int)sizeof(pre_read_buf);
            memcpy(pre_read_buf, json_buf, pre_read_len);

            cJSON *root = cJSON_Parse(json_buf);
            if (root) {
                cJSON *type_item = cJSON_GetObjectItem(root, "type");
                if (type_item && cJSON_IsString(type_item) &&
                    strcmp(cJSON_GetStringValue(type_item), "esp32") == 0) {
                    is_esp32_ota = true;
                    DEBUG_INFO("WIFI", "检测到 ESP32 OTA, JSON 头长度: %d 字节", json_header_len);
                }
                cJSON *size_item = cJSON_GetObjectItem(root, "total_size");
                if (size_item) {
                    total_size = (uint32_t)cJSON_GetNumberValue(size_item);
                }
                cJSON_Delete(root);
            }
        }
    }

    if (is_esp32_ota) {
        // --- ESP32 OTA ---
        if (total_size == 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing total_size");
            return ESP_FAIL;
        }

        // ★ 通过协调器发起（内部已含 forceCleanup 重试）
        esp_err_t err = OtaCoordinator::instance().beginEsp32OtaWithRetry(total_size);
        if (err != ESP_OK) {
            if (err == ESP_ERR_INVALID_STATE) {
                httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "ESP32 OTA occupied by another channel");
            } else {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
            }
            return ESP_FAIL;
        }

        Esp32OtaCore &core = Esp32OtaCore::instance();

        // ★ 使用文件级 static 缓冲区，避免堆碎片化（HTTP server 栈仅 8KB）
        uint8_t *fw_buf = s_ota_recv_buf;

        size_t total_firmware_written = 0;
        size_t total_received = json_header_len;  // 已消费的字节数
        int last_percent = 0;

        while (total_received < req->content_len) {
            size_t to_read = req->content_len - total_received;
            if (to_read > OTA_BLOCK_SIZE) to_read = OTA_BLOCK_SIZE;

            int ret = httpd_req_recv(req, (char *)fw_buf, to_read);
            if (ret <= 0) {
                DEBUG_ERR("WIFI", "HTTP recv failed (ret=%d), aborting OTA", ret);
                core.abort();
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "HTTP recv failed");
                return ESP_FAIL;
            }

            err = core.write(fw_buf, (size_t)ret);
            if (err != ESP_OK) {
                DEBUG_ERR("WIFI", "OTA write failed: %s", esp_err_to_name(err));
                core.abort();
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
                return ESP_FAIL;
            }

            total_firmware_written += ret;
            total_received += ret;

            if (total_size > 0) {
                int percent = (int)((total_firmware_written * 100) / total_size);
                if (percent / 10 > last_percent / 10) {
                    last_percent = percent;
                    DEBUG_INFO("WIFI", "ESP32 OTA 进度: %u / %u (%d%%)",
                               (unsigned int)total_firmware_written, total_size, percent);
                }
            }
        }

        DEBUG_INFO("WIFI", "ESP32 OTA 存储完成: %u 字节 (跳过 JSON 头 %d 字节)", (unsigned int)core.getReceived(), json_header_len);
        const char resp[] = "{\"status\": \"ok\", \"message\": \"stored, call POST /ota/update to finish\", \"target\": \"esp32\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }

    // --- GD32 OTA（原逻辑）---
    const uint32_t gd32_fw_size = (uint32_t)req->content_len;
    // 网页上传协议：JSON 头 + raw binary。raw binary 大小优先取 JSON 中的 total_size，否则按 content_len - json_header_len 计算。
    uint32_t gd32_valid_size;
    if (json_header_len > 0 && (uint32_t)json_header_len < gd32_fw_size) {
        gd32_valid_size = (total_size > 0 && total_size < gd32_fw_size) ? total_size : (gd32_fw_size - (uint32_t)json_header_len);
    } else {
        gd32_valid_size = gd32_fw_size;
    }
    const uint32_t gd32_rounded_size = (uint32_t)(((gd32_valid_size + (uint32_t)GD32_OTA_PACKET_SIZE - 1U) / (uint32_t)GD32_OTA_PACKET_SIZE) * (uint32_t)GD32_OTA_PACKET_SIZE);
    if (gd32_valid_size == 0U || gd32_valid_size > OTA_MAX_FIRMWARE_SIZE || gd32_rounded_size > OTA_MAX_FIRMWARE_SIZE) {
        DEBUG_ERR("WIFI", "firmware size invalid: valid=%u rounded48=%u max=%u",
                  (unsigned)gd32_valid_size, (unsigned)gd32_rounded_size, (unsigned)OTA_MAX_FIRMWARE_SIZE);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "firmware too large or alignment overflow");
        return ESP_FAIL;
    }
    if (OtaRelay::getState() != RELAY_IDLE) {
        DEBUG_WARN("WIFI", "GD32 OTA 开始前复位中继状态: %d", OtaRelay::getState());
        OtaRelay::instance().reset();
    }

    // ★ 通过全局协调器注册 GD32 存储开始
    if (OtaCoordinator::instance().beginGd32Storage(gd32_valid_size) != ESP_OK) {
        DEBUG_ERR("WIFI", "GD32 OTA 协调器拒绝存储（系统忙）");
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "system busy with another OTA operation");
        return ESP_FAIL;
    }

    WifiOta::instance().stateLock();
    WifiOta::instance().setGd32StorageDone(false);
    WifiOta::instance().setRelayTriggered(false);
    WifiOta::instance().stateUnlock();

    const esp_partition_t *gd32_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "gd32_ota");
    if (!gd32_partition) {
        DEBUG_ERR("WIFI", "找不到 gd32_ota 分区");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "找不到 gd32_ota 分区");
        return ESP_FAIL;
    }
    const uint32_t gd32_erase_size = align_up_u32(gd32_valid_size, GD32_OTA_FLASH_SECTOR_SIZE);
    if (gd32_erase_size > gd32_partition->size) {
        DEBUG_ERR("WIFI", "gd32_ota 分区空间不足: erase=%u partition=%u",
                  (unsigned)gd32_erase_size, (unsigned)gd32_partition->size);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "gd32_ota partition too small");
        return ESP_FAIL;
    }

    DEBUG_INFO("WIFI", "擦除 gd32_ota 分区: %u bytes (fw=%u, partition=%u)",
               (unsigned)gd32_erase_size, (unsigned)gd32_valid_size, (unsigned)gd32_partition->size);
    esp_err_t erase_err = esp_partition_erase_range(gd32_partition, 0, gd32_erase_size);
    if (erase_err != ESP_OK) {
        DEBUG_ERR("WIFI", "擦除 gd32_ota 分区失败: %s", esp_err_to_name(erase_err));
        char err_msg[96];
        snprintf(err_msg, sizeof(err_msg), "erase gd32_ota failed: %s", esp_err_to_name(erase_err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err_msg);
        return ESP_FAIL;
    }
    OtaRelay::metaClear();

    uint8_t *buf = s_ota_recv_buf;

    int received = 0;

    // GD32 路径与网页上传协议对齐：网页上传的是 raw binary（不带 6B 头）。
    uint32_t expected_fw_size_raw = gd32_valid_size;
    ota_meta_t meta_raw = {
        .total_size = expected_fw_size_raw,
        .stored_size = 0,
        .last_seq = 0,
        .checksum = {0}
    };
    OtaRelay::metaSave(&meta_raw);
    uint32_t gd32_offset = 0;

    DEBUG_INFO("WIFI", "GD32 raw upload start: %u bytes", expected_fw_size_raw);

    // JSON 头已解析完毕，pre_read_buf 中的 JSON 字节不是固件数据，不能写入分区。
    // 仅在纯二进制模式（无 JSON 头）时写入 peek 的首字节。
    if (json_header_len == 0 && pre_read_len > 0) {
        int first_chunk_len = pre_read_len;
        if (first_chunk_len > req->content_len) first_chunk_len = req->content_len;
        if (first_chunk_len > 0) {
            gd32_offset = 0;
            esp_err_t err = OtaRelay::flashWriteChunk(gd32_offset, pre_read_buf, (size_t)first_chunk_len);
            if (err != ESP_OK) {
                DEBUG_ERR("WIFI", "GD32 first chunk write failed: len=%d err=%s",
                          first_chunk_len, esp_err_to_name(err));
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write gd32_ota failed");
                return ESP_FAIL;
            }
            received = first_chunk_len;
            gd32_offset += (uint32_t)first_chunk_len;
            OtaRelay::metaUpdate(gd32_offset, (uint32_t)(gd32_offset / OTA_BLOCK_SIZE));
        }
    } else {
        received = pre_read_len;  // 跳过已消费的 JSON 头字节
    }

    while (received < req->content_len) {
        size_t to_read = req->content_len - received;
        if (to_read > OTA_BLOCK_SIZE) to_read = OTA_BLOCK_SIZE;

        int ret = httpd_req_recv(req, (char *)buf, to_read);
        if (ret <= 0) {
            DEBUG_ERR("WIFI", "GD32 upload interrupted: received=%d expected=%d",
                      received, req->content_len);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload interrupted");
            return ESP_FAIL;
        }

        esp_err_t err = OtaRelay::flashWriteChunk(gd32_offset, buf, (size_t)ret);
        if (err != ESP_OK) {
            DEBUG_ERR("WIFI", "GD32 write failed: offset=%u len=%d err=%s",
                      (unsigned)gd32_offset, ret, esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write gd32_ota failed");
            return ESP_FAIL;
        }

        received += ret;
        gd32_offset += (uint32_t)ret;
    }

    // update meta with written size
    OtaRelay::metaUpdate(gd32_offset, (uint32_t)(gd32_offset / OTA_BLOCK_SIZE));


    DEBUG_INFO("WIFI", "GD32 upload done: %u / %u bytes", (unsigned)gd32_offset, expected_fw_size_raw);

    ota_meta_t final_meta;
    OtaRelay::metaLoad(&final_meta);
    if (final_meta.total_size > 0 && final_meta.stored_size >= final_meta.total_size) {
        // ★ 通知协调器 GD32 存储完成
        OtaCoordinator::instance().completeGd32Storage();
        WifiOta::instance().stateLock();
        WifiOta::instance().setGd32StorageDone(true);
        WifiOta::instance().stateUnlock();
        DEBUG_INFO("WIFI", "GD32 firmware fully stored, pre-computing SHA-256...");
        // 预计算 SHA-256（写入 NVS），由前端在 /ota/relay 时再校验通过后触发中继
        esp_err_t sha_err = OtaRelay::metaFinalizeChecksum();
        if (sha_err != ESP_OK) {
            DEBUG_ERR("WIFI", "GD32 固件 SHA-256 计算失败: %s", esp_err_to_name(sha_err));
        }
        // ★ 不再自动 triggerGd32Relay：安全闸门已统一在 /ota/relay 处理（带 SHA 校验）
    }

    const char resp[] = "{\"status\": \"ok\", \"progress\": 100, \"target\": \"gd32\", "
                       "\"message\": \"stored, call POST /ota/relay with sha256 to trigger\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/**
 * @brief HTTP POST /ota/relay 处理器
 *        必传 body: {"sha256": "<64-char hex>"}。
 *        校验通过后触发 GD32 中继；响应回传设备端 SHA（对齐 BLE 的 sendSha256Notify）。
 */
static esp_err_t ota_relay_trigger_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    DEBUG_INFO("WIFI", "=== HTTP: POST /ota/relay ===");
    DEBUG_INFO("WIFI", "收到触发 OTA 中继请求");

    // 1) 固件就绪检查
    ota_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    if (OtaRelay::metaLoad(&meta) != ESP_OK || meta.total_size == 0 || meta.stored_size < meta.total_size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "firmware not fully stored");
        return ESP_FAIL;
    }

    // 2) 解析前端 SHA-256（必须 64 字符 hex）
    char frontend_sha_hex[65] = {0};
    if (req->content_len > 0 && req->content_len < 512) {
        char buf[512] = {0};
        int ret = httpd_req_recv(req, buf, req->content_len);
        if (ret > 0) {
            buf[ret] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *sha_item = cJSON_GetObjectItem(root, "sha256");
                if (sha_item && cJSON_IsString(sha_item) &&
                    strlen(cJSON_GetStringValue(sha_item)) == 64) {
                    strncpy(frontend_sha_hex, cJSON_GetStringValue(sha_item), 64);
                    frontend_sha_hex[64] = '\0';
                }
                cJSON_Delete(root);
            }
        }
    }
    if (frontend_sha_hex[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            "missing sha256 in body (frontend SHA-256 required)");
        return ESP_FAIL;
    }

    // 3) hex(64) → 32 字节
    uint8_t frontend_sha[32] = {0};
    for (int i = 0; i < 32; ++i) {
        char hex[3] = { frontend_sha_hex[i*2], frontend_sha_hex[i*2+1], 0 };
        frontend_sha[i] = (uint8_t)strtoul(hex, nullptr, 16);
    }

    // 4) 共享 helper：计算设备端 SHA → 比对 → 回传 device_sha（无论是否匹配都回传）
    uint8_t device_sha[32] = {0};
    char device_sha_hex[65] = {0};
    esp_err_t verify_err = OtaRelay::verifyFrontendSha(frontend_sha, device_sha);
    for (int i = 0; i < 32; ++i) {
        snprintf(&device_sha_hex[i*2], 3, "%02X", device_sha[i]);
    }

    if (verify_err == ESP_ERR_INVALID_CRC) {
        char err_resp[256];
        snprintf(err_resp, sizeof(err_resp),
                 "{\"status\": \"error\", \"message\": \"SHA-256 mismatch\", "
                 "\"device\": \"%s\"}", device_sha_hex);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, err_resp, strlen(err_resp));
        return ESP_FAIL;
    } else if (verify_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SHA compute failed");
        return ESP_FAIL;
    }

    // 5) 校验通过 → 触发中继
    esp_err_t trigger_err = OtaCoordinator::instance().triggerGd32Relay();
    if (trigger_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            "relay trigger failed - system busy or firmware not ready");
        return ESP_FAIL;
    }

    // 6) 成功响应回传设备端 SHA（对齐 BLE 的 sendSha256Notify）
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"status\": \"ok\", \"message\": \"OTA relay triggered\", "
             "\"sha256\": \"%s\"}", device_sha_hex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    return ESP_OK;
}

/**
 * @brief HTTP GET /logs - 拉取并清空日志缓冲区
 * @param req
 * @return
 */
static esp_err_t logs_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);

    int json_len = 0;
    char *json = DebugLog::instance().ringDrainJson(&json_len);

    if (json && json_len > 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json, json_len);
        free(json);
    } else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", 2);
    }
    return ESP_OK;
}

// HTTP POST /can/send 处理器
// 上位机发送 CAN 帧，经 ESP32 转发到 GD32
// 请求体: {"ch": 0, "id": 123456, "data": "0102030405060708", "dlc": 8}
/**
 * @brief data 为 hex 字符串（可不填，自动用 dlc 决定实际发送长度）
 * @param req
 * @return
 */
static esp_err_t can_send_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "接收数据失败");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 解析失败");
        return ESP_FAIL;
    }

    cJSON *ch_item = cJSON_GetObjectItem(root, "ch");
    cJSON *id_item = cJSON_GetObjectItem(root, "id");
    cJSON *data_item = cJSON_GetObjectItem(root, "data");
    cJSON *dlc_item = cJSON_GetObjectItem(root, "dlc");

    if (!ch_item || !id_item) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 ch 或 id 字段");
        return ESP_FAIL;
    }

    can_channel_t ch = (can_channel_t)cJSON_GetNumberValue(ch_item);
    uint32_t ext_id = (uint32_t)cJSON_GetNumberValue(id_item);
    uint8_t dlc = dlc_item ? (uint8_t)cJSON_GetNumberValue(dlc_item) : 0;

    uint8_t tx_data[CAN_MAX_DLC] = {0};
    uint8_t tx_dlc = 0;

    if (data_item && cJSON_IsString(data_item)) {
        const char *hex_str = cJSON_GetStringValue(data_item);
        size_t hex_len = strlen(hex_str);
        for (size_t i = 0; i < hex_len / 2 && i < CAN_MAX_DLC; i++) {
            char byte_str[3] = { hex_str[i * 2], hex_str[i * 2 + 1], '\0' };
            tx_data[i] = (uint8_t)strtol(byte_str, NULL, 16);
            tx_dlc++;
        }
    }

    if (tx_dlc == 0 && dlc > 0) {
        tx_dlc = (dlc > CAN_MAX_DLC) ? CAN_MAX_DLC : dlc;
    }
    cJSON_Delete(root);

    // 发送 CAN 帧到 GD32
    uint8_t resp_data[CAN_MAX_DLC] = {0};
    int resp_len = SpiMaster::instance().canSend(ch, ext_id, tx_data, tx_dlc,
                                   resp_data, sizeof(resp_data), 1000);

    char resp[512];
    if (resp_len >= 0) {
        // 将响应 data 转为 hex 字符串
        char resp_hex[CAN_MAX_DLC * 2 + 1] = {0};
        for (int i = 0; i < resp_len; i++) {
            sprintf(&resp_hex[i * 2], "%02X", resp_data[i]);
        }
        snprintf(resp, sizeof(resp),
            "{\"status\": \"ok\", \"ch\": %d, \"id\": %lu, \"sent\": %d, \"recv_len\": %d, \"recv\": \"%s\"}",
            ch, (unsigned long)ext_id, tx_dlc, resp_len, resp_hex);
    } else {
        snprintf(resp, sizeof(resp),
            "{\"status\": \"error\", \"code\": %d}", resp_len);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

// HTTP POST /spi/send 处理器
// 网页传 type + payload，ESP32 自动构建 SOF+TYPE+LEN(L/H)+PAYLOAD+CRC 帧
// 并通过 spi_ota_transfer 完成完整事务（发送+轮询响应帧+解析）。
//
// 请求体: {"type": "01", "data": "01020304", "timeout_ms": 1000}
//   type: 帧类型 (hex 1 字节，如 01=REQ, 02=DATA, B0=BOOT)
//   data: 负载数据 (hex 字符串，可选)
//   timeout_ms: 超时毫秒 (可选, 默认 1000)
/**
 * @brief HTTP POST /spi/send 处理器，转发上位机请求到 SPI 并返回结果。
 * @param req HTTP 请求对象。
 * @return ESP_OK 成功，ESP_FAIL 表示请求处理失败。
 */
static esp_err_t spi_send_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    char buf[512] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "接收数据失败");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 解析失败");
        return ESP_FAIL;
    }

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    cJSON *data_item = cJSON_GetObjectItem(root, "data");
    cJSON *timeout_item = cJSON_GetObjectItem(root, "timeout_ms");

    if (!type_item || !cJSON_IsString(type_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 type 字段");
        return ESP_FAIL;
    }
    const char *type_hex = cJSON_GetStringValue(type_item);
    uint8_t frame_type = (uint8_t)strtol(type_hex, NULL, 16);

    uint32_t timeout_ms = timeout_item ? (uint32_t)cJSON_GetNumberValue(timeout_item) : 1000;

    // 解析 payload (hex 字符串 → 字节数组)
    uint8_t payload_data[SPI_OTA_FRAME_MAX] = {0};
    size_t payload_len = 0;
    if (data_item && cJSON_IsString(data_item)) {
        const char *hex_str = cJSON_GetStringValue(data_item);
        size_t hex_len = strlen(hex_str);
        for (size_t i = 0; i < hex_len / 2 && i < SPI_OTA_FRAME_MAX; i++) {
            char byte_str[3] = { hex_str[i * 2], hex_str[i * 2 + 1], '\0' };
            payload_data[i] = (uint8_t)strtol(byte_str, NULL, 16);
            payload_len++;
        }
    }

    DEBUG_INFO("SPI", "WEB SPI: type=0x%02X, payload=%zu 字节",
               frame_type, payload_len);
    cJSON_Delete(root);

    char resp[1024];
    uint8_t resp_type = 0;
    uint8_t resp_payload[SPI_OTA_FRAME_MAX] = {0};
    size_t resp_len = 0;
    esp_err_t spi_err = SpiMaster::instance().otaTransfer(frame_type, payload_data, payload_len,
                                         &resp_type, resp_payload, sizeof(resp_payload), &resp_len,
                                         timeout_ms);

    if (spi_err == ESP_OK) {
        char resp_hex[SPI_OTA_FRAME_MAX * 2 + 1] = {0};
        for (size_t i = 0; i < resp_len; i++) {
            sprintf(&resp_hex[i * 2], "%02X", resp_payload[i]);
        }
        snprintf(resp, sizeof(resp),
            "{\"status\": \"ok\", \"type\": \"%02X\", \"payload_len\": %zu, \"resp_type\": \"%02X\", \"resp_len\": %zu, \"recv\": \"%s\"}",
            frame_type, payload_len, resp_type, resp_len, resp_hex);
        DEBUG_INFO("SPI", "WEB SPI 响应: type=0x%02X len=%zu payload=%s",
                   resp_type, resp_len, resp_hex);
    } else {
        snprintf(resp, sizeof(resp),
            "{\"status\": \"error\", \"code\": %d, \"message\": \"%s\"}",
            spi_err, esp_err_to_name(spi_err));
        DEBUG_ERR("SPI", "WEB SPI 失败: %s", esp_err_to_name(spi_err));
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static constexpr uint32_t INTERNAL_HEAP_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

struct TaskStackMetric {
    const char* name;
    TaskHandle_t handle;
    size_t stack_size_bytes;
};

static size_t get_task_stack_high_watermark_bytes(TaskHandle_t handle)
{
    if (handle == NULL) return 0;
    UBaseType_t highWaterMark = uxTaskGetStackHighWaterMark(handle);
    return (size_t)highWaterMark;
}

static size_t get_task_stack_peak_used_bytes(TaskHandle_t handle, size_t stack_size_bytes)
{
    size_t high_watermark = get_task_stack_high_watermark_bytes(handle);
    return (stack_size_bytes >= high_watermark) ? (stack_size_bytes - high_watermark) : 0;
}

/**
 * @brief 追加 JSON 内容到响应缓冲区，自动处理 snprintf 截断。
 */
static int append_jsonf(char* buf, size_t cap, int offset, const char* fmt, ...)
{
    if (!buf || cap == 0 || offset < 0 || (size_t)offset >= cap) return offset;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + offset, cap - (size_t)offset, fmt, args);
    va_end(args);
    if (n < 0) return offset;
    size_t written = (size_t)n;
    if (offset + (int)written >= (int)cap) {
        buf[cap - 1] = '\0';
        return (int)(cap - 1);
    }
    return offset + (int)written;
}

/**
 * @brief HTTP GET /system/status 处理器
 * @param req
 * @return
 */
static esp_err_t system_status_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);

    char ip_str[16] = "0.0.0.0";
    int8_t rssi = -127;
    bool connected = false;

    if (WifiOta::instance().m_wifiMode == OTA_WIFI_MODE_STA && WifiOta::instance().getStaConnected()) {
        esp_netif_ip_info_t ip_info;
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }
        connected = true;
    }
    gd32_version_t pingVer = {};
    OtaRelay::getGd32Version(&pingVer);

    /* 若尚无 GD32 版本信息且 OTA 中继空闲，尝试 SPI PING 获取（最多 5 次） */
    // ★ 修复：OTA 活跃期间（存储/中继/ESP32 OTA）跳过 appPing()，
    //   避免 HTTP 任务长期持有 SPI mutex 与 relay 任务争用，
    //   导致优先级继承异常 → vTaskPriorityDisinheritAfterTimeout assert。
    ota_system_state_t curSysState = OtaCoordinator::instance().getSystemState();
    bool ota_busy = (curSysState == OTA_SYS_GD32_STORING ||
                     curSysState == OTA_SYS_GD32_RELAYING ||
                     curSysState == OTA_SYS_ESP32_RECEIVING ||
                     curSysState == OTA_SYS_ESP32_FINALIZING ||
                     curSysState == OTA_SYS_ESP32_REBOOTING);
    if (!ota_busy &&
        pingVer.app_major == 0 &&
        pingVer.boot_major == 0 &&
        OtaRelay::getState() == RELAY_IDLE) {
        static uint32_t last_ping_ms = 0;
        static uint8_t  ping_attempts = 0;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (ping_attempts < 5 && now_ms - last_ping_ms > 5000) {  /* 5 秒节流 */
            last_ping_ms = now_ms;
            ++ping_attempts;
            uint32_t uptime = 0;
            gd32_version_t ver = {};
            if (SpiMaster::instance().appPing(500, &uptime, &ver) == ESP_OK) {
                OtaRelay::setGd32AppAlive(true);
                OtaRelay::setGd32Version(ver);
                ping_attempts = 0;  /* 成功后重置，允许后续更新 */
            } else {
                OtaRelay::setGd32AppAlive(false);
                if (ping_attempts >= 5) {
                    DEBUG_WARN("STATUS", "GD32 版本号读取失败，已达最大重试次数 (5)，停止尝试");
                }
            }
        }
    }

    ota_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    OtaRelay::metaLoad(&meta);

    gd32_version_t snapVer2 = {};
    OtaRelay::getGd32Version(&snapVer2);
    ota_system_state_t sysState = OtaCoordinator::instance().getSystemState();
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    size_t largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    size_t internal_free_heap = heap_caps_get_free_size(INTERNAL_HEAP_CAPS);
    size_t internal_min_free_heap = heap_caps_get_minimum_free_size(INTERNAL_HEAP_CAPS);
    size_t internal_largest_free_block = heap_caps_get_largest_free_block(INTERNAL_HEAP_CAPS);
    size_t internal_heap_total = heap_caps_get_total_size(INTERNAL_HEAP_CAPS);
    size_t default_heap_total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    TaskStackMetric task_metrics[] = {
        { "http_server", xTaskGetCurrentTaskHandle(), HTTP_SERVER_STACK_SIZE },
        { "health_mon", HealthMonitor::instance().getTaskHandle(), 4096 },
        { "ota_relay", OtaRelay::getTaskHandle(), 6144 },
        { "wifi_ota", WifiOta::instance().getTaskHandle(), 8192 },
        { "wifi_reconnect", WifiOta::instance().m_reconnectTask, 4096 },
        { "ble_ota", BleOta::instance().getTaskHandle(), 4096 },
        { "ble_ota_worker", BleOta::instance().getWorkerTaskHandle(), 4096 }
    };

    char resp[4096];
    int len = snprintf(resp, sizeof(resp),
        "{\"version\": \"%s\", \"build_time\": \"%s\", \"git_hash\": \"%s\", \"health\": %d, "
        "\"free_heap\": %lu, \"min_free_heap\": %lu, \"largest_free_block\": %lu, "
        "\"internal_free_heap\": %lu, \"internal_min_free_heap\": %lu, \"internal_largest_free_block\": %lu, "
        "\"internal_heap_total\": %lu, \"default_heap_total\": %lu, "
        "\"temperature\": %.1f, \"uptime_sec\": %lu, "
        "\"wifi\": {\"mode\": \"%s\", \"ip\": \"%s\", \"rssi\": %d, \"connected\": %s}, "
        "\"ota\": {\"sys_state\": \"%s\", \"sys_state_code\": %d, \"total_size\": %lu, \"stored_size\": %lu, \"storage_done\": %s, "
        "\"relay_state\": %d, \"relay_seq\": %lu, \"relay_total_packets\": %lu, \"gd32_app_alive\": %s, "
        "\"gd32_app\": \"v%d.%d.%d.%d\", \"gd32_boot\": \"v%d.%d.%d.%d\"}, "
        "\"task_stack\": {",
        FW_VERSION, FW_BUILD_TIME, FW_GIT_HASH, HealthMonitor::instance().getCurrent(),
        (unsigned long)free_heap,
        (unsigned long)min_free_heap,
        (unsigned long)largest_free_block,
        (unsigned long)internal_free_heap,
        (unsigned long)internal_min_free_heap,
        (unsigned long)internal_largest_free_block,
        (unsigned long)internal_heap_total,
        (unsigned long)default_heap_total,
        (double)HealthMonitor::instance().getLastTemperature(),
        (unsigned long)(esp_timer_get_time() / 1000000),
        WifiOta::instance().m_wifiMode == OTA_WIFI_MODE_AP ? "AP" : "STA",
        ip_str,
        rssi,
        connected ? "true" : "false",
        OtaCoordinator::instance().getSystemStateString(),
        (int)sysState,
        (unsigned long)meta.total_size,
        (unsigned long)meta.stored_size,
        OtaCoordinator::instance().isGd32StorageDone() ? "true" : "false",
        OtaRelay::getState(),
        (unsigned long)OtaRelay::getRelaySeq(),
        (unsigned long)OtaRelay::getRelayTotalPackets(),
        OtaRelay::getGd32AppAlive() ? "true" : "false",
        snapVer2.app_major, snapVer2.app_minor,
        snapVer2.app_patch, snapVer2.app_build,
        snapVer2.boot_major, snapVer2.boot_minor,
        snapVer2.boot_patch, snapVer2.boot_build);

    for (size_t i = 0; i < sizeof(task_metrics) / sizeof(task_metrics[0]); ++i) {
        len = append_jsonf(resp, sizeof(resp), len,
            "%s\"%s\":{\"stack_size_bytes\":%lu,\"high_watermark_bytes\":%lu,\"peak_used_bytes\":%lu}",
            i == 0 ? "" : ",",
            task_metrics[i].name,
            (unsigned long)task_metrics[i].stack_size_bytes,
            (unsigned long)get_task_stack_high_watermark_bytes(task_metrics[i].handle),
            (unsigned long)get_task_stack_peak_used_bytes(task_metrics[i].handle, task_metrics[i].stack_size_bytes));
    }
    len = append_jsonf(resp, sizeof(resp), len, "}}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

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
        if (OtaRelay::metaLoad(&boot_meta) == ESP_OK &&
            boot_meta.total_size > 0 && boot_meta.stored_size >= boot_meta.total_size) {
            WifiOta::instance().setGd32StorageDone(true);
            DEBUG_INFO("WIFI", "从 NVS 恢复: 固件已就绪 (%u bytes)", boot_meta.total_size);
        }
    }

    httpd_uri_t upload_uri = {
        .uri = "/ota/upload",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &upload_uri);
    httpd_uri_t upload_options_uri = {
        .uri = "/ota/upload",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &upload_options_uri);

    httpd_uri_t relay_uri = {
        .uri = "/ota/relay",
        .method = HTTP_POST,
        .handler = ota_relay_trigger_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &relay_uri);
    httpd_uri_t relay_options_uri = {
        .uri = "/ota/relay",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &relay_options_uri);

    httpd_uri_t esp32_update_uri = {
        .uri = "/ota/update",
        .method = HTTP_POST,
        .handler = esp32_ota_update_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &esp32_update_uri);
    httpd_uri_t esp32_update_options_uri = {
        .uri = "/ota/update",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &esp32_update_options_uri);

    httpd_uri_t restart_uri = {
        .uri = "/restart",
        .method = HTTP_POST,
        .handler = restart_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &restart_uri);
    httpd_uri_t restart_options_uri = {
        .uri = "/restart",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &restart_options_uri);

    httpd_uri_t logs_uri = {
        .uri = "/logs",
        .method = HTTP_GET,
        .handler = logs_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &logs_uri);
    httpd_uri_t logs_options_uri = {
        .uri = "/logs",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &logs_options_uri);

    httpd_uri_t system_status_uri = {
        .uri = "/system/status",
        .method = HTTP_GET,
        .handler = system_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &system_status_uri);
    httpd_uri_t system_status_options_uri = {
        .uri = "/system/status",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &system_status_options_uri);

    // CAN 帧转发
    httpd_uri_t can_send_uri = {
        .uri = "/can/send",
        .method = HTTP_POST,
        .handler = can_send_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &can_send_uri);
    httpd_uri_t can_send_options_uri = {
        .uri = "/can/send",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &can_send_options_uri);

    // SPI 直接发送
    httpd_uri_t spi_send_uri = {
        .uri = "/spi/send",
        .method = HTTP_POST,
        .handler = spi_send_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &spi_send_uri);
    httpd_uri_t spi_send_options_uri = {
        .uri = "/spi/send",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(WifiOta::instance().m_httpServer, &spi_send_options_uri);

    DEBUG_INFO("WIFI", "注册端点: /ota/upload, /ota/relay, /ota/update, /restart, /logs, /system/status, /can/send, /spi/send");

    return ESP_OK;
}

// ----------------------------------------
// 公开 API
/**
  * @brief 通知 OTA 中继开始向 GD32 转发固件。
 */
void WifiOta::signalRelay()
{
    ota_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    if (OtaRelay::metaLoad(&meta) != ESP_OK || meta.total_size == 0 || meta.stored_size < meta.total_size) {
        DEBUG_WARN("WIFI", "GD32 固件未完整存储，拒绝触发转发: stored=%" PRIu32 " total=%" PRIu32,
                   meta.stored_size, meta.total_size);
        return;
    }

    // ★ 通过全局协调器触发中继
    esp_err_t err = OtaCoordinator::instance().triggerGd32Relay();
    if (err == ESP_OK) {
        WifiOta::instance().stateLock();
        WifiOta::instance().setRelayTriggered(true);
        WifiOta::instance().stateUnlock();
        DEBUG_INFO("WIFI", "OTA 中继已通过协调器触发");
    } else {
        DEBUG_ERR("WIFI", "OTA 中继触发失败: %s", esp_err_to_name(err));
        WifiOta::instance().setRelayTriggered(false);
    }
}

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

