/*
 * wifi_ota.c - WiFi OTA 服务器实现
 * T4.1 - T4.5
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
#include "wifi_ota.h"
#include "ota_relay.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"
#include "spi_master.h"
#include "cJSON.h"

/** @brief 当前 WiFi 工作模式（AP 或 STA）。 */
static ota_wifi_mode_t s_wifi_mode = OTA_WIFI_MODE_AP;
/** @brief 冷启动标志，上电复位或掉电复位时为 true。 */
static bool s_cold_boot = false;
/** @brief OTA 中继是否已触发标志。 */
static bool s_relay_triggered = false;
/** @brief STA 模式连接状态。 */
static bool s_sta_connected = false;
/** @brief STA 连接重试次数。 */
static int s_sta_retry_count = 0;
/** @brief STA 重试已耗尽标志，用于加速 wifi_ota_task 中的 AP 切换。 */
static bool s_sta_retry_exhausted = false;
/** @brief STA 连接成功信号量。 */
static SemaphoreHandle_t s_sta_connected_sem = NULL;
/** @brief STA 获取 IP 信号量。 */
static SemaphoreHandle_t s_sta_got_ip_sem = NULL;

// ESP32 OTA 状态
static bool s_esp32_ota_active = false;
/** @brief ESP32 OTA 写入句柄。 */
static esp_ota_handle_t s_esp32_ota_handle = 0;
static const esp_partition_t *s_esp32_update_partition = NULL;
/** @brief ESP32 OTA 已接收字节数。 */
static size_t s_esp32_received = 0;

// GD32 OTA 存储状态
static bool s_gd32_storage_done = false;

/** @brief HTTP 服务器句柄。 */
static httpd_handle_t s_http_server = NULL;

static constexpr uint32_t GD32_OTA_FLASH_SECTOR_SIZE = 4096U;

static uint32_t align_up_u32(uint32_t value, uint32_t alignment)
{
    return ((value + alignment - 1U) / alignment) * alignment;
}

/**
 * @brief 预先声明：为 STA 模式配置静态 IP、网关和子网掩码。
 */
static void wifi_set_static_ip(void);

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
// WiFi/IP 事件回调（事件驱动连接）
// ----------------------------------------
/**
 * @brief WiFi/IP 事件回调，处理 STA 启停、重连和获取 IP 等状态变化。
 * @param arg 回调用户参数。
 * @param event_base 事件基类（WIFI_EVENT 或 IP_EVENT）。
 * @param event_id 具体事件 ID。
 * @param event_data 事件附带数据。
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                      int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            DEBUG_INFO("WIFI", "STA 已启动，开始连接...");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED:
            DEBUG_INFO("WIFI", "STA 已关联到 AP");
            /** @brief STA 模式连接状态。 */
            s_sta_connected = true;
            /** @brief STA 连接重试次数。 */
            s_sta_retry_count = 0;
            s_sta_retry_exhausted = false;  // ★ 连接成功，重置耗尽标志
            if (s_sta_connected_sem) {
                xSemaphoreGive(s_sta_connected_sem);
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*)event_data;
            DEBUG_WARN("WIFI", "STA 断开, reason=%d, retry=%d", disconn->reason, s_sta_retry_count);
            /** @brief STA 模式连接状态。 */
            s_sta_connected = false;

            if (s_sta_retry_count < WIFI_STA_MAX_RETRY) {
                s_sta_retry_count++;
                // ★ 冷启动首次连接失败时增加退避延迟
                // 长时间断电后路由器可能还在初始化（ARP/DHCP/信道选择）
                // reason 201=NO_AP_FOUND, 15=AUTH_FAIL, 2=AUTH_EXPIRE, 204=HANDSHAKE_TIMEOUT
                if (s_cold_boot && s_sta_retry_count <= 3) {
                    int delay_ms = 2000 * s_sta_retry_count;  // 2s, 4s, 6s 递增退避
                    DEBUG_INFO("WIFI", "冷启动退避重连第 %d 次 (reason=%d, 等 %dms)...",
                               s_sta_retry_count, disconn->reason, delay_ms);
                    vTaskDelay(pdMS_TO_TICKS(delay_ms));
                } else {
                    DEBUG_INFO("WIFI", "重连第 %d 次...", s_sta_retry_count);
                }
                esp_wifi_connect();
            } else {
                DEBUG_ERR("WIFI", "STA 重连失败 %d 次，放弃", s_sta_retry_count);
                s_sta_retry_exhausted = true;
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
            if (s_sta_got_ip_sem) {
                xSemaphoreGive(s_sta_got_ip_sem);
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
static esp_err_t wifi_init_ap(void)
{
    DEBUG_INFO("WIFI", "=== 初始化 WiFi AP 模式 ===");

    /** @brief 当前 WiFi 工作模式（AP 或 STA）。 */
    s_wifi_mode = OTA_WIFI_MODE_AP;

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

    return ESP_OK;
}

// ----------------------------------------
// WiFi STA 模式初始化
/**
  * @brief 初始化 STA 模式并连接预配置的路由器。
 * @return ESP_OK 成功，其他失败。
 */
static esp_err_t wifi_init_sta(void)
{
    DEBUG_INFO("WIFI", "=== 初始化 WiFi STA 模式 ===");

    /** @brief 当前 WiFi 工作模式（AP 或 STA）。 */
    s_wifi_mode = OTA_WIFI_MODE_STA;
    /** @brief STA 连接重试次数。 */
    s_sta_retry_count = 0;
    /** @brief STA 重试已耗尽标志。 */
    s_sta_retry_exhausted = false;

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

    wifi_set_static_ip();

    // ★ 冷启动时在 wifi_start 前增加延迟
    // 长时间断电后首次上电，WiFi PHY 校准需要额外时间
    // esp_wifi_start() 内部会触发 PHY 初始化和校准
    // 如果校准未完成就启动射频，可能导致连接失败
    if (s_cold_boot) {
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
static esp_err_t wifi_wait_sta_connected(char *ip_str, size_t len)
{
    SemaphoreHandle_t wait_sem = WIFI_STA_USE_STATIC_IP ? s_sta_connected_sem : s_sta_got_ip_sem;
    const char *wait_name = WIFI_STA_USE_STATIC_IP ? "STA_CONNECTED" : "STA_GOT_IP";

    if (wait_sem == NULL) {
        DEBUG_ERR("WIFI", "%s 信号量未创建", wait_name);
        return ESP_ERR_INVALID_STATE;
    }

    // ★ 使用轮询等待，检查 s_sta_retry_exhausted 标志加速退出
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
                    snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
                }
                DEBUG_INFO("WIFI", "%s 等待成功, 当前 IP: " IPSTR, wait_name, IP2STR(&ip_info.ip));
            }
            /** @brief STA 模式连接状态。 */
            s_sta_connected = true;
            return ESP_OK;
        }

        // ★ 重试已耗尽，不再等待
        if (s_sta_retry_exhausted) {
            DEBUG_ERR("WIFI", "STA 重试已耗尽 (%d 次)，立即切换 AP", s_sta_retry_count);
            return ESP_ERR_TIMEOUT;
        }
    }

    DEBUG_ERR("WIFI", "等待 %s 超时 (%d ms), 重连次数: %d", wait_name, WIFI_STA_CONNECT_TIMEOUT_MS, s_sta_retry_count);
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 设置 STA 静态 IP
 */
static void wifi_set_static_ip(void)
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
/**
   * @brief 初始化 ESP32 自身的 OTA 写入流程。
 * @param total_size
 * @return ESP_OK 成功，其他失败。
 */
static esp_err_t esp32_ota_begin(size_t total_size)
{
    const esp_partition_t *boot_partition = esp_ota_get_running_partition();
    DEBUG_INFO("OTA", "当前启动分区: %s, size=%u, address=0x%lx",
               boot_partition->label, boot_partition->size, boot_partition->address);

    s_esp32_update_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (s_esp32_update_partition == NULL ||
        s_esp32_update_partition->address == boot_partition->address) {
        s_esp32_update_partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    }

    if (s_esp32_update_partition == NULL) {
        DEBUG_ERR("OTA", "找不到可用的 OTA 分区");
        return ESP_FAIL;
    }

    DEBUG_INFO("OTA", "OTA 目标分区: %s, size=%u, address=0x%lx",
               s_esp32_update_partition->label,
               s_esp32_update_partition->size,
               s_esp32_update_partition->address);

    if (total_size > s_esp32_update_partition->size) {
        DEBUG_ERR("OTA", "固件太大: %u > 分区大小: %u",
                  total_size, s_esp32_update_partition->size);
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_begin(s_esp32_update_partition, total_size, &s_esp32_ota_handle);
    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "esp_ota_begin 失败: %s", esp_err_to_name(err));
        /** @brief ESP32 OTA 目标分区。 */
        s_esp32_update_partition = NULL;
        return err;
    }

    /** @brief ESP32 OTA 已接收字节数。 */
    s_esp32_received = 0;
    /** @brief ESP32 自身 OTA 写入进行中标志。 */
    s_esp32_ota_active = true;
    DEBUG_INFO("OTA", "ESP32 OTA 开始，目标分区: %s", s_esp32_update_partition->label);
    return ESP_OK;
}

/**
  * @brief 将接收到的数据块写入 ESP32 OTA 分区。
 * @param data
 * @param len
 * @return ESP_OK 成功，其他失败。
 */
static esp_err_t esp32_ota_write(const uint8_t *data, size_t len)
{
    if (!s_esp32_ota_active || s_esp32_update_partition == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_ota_write(s_esp32_ota_handle, data, len);
    if (err == ESP_OK) {
        s_esp32_received += len;
    }
    return err;
}

/**
 * @brief HTTP POST /ota/update - 完成 ESP32 OTA 并重启
 * @param req
 * @return
 */
static esp_err_t esp32_ota_update_handler(httpd_req_t *req)
{
    (void)req;
    http_set_cors_headers(req);
    if (!s_esp32_ota_active) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA not started");
        return ESP_FAIL;
    }

    DEBUG_INFO("OTA", "ESP32 OTA 完成, 共 %u 字节, 设置启动分区...", s_esp32_received);

    esp_err_t err = esp_ota_end(s_esp32_ota_handle);
    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "esp_ota_end 失败: %s", esp_err_to_name(err));
        /** @brief ESP32 自身 OTA 写入进行中标志。 */
        s_esp32_ota_active = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(s_esp32_update_partition);
    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "esp_ota_set_boot_partition 失败: %s", esp_err_to_name(err));
        /** @brief ESP32 自身 OTA 写入进行中标志。 */
        s_esp32_ota_active = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    /** @brief ESP32 自身 OTA 写入进行中标志。 */
    s_esp32_ota_active = false;
    DEBUG_INFO("OTA", "ESP32 OTA 完成，重启中...");

    const char resp[] = "{\"status\": \"ok\", \"message\": \"OTA finished, rebooting\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
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

    DEBUG_INFO("OTA", "收到重启请求，500ms 后重启...");

    const char resp[] = "{\"status\": \"ok\", \"message\": \"rebooting\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/**
 * @brief HTTP GET /ota/update - 查询 ESP32 OTA 状态
 * @param req
 * @return
 */
static esp_err_t esp32_ota_status_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    char resp[128];
    if (s_esp32_ota_active) {
        snprintf(resp, sizeof(resp),
            "{\"active\": true, \"received\": %u, \"partition\": \"%s\"}",
            (unsigned int)s_esp32_received,
            s_esp32_update_partition ? s_esp32_update_partition->label : "null");
    } else {
        snprintf(resp, sizeof(resp), "{\"active\": false}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

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
    DEBUG_INFO("WIFI", "=== HTTP: POST /ota/upload ===");
    DEBUG_INFO("WIFI", "收到固件上传请求, 内容长度: %d 字节", req->content_len);

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

        esp_err_t err = esp32_ota_begin(total_size);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
            return ESP_FAIL;
        }

        // 堆分配缓冲区，避免栈溢出（HTTP server 栈仅 8KB）
        uint8_t *fw_buf = (uint8_t *)malloc(OTA_BLOCK_SIZE);
        if (!fw_buf) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存分配失败");
            return ESP_FAIL;
        }

        size_t total_firmware_written = 0;
        size_t total_received = json_header_len;  // 已消费的字节数
        int last_percent = 0;

        while (total_received < req->content_len) {
            size_t to_read = req->content_len - total_received;
            if (to_read > OTA_BLOCK_SIZE) to_read = OTA_BLOCK_SIZE;

            int ret = httpd_req_recv(req, (char *)fw_buf, to_read);
            if (ret <= 0) break;

            err = esp32_ota_write(fw_buf, ret);
            if (err != ESP_OK) {
                DEBUG_ERR("WIFI", "OTA write 失败");
                break;
            }
            total_received += ret;
            total_firmware_written += ret;

            if (total_size > 0) {
                int percent = (int)((total_firmware_written * 100) / total_size);
                if (percent / 10 > last_percent / 10) {
                    last_percent = percent;
                    DEBUG_INFO("WIFI", "ESP32 OTA 进度: %u / %u (%d%%)",
                               (unsigned int)total_firmware_written, total_size, percent);
                }
            }
        }

        free(fw_buf);
        DEBUG_INFO("WIFI", "ESP32 OTA 存储完成: %u 字节 (跳过 JSON 头 %d 字节)", (unsigned int)s_esp32_received, json_header_len);
        const char resp[] = "{\"status\": \"ok\", \"message\": \"stored, call POST /ota/update to finish\", \"target\": \"esp32\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }

    // --- GD32 OTA（原逻辑）---
    const uint32_t gd32_fw_size = (uint32_t)req->content_len;
    const uint32_t gd32_rounded_size = (uint32_t)(((gd32_fw_size + (uint32_t)GD32_OTA_PACKET_SIZE - 1U) / (uint32_t)GD32_OTA_PACKET_SIZE) * (uint32_t)GD32_OTA_PACKET_SIZE);
    if (gd32_fw_size == 0U || gd32_fw_size > OTA_MAX_FIRMWARE_SIZE || gd32_rounded_size > OTA_MAX_FIRMWARE_SIZE) {
        DEBUG_ERR("WIFI", "固件大小非法: raw=%u rounded48=%u max=%u",
                  (unsigned)gd32_fw_size, (unsigned)gd32_rounded_size, (unsigned)OTA_MAX_FIRMWARE_SIZE);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "固件太大或48B对齐后越界");
        return ESP_FAIL;
    }

    if (g_relay_state != RELAY_IDLE) {
        DEBUG_WARN("WIFI", "GD32 OTA 开始前复位中继状态: %d", g_relay_state);
        ota_relay_reset();
    }
    s_gd32_storage_done = false;
    s_relay_triggered = false;

    const esp_partition_t *gd32_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "gd32_ota");
    if (!gd32_partition) {
        DEBUG_ERR("WIFI", "找不到 gd32_ota 分区");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "找不到 gd32_ota 分区");
        return ESP_FAIL;
    }
    const uint32_t gd32_erase_size = align_up_u32(gd32_fw_size, GD32_OTA_FLASH_SECTOR_SIZE);
    if (gd32_erase_size > gd32_partition->size) {
        DEBUG_ERR("WIFI", "gd32_ota 分区空间不足: erase=%u partition=%u",
                  (unsigned)gd32_erase_size, (unsigned)gd32_partition->size);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "gd32_ota partition too small");
        return ESP_FAIL;
    }

    DEBUG_INFO("WIFI", "擦除 gd32_ota 分区: %u bytes (fw=%u, partition=%u)",
               (unsigned)gd32_erase_size, (unsigned)gd32_fw_size, (unsigned)gd32_partition->size);
    esp_err_t erase_err = esp_partition_erase_range(gd32_partition, 0, gd32_erase_size);
    if (erase_err != ESP_OK) {
        DEBUG_ERR("WIFI", "擦除 gd32_ota 分区失败: %s", esp_err_to_name(erase_err));
        char err_msg[96];
        snprintf(err_msg, sizeof(err_msg), "erase gd32_ota failed: %s", esp_err_to_name(erase_err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err_msg);
        return ESP_FAIL;
    }
    meta_clear();

    uint8_t *buf = (uint8_t *)malloc(OTA_BLOCK_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存分配失败");
        return ESP_FAIL;
    }

    int received = 0;

    // GD32 路径与网页上传协议对齐：网页上传的是 raw binary（不带 6B 头）。
    uint32_t expected_fw_size_raw = gd32_fw_size;
    ota_meta_t meta_raw = {
        .total_size = expected_fw_size_raw,
        .stored_size = 0,
        .last_seq = 0,
        .chunk_count = (expected_fw_size_raw + OTA_BLOCK_SIZE - 1U) / OTA_BLOCK_SIZE,
        .checksum = {0}
    };
    meta_save(&meta_raw);
    DEBUG_INFO("WIFI", "GD32 raw upload start: %u bytes", expected_fw_size_raw);

    int first_chunk_len = pre_read_len;
    if (first_chunk_len > req->content_len) first_chunk_len = req->content_len;
    if (first_chunk_len > 0) {
        esp_err_t err = flash_write_chunk(0, pre_read_buf, first_chunk_len);
        if (err != ESP_OK) {
            DEBUG_ERR("WIFI", "GD32 first chunk write failed: len=%d err=%s",
                      first_chunk_len, esp_err_to_name(err));
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write gd32_ota failed");
            return ESP_FAIL;
        }
        received = first_chunk_len;
        meta_update((uint32_t)received, (uint32_t)(received / OTA_BLOCK_SIZE));
    }

    while (received < req->content_len) {
        size_t to_read = req->content_len - received;
        if (to_read > OTA_BLOCK_SIZE) to_read = OTA_BLOCK_SIZE;

        int ret = httpd_req_recv(req, (char *)buf, to_read);
        if (ret <= 0) {
            DEBUG_ERR("WIFI", "GD32 upload interrupted: received=%d expected=%d",
                      received, req->content_len);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload interrupted");
            return ESP_FAIL;
        }

        uint32_t flash_offset = (uint32_t)received;
        esp_err_t err = flash_write_chunk(flash_offset, buf, ret);
        if (err != ESP_OK) {
            DEBUG_ERR("WIFI", "GD32 flash write failed: offset=%u len=%d err=%s",
                      flash_offset, ret, esp_err_to_name(err));
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write gd32_ota failed");
            return ESP_FAIL;
        }

        received += ret;
        meta_update((uint32_t)received, (uint32_t)(received / OTA_BLOCK_SIZE));
    }

    free(buf);
    DEBUG_INFO("WIFI", "GD32 raw upload done: %d / %u bytes", received, expected_fw_size_raw);

    ota_meta_t final_meta;
    meta_load(&final_meta);
    if (final_meta.total_size > 0 && final_meta.stored_size >= final_meta.total_size) {
        s_gd32_storage_done = true;
        DEBUG_INFO("WIFI", "GD32 firmware fully stored, auto-triggering relay");
        // 自动触发中继
        if (g_ota_dispatch_queue) {
            uint32_t cmd = 1;  // 1 = start relay
            xQueueSend(g_ota_dispatch_queue, &cmd, 0);
        }
    }

    const char resp[] = "{\"status\": \"ok\", \"progress\": 100, \"target\": \"gd32\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/**
 * @brief HTTP POST /ota/relay 处理器
 * @param req
 * @return
 */
static esp_err_t ota_relay_trigger_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    DEBUG_INFO("WIFI", "=== HTTP: POST /ota/relay ===");
    DEBUG_INFO("WIFI", "收到触发 OTA 中继请求");

    ota_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_load(&meta) != ESP_OK || meta.total_size == 0 || meta.stored_size < meta.total_size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "firmware not fully stored");
        return ESP_FAIL;
    }

    if (s_relay_triggered && g_relay_state != RELAY_IDLE &&
        g_relay_state != RELAY_DONE && g_relay_state != RELAY_ERROR) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay already triggered");
        return ESP_FAIL;
    }
    /* 上一轮中继已结束/失败/空闲，重置标志允许重新触发 */
    s_relay_triggered = false;
    s_gd32_storage_done = false;

    if (!g_ota_dispatch_queue) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "relay queue not ready");
        return ESP_FAIL;
    }

    wifi_ota_signal_relay();
    if (!s_relay_triggered) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "relay trigger failed");
        return ESP_FAIL;
    }

    const char resp[] = "{\"status\": \"ok\", \"message\": \"OTA relay triggered\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    return ESP_OK;
}

/**
 * @brief HTTP GET /ota/status 处理器
 * @param req
 * @return
 */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    DEBUG_INFO("WIFI", "=== HTTP: GET /ota/status ===");
    
    ota_meta_t meta;
    meta_load(&meta);

    char resp[256];
    int len = snprintf(resp, sizeof(resp),
        "{\"total_size\": %lu, \"stored_size\": %lu, \"last_seq\": %lu, "
        "\"storage_done\": %s, \"relay_state\": %d, \"gd32_app_alive\": %s}",
        (unsigned long)meta.total_size, (unsigned long)meta.stored_size,
        (unsigned long)meta.last_seq,
        s_gd32_storage_done ? "true" : "false",
        g_relay_state,
        g_gd32_app_alive ? "true" : "false");

    DEBUG_INFO("WIFI", "OTA 状态: total=%lu, stored=%lu, done=%s, app_alive=%s",
               (unsigned long)meta.total_size, (unsigned long)meta.stored_size,
               s_gd32_storage_done ? "true" : "false",
               g_gd32_app_alive ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);

    return ESP_OK;
}

/**
 * @brief HTTP GET /health 处理器
 * @param req
 * @return
 */
static esp_err_t health_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    DEBUG_INFO("WIFI", "=== HTTP: GET /health ===");
    
    health_state_t state = health_get_current();
    uint32_t free_heap = esp_get_free_heap_size();

    char resp[128];
    int len = snprintf(resp, sizeof(resp),
        "{\"health\": %d, \"free_heap\": %lu}",
        state, (unsigned long)free_heap);

    DEBUG_INFO("WIFI", "健康状态: %d, 空闲堆: %lu", state, (unsigned long)free_heap);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);

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
    char *json = debug_log_ring_drain_json(&json_len);

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
    int resp_len = gd32_can_send(ch, ext_id, tx_data, tx_dlc,
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
    esp_err_t spi_err = spi_ota_transfer(frame_type, payload_data, payload_len,
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

/**
 * @brief HTTP GET /can/status 处理器
 * @param req
 * @return
 */
static esp_err_t can_status_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    char resp[128];
    snprintf(resp, sizeof(resp),
        "{\"channels\": 3, \"max_dlc\": %d, \"spi_init\": %s}",
        CAN_MAX_DLC,
        g_spi_device != NULL ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/**
 * @brief HTTP GET /wifi/status 处理器
 * @param req
 * @return
 */
static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    char resp[256];
    char ip_str[16] = "0.0.0.0";
    int8_t rssi = -127;

    if (s_wifi_mode == OTA_WIFI_MODE_STA && s_sta_connected) {
        esp_netif_ip_info_t ip_info;
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }
    }

    int len = snprintf(resp, sizeof(resp),
        "{\"mode\": \"%s\", \"ip\": \"%s\", \"rssi\": %d, \"connected\": %s}",
        s_wifi_mode == OTA_WIFI_MODE_AP ? "AP" : "STA",
        ip_str, rssi,
        (s_wifi_mode == OTA_WIFI_MODE_STA) ? (s_sta_connected ? "true" : "false") : "N/A");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);

    return ESP_OK;
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

    if (s_wifi_mode == OTA_WIFI_MODE_STA && s_sta_connected) {
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

    ota_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    meta_load(&meta);

    char resp[800];
    int len = snprintf(resp, sizeof(resp),
        "{\"version\": \"%s\", \"health\": %d, \"free_heap\": %lu, \"min_free_heap\": %lu, \"temperature\": %.1f, \"uptime_sec\": %lu, \"wifi\": {\"mode\": \"%s\", \"ip\": \"%s\", \"rssi\": %d, \"connected\": %s}, \"ota\": {\"total_size\": %lu, \"stored_size\": %lu, \"storage_done\": %s, \"relay_state\": %d, \"relay_seq\": %lu, \"relay_total_packets\": %lu}}",
        FW_VERSION,health_get_current(),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (double)health_get_last_temperature(),
        (unsigned long)(esp_timer_get_time() / 1000000),
        s_wifi_mode == OTA_WIFI_MODE_AP ? "AP" : "STA",
        ip_str,
        rssi,
        connected ? "true" : "false",
        (unsigned long)meta.total_size,
        (unsigned long)meta.stored_size,
        s_gd32_storage_done ? "true" : "false",
        g_relay_state,
        (unsigned long)g_relay_seq,
        (unsigned long)g_relay_total_packets);

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
static esp_err_t http_ota_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_SERVER_PORT;
    config.stack_size = HTTP_SERVER_STACK_SIZE;
    config.max_uri_handlers = 32;  // 现有业务端点 + 对应 OPTIONS 预检
    config.recv_wait_timeout = 30;  // GD32 上传前会擦除 Flash，5 秒默认值过紧
    config.send_wait_timeout = 30;

    DEBUG_INFO("WIFI", "=== 启动 HTTP OTA 服务器 ===");
    DEBUG_INFO("WIFI", "HTTP 服务器端口: %d", config.server_port);

    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        DEBUG_ERR("WIFI", "HTTP 服务器启动失败: %s", esp_err_to_name(err));
        return err;
    }

    // 从 NVS 恢复固件存储状态（重启后保留）
    {
        ota_meta_t boot_meta;
        if (meta_load(&boot_meta) == ESP_OK &&
            boot_meta.total_size > 0 && boot_meta.stored_size >= boot_meta.total_size) {
            s_gd32_storage_done = true;
            DEBUG_INFO("WIFI", "从 NVS 恢复: 固件已就绪 (%u bytes)", boot_meta.total_size);
        }
    }

    httpd_uri_t upload_uri = {
        .uri = "/ota/upload",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &upload_uri);
    httpd_uri_t upload_options_uri = {
        .uri = "/ota/upload",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &upload_options_uri);

    httpd_uri_t relay_uri = {
        .uri = "/ota/relay",
        .method = HTTP_POST,
        .handler = ota_relay_trigger_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &relay_uri);
    httpd_uri_t relay_options_uri = {
        .uri = "/ota/relay",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &relay_options_uri);

    httpd_uri_t status_uri = {
        .uri = "/ota/status",
        .method = HTTP_GET,
        .handler = ota_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &status_uri);
    httpd_uri_t status_options_uri = {
        .uri = "/ota/status",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &status_options_uri);

    httpd_uri_t esp32_update_uri = {
        .uri = "/ota/update",
        .method = HTTP_POST,
        .handler = esp32_ota_update_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &esp32_update_uri);
    httpd_uri_t esp32_update_options_uri = {
        .uri = "/ota/update",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &esp32_update_options_uri);

    httpd_uri_t restart_uri = {
        .uri = "/restart",
        .method = HTTP_POST,
        .handler = restart_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &restart_uri);
    httpd_uri_t restart_options_uri = {
        .uri = "/restart",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &restart_options_uri);

    httpd_uri_t esp32_status_uri = {
        .uri = "/ota/esp32_status",
        .method = HTTP_GET,
        .handler = esp32_ota_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &esp32_status_uri);
    httpd_uri_t esp32_status_options_uri = {
        .uri = "/ota/esp32_status",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &esp32_status_options_uri);

    httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &health_uri);
    httpd_uri_t health_options_uri = {
        .uri = "/health",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &health_options_uri);

    httpd_uri_t logs_uri = {
        .uri = "/logs",
        .method = HTTP_GET,
        .handler = logs_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &logs_uri);
    httpd_uri_t logs_options_uri = {
        .uri = "/logs",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &logs_options_uri);

    httpd_uri_t wifi_status_uri = {
        .uri = "/wifi/status",
        .method = HTTP_GET,
        .handler = wifi_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &wifi_status_uri);
    httpd_uri_t wifi_status_options_uri = {
        .uri = "/wifi/status",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &wifi_status_options_uri);

    httpd_uri_t system_status_uri = {
        .uri = "/system/status",
        .method = HTTP_GET,
        .handler = system_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &system_status_uri);
    httpd_uri_t system_status_options_uri = {
        .uri = "/system/status",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &system_status_options_uri);

    // CAN 帧转发
    httpd_uri_t can_send_uri = {
        .uri = "/can/send",
        .method = HTTP_POST,
        .handler = can_send_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &can_send_uri);
    httpd_uri_t can_send_options_uri = {
        .uri = "/can/send",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &can_send_options_uri);

    httpd_uri_t can_status_uri = {
        .uri = "/can/status",
        .method = HTTP_GET,
        .handler = can_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &can_status_uri);
    httpd_uri_t can_status_options_uri = {
        .uri = "/can/status",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &can_status_options_uri);

    DEBUG_INFO("WIFI", "HTTP 服务器已启动");
    // SPI 直接发送
    httpd_uri_t spi_send_uri = {
        .uri = "/spi/send",
        .method = HTTP_POST,
        .handler = spi_send_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &spi_send_uri);
    httpd_uri_t spi_send_options_uri = {
        .uri = "/spi/send",
        .method = HTTP_OPTIONS,
        .handler = http_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &spi_send_options_uri);

    DEBUG_INFO("WIFI", "注册端点: /ota/upload, /ota/relay, /ota/update, /ota/status, /ota/esp32_status, /health, /wifi/status, /system/status, /can/send, /can/status, /spi/send");

    return ESP_OK;
}

// ----------------------------------------
// 公开 API
/**
  * @brief 通知 OTA 中继开始向 GD32 转发固件。
 */
void wifi_ota_signal_relay(void)
{
    ota_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_load(&meta) != ESP_OK || meta.total_size == 0 || meta.stored_size < meta.total_size) {
        DEBUG_WARN("WIFI", "GD32 固件未完整存储，拒绝触发转发: stored=%lu total=%lu",
                   (unsigned long)meta.stored_size, (unsigned long)meta.total_size);
        return;
    }

    if (s_relay_triggered && g_relay_state != RELAY_IDLE &&
        g_relay_state != RELAY_DONE && g_relay_state != RELAY_ERROR) {
        DEBUG_WARN("WIFI", "转发正在执行中，状态: %d", g_relay_state);
        return;
    }
    /* 上一轮中继已结束/失败/空闲，重置标志 */
    s_relay_triggered = false;
    s_gd32_storage_done = false;

    /** @brief OTA 中继是否已触发标志。 */
    s_relay_triggered = true;

    if (g_ota_dispatch_queue) {
        uint32_t msg = 1;
        BaseType_t ret = xQueueSend(g_ota_dispatch_queue, &msg, pdMS_TO_TICKS(100));
        if (ret == pdTRUE) {
            DEBUG_INFO("WIFI", "OTA 调度信号已发送到中继任务");
        } else {
            DEBUG_ERR("WIFI", "OTA 调度信号发送失败");
            s_relay_triggered = false;
        }
    } else {
        DEBUG_ERR("WIFI", "OTA 中继队列尚未就绪");
        s_relay_triggered = false;
    }
}

/**
  * @brief 获取当前 WiFi 工作模式（AP 或 STA）。
 * @return 当前 WiFi 模式枚举值。
 */
ota_wifi_mode_t wifi_get_mode(void)
{
    /** @brief 当前 WiFi 工作模式（AP 或 STA）。 */
    return s_wifi_mode;
}

/**
  * @brief 获取当前 WiFi 信号强度。
 * @return 信号强度（dBm），未连接时返回无效值。
 */
int8_t wifi_get_rssi(void)
{
    if (s_wifi_mode != OTA_WIFI_MODE_STA || !s_sta_connected) {
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
bool wifi_is_connected(void)
{
    /** @brief STA 模式连接状态。 */
    return s_sta_connected;
}

// ----------------------------------------
// ★ WiFi 早期初始化（在 app_main 调度器启动前调用）
// 包括：NVS → netif → event handler → WiFi driver → 配置 → connect
// 返回：true = STA 模式已启动（正在连接），false = 需要切 AP
/**
  * @brief 在调度器启动前初始化 WiFi 并尝试 STA 连接。
 * @return true STA 初始化中，false 需回退 AP。
 */
bool wifi_early_init(void)
{
    DEBUG_INFO("WIFI", "=== WiFi 早期初始化 (调度器启动前) ===");

    // ★ 冷启动检测：判断是否为上电复位
    esp_reset_reason_t reset_reason = esp_reset_reason();
    bool is_cold_boot = (reset_reason == ESP_RST_POWERON || reset_reason == ESP_RST_BROWNOUT);
    s_cold_boot = is_cold_boot;  // 保存到模块级标志，供事件回调使用
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
               is_cold_boot ? "是" : "否");

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
    if (is_cold_boot) {
        nvs_handle_t nvs_handle;
        err = nvs_open("nvs.net80211", NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK) {
            // 清除 WiFi 驱动内部 NVS 缓存键
            nvs_erase_all(nvs_handle);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            DEBUG_INFO("WIFI", "冷启动: 已清除 WiFi NVS 缓存 (nvs.net80211)");
        } else {
            DEBUG_INFO("WIFI", "冷启动: 无 WiFi NVS 缓存分区，跳过清除");
        }
        // 同样清除 PHY 校准数据分区，强制下次全量校准
        err = nvs_open("phy", NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK) {
            nvs_erase_all(nvs_handle);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            DEBUG_INFO("WIFI", "冷启动: 已清除 PHY 校准缓存，将重新校准");
        }
    }

    /** @brief STA 连接成功信号量。 */
    s_sta_connected_sem = xSemaphoreCreateBinary();
    /** @brief STA 获取 IP 信号量。 */
    s_sta_got_ip_sem = xSemaphoreCreateBinary();
    if (!s_sta_connected_sem || !s_sta_got_ip_sem) {
        DEBUG_ERR("WIFI", "创建信号量失败");
    }

    // 注册 WiFi/IP 事件回调（必须在 esp_wifi_init 之前）
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    DEBUG_INFO("WIFI", "WiFi/IP 事件回调已注册");

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == NULL) {
        sta_netif = esp_netif_create_default_wifi_sta();
        if (sta_netif == NULL) {
            DEBUG_ERR("WIFI", "创建 STA netif 失败");
        } else {
            DEBUG_INFO("WIFI", "STA netif 已创建");
        }
    } else {
        DEBUG_INFO("WIFI", "STA netif 已存在，跳过创建");
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    DEBUG_INFO("WIFI", "WiFi driver 初始化成功");

    // STA 优先，失败时返回 false 让 task 切 AP
    if (CONFIG_WIFI_MODE == 1) {
        DEBUG_INFO("WIFI", "工作模式: STA (优先)");
        err = wifi_init_sta();
        if (err == ESP_OK) {
            // 连接已触发（STA_START 事件驱动），返回 true
            // 实际连接状态由 wifi_ota_task 等待确认
            return true;
        }
        DEBUG_WARN("WIFI", "STA 初始化失败，将切换 AP");
        return false;
    } else {
        DEBUG_INFO("WIFI", "工作模式: AP");
        wifi_init_ap();
        return false;  // AP 模式，不需要等待连接
    }
}

// ----------------------------------------
// WiFi OTA FreeRTOS 任务
// WiFi 初始化已在 app_main 中通过 wifi_early_init() 完成
// 此任务负责：等待连接结果 → 启动 HTTP 服务器 → 状态监控
/**
  * @brief WiFi OTA FreeRTOS 任务，提供 HTTP 上传和中继触发。
 * @param pvParameters FreeRTOS 任务参数，未使用。
 */
void wifi_ota_task(void *pvParameters)
{
    (void)pvParameters;

    DEBUG_INFO("WIFI", "=== WiFi OTA 任务启动 ===");

    // 等待 STA 连接结果（wifi_early_init 已触发连接）
    if (wifi_get_mode() == OTA_WIFI_MODE_STA) {
        char ip_str[16] = {0};
        esp_err_t err = wifi_wait_sta_connected(ip_str, sizeof(ip_str));
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
                    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
                    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
                    DEBUG_INFO("WIFI", "WiFi/IP 事件回调已重新注册");
                }
            }
            /** @brief 当前 WiFi 工作模式（AP 或 STA）。 */
            s_wifi_mode = OTA_WIFI_MODE_AP;
            wifi_init_ap();
        }
    }
    // AP 模式：wifi_early_init 已完成，无需额外操作

    esp_err_t err = http_ota_server_start();
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
        DEBUG_INFO("WIFI", "[启动摘要] WiFi: %s", wifi_get_mode() == OTA_WIFI_MODE_STA ? "STA" : "AP");
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

        // 每 10 秒检查一次 RSSI
        if (tick % 10 == 0) {
            int8_t rssi = wifi_get_rssi();
            if (rssi != -127) {
                health_update_from_wifi_rssi(rssi);

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
