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
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lwip/netdb.h"
#include "esp_netif.h"
#include "esp_netif_types.h"

#include "app_config.h"
#include "wifi_ota.h"
#include "ota_relay.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"

static ota_wifi_mode_t s_wifi_mode = OTA_WIFI_MODE_AP;
static bool s_ota_storage_done = false;
static bool s_relay_triggered = false;
static bool s_sta_connected = false;

static httpd_handle_t s_http_server = NULL;

// ----------------------------------------
// WiFi AP 模式初始化
// ----------------------------------------
static esp_err_t wifi_init_ap(void)
{
    DEBUG_INFO("WIFI", "初始化 WiFi AP 模式...");

    s_wifi_mode = OTA_WIFI_MODE_AP;

    wifi_config_t wifi_config = {};
    memcpy(wifi_config.ap.ssid, WIFI_AP_SSID, strlen(WIFI_AP_SSID) + 1);
    wifi_config.ap.ssid_len = strlen(WIFI_AP_SSID);
    memcpy(wifi_config.ap.password, WIFI_AP_PASSWORD, strlen(WIFI_AP_PASSWORD) + 1);
    wifi_config.ap.max_connection = WIFI_AP_MAX_CONNECT;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    wifi_config.ap.channel = WIFI_AP_CHANNEL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    DEBUG_INFO("WIFI", "WiFi AP 已启动: %s", WIFI_AP_SSID);
    DEBUG_INFO("WIFI", "AP IP 地址: 192.168.4.1");

    return ESP_OK;
}

// ----------------------------------------
// WiFi STA 模式初始化
// ----------------------------------------
static esp_err_t wifi_init_sta(void)
{
    DEBUG_INFO("WIFI", "初始化 WiFi STA 模式...");

    s_wifi_mode = OTA_WIFI_MODE_STA;

    wifi_config_t wifi_config = {};
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    memcpy(wifi_config.sta.ssid, WIFI_STA_SSID, strlen(WIFI_STA_SSID) + 1);
    memcpy(wifi_config.sta.password, WIFI_STA_PASSWORD, strlen(WIFI_STA_PASSWORD) + 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    DEBUG_INFO("WIFI", "WiFi STA 连接中, SSID: %s...", WIFI_STA_SSID);

    return ESP_OK;
}

// 等待 STA 连接成功并获取 IP
static esp_err_t wifi_wait_sta_connected(char *ip_str, size_t len)
{
    int retry = 0;
    while (retry < 60) {  // 最多等 6 秒
        vTaskDelay(pdMS_TO_TICKS(100));
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_sta_connected = true;
            esp_netif_ip_info_t ip_info;
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                if (ip_str) {
                    snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
                }
            }
            return ESP_OK;
        }
        retry++;
    }
    return ESP_ERR_TIMEOUT;
}

// 设置 STA 静态 IP
static void wifi_set_static_ip(void)
{
#if WIFI_STA_STATIC_IP_IS_ENABLED
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta_netif));

        esp_netif_ip_info_t info;
        memset(&info, 0, sizeof(info));
        inet_pton(AF_INET, WIFI_STA_STATIC_IP, &info.ip);
        inet_pton(AF_INET, WIFI_STA_STATIC_GW, &info.gw);
        inet_pton(AF_INET, WIFI_STA_STATIC_NETMASK, &info.netmask);

        ESP_ERROR_CHECK(esp_netif_set_ip_info(sta_netif, &info));
        DEBUG_INFO("WIFI", "静态 IP 已设置: %s", WIFI_STA_STATIC_IP);
    }
#else
    DEBUG_INFO("WIFI", "使用 DHCP 获取 IP");
#endif
}

// ----------------------------------------
// HTTP POST /ota/upload 处理器
// ----------------------------------------
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    DEBUG_INFO("WIFI", "收到固件上传请求, 内容长度: %d", req->content_len);

    if (req->content_len > OTA_MAX_FIRMWARE_SIZE) {
        DEBUG_ERR("WIFI", "固件太大: %d 字节", req->content_len);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "固件太大");
        return ESP_FAIL;
    }

    uint8_t buf[OTA_BLOCK_SIZE];
    int received = 0;
    int first_chunk = 1;

    while (received < req->content_len) {
        int remaining = req->content_len - received;
        int to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);

        int ret = httpd_req_recv(req, (char *)buf, to_read);
        if (ret <= 0) {
            DEBUG_ERR("WIFI", "请求接收失败");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败");
            return ESP_FAIL;
        }

        if (first_chunk && ret >= 6) {
            uint16_t seq = buf[0] | (buf[1] << 8);
            uint32_t total_size = buf[2] | (buf[3] << 8) | (buf[4] << 16) | (buf[5] << 24);

            if (seq == 0) {
                ota_meta_t meta = {
                    .total_size = total_size,
                    .stored_size = 0,
                    .last_seq = 0,
                    .chunk_count = (total_size + 4095) / 4096,
                    .checksum = {0}
                };
                meta_save(&meta);
                DEBUG_INFO("WIFI", "开始固件存储: %u 字节", total_size);
            }
            first_chunk = 0;
        }

        uint32_t flash_offset = (received / OTA_BLOCK_SIZE) * OTA_BLOCK_SIZE;
        esp_err_t err = flash_write_chunk(flash_offset, buf, ret);
        if (err != ESP_OK) {
            DEBUG_ERR("WIFI", "Flash 写入失败 at offset %u", flash_offset);
        }

        meta_update(flash_offset + ret, received / OTA_BLOCK_SIZE);

        received += ret;
        DEBUG_DBG("WIFI", "已接收: %d / %d 字节", received, req->content_len);
    }

    DEBUG_INFO("WIFI", "固件存储完成: %d 字节", received);

    ota_meta_t meta;
    meta_load(&meta);
    if (meta.stored_size >= meta.total_size) {
        s_ota_storage_done = true;
        DEBUG_INFO("WIFI", "全部固件已存储，触发转发...");
        wifi_ota_signal_relay();
    }

    const char resp[] = "{\"status\": \"ok\", \"progress\": 100}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    return ESP_OK;
}

// HTTP POST /ota/relay 处理器
static esp_err_t ota_relay_trigger_handler(httpd_req_t *req)
{
    (void)req;
    DEBUG_INFO("WIFI", "收到触发 OTA 中继请求");
    wifi_ota_signal_relay();

    const char resp[] = "{\"status\": \"ok\", \"message\": \"OTA relay triggered\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    return ESP_OK;
}

// HTTP GET /ota/status 处理器
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    ota_meta_t meta;
    meta_load(&meta);

    char resp[256];
    int len = snprintf(resp, sizeof(resp),
        "{\"total_size\": %lu, \"stored_size\": %lu, \"last_seq\": %lu, "
        "\"storage_done\": %s, \"relay_state\": %d}",
        (unsigned long)meta.total_size, (unsigned long)meta.stored_size,
        (unsigned long)meta.last_seq,
        s_ota_storage_done ? "true" : "false",
        g_relay_state);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);

    return ESP_OK;
}

// HTTP GET /health 处理器
static esp_err_t health_handler(httpd_req_t *req)
{
    health_state_t state = health_get_current();

    char resp[128];
    int len = snprintf(resp, sizeof(resp),
        "{\"health\": %d, \"free_heap\": %lu}",
        state, (unsigned long)esp_get_free_heap_size());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);

    return ESP_OK;
}

// HTTP GET /wifi/status 处理器
static esp_err_t wifi_status_handler(httpd_req_t *req)
{
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

// ----------------------------------------
// 启动 HTTP OTA 服务器
// ----------------------------------------
static esp_err_t http_ota_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_SERVER_PORT;
    config.stack_size = HTTP_SERVER_STACK_SIZE;

    DEBUG_INFO("WIFI", "启动 HTTP 服务器，端口 %d...", config.server_port);

    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        DEBUG_ERR("WIFI", "HTTP 服务器启动失败: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t upload_uri = {
        .uri = "/ota/upload",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &upload_uri);

    httpd_uri_t relay_uri = {
        .uri = "/ota/relay",
        .method = HTTP_POST,
        .handler = ota_relay_trigger_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &relay_uri);

    httpd_uri_t status_uri = {
        .uri = "/ota/status",
        .method = HTTP_GET,
        .handler = ota_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &status_uri);

    httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &health_uri);

    httpd_uri_t wifi_status_uri = {
        .uri = "/wifi/status",
        .method = HTTP_GET,
        .handler = wifi_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_http_server, &wifi_status_uri);

    DEBUG_INFO("WIFI", "HTTP 服务器已启动");

    return ESP_OK;
}

// ----------------------------------------
// 公开 API
// ----------------------------------------
void wifi_ota_signal_relay(void)
{
    if (s_relay_triggered) {
        DEBUG_WARN("WIFI", "转发已经触发过");
        return;
    }

    s_relay_triggered = true;

    if (g_ota_dispatch_queue) {
        uint32_t msg = 1;
        xQueueSend(g_ota_dispatch_queue, &msg, pdMS_TO_TICKS(100));
        DEBUG_INFO("WIFI", "OTA 调度信号已发送到中继任务");
    }
}

ota_wifi_mode_t wifi_get_mode(void)
{
    return s_wifi_mode;
}

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

bool wifi_is_connected(void)
{
    return s_sta_connected;
}

// ----------------------------------------
// WiFi OTA FreeRTOS 任务
// ----------------------------------------
void wifi_ota_task(void *pvParameters)
{
    (void)pvParameters;

    DEBUG_INFO("WIFI", "WiFi OTA 任务启动");

    // 初始化 NVS
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        DEBUG_ERR("WIFI", "NVS 初始化失败: %s", esp_err_to_name(err));
    }

    // 初始化网络接口
    ESP_ERROR_CHECK(esp_netif_init());

    // STA 优先，失败自动切 AP
#if CONFIG_WIFI_MODE_IS_STA
    err = wifi_init_sta();
    if (err == ESP_OK) {
        char ip_str[16] = {0};

        // 设置静态 IP（如果配置了）
        wifi_set_static_ip();

        // 等待连接成功
        err = wifi_wait_sta_connected(ip_str, sizeof(ip_str));
        if (err == ESP_OK) {
            DEBUG_INFO("WIFI", "STA 已连接, IP: %s", ip_str);
        } else {
            DEBUG_WARN("WIFI", "STA 连接超时，切换到 AP 模式...");
            esp_wifi_stop();
            s_wifi_mode = OTA_WIFI_MODE_AP;
            wifi_init_ap();
        }
    }
#else
    wifi_init_ap();
#endif

    // 启动 HTTP 服务器
    err = http_ota_server_start();
    if (err != ESP_OK) {
        DEBUG_ERR("WIFI", "HTTP 服务器启动失败");
    }

    DEBUG_INFO("WIFI", "WiFi OTA 服务器就绪");

    // 定期检查 WiFi 状态
    while (1) {
        int8_t rssi = wifi_get_rssi();
        if (rssi != -127) {
            health_update_from_wifi_rssi(rssi);

            if (rssi < WIFI_RSSI_CRITICAL) {
                DEBUG_WARN("WIFI", "WiFi RSSI 危险: %d dBm", rssi);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
