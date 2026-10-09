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
#include "monitor/spi_speed_test.h"  /* [SPI-SPEEDTEST] 临时：可整行删除 */


// ----------------------------------------
// 文件作用域 static helper
// ----------------------------------------
/**
 * @brief 清除指定 NVS 命名空间下的全部键（erase_all + commit + close）。
 *        纯函数，内部不打日志，由调用方负责日志输出。
 * @param ns NVS 命名空间字符串（例如 "nvs.net80211"、"phy"）。
 * @return ESP_OK 成功；否则返回 nvs_open 的错误码。
 */
static esp_err_t clearNvsNamespace(const char *ns)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}


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
            // 推送 WiFi 连接状态给 HealthMonitor
            HealthMonitor::instance().setWifiConnected(true);
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*)event_data;
            inst.stateLock();
            inst.setStaConnected(false);
            bool exhausted = inst.getStaRetryExhausted();
            int retryCount = inst.getStaRetryCount();
            inst.stateUnlock();
            // 推送 WiFi 断开状态给 HealthMonitor
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
    /* FAST_SCAN：扫到目标 SSID 即停并连接（多 AP 同名时取先扫到的，可能非最强）。
     * 原 ALL_CHANNEL_SCAN 要把 13 信道全扫完才连，OTA 重启后页面恢复要多等 3~8s。
     * esp_restart 不清 nvs.net80211 缓存，驱动可复用上次 AP 信息，恢复更快。 */
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;
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

    /* 冷启动时在 wifi_start 前短延时 */
    if (WifiOta::instance().m_coldBoot) {
        DEBUG_INFO("WIFI", "冷启动: wifi_start 前等待 300ms");
        vTaskDelay(pdMS_TO_TICKS(300));
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
    SemaphoreHandle_t wait_sem = WifiOta::instance().m_staGotIpSem;
    const char *wait_name = "STA_GOT_IP";

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
    /* 路由上限：当前 18 个 main handler + 17 个唯一 path 的 OPTIONS = 35 次注册。
     * max_uri_handlers 必须 ≥ 该总数，否则超出部分被 httpd 静默丢弃
     *（httpd_register_uri_handler 返回 ESP_ERR_HTTPD_HANDLERS_FULL，下方已检查并报错）。
     * 曾因 =32 导致按注册顺序排在末尾的 /motor/ota/cancel(POST) 未注册，
     * 取消请求恒返回 404、httpd 其它读取被在途 SPI 事务短暂饿死后才回 404。
     * 取 48 留足后续新增端点余量（每槽仅一个指针，内存开销可忽略）。 */
    config.max_uri_handlers = 48;
    /* Socket 预算（无法“合并”httpd 内部 3 个 fd：listen + ctrl 对）：
     *
     *   LWIP 总池 CONFIG_LWIP_MAX_SOCKETS = 12
     *     ├─ httpd 内部固定 3（listen + ctrl socket 对）
     *     ├─ max_open_sockets = 业务并发（下面 want）
     *     └─ 余量：UDP 调试日志 1 + 偶发其它
     *
     * 约束：max_open_sockets <= LWIP_MAX_SOCKETS - 3 = 9
     *
     * 业务并发需要 6（前端三链路 motor/feedback/status + OTA 上传 + 操作 + 余量）：
     *   - motor.html 独立 3 条轮询链路（已剥离互不阻塞）
     *   - OTA 大上传占 1 条长连接
     *   - 双 tab / 偶发脚本 1 条
     *   - 余量 1
     * 4 太紧会触发 lru_purge 踢连接，表现为「请求挂起 + age 不动」。
     * lru_purge：满槽时踢最久空闲连接，避免 EMFILE 卡死。
     * 硬编码 7 在 LWIP=8 时会 ESP_ERR_INVALID_ARG → HTTP 起不来。 */
    {
        const int lwip_cap = CONFIG_LWIP_MAX_SOCKETS - 3;  // httpd 硬上限
        const int want = 6;  // 前端 3 链路 + OTA + 操作 + 余量
        config.max_open_sockets = (lwip_cap < want) ? ((lwip_cap > 1) ? lwip_cap : 1) : want;
    }
    /* 满连接时淘汰最久未用的空闲 socket（相当于“复用/腾挪”槽位） */
    config.lru_purge_enable = true;
    /* idle 收发超时（秒）；进行中的 handler 连接不受此 idle 检测踢掉。
     * 缩到 5s：WiFi 暴断时挂死连接更快释放槽位，避免长时占满 6 槽导致卡顿。 */
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    /* TCP keep-alive 探针：5s idle 无数据 → 每 1s 探测 3 次失败立即踢。
     * 不依赖 recv_wait_timeout（要等 socket 主动读失败），WiFi 半挂/暴断时
     * 探针能更快感知，槽位早释放。原默认 false 会让断连占槽到 5s 超时。 */
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 1;
    config.keep_alive_count = 3;

    DEBUG_INFO("WIFI", "=== 启动 HTTP OTA 服务器 ===");
    DEBUG_INFO("WIFI", "HTTP 服务器端口: %d", config.server_port);

    esp_err_t err = httpd_start(&WifiOta::instance().m_httpServer, &config);
    if (err != ESP_OK) {
        DEBUG_ERR("WIFI", "HTTP 服务器启动失败: %s", esp_err_to_name(err));
        return err;
    }

    // GD32 固件就绪真相源为 OtaCoordinator / NVS meta（/system/status），
    // 不再在 WifiOta 上维护影子 storage_done 标志。
    {
        ota_meta_t boot_meta;
        if (OtaCoordinator::instance().loadGd32Meta(&boot_meta) == ESP_OK &&
            boot_meta.total_size > 0 && boot_meta.stored_size >= boot_meta.total_size) {
            DEBUG_INFO("WIFI", "从 NVS 恢复: GD32 固件已就绪 (%u bytes)", boot_meta.total_size);
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
        {"/system/heap_low_log", HTTP_GET,  system_heap_low_log_handler},
        {"/can/send_batch", HTTP_POST, can_send_batch_handler},
        {"/can/send_burst", HTTP_POST, can_send_burst_handler},
        {"/can/feedback",  HTTP_GET,  can_feedback_handler},
        {"/can/mode",      HTTP_GET,  can_mode_handler},
        {"/can/mode",      HTTP_POST, can_mode_handler},
        {"/can/silent",    HTTP_POST, can_silent_handler},
        {"/spi/send",      HTTP_POST, spi_send_handler},
        {"/motor/state",   HTTP_GET,  motor_state_handler},
        {"/motor/version", HTTP_GET,  motor_version_handler},
        {"/motor/ota",       HTTP_POST, motor_ota_upload_handler},
        {"/motor/ota/relay", HTTP_POST, motor_ota_relay_handler},
        {"/motor/ota/status", HTTP_GET,  motor_ota_status_handler},
        {"/motor/ota/cancel", HTTP_POST, motor_ota_cancel_handler},
        {"/spi/speedtest",  HTTP_GET,  spi_speedtest_handler},  /* [SPI-SPEEDTEST] 临时：可整行删除 */
    };

    /* 同一 path 可有多 method（如 GET+POST）；OPTIONS 每个 path 只注册一次，
     * 否则 httpd 报 “handler ... method 6 already registered”。 */
    for (size_t i = 0; i < sizeof(kHttpRoutes) / sizeof(kHttpRoutes[0]); ++i) {
        const UriEntry &r = kHttpRoutes[i];
        httpd_uri_t main_uri = {};
        main_uri.uri = r.uri;
        main_uri.method = r.method;
        main_uri.handler = r.handler;
        main_uri.user_ctx = NULL;
        esp_err_t reg_err = httpd_register_uri_handler(WifiOta::instance().m_httpServer, &main_uri);
        if (reg_err != ESP_OK) {
            /* 静默丢弃会导致对应端点 404（如曾经的 /motor/ota/cancel）。
             * 明确报错，提示提高 max_uri_handlers。 */
            DEBUG_ERR("WIFI", "URI 注册失败 %s method=%d: %s（提高 max_uri_handlers）",
                      r.uri, (int)r.method, esp_err_to_name(reg_err));
        }

        bool need_opts = true;
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(kHttpRoutes[j].uri, r.uri) == 0) {
                need_opts = false;
                break;
            }
        }
        if (need_opts) {
            httpd_uri_t opts_uri = {};
            opts_uri.uri = r.uri;
            opts_uri.method = HTTP_OPTIONS;
            opts_uri.handler = http_options_handler;
            opts_uri.user_ctx = NULL;
            esp_err_t opts_err = httpd_register_uri_handler(WifiOta::instance().m_httpServer, &opts_uri);
            if (opts_err != ESP_OK) {
                DEBUG_ERR("WIFI", "OPTIONS 注册失败 %s: %s（提高 max_uri_handlers）",
                          r.uri, esp_err_to_name(opts_err));
            }
        }
    }

    DEBUG_INFO("WIFI", "HTTP端点已注册 (ota/can/spi/logs/status)");

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
// IBleCoexListener 接口实现
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
    DEBUG_INFO("WIFI", "=== WiFi 早期初始化 ===");

    // ★ 冷启动检测：判断是否为上电/欠压复位
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

    /* 冷启动：电源与外设上电完成后再初始化 NVS/WiFi */
    if (WifiOta::instance().m_coldBoot) {
        DEBUG_INFO("WIFI", "冷启动: 等待 400ms 后初始化 NVS/WiFi");
        vTaskDelay(pdMS_TO_TICKS(400));
    }

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
            DEBUG_WARN("WIFI", "NVS重建,PHY校准已清,将重校");
        }
        DEBUG_INFO("WIFI", "NVS 初始化成功");
    }

    /* 冷启动清理 WiFi 连接缓存；欠压复位后再清 PHY 强制重校准 */
    if (WifiOta::instance().m_coldBoot) {
        err = clearNvsNamespace("nvs.net80211");
        if (err == ESP_OK) {
            DEBUG_DBG("WIFI", "冷启动: 已清除 WiFi NVS 缓存 (nvs.net80211)");
        } else {
            DEBUG_INFO("WIFI", "冷启动: 无 WiFi NVS 缓存分区，跳过清除");
        }
        if (reset_reason == ESP_RST_BROWNOUT) {
            err = clearNvsNamespace("phy");
            if (err == ESP_OK) {
                DEBUG_WARN("WIFI", "欠压复位后: 已清除 PHY 校准数据");
            }
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

    WifiOta::instance().m_staGotIpSem = xSemaphoreCreateBinary();
    if (!WifiOta::instance().m_staGotIpSem) {
        DEBUG_ERR("WIFI", "创建信号量失败");
    }

    // 创建后台重连任务：避免在系统事件任务中调用 esp_wifi_connect()/vTaskDelay 导致死锁
    if (WifiOta::instance().m_reconnectTask == nullptr) {
        BaseType_t ret = xTaskCreate(&WifiOta::reconnectTask, "wifi_reconnect",
                                     TASK_STACK_WIFI_RECONNECT, NULL, 5,
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

    // ★ 开机清理：如果 NVS 中有不完整的 OTA 元数据（上次 OTA 中断），
    //   GD32 可能还卡在 Bootloader 中，导致 SPI 全面超时 → HTTP socket 饥荒。
    //   主动清除残留元数据并尝试恢复 GD32 APP。
    {
        ota_meta_t boot_meta;
        if (OtaCoordinator::instance().loadGd32Meta(&boot_meta) == ESP_OK &&
            boot_meta.total_size > 0 && boot_meta.stored_size < boot_meta.total_size) {
            DEBUG_WARN("WIFI", "不完整OTA缓存 stored=%u total=%u,清残留",
                       (unsigned)boot_meta.stored_size, (unsigned)boot_meta.total_size);
            OtaCoordinator::instance().clearGd32Meta();
            OtaCoordinator::instance().abortCurrent();
            // 给 GD32 时间从 Bootloader 超时退出并重启到 APP
            vTaskDelay(pdMS_TO_TICKS(3000));
            // 尝试探测 GD32 APP 是否恢复
            gd32_version_t ver = {};
            if (OtaCoordinator::instance().probeGd32AppOnce(&ver, nullptr) == ESP_OK) {
                DEBUG_INFO("WIFI", "GD32 APP 已恢复: v%d.%d.%d.%d",
                           ver.app_major, ver.app_minor, ver.app_patch, ver.app_build);
            } else {
                DEBUG_WARN("WIFI", "GD32 APP 未恢复，SPI 链路可能不可用");
            }
        }
    }

    // ★ WiFi 已连接，UDP 从这里开始能通
    // 等待网络栈稳定（ARP 表建立、路由就绪），避免首批 UDP 包丢失
    vTaskDelay(pdMS_TO_TICKS(500));

    // 启动摘要（单次；前面已 delay 等网络/ARP 稳定）
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    DEBUG_INFO("WIFI", "========================================");
    DEBUG_INFO("WIFI", "[启动摘要] ESP32-C3 OTA Relay v%s", FW_VERSION);
    DEBUG_INFO("WIFI", "[启动摘要] Chip: %s, Rev %d", CONFIG_IDF_TARGET, chip.revision);
    DEBUG_INFO("WIFI", "[启动摘要] Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    DEBUG_INFO("WIFI", "[启动摘要] WiFi: %s", WifiOta::getMode() == OTA_WIFI_MODE_STA ? "STA" : "AP");
    DEBUG_INFO("WIFI", "[启动摘要] NVS + WiFi driver + netif + event handler 初始化完成");
    DEBUG_INFO("WIFI", "[启动摘要] HTTP 服务器端口: %d", HTTP_SERVER_PORT);
    DEBUG_INFO("WIFI", "========================================");
    DEBUG_INFO("WIFI", "=== WiFi OTA 服务器就绪 ===");

    /* 启动日志 burst 已过：环 64→16 槽，释放大约 7.5KB 给业务堆 */
    DebugLog::instance().shrinkRingAfterBoot();

    /* ★ 重启完成判定信号（前端 polling.js 匹配 /OTA Relay .*启动/ + tag=MAIN）：
     *  必须在本函数（httpd 就绪、shrinkRingAfterBoot 之后）打印——main.cpp 的
     *  早期 banner 在环缩容时会被裁掉，前端首轮 /logs 拉不到，只能干等 10s
     *  安全阈值才恢复页面。此处是环内最新日志，前端一定能拉到。 */
    DEBUG_INFO("MAIN", "ESP32-C3 OTA Relay v%s 启动", FW_VERSION);

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
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
