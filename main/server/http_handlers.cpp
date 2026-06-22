/*
 * http_handlers.cpp - HTTP 请求处理器实现
 * 从 wifi_ota.cpp 提取的 HTTP handler 函数
 */
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "cJSON.h"

#include "app_config.h"
#include "http_handlers.h"
#include "wifi_ota.h"
#include "esp32_ota_core.h"
#include "ota_coordinator.h"
#include "ota_relay.h"
#include "ota_snapshot.h"
#include "storage/gd32_flash_store.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"
#include "spi_master.h"
#include "canfd_bridge.h"
#include "ble_ota.h"

static constexpr uint32_t GD32_OTA_FLASH_SECTOR_SIZE = 4096U;

// OTA 接收共享缓冲区
static uint8_t s_ota_recv_buf[OTA_BLOCK_SIZE];

static uint32_t align_up_u32(uint32_t value, uint32_t alignment)
{
    return ((value + alignment - 1U) / alignment) * alignment;
}

void http_set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
}

esp_err_t http_options_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t esp32_ota_update_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    Esp32OtaCore &core = Esp32OtaCore::instance();
    if (!core.isActive()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA not started");
        return ESP_FAIL;
    }

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

    DEBUG_INFO("OTA", "ESP32 OTA finalize, %u bytes...", (unsigned)core.getReceived());
    Esp32FinalizeResult fr = OtaCoordinator::instance().finalizeForReboot(
        has_frontend_sha ? frontend_sha_hex : nullptr);

    if (fr.err == ESP_OK) {
        char resp[320] = {0};
        snprintf(resp, sizeof(resp),
                 "{\"status\": \"ok\", \"message\": \"OTA finished, rebooting\", "
                 "\"sha256\": \"%s\"}",
                 fr.device_sha_hex);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));

        DEBUG_INFO("OTA", "响应已发送，重启...");
        OtaCoordinator::instance().restartEsp32();
        return ESP_OK;
    }

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

esp_err_t restart_handler(httpd_req_t *req)
{
    (void)req;
    http_set_cors_headers(req);

    DEBUG_INFO("OTA", "收到重启请求，重启...");

    const char resp[] = "{\"status\": \"ok\", \"message\": \"rebooting\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    OtaCoordinator::instance().restartEsp32();
    return ESP_OK;
}

static int recv_json_header(httpd_req_t *req, char *buf, int buf_size)
{
    int total = 0;
    int depth = 1;
    while (total < buf_size - 1) {
        int ret = httpd_req_recv(req, buf + total, 1);
        if (ret <= 0) return total;
        total += ret;
        buf[total] = '\0';
        if (buf[total - 1] == '{') depth++;
        else if (buf[total - 1] == '}') {
            depth--;
            if (depth == 0) return total;
        }
    }
    return total;
}

esp_err_t ota_upload_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    DEBUG_WARN("WIFI", "=== HTTP: POST /ota/upload === content_len=%d", req->content_len);
    DEBUG_INFO("WIFI", "收到固件上传请求, 内容长度: %d 字节", req->content_len);

    esp_wifi_set_ps(WIFI_PS_NONE);
    // ★ L1: WiFi OTA 开始时恢复 BLE 共存状态（通过协调器）
    OtaCoordinator::instance().notifyBleCoexEnd();

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
        char json_buf[256];
        json_buf[0] = peek;
        json_header_len = recv_json_header(req, json_buf + 1, sizeof(json_buf) - 1);
        if (json_header_len > 0) {
            json_header_len++;
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
        if (total_size == 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing total_size");
            return ESP_FAIL;
        }

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
        uint8_t *fw_buf = s_ota_recv_buf;

        size_t total_firmware_written = 0;
        size_t total_received = json_header_len;
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

    const uint32_t gd32_fw_size = (uint32_t)req->content_len;
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

    if (OtaCoordinator::instance().beginGd32Storage(gd32_valid_size) != ESP_OK) {
        DEBUG_ERR("WIFI", "GD32 OTA 协调器拒绝存储（系统忙）");
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "system busy with another OTA operation");
        return ESP_FAIL;
    }

    WifiOta::instance().stateLock();
    WifiOta::instance().setGd32StorageDone(false);
    WifiOta::instance().stateUnlock();

    const uint32_t gd32_erase_size = align_up_u32(gd32_valid_size, GD32_OTA_FLASH_SECTOR_SIZE);
    DEBUG_INFO("WIFI", "擦除 gd32_ota 分区: %u bytes (fw=%u)",
               (unsigned)gd32_erase_size, (unsigned)gd32_valid_size);
    esp_err_t erase_err = OtaCoordinator::instance().eraseGd32Partition(gd32_erase_size);
    if (erase_err != ESP_OK) {
        DEBUG_ERR("WIFI", "擦除 gd32_ota 分区失败: %s", esp_err_to_name(erase_err));
        char err_msg[96];
        snprintf(err_msg, sizeof(err_msg), "erase gd32_ota failed: %s", esp_err_to_name(erase_err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err_msg);
        return ESP_FAIL;
    }
    OtaCoordinator::instance().clearGd32Meta();

    uint8_t *buf = s_ota_recv_buf;

    int received = 0;

    uint32_t expected_fw_size_raw = gd32_valid_size;
    ota_meta_t meta_raw = {
        .total_size = expected_fw_size_raw,
        .stored_size = 0,
        .last_seq = 0,
        .checksum = {0}
    };
    OtaCoordinator::instance().saveGd32Meta(&meta_raw);
    uint32_t gd32_offset = 0;

    DEBUG_INFO("WIFI", "GD32 raw upload start: %u bytes", expected_fw_size_raw);

    if (json_header_len == 0 && pre_read_len > 0) {
        int first_chunk_len = pre_read_len;
        if (first_chunk_len > req->content_len) first_chunk_len = req->content_len;
        if (first_chunk_len > 0) {
            gd32_offset = 0;
            esp_err_t err = OtaCoordinator::instance().writeGd32Chunk(gd32_offset, pre_read_buf, (size_t)first_chunk_len);
            if (err != ESP_OK) {
                DEBUG_ERR("WIFI", "GD32 first chunk write failed: len=%d err=%s",
                          first_chunk_len, esp_err_to_name(err));
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write gd32_ota failed");
                return ESP_FAIL;
            }
            received = first_chunk_len;
            gd32_offset += (uint32_t)first_chunk_len;
            OtaCoordinator::instance().updateGd32Meta(gd32_offset, (uint32_t)(gd32_offset / OTA_BLOCK_SIZE));
        }
    } else {
        received = pre_read_len;
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

        esp_err_t err = OtaCoordinator::instance().writeGd32Chunk(gd32_offset, buf, (size_t)ret);
        if (err != ESP_OK) {
            DEBUG_ERR("WIFI", "GD32 write failed: offset=%u len=%d err=%s",
                      (unsigned)gd32_offset, ret, esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write gd32_ota failed");
            return ESP_FAIL;
        }

        received += ret;
        gd32_offset += (uint32_t)ret;
    }

    OtaCoordinator::instance().updateGd32Meta(gd32_offset, (uint32_t)(gd32_offset / OTA_BLOCK_SIZE));

    DEBUG_INFO("WIFI", "GD32 upload done: %u / %u bytes", (unsigned)gd32_offset, expected_fw_size_raw);

    ota_meta_t final_meta;
    OtaCoordinator::instance().loadGd32Meta(&final_meta);
    if (final_meta.total_size > 0 && final_meta.stored_size >= final_meta.total_size) {
        OtaCoordinator::instance().completeGd32Storage();
        WifiOta::instance().stateLock();
        WifiOta::instance().setGd32StorageDone(true);
        WifiOta::instance().stateUnlock();
        DEBUG_INFO("WIFI", "GD32 firmware fully stored, pre-computing SHA-256...");
        esp_err_t sha_err = OtaCoordinator::instance().finalizeGd32Checksum();
        if (sha_err != ESP_OK) {
            DEBUG_ERR("WIFI", "GD32 固件 SHA-256 计算失败: %s", esp_err_to_name(sha_err));
        }
    }

    const char resp[] = "{\"status\": \"ok\", \"progress\": 100, \"target\": \"gd32\", "
                       "\"message\": \"stored, call POST /ota/relay with sha256 to trigger\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

esp_err_t ota_relay_trigger_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    DEBUG_INFO("WIFI", "=== HTTP: POST /ota/relay ===");
    DEBUG_INFO("WIFI", "收到触发 OTA 中继请求");

    ota_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    if (OtaCoordinator::instance().loadGd32Meta(&meta) != ESP_OK || meta.total_size == 0 || meta.stored_size < meta.total_size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "firmware not fully stored");
        return ESP_FAIL;
    }

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

    uint8_t frontend_sha[32] = {0};
    for (int i = 0; i < 32; ++i) {
        char hex[3] = { frontend_sha_hex[i*2], frontend_sha_hex[i*2+1], 0 };
        frontend_sha[i] = (uint8_t)strtoul(hex, nullptr, 16);
    }

    uint8_t device_sha[32] = {0};
    char device_sha_hex[65] = {0};
    esp_err_t verify_err = OtaCoordinator::instance().verifyGd32Firmware(frontend_sha, device_sha);
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

    esp_err_t trigger_err = OtaCoordinator::instance().triggerGd32Relay();
    if (trigger_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            "relay trigger failed - system busy or firmware not ready");
        return ESP_FAIL;
    }

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"status\": \"ok\", \"message\": \"OTA relay triggered\", "
             "\"sha256\": \"%s\"}", device_sha_hex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    return ESP_OK;
}

esp_err_t logs_handler(httpd_req_t *req)
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

esp_err_t can_send_handler(httpd_req_t *req)
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

    uint8_t resp_data[CAN_MAX_DLC] = {0};
    int resp_len = canFdSend(ch, ext_id, tx_data, tx_dlc,
                                   resp_data, sizeof(resp_data), 1000);

    char resp[512];
    if (resp_len >= 0) {
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

esp_err_t spi_send_handler(httpd_req_t *req)
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

esp_err_t system_status_handler(httpd_req_t *req)
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

    // ★ PR1: 一次性快照，替代散读 OtaCoordinator / OtaRelay 多个 getter
    OtaSnapshot snap = OtaCoordinator::instance().getSnapshot();

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
        snap.sys_state_str,
        (int)snap.sys_state,
        (unsigned long)snap.meta.total_size,
        (unsigned long)snap.meta.stored_size,
        snap.storage_done ? "true" : "false",
        snap.relay.state,
        (unsigned long)snap.relay.seq,
        (unsigned long)snap.relay.total_packets,
        snap.relay.gd32_app_alive ? "true" : "false",
        snap.relay.version.app_major, snap.relay.version.app_minor,
        snap.relay.version.app_patch, snap.relay.version.app_build,
        snap.relay.version.boot_major, snap.relay.version.boot_minor,
        snap.relay.version.boot_patch, snap.relay.version.boot_build);

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

// ============================================================
// GET /system/probe — GD32 APP 单次 SPI 探测（PR2）
// ============================================================
/**
 * @brief 显式触发一次 GD32 APP 存活探测。
 *        从 system_status_handler 移出的副作用，改为独立端点。
 *        成功时更新 OtaRelay 缓存的 alive/version 并返回版本信息。
 */
esp_err_t system_probe_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);

    gd32_version_t ver = {};
    uint32_t uptime = 0;
    esp_err_t err = OtaCoordinator::instance().probeGd32AppOnce(&ver, &uptime);

    char resp[256];
    int len = snprintf(resp, sizeof(resp),
        "{\"alive\": %s, \"uptime_sec\": %" PRIu32 ", "
        "\"gd32_app\": \"v%d.%d.%d.%d\", \"gd32_boot\": \"v%d.%d.%d.%d\"}",
        (err == ESP_OK) ? "true" : "false",
        uptime,
        ver.app_major, ver.app_minor, ver.app_patch, ver.app_build,
        ver.boot_major, ver.boot_minor, ver.boot_patch, ver.boot_build);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}
