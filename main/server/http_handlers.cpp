/*
 * http_handlers.cpp - HTTP 请求处理器实现
 */
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "soc/soc.h"
#include "cJSON.h"

#include "app_config.h"
#include "http_handlers.h"
#include "wifi_ota.h"
#include "esp32_ota_core.h"
#include "ota_coordinator.h"
#include "ota_relay.h"
#include "storage/gd32_flash_store.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"
#include "monitor/cpu_stats.h"
#include "spi_master.h"
#include "canfd_bridge.h"
#include "monitor/can_feedback_task.h"
#include "monitor/gd32_adc_feedback.h"
#include "monitor/motor_state.h"
#include "monitor/motor_version.h"
#include "monitor/motor_ota_relay.h"
#include "monitor/task_stack.h"
#include "ble_ota.h"
#include "system_boot.h"

static size_t parseHexToBytes(const char *hex, uint8_t *out, size_t max_out)
{
    size_t hex_len = strlen(hex);
    size_t n = 0;
    for (size_t i = 0; i + 1 < hex_len && n < max_out; i += 2) {
        char byte_str[3] = { hex[i], hex[i + 1], 0 };
        out[n++] = (uint8_t)strtol(byte_str, NULL, 16);
    }
    return n;
}

static void bytesToHexUpper(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; ++i) {
        snprintf(out + i * 2, 3, "%02X", in[i]);
    }
    out[n * 2] = '\0';
}

/**
 * @brief 统一 JSON 错误响应（避免前端 resp.json() 被 httpd 明文坑）。
 * @param http_status 如 "400 Bad Request" / "403 Forbidden" / "500 Internal Server Error"
 * @param code        机器可读错误码（snake_case）
 * @param message     人类可读说明（勿含未转义双引号）
 */
static esp_err_t http_send_json_error(httpd_req_t *req, const char *http_status,
                                      const char *code, const char *message)
{
    char body[320];
    snprintf(body, sizeof(body),
             "{\"status\":\"error\",\"code\":\"%s\",\"message\":\"%s\"}",
             code ? code : "error",
             message ? message : "error");
    httpd_resp_set_status(req, http_status ? http_status : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

/** @brief OTA 收流：堆紧时让 LWIP 回收，再 httpd_req_recv。 */
static void otaUploadPaceIfHeapTight(void)
{
    const uint32_t free_pre = esp_get_free_heap_size();
    if (free_pre < 8192U) {
        (void)esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(free_pre < 4096U ? 5U : 1U));
    }
}

void http_set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
}

/* 显式声明 keep-alive，让浏览器明确复用 TCP 连接。
 * ESP-IDF httpd 默认 HTTP/1.1 持久连接，但不发此头时部分客户端/代理
 * 按 HTTP/1.0 语义关闭。4Hz motor 轮询每次新建 TCP 握手 ~10~30ms
 * 纯开销；keep-alive 后复用同一 TCP，省全部握手时间。
 *
 * 不放在 http_set_cors_headers 里：OTA 上传/relay handler 需要 Connection: close
 * （设备即将重启），它们仍调 cors_headers 但不应加 keep-alive，否则会与
 * httpd_resp_set_hdr("Connection","close") 产生重复头。 */
void http_set_keepalive_hdr(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
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
        /* 常见场景：finalize 已成功并重启 → 新固件无 OTA 会话 → 迟到的 /ota/update。 */
        return http_send_json_error(req, "400 Bad Request",
                                    "ota_not_started", "OTA not started");
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
            return http_send_json_error(req, "500 Internal Server Error",
                                        "sha_compute_failed", esp_err_to_name(sha_err));
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

        DEBUG_INFO("OTA", "响应已发送，等待 TCP 刷新...");
        vTaskDelay(pdMS_TO_TICKS(100));
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
        return http_send_json_error(req, "500 Internal Server Error",
                                    "finalize_failed", esp_err_to_name(fr.err));
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

/* OTA 上传静态收块：避免 OTA 中途再 malloc(2KB) 与 TCP 窗口抢堆。 */
static uint8_t s_ota_upload_recv[OTA_BLOCK_SIZE];
static uint8_t s_ota_json_scratch[256];
static uint8_t s_ota_preread[256];

esp_err_t ota_upload_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);

    uint8_t *recv_buf = s_ota_upload_recv;
    DEBUG_WARN("WIFI", "POST /ota/upload clen=%d free=%lu big=%lu",
               req->content_len,
               (unsigned long)esp_get_free_heap_size(),
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    DEBUG_INFO("WIFI", "收到固件上传请求, 内容长度: %d 字节", req->content_len);

    esp_wifi_set_ps(WIFI_PS_NONE);
    // WiFi OTA 开始时恢复 BLE 共存状态（通过协调器）
    OtaCoordinator::instance().notifyBleCoexEnd();

    char peek;
    int peek_ret = httpd_req_recv(req, &peek, 1);
    if (peek_ret <= 0) {
        return http_send_json_error(req, "500 Internal Server Error",
                                    "recv_failed", "receive failed");
    }

    bool is_esp32_ota = false;
    uint32_t total_size = 0;
    int json_header_len = 0;
    /* 静态 scratch：OTA 期间少两次 256B heap 分配 */
    uint8_t *pre_read_buf = s_ota_preread;
    memset(pre_read_buf, 0, 256);
    int pre_read_len = 1;
    pre_read_buf[0] = (uint8_t)peek;

    if (peek == '{') {
        char *json_buf = (char *)s_ota_json_scratch;
        memset(json_buf, 0, 256);
        json_buf[0] = peek;
        json_header_len = recv_json_header(req, json_buf + 1, 255);
        if (json_header_len > 0) {
            json_header_len++;
            json_buf[json_header_len] = '\0';
            pre_read_len = json_header_len;
            if (pre_read_len > 256) pre_read_len = 256;
            memcpy(pre_read_buf, json_buf, pre_read_len);

            cJSON *root = cJSON_Parse(json_buf);
            if (root) {
                cJSON *type_item = cJSON_GetObjectItem(root, "type");
                if (type_item && cJSON_IsString(type_item) &&
                    strcmp(cJSON_GetStringValue(type_item), "esp32") == 0) {
                    is_esp32_ota = true;
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
            return http_send_json_error(req, "400 Bad Request",
                                        "missing_total_size", "missing total_size");
        }

        /* ESP32 路径不再需要 pre_read（JSON 已消费，固件从流继续读） */
        pre_read_len = 0;

        esp_err_t err = OtaCoordinator::instance().beginEsp32OtaWithRetry(total_size, OTA_CHANNEL_WIFI);
        if (err != ESP_OK) {
            if (err == ESP_ERR_INVALID_STATE) {
                return http_send_json_error(req, "403 Forbidden",
                    "ota_occupied", "ESP32 OTA occupied by another channel");
            }
            return http_send_json_error(req, "500 Internal Server Error",
                                        "ota_begin_failed", "OTA begin failed");
        }

        Esp32OtaCore &core = Esp32OtaCore::instance();
        uint8_t *fw_buf = recv_buf;

        // ★ 捕获当前 epoch，用于失败清理时检查会话是否仍属于本 handler
        //   若同类型重启已发生（新请求接管），旧 handler 的 abortCurrentIfEpoch 不会中止新会话
        uint32_t my_epoch = OtaCoordinator::instance().getOtaEpoch();

        size_t total_firmware_written = 0;
        size_t total_received = json_header_len;

        DEBUG_INFO("WIFI", "ESP32收流 clen=%d hdr=%d exp=%d",
                   req->content_len, json_header_len,
                   (req->content_len > json_header_len)
                       ? (req->content_len - json_header_len) : 0);

        /* 浏览器 upload 进度到 100% 只表示 TCP 发完；设备仍在 recv+Flash。
         * sequential 擦写期间仍需让 LWIP 有机会跑；堆紧时 pacing。 */
        const int64_t t_recv0 = esp_timer_get_time();
        uint32_t write_chunks = 0U;

        while (total_received < (size_t)req->content_len) {
            size_t to_read = (size_t)req->content_len - total_received;
            if (to_read > OTA_BLOCK_SIZE) {
                to_read = OTA_BLOCK_SIZE;
            }

            otaUploadPaceIfHeapTight();

            int ret = httpd_req_recv(req, (char *)fw_buf, to_read);
            if (ret <= 0) {
                DEBUG_ERR("WIFI", "HTTP recv fail ret=%d OTA %u/%d free=%lu",
                          ret, (unsigned)total_received, req->content_len,
                          (unsigned long)esp_get_free_heap_size());
                OtaCoordinator::instance().abortCurrentIfEpoch(my_epoch);
                return http_send_json_error(req, "500 Internal Server Error",
                                            "http_recv_failed", "HTTP recv failed");
            }

            err = core.write(fw_buf, (size_t)ret);
            if (err != ESP_OK) {
                DEBUG_ERR("WIFI", "OTA write failed: %s @%u free=%lu",
                          esp_err_to_name(err), (unsigned)total_firmware_written,
                          (unsigned long)esp_get_free_heap_size());
                OtaCoordinator::instance().abortCurrentIfEpoch(my_epoch);
                return http_send_json_error(req, "500 Internal Server Error",
                                            "ota_write_failed", "OTA write failed");
            }

            total_firmware_written += (size_t)ret;
            total_received += (size_t)ret;
            ++write_chunks;

            (void)esp_task_wdt_reset();

        }

        /* 回包前不打 INFO：堆紧时 DEBUG_INFO→UDP 会与 httpd_resp_send 抢 TX pbuf。
         * 先让 LWIP 回收，成功后再记一条。 */
        vTaskDelay(pdMS_TO_TICKS(10));
        (void)esp_task_wdt_reset();
        taskYIELD();

        const char resp[] =
            "{\"status\": \"ok\", \"message\": \"stored, call POST /ota/update to finish\", "
            "\"target\": \"esp32\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        esp_err_t send_err = httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        if (send_err != ESP_OK) {
            DEBUG_ERR("WIFI", "ESP32 OTA回包失败: %s free=%lu",
                      esp_err_to_name(send_err),
                      (unsigned long)esp_get_free_heap_size());
            /* 再试一次极简回包 */
            send_err = httpd_resp_send(req, "{\"status\":\"ok\",\"target\":\"esp32\"}",
                                       HTTPD_RESP_USE_STRLEN);
            if (send_err != ESP_OK) {
                DEBUG_ERR("WIFI", "ESP32 OTA 简短响应仍失败: %s", esp_err_to_name(send_err));
            }
        } else {
            DEBUG_INFO("WIFI", "ESP32存完 %uB %lldms free=%lu",
                       (unsigned int)core.getReceived(),
                       (long long)((esp_timer_get_time() - t_recv0) / 1000),
                       (unsigned long)esp_get_free_heap_size());
        }
        return (send_err == ESP_OK) ? ESP_OK : ESP_FAIL;
    }

    const uint32_t gd32_fw_size = (uint32_t)req->content_len;
    uint32_t gd32_valid_size;
    if (json_header_len > 0 && (uint32_t)json_header_len < gd32_fw_size) {
        gd32_valid_size = (total_size > 0 && total_size < gd32_fw_size) ? total_size : (gd32_fw_size - (uint32_t)json_header_len);
    } else {
        gd32_valid_size = gd32_fw_size;
    }
    if (OtaRelay::getState() != RELAY_IDLE) {
        DEBUG_WARN("WIFI", "GD32 OTA 开始前复位中继状态: %d", OtaRelay::getState());
        /* 上次失败可能残留本端状态；同时尽力 CANCEL 远端半会话 */
        OtaRelay::instance().abortRemoteAndReset(true);
    }

    // 与 BLE 共用 prepareGd32Storage（内含 validateGd32Size + begin + erase + meta）
    esp_err_t prep_err = OtaCoordinator::instance().prepareGd32Storage(gd32_valid_size, OTA_CHANNEL_WIFI);
    if (prep_err == ESP_ERR_INVALID_SIZE) {
        DEBUG_ERR("WIFI", "firmware size invalid: valid=%u max=%u",
                  (unsigned)gd32_valid_size, (unsigned)OTA_MAX_FIRMWARE_SIZE);
        return http_send_json_error(req, "500 Internal Server Error",
            "firmware_too_large", "firmware too large or alignment overflow");
    } else if (prep_err == ESP_ERR_INVALID_STATE) {
        DEBUG_ERR("WIFI", "GD32 OTA 协调器拒绝存储（系统忙）");
        return http_send_json_error(req, "403 Forbidden",
            "system_busy", "system busy with another OTA operation");
    } else if (prep_err != ESP_OK) {
        DEBUG_ERR("WIFI", "GD32 OTA 准备失败: %s", esp_err_to_name(prep_err));
        return http_send_json_error(req, "500 Internal Server Error",
            "prepare_failed", esp_err_to_name(prep_err));
    }

    // ★ 捕获当前 epoch，用于失败清理时检查会话是否仍属于本 handler
    uint32_t my_epoch = OtaCoordinator::instance().getOtaEpoch();

    uint8_t *buf = recv_buf;

    int received = 0;

    // expected_fw_size_raw 仅用于后续日志；meta 已由 prepareGd32Storage 写入
    uint32_t expected_fw_size_raw = gd32_valid_size;
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
                OtaCoordinator::instance().abortCurrentIfEpoch(my_epoch);
                return http_send_json_error(req, "500 Internal Server Error",
                                            "gd32_write_failed", "write gd32_ota failed");
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

        otaUploadPaceIfHeapTight();

        int ret = httpd_req_recv(req, (char *)buf, to_read);
        if (ret <= 0) {
            DEBUG_ERR("WIFI", "GD32 upload interrupted: received=%d expected=%d",
                      received, req->content_len);
            OtaCoordinator::instance().abortCurrentIfEpoch(my_epoch);
            return http_send_json_error(req, "500 Internal Server Error",
                                        "upload_interrupted", "upload interrupted");
        }

        esp_err_t err = OtaCoordinator::instance().writeGd32Chunk(gd32_offset, buf, (size_t)ret);
        if (err != ESP_OK) {
            DEBUG_ERR("WIFI", "GD32 write failed: offset=%u len=%d err=%s",
                      (unsigned)gd32_offset, ret, esp_err_to_name(err));
            OtaCoordinator::instance().abortCurrentIfEpoch(my_epoch);
            return http_send_json_error(req, "500 Internal Server Error",
                                        "gd32_write_failed", "write gd32_ota failed");
        }

        received += ret;
        gd32_offset += (uint32_t)ret;

        (void)esp_task_wdt_reset();
    }

    OtaCoordinator::instance().updateGd32Meta(gd32_offset, (uint32_t)(gd32_offset / OTA_BLOCK_SIZE));

    ota_meta_t final_meta;
    OtaCoordinator::instance().loadGd32Meta(&final_meta);
    if (final_meta.total_size > 0 && final_meta.stored_size >= final_meta.total_size) {
        /* 先让出再算 SHA；回包前不打 INFO，避免与 httpd_resp_send 抢堆 */
        vTaskDelay(pdMS_TO_TICKS(10));
        (void)esp_task_wdt_reset();

        // 与 BLE 共用 finishGd32StorageAfterReceive：complete + SHA
        esp_err_t sha_err = OtaCoordinator::instance().finishGd32StorageAfterReceive();
        if (sha_err != ESP_OK) {
            /* 存盘已完成，仍回 status=ok；relay 阶段会再比前端 SHA。
             * 此处必须醒目告警，避免只当 INFO 被忽略。 */
            DEBUG_WARN("WIFI", "GD32 SHA预算失败:%s (relay再校)",
                       esp_err_to_name(sha_err));
        }
    }

    const char resp[] = "{\"status\": \"ok\", \"progress\": 100, \"target\": \"gd32\", "
                       "\"message\": \"stored, call POST /ota/relay with sha256 to trigger\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t send_err = httpd_resp_send(req, resp, strlen(resp));
    if (send_err != ESP_OK) {
        DEBUG_ERR("WIFI", "GD32 OTA回包失败: %s free=%lu",
                  esp_err_to_name(send_err),
                  (unsigned long)esp_get_free_heap_size());
    } else {
        DEBUG_INFO("WIFI", "GD32 OTA 存储+响应完成: %u / %u free=%lu",
                   (unsigned)gd32_offset, expected_fw_size_raw,
                   (unsigned long)esp_get_free_heap_size());
    }
    return (send_err == ESP_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t ota_relay_trigger_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    DEBUG_INFO("WIFI", "=== HTTP: POST /ota/relay ===");
    DEBUG_INFO("WIFI", "收到触发 OTA 中继请求");

    ota_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    if (OtaCoordinator::instance().loadGd32Meta(&meta) != ESP_OK || meta.total_size == 0 || meta.stored_size < meta.total_size) {
        return http_send_json_error(req, "400 Bad Request",
                                    "firmware_not_stored", "firmware not fully stored");
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
        return http_send_json_error(req, "400 Bad Request",
            "missing_sha256", "missing sha256 in body (frontend SHA-256 required)");
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
        DEBUG_ERR("OTA", "GD32 SHA-256 不匹配");
        DEBUG_ERR("OTA", "frontend=%s", frontend_sha_hex);
        DEBUG_ERR("OTA", "device=%s", device_sha_hex);
        /* 未进中继：须恢复 WiFi 上传期挂起的 BLE */
        (void)BleOta::instance().resumeStack();
        char err_resp[256];
        snprintf(err_resp, sizeof(err_resp),
                 "{\"status\": \"error\", \"message\": \"SHA-256 mismatch\", "
                 "\"device\": \"%s\"}", device_sha_hex);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, err_resp, strlen(err_resp));
        return ESP_FAIL;
    } else if (verify_err != ESP_OK) {
        (void)BleOta::instance().resumeStack();
        return http_send_json_error(req, "500 Internal Server Error",
                                    "sha_compute_failed", "SHA compute failed");
    }

    esp_err_t trigger_err = OtaCoordinator::instance().triggerGd32Relay();
    if (trigger_err != ESP_OK) {
        (void)BleOta::instance().resumeStack();
        return http_send_json_error(req, "400 Bad Request",
            "relay_trigger_failed",
            "relay trigger failed - system busy or firmware not ready");
    }

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"status\": \"ok\", \"message\": \"OTA relay triggered\", "
             "\"sha256\": \"%s\"}", device_sha_hex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    return ESP_OK;
}

/* /logs 分块流式 sink：每个 chunk 经 httpd_resp_send_chunk 发出。 */
static bool logs_chunk_sink(void *ctx, const char *data, int len)
{
    httpd_req_t *req = static_cast<httpd_req_t *>(ctx);
    return httpd_resp_send_chunk(req, data, len) == ESP_OK;
}

esp_err_t logs_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    http_set_keepalive_hdr(req);
    httpd_resp_set_type(req, "application/json");

    /* 流式发送：消除原 ~4KB 一次性 malloc，改为 512B 栈缓冲 + 分块。
     * lwip TX pbuf 峰值与应用堆峰值同时下降。 */
    int r = DebugLog::instance().ringDrainJsonStream(logs_chunk_sink, req);
    if (r == 1) {
        /* 各数据 chunk 已发完，补发终止空 chunk 结束 chunked 响应。 */
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    if (r == 0) {
        /* 无日志：一次性小响应（不进 chunked 编码）。 */
        return httpd_resp_send(req, "[]", 2);
    }
    /* r < 0：中途 send 失败，连接多半已断。 */
    return ESP_FAIL;
}


// ============================================================
// HTTP JSON 共享工作区（.bss 单份 ~3KB）
// 供 /system/status、/can/feedback、/system/heap_low_log 串行复用；
// 互斥保护，禁止并发写同一缓冲（httpd 多 worker 时会排队）。
// ============================================================
static constexpr size_t HTTP_JSON_WORK_CAP = 3072;
static char s_http_json_work[HTTP_JSON_WORK_CAP];
static SemaphoreHandle_t s_http_json_mux = nullptr;
static portMUX_TYPE s_http_json_init_mux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t httpJsonWorkMutex(void)
{
    if (s_http_json_mux == nullptr) {
        portENTER_CRITICAL(&s_http_json_init_mux);
        if (s_http_json_mux == nullptr) {
            s_http_json_mux = xSemaphoreCreateMutex();
        }
        portEXIT_CRITICAL(&s_http_json_init_mux);
    }
    return s_http_json_mux;
}

// ============================================================
// GET /can/feedback — 三路 CAN 反馈快照（GD32 推送，ESP32 缓存）
// 分块发送：复用 s_http_json_work，一次 HTTP 内按通道 send_chunk；
// 前端仍一次 fetch 收完整 JSON。
// ============================================================
/**
 * 监控/业务共用：entry data hex 上限。
 * 原 8B 只够 STATUS 预览，会截掉 GET_VERSION(0x00FF) 的 hard/boot（需 ≥22B）。
 * 提到 64 与 CANFD_ID_SNAPSHOT_DATA_MAX 对齐，完整透出 FD 帧。
 */
static constexpr uint8_t FEEDBACK_DATA_HEX_MAX = 64;

static int feedback_append(char *buf, size_t cap, int off, const char *fmt, ...)
{
    if (!buf || cap == 0 || off < 0 || (size_t)off >= cap) {
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, cap - (size_t)off, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return -1;
    }
    if (off + n >= (int)cap) {
        return -1; /* 分块装不下：调用方截断 entries */
    }
    return off + n;
}

esp_err_t can_feedback_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    http_set_keepalive_hdr(req);

    /* ?lite=1：轻量路径，跳过 entries 数组构造（~3KB → ~200B）。
     * motor.html 的 onFeedback 只用 ch/enabled/valid/rx/tx/err/tec/rec/bus_state，
     * 不需要 entries（CAN 帧快照，index.html/can.html 才用）。
     * 跳过 entries 省掉 N×snprintf+hex 编码 + chunked 发送，handler 从 ~10ms 降到 <1ms。 */
    bool want_lite = false;
    {
        char query[32];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char val[4] = {};
            if (httpd_query_key_value(query, "lite", val, sizeof(val)) == ESP_OK) {
                want_lite = (val[0] == '1' || val[0] == 'y' || val[0] == 't');
            }
        }
    }

    can_feedback_result_t results[CAN_CHANNEL_MAX] = {};
    bool valid[CAN_CHANNEL_MAX] = {};
    for (uint8_t i = 0; i < CAN_CHANNEL_MAX; ++i) {
        valid[i] = getLatestCanFeedback(i, &results[i]);
    }

    const bool feat_known = SpiMaster::instance().gd32FeatKnown();
    const uint32_t feat = SpiMaster::instance().gd32FeatFlags();
    const bool ch_en[CAN_CHANNEL_MAX] = {
        !feat_known || (feat & V2_FEAT_CAN_CH0) != 0U,
        !feat_known || (feat & V2_FEAT_CAN_CH1) != 0U,
        !feat_known || (feat & V2_FEAT_CAN_CH2) != 0U,
    };
    const unsigned ch_mask = feat_known
        ? (unsigned)((feat & V2_FEAT_CAN_CH_MASK) >> V2_FEAT_CAN_CH_SHIFT)
        : 0x7U;

    /* lite 路径：一次性小响应，不走 chunked，不需要 httpJsonWorkMutex（用栈缓冲即可）。
     * 体积上限：3 通道 × ~80B + header ~60B ≈ 300B，远小于 1024 栈预算。 */
    if (want_lite) {
        char resp[512];
        int off = snprintf(resp, sizeof(resp),
            "{\"feat_known\":%s,\"can_ch_mask\":%u,\"can_brs\":%s,\"channels\":[",
            feat_known ? "true" : "false",
            ch_mask,
            (feat_known && (feat & V2_FEAT_CAN_BRS)) ? "true" : "false");
        if (off < 0 || (size_t)off >= sizeof(resp)) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fb lite hdr");
            return ESP_FAIL;
        }
        for (uint8_t i = 0; i < CAN_CHANNEL_MAX; ++i) {
            const can_feedback_result_t &r = results[i];
            const char *stats_fmt;
            if (valid[i] && ch_en[i]) {
                stats_fmt = ",\"mode\":%u,\"seq\":%lu,\"rx\":%lu,\"tx\":%lu,"
                            "\"drop\":%lu,\"err\":%lu,\"tec\":%u,\"rec\":%u,"
                            "\"bus_state\":%u,\"count\":%u";
            } else {
                stats_fmt = "";
            }
            /* entries 省略，count 仍报真实 total，前端可据此判断有无快照 */
            int n = snprintf(resp + off, sizeof(resp) - (size_t)off,
                "%s{\"ch\":%u,\"enabled\":%s,\"valid\":%s",
                i == 0 ? "" : ",",
                (unsigned)i,
                ch_en[i] ? "true" : "false",
                valid[i] ? "true" : "false");
            if (n < 0 || off + n >= (int)sizeof(resp)) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fb lite ch");
                return ESP_FAIL;
            }
            off += n;
            if (valid[i] && ch_en[i]) {
                n = snprintf(resp + off, sizeof(resp) - (size_t)off,
                    stats_fmt,
                    (unsigned)r.stats.mode,
                    (unsigned long)r.stats.snapshot_seq,
                    (unsigned long)r.stats.rx_count,
                    (unsigned long)r.stats.tx_count,
                    (unsigned long)r.stats.rx_drop_count,
                    (unsigned long)r.stats.error_count,
                    (unsigned)r.stats.tec,
                    (unsigned)r.stats.rec,
                    (unsigned)r.stats.bus_state,
                    (unsigned)r.total_count);
                if (n < 0 || off + n >= (int)sizeof(resp)) {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fb lite stats");
                    return ESP_FAIL;
                }
                off += n;
            }
            /* 闭合 channel 对象 */
            n = snprintf(resp + off, sizeof(resp) - (size_t)off, "}");
            if (n < 0 || off + n >= (int)sizeof(resp)) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fb lite close");
                return ESP_FAIL;
            }
            off += n;
        }
        int n = snprintf(resp + off, sizeof(resp) - (size_t)off, "]}");
        if (n < 0 || off + n >= (int)sizeof(resp)) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fb lite end");
            return ESP_FAIL;
        }
        off += n;
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, resp, off);
    }

    /* 完整路径（含 entries）：保留给 index.html / can.html 的 CAN 监控页 */

    SemaphoreHandle_t mux = httpJsonWorkMutex();
    if (mux && xSemaphoreTake(mux, pdMS_TO_TICKS(200)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fb busy");
        return ESP_FAIL;
    }

    char *work = s_http_json_work;
    httpd_resp_set_type(req, "application/json");

    auto fail_unlock = [&](esp_err_t err) -> esp_err_t {
        if (mux) {
            xSemaphoreGive(mux);
        }
        return err;
    };

    int n = snprintf(work, HTTP_JSON_WORK_CAP,
        "{\"feat_known\":%s,\"can_ch_mask\":%u,\"can_brs\":%s,\"channels\":[",
        feat_known ? "true" : "false",
        ch_mask,
        (feat_known && (feat & V2_FEAT_CAN_BRS)) ? "true" : "false");
    if (n < 0 || (size_t)n >= HTTP_JSON_WORK_CAP) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fb hdr");
        return fail_unlock(ESP_FAIL);
    }
    if (httpd_resp_send_chunk(req, work, n) != ESP_OK) {
        return fail_unlock(ESP_FAIL);
    }

    for (uint8_t i = 0; i < CAN_CHANNEL_MAX; ++i) {
        const can_feedback_result_t &r = results[i];
        int off = 0;
        off = feedback_append(work, HTTP_JSON_WORK_CAP, off,
            "%s{\"ch\":%u,\"enabled\":%s,\"valid\":%s",
            i == 0 ? "" : ",",
            (unsigned)i,
            ch_en[i] ? "true" : "false",
            valid[i] ? "true" : "false");
        if (off < 0) {
            break;
        }

        if (valid[i] && ch_en[i]) {
            off = feedback_append(work, HTTP_JSON_WORK_CAP, off,
                ",\"mode\":%u,\"seq\":%lu,\"rx\":%lu,\"tx\":%lu,"
                "\"drop\":%lu,\"err\":%lu,\"tec\":%u,\"rec\":%u,"
                "\"bus_state\":%u,\"count\":%u,\"entries\":[",
                (unsigned)r.stats.mode,
                (unsigned long)r.stats.snapshot_seq,
                (unsigned long)r.stats.rx_count,
                (unsigned long)r.stats.tx_count,
                (unsigned long)r.stats.rx_drop_count,
                (unsigned long)r.stats.error_count,
                (unsigned)r.stats.tec,
                (unsigned)r.stats.rec,
                (unsigned)r.stats.bus_state,
                (unsigned)r.total_count);
            if (off < 0) {
                break;
            }

            uint8_t emitted = 0;
            for (uint8_t j = 0; j < r.total_count && j < CANFD_ID_SNAPSHOT_MAX; ++j) {
                const id_snapshot_entry_t &e = r.entries[j];
                uint8_t dataLen = (e.dlc < CANFD_ID_SNAPSHOT_DATA_MAX)
                    ? e.dlc : (uint8_t)CANFD_ID_SNAPSHOT_DATA_MAX;
                if (dataLen > FEEDBACK_DATA_HEX_MAX) {
                    dataLen = FEEDBACK_DATA_HEX_MAX;
                }

                /* 先估长度：装不下则截断 entries（count 仍报真实 total） */
                char entry_tmp[96 + FEEDBACK_DATA_HEX_MAX * 2];
                int elen = snprintf(entry_tmp, sizeof(entry_tmp),
                    "%s{\"id\":\"0x%08lX\",\"dlc\":%u,\"fd\":%u,"
                    "\"brs\":%u,\"esi\":%u,\"data\":\"",
                    emitted ? "," : "",
                    (unsigned long)e.id,
                    (unsigned)e.dlc,
                    (unsigned)e.fd_flag,
                    (unsigned)e.brs,
                    (unsigned)e.esi);
                if (elen < 0 || (size_t)elen >= sizeof(entry_tmp)) {
                    break;
                }
                for (uint8_t d = 0; d < dataLen; ++d) {
                    int hn = snprintf(entry_tmp + elen, sizeof(entry_tmp) - (size_t)elen,
                                      "%02X", e.data[d]);
                    if (hn != 2) {
                        break;
                    }
                    elen += hn;
                }
                if (elen + 2 >= (int)sizeof(entry_tmp)) {
                    break;
                }
                entry_tmp[elen++] = '"';
                entry_tmp[elen++] = '}';
                entry_tmp[elen] = '\0';

                if (off + elen + 2 >= (int)HTTP_JSON_WORK_CAP) {
                    break; /* 本通道 entries 截断 */
                }
                memcpy(work + off, entry_tmp, (size_t)elen);
                off += elen;
                emitted++;
            }
            off = feedback_append(work, HTTP_JSON_WORK_CAP, off, "]");
            if (off < 0) {
                break;
            }
        }
        off = feedback_append(work, HTTP_JSON_WORK_CAP, off, "}");
        if (off < 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, work, off) != ESP_OK) {
            return fail_unlock(ESP_FAIL);
        }
    }

    if (httpd_resp_send_chunk(req, "]}", 2) != ESP_OK) {
        return fail_unlock(ESP_FAIL);
    }
    esp_err_t end_err = httpd_resp_send_chunk(req, NULL, 0);
    return fail_unlock(end_err == ESP_OK ? ESP_OK : ESP_FAIL);
}

// ============================================================
// GET/POST /can/mode — 查询或切换三路 CAN 接收模式
// ============================================================
esp_err_t can_mode_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);

    if (req->method == HTTP_GET) {
        http_set_keepalive_hdr(req);   /* GET 轮询复用 TCP；POST 设置模式不复用 */
        can_feedback_result_t fb{};
        if (getLatestCanFeedback(0, &fb)) {
            char resp[64];
            int len = snprintf(resp, sizeof(resp),
                "{\"mode\":%u}", (unsigned)fb.stats.mode);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, resp, len);
        } else {
            can_rx_mode_t actual = CAN_RX_MODE_ALL_PASS;
            int rc = canFdSetRxMode(CAN_RX_MODE_ALL_PASS, &actual, 2000);
            char resp[64];
            int len = snprintf(resp, sizeof(resp),
                "{\"mode\":%u,\"source\":\"spi\",\"ok\":%s}",
                (unsigned)actual, rc == 0 ? "true" : "false");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, resp, len);
        }
        return ESP_OK;
    }

    char buf[128] = {0};
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

    cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
    if (!mode_item || !cJSON_IsNumber(mode_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 mode 字段");
        return ESP_FAIL;
    }

    int requested = (int)cJSON_GetNumberValue(mode_item);
    cJSON_Delete(root);

    can_rx_mode_t target = CAN_RX_MODE_ALL_PASS;
    if (requested == 1) {
        target = CAN_RX_MODE_FILTER_ID;
    } else if (requested == 2) {
        target = CAN_RX_MODE_BRIDGE_0_TO_12;
    }
    can_rx_mode_t actual = CAN_RX_MODE_ALL_PASS;
    int rc = canFdSetRxMode(target, &actual, 2000);

    char resp[96];
    int len = snprintf(resp, sizeof(resp),
        "{\"ok\":%s,\"requested\":%d,\"actual\":%u}",
        rc == 0 ? "true" : "false", requested, (unsigned)actual);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}
/**
 * @brief POST /can/send_batch — CAN 发送唯一入口（单帧 = frames 长度 1）。
 *
 * JSON: { "ch": 0, "frames": [ { "id": n, "dlc": n, "data": "HEX" }, ... ] }
 * 返回: { "status":"ok", "results":[ ... ], 可选 skipped/abort_* }
 * 单帧 canFdSend 超时 2000ms（等应答）；多帧 80ms/帧 + 帧间隔。
 * 发送期间暂停 CAN 反馈任务。
 */
esp_err_t can_send_batch_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);

    constexpr int kMaxBody = 49152;
    constexpr int kMaxFrames = 128;
    constexpr uint32_t kPerFrameTimeoutMs = 80U;
    constexpr uint32_t kSingleFrameTimeoutMs = 1200U;   /* 正常 <20ms；httpd 单线程，此值决定其它端点最坏阻塞 */
    constexpr uint32_t kInterFrameDelayMs = 1U;

    int content_len = req->content_len;
    if (content_len <= 0 || content_len > kMaxBody) {
        return http_send_json_error(req, "400 Bad Request", "bad_body",
                                    "body 长度无效或过大");
    }

    char *buf = static_cast<char *>(malloc(static_cast<size_t>(content_len) + 1U));
    if (!buf) {
        return http_send_json_error(req, "500 Internal Server Error", "oom", "内存不足");
    }

    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, buf + received, content_len - received);
        if (ret <= 0) {
            free(buf);
            return http_send_json_error(req, "400 Bad Request", "recv_fail", "接收数据失败");
        }
        received += ret;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    buf = nullptr;
    if (!root) {
        return http_send_json_error(req, "400 Bad Request", "json_parse", "JSON 解析失败");
    }

    cJSON *ch_item = cJSON_GetObjectItem(root, "ch");
    cJSON *frames = cJSON_GetObjectItem(root, "frames");
    if (!ch_item || !cJSON_IsArray(frames)) {
        cJSON_Delete(root);
        return http_send_json_error(req, "400 Bad Request", "bad_args", "需要 ch 与 frames 数组");
    }

    can_channel_t ch = static_cast<can_channel_t>(cJSON_GetNumberValue(ch_item));
    const int frame_count = cJSON_GetArraySize(frames);
    if (frame_count <= 0 || frame_count > kMaxFrames) {
        cJSON_Delete(root);
        return http_send_json_error(req, "400 Bad Request", "bad_frames",
                                    "frames 数量无效（1~128）");
    }
    const uint32_t frame_timeout_ms =
        (frame_count == 1) ? kSingleFrameTimeoutMs : kPerFrameTimeoutMs;

    cJSON *out_root = cJSON_CreateObject();
    cJSON *results = cJSON_CreateArray();
    if (out_root == nullptr || results == nullptr) {
        cJSON_Delete(out_root);
        cJSON_Delete(results);
        cJSON_Delete(root);
        return http_send_json_error(req, "500 Internal Server Error", "oom", "JSON 分配失败");
    }
    cJSON_AddStringToObject(out_root, "status", "ok");
    cJSON_AddItemToObject(out_root, "results", results);

    /* 发送期间暂停 CAN 反馈，独占 SPI 业务路径 */
    pauseCanFeedbackTask(true);

    auto fill_err = [](cJSON *item, int code, uint8_t gd32_err) {
        if (!item) {
            return;
        }
        cJSON_AddStringToObject(item, "status", "error");
        cJSON_AddNumberToObject(item, "code", code);
        cJSON_AddNumberToObject(item, "gd32_err", gd32_err);
    };

    for (int i = 0; i < frame_count; i++) {
        (void)esp_task_wdt_reset();

        cJSON *item = cJSON_CreateObject();
        if (item == nullptr) {
            break;
        }
        cJSON_AddItemToArray(results, item);

        cJSON *fr = cJSON_GetArrayItem(frames, i);
        if (!cJSON_IsObject(fr)) {
            fill_err(item, -1, 0);
            continue;
        }

        cJSON *id_item = cJSON_GetObjectItem(fr, "id");
        cJSON *data_item = cJSON_GetObjectItem(fr, "data");
        cJSON *dlc_item = cJSON_GetObjectItem(fr, "dlc");
        if (!id_item) {
            fill_err(item, -1, 0);
            continue;
        }

        uint32_t ext_id = static_cast<uint32_t>(cJSON_GetNumberValue(id_item));
        uint8_t dlc = dlc_item ? static_cast<uint8_t>(cJSON_GetNumberValue(dlc_item)) : 0U;
        if (dlc > CAN_MAX_DLC) {
            dlc = CAN_MAX_DLC;
        }
        uint8_t tx_data[CAN_MAX_DLC];
        memset(tx_data, 0, sizeof(tx_data));
        uint8_t tx_dlc = 0U;
        if (data_item && cJSON_IsString(data_item)) {
            const char *hex = cJSON_GetStringValue(data_item);
            if (hex) {
                tx_dlc = static_cast<uint8_t>(parseHexToBytes(hex, tx_data, CAN_MAX_DLC));
            }
        }
        if (tx_dlc == 0U && dlc > 0U) {
            tx_dlc = dlc;
        }

        uint8_t resp_data[CAN_MAX_DLC];
        memset(resp_data, 0, sizeof(resp_data));
        uint8_t gd32_err = 0U;
        int resp_len = canFdSend(ch, ext_id, tx_data, tx_dlc,
                                 resp_data, sizeof(resp_data), frame_timeout_ms, &gd32_err);

        if (resp_len >= 0) {
            char resp_hex[CAN_MAX_DLC * 2 + 1];
            memset(resp_hex, 0, sizeof(resp_hex));
            const size_t copy_n = (static_cast<size_t>(resp_len) < sizeof(resp_data))
                                      ? static_cast<size_t>(resp_len) : sizeof(resp_data);
            bytesToHexUpper(resp_data, copy_n, resp_hex);
            cJSON_AddStringToObject(item, "status", "ok");
            cJSON_AddNumberToObject(item, "sent", tx_dlc);
            cJSON_AddNumberToObject(item, "recv_len", static_cast<double>(copy_n));
            cJSON_AddStringToObject(item, "recv", resp_hex);
        } else {
            fill_err(item, resp_len, gd32_err);
            /* SPI 失败 / BUS_OFF / TEC 过高：中止剩余帧，由 skipped 字段告知前端 */
            const bool fatal_spi = (resp_len == -2);
            const bool fatal_bus = (resp_len == -3) &&
                                   (gd32_err == 0xE2U || gd32_err == 0xE3U);
            if (fatal_spi || fatal_bus) {
                const int skipped = frame_count - i - 1;
                if (skipped > 0) {
                    cJSON_AddNumberToObject(out_root, "skipped", skipped);
                    cJSON_AddNumberToObject(out_root, "abort_code", resp_len);
                    cJSON_AddNumberToObject(out_root, "abort_gd32_err", gd32_err);
                }
                break;
            }
        }

        if (i + 1 < frame_count) {
            vTaskDelay(pdMS_TO_TICKS(kInterFrameDelayMs));
        }
    }

    pauseCanFeedbackTask(false);
    cJSON_Delete(root);
    root = nullptr;

    char *printed = cJSON_PrintUnformatted(out_root);
    cJSON_Delete(out_root);
    out_root = nullptr;
    if (!printed) {
        static const char kOom[] =
            "{\"status\":\"error\",\"code\":\"json_print\",\"message\":\"JSON输出失败\"}";
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, kOom, HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, printed, HTTPD_RESP_USE_STRLEN);
    free(printed);
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
        payload_len = parseHexToBytes(cJSON_GetStringValue(data_item), payload_data, SPI_OTA_FRAME_MAX);
    }

    DEBUG_INFO("SPI", "WEB SPI: type=0x%02X, payload=%zu 字节",
               frame_type, payload_len);
    cJSON_Delete(root);

    char resp[1024];
    uint8_t resp_type = 0;
    uint8_t resp_payload[SPI_OTA_FRAME_MAX] = {0};
    size_t resp_len = 0;
    esp_err_t spi_err = SpiMaster::instance().transact(frame_type, payload_data, payload_len,
                                         &resp_type, resp_payload, sizeof(resp_payload), &resp_len,
                                         timeout_ms);

    if (spi_err == ESP_OK) {
        char resp_hex[SPI_OTA_FRAME_MAX * 2 + 1] = {0};
        bytesToHexUpper(resp_payload, resp_len, resp_hex);
        snprintf(resp, sizeof(resp),
            "{\"status\": \"ok\", \"type\": \"%02X\", \"payload_len\": %zu, \"resp_type\": \"%02X\", \"resp_len\": %zu, \"recv\": \"%s\"}",
            frame_type, payload_len, resp_type, resp_len, resp_hex);
        /* 日志只记 type/len，完整 payload 仅在 HTTP JSON 返回给前端 */
        DEBUG_INFO("SPI", "WEB SPI ok: type=0x%02X len=%zu",
                   resp_type, resp_len);
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

/** 发送当前工作区并清空 offset（分块；失败返回 false） */
static bool status_flush_chunk(httpd_req_t *req, char *buf, size_t cap, int *len)
{
    if (!req || !buf || !len || *len <= 0) {
        return true;
    }
    if (httpd_resp_send_chunk(req, buf, *len) != ESP_OK) {
        return false;
    }
    *len = 0;
    if (cap > 0) {
        buf[0] = '\0';
    }
    return true;
}

/* CPU 快照静态缓冲：替代每秒 calloc(1.2KB)，消除 IDLE 期持续的堆抖动。
 * 与 s_http_json_work 同由 httpJsonWorkMutex 串行化。 */
static CpuUsageSnapshot s_cpu_snapshot;

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

/** 将 CpuStats 1Hz 快照写入 JSON（不在 HTTP 线程采样）。最多 16 条任务以免撑爆 3KB 分块。 */
static int append_cpu_status_json(char* buf, size_t cap, int offset, const CpuUsageSnapshot& cpu)
{
    static constexpr size_t kCpuTaskJsonMax = 16;
    offset = append_jsonf(buf, cap, offset,
        "\"cpu\":{\"available\":%s,\"valid\":%s,\"usage_pct\":%.2f,\"idle_pct\":%.2f,\"sample_ms\":%lu,\"tasks\":[",
        cpu.available ? "true" : "false",
        cpu.valid ? "true" : "false",
        cpu.valid ? cpu.usage_pct : 0.0,
        cpu.valid ? cpu.idle_pct : 0.0,
        (unsigned long)(cpu.valid ? cpu.sample_ms : 0U));

    const size_t ntask =
        (cpu.task_count > kCpuTaskJsonMax) ? kCpuTaskJsonMax : cpu.task_count;
    for (size_t i = 0; i < ntask; ++i) {
        const CpuTaskMetric& task = cpu.tasks[i];
        offset = append_jsonf(buf, cap, offset,
            "%s{\"name\":\"%s\",\"task_number\":%lu,\"runtime_delta\":%" PRIu64 ",\"usage_pct\":%.2f}",
            i == 0 ? "" : ",",
            task.name,
            (unsigned long)task.task_number,
            task.runtime_delta,
            task.usage_pct);
    }

    // 不带尾随逗号；由后续拼接块自行添加前导逗号，避免双重逗号
    return append_jsonf(buf, cap, offset, "]}");
}

esp_err_t system_status_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    http_set_keepalive_hdr(req);

    /* ?lite=1：轻量路径，只返回 motor.html 高频轮询需要的 4 个字段
     * (wifi.ip/mode/connected + gd32_features)。不调 heap_caps_get_info /
     * collectTaskStackMetrics / OtaCoordinator::getSnapshot / SystemBoot::appendStatusJson
     * 等重活，避免在 httpd 单线程里阻塞几十~上百 ms 拖慢 motor/feedback 请求。
     * 完整 status 由前端 5s 拉一次。 */
    bool want_lite = false;
    {
        char query[32];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char val[4] = {};
            if (httpd_query_key_value(query, "lite", val, sizeof(val)) == ESP_OK) {
                want_lite = (val[0] == '1' || val[0] == 'y' || val[0] == 't');
            }
        }
    }
    if (want_lite) {
        char ip_str[16] = "0.0.0.0";
        const char *mode_str = "AP";
        bool connected = false;
        if (WifiOta::instance().m_wifiMode == OTA_WIFI_MODE_STA && WifiOta::instance().getStaConnected()) {
            esp_netif_ip_info_t ip_info;
            esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            }
            mode_str = "STA";
            connected = true;
        } else if (WifiOta::instance().m_wifiMode == OTA_WIFI_MODE_AP) {
            connected = true;
        }
        const bool feat_known = SpiMaster::instance().gd32FeatKnown();
        const uint32_t fl = SpiMaster::instance().gd32FeatFlags();
        char resp[256];
        int n = snprintf(resp, sizeof(resp),
            "{\"wifi\":{\"mode\":\"%s\",\"ip\":\"%s\",\"connected\":%s},"
            "\"gd32_features\":{\"known\":%s,\"flags\":%lu,"
            "\"can_ch0\":%s,\"can_ch1\":%s,\"can_ch2\":%s,\"can_ch_mask\":%u}}",
            mode_str, ip_str, connected ? "true" : "false",
            feat_known ? "true" : "false",
            (unsigned long)fl,
            (feat_known && (fl & V2_FEAT_CAN_CH0)) ? "true" : "false",
            (feat_known && (fl & V2_FEAT_CAN_CH1)) ? "true" : "false",
            (feat_known && (fl & V2_FEAT_CAN_CH2)) ? "true" : "false",
            feat_known ? (unsigned)((fl & V2_FEAT_CAN_CH_MASK) >> V2_FEAT_CAN_CH_SHIFT) : 7U);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, resp, n > 0 ? n : 0);
    }

    /* layout=1：附带链接期静态布局（IRAM/.data/.bss/堆容量等），前端缓存后轮询不再要。
     * 默认只下发 memory 动态字段，减 status JSON 体积。 */
    bool want_mem_layout = false;
    {
        char query[48];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char val[8] = {};
            if (httpd_query_key_value(query, "layout", val, sizeof(val)) == ESP_OK) {
                want_mem_layout = (val[0] == '1' || val[0] == 'y' || val[0] == 'Y' ||
                                   val[0] == 't' || val[0] == 'T');
            }
        }
    }

    /* 堆极紧时仍返回精简 JSON，禁止 500（前端轮询会刷红）。 */
    const size_t largest_now = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    if (largest_now < 512U) {
        char tiny[192];
        int n = snprintf(tiny, sizeof(tiny),
            "{\"version\":\"%s\",\"free_heap\":%lu,\"largest_free_block\":%lu,"
            "\"wifi\":{\"connected\":%s},\"ota\":{\"sys_state\":\"degraded\"},"
            "\"cpu\":{\"available\":false,\"valid\":false}}",
            FW_VERSION,
            (unsigned long)esp_get_free_heap_size(),
            (unsigned long)largest_now,
            (WifiOta::instance().m_wifiMode == OTA_WIFI_MODE_STA &&
             WifiOta::instance().getStaConnected()) ? "true" : "false");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, tiny, n > 0 ? n : 0);
        return ESP_OK;
    }

    SemaphoreHandle_t mux = httpJsonWorkMutex();
    if (mux) {
        if (xSemaphoreTake(mux, pdMS_TO_TICKS(200)) != pdTRUE) {
            /* 与 feedback/heap_low 争用工作区：仍回简包，避免 500 刷红 */
            char busy[160];
            snprintf(busy, sizeof(busy),
                     "{\"version\":\"%s\",\"ota\":{\"sys_state\":\"busy\"},"
                     "\"cpu\":{\"available\":false,\"valid\":false}}",
                     FW_VERSION);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, busy, HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }

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

    // 先自愈孤儿忙态（如 SHA 失败后卡在 ESP32_FINALIZING），再取快照给 UI
    OtaCoordinator::instance().healOrphanStates();
    // 一次性快照，替代散读 OtaCoordinator / OtaRelay 多个 getter
    OtaSnapshot snap = OtaCoordinator::instance().getSnapshot();

    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    size_t largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    size_t internal_free_heap = heap_caps_get_free_size(INTERNAL_HEAP_CAPS);
    size_t internal_min_free_heap = heap_caps_get_minimum_free_size(INTERNAL_HEAP_CAPS);
    size_t internal_largest_free_block = heap_caps_get_largest_free_block(INTERNAL_HEAP_CAPS);
    size_t internal_heap_total = heap_caps_get_total_size(INTERNAL_HEAP_CAPS);
    size_t default_heap_total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);

    /* 内存全景：堆只是 SRAM 中可 malloc 的部分。
     * 静态段用链接符号拆分（与 map 文件一致）：
     *   [IRAM code | .data | .bss | heap ...]  （C3 上 IRAM/DRAM 同一物理 SRAM）
     * 手册 400KB = DRAM 窗口 + 约 16KB 指令 cache（cache 不在 SOC_DRAM_* 窗口内）。 */
    multi_heap_info_t internal_info = {};
    heap_caps_get_info(&internal_info, INTERNAL_HEAP_CAPS);

    extern char _iram_start[];
    extern char _iram_end[];
    extern char _data_start[];
    extern char _data_end[];
    extern char _bss_start[];
    extern char _bss_end[];
    extern char _heap_start[];
    auto sym_span = [](const char *a, const char *b) -> size_t {
        const uintptr_t ua = reinterpret_cast<uintptr_t>(a);
        const uintptr_t ub = reinterpret_cast<uintptr_t>(b);
        return (ub >= ua) ? static_cast<size_t>(ub - ua) : 0U;
    };
    const size_t iram_bytes = sym_span(_iram_start, _iram_end);
    const size_t data_bytes = sym_span(_data_start, _data_end);
    const size_t bss_bytes = sym_span(_bss_start, _bss_end);
    /* C3 DIRAM：DRAM 基址起至 _heap_start 即为 IRAM+.data+.bss 占位 */
    const uintptr_t heap_start_addr = reinterpret_cast<uintptr_t>(_heap_start);
    const size_t static_to_heap =
        (heap_start_addr >= static_cast<uintptr_t>(SOC_DRAM_LOW))
            ? static_cast<size_t>(heap_start_addr - static_cast<uintptr_t>(SOC_DRAM_LOW))
            : (iram_bytes + data_bytes + bss_bytes);

    const size_t sram_soc =
        (SOC_DRAM_HIGH > SOC_DRAM_LOW) ? (size_t)(SOC_DRAM_HIGH - SOC_DRAM_LOW) : 0U;
    /* 应用实际接管：静态链接区 + 内部堆（比整窗更贴近“可用 DRAM”） */
    const size_t sram_app = static_to_heap + internal_heap_total;
    const size_t sram_total = (sram_app > 0) ? sram_app : sram_soc;
    const size_t internal_heap_used = internal_info.total_allocated_bytes;
    const size_t static_reserved = static_to_heap;
    const size_t reserved_tail =
        (sram_soc > sram_app) ? (sram_soc - sram_app) : 0U;
    const size_t dma_total = heap_caps_get_total_size(MALLOC_CAP_DMA);
    const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t rtc_total = heap_caps_get_total_size(MALLOC_CAP_RTCRAM);
    const size_t rtc_free = heap_caps_get_free_size(MALLOC_CAP_RTCRAM);

    // 任务栈指标复用 health_monitor 共享的任务表（collectTaskStackMetrics），
    //   避免与巡检逻辑两地维护；http_server 在本 handler 上下文中，句柄即当前任务。
    TaskStackMetric task_metrics[12];
    size_t task_metrics_count = collectTaskStackMetrics(task_metrics, 12);
    size_t task_stack_total_bytes = 0;
    for (size_t i = 0; i < task_metrics_count; ++i) {
        task_stack_total_bytes += task_metrics[i].stack_size_bytes;
    }
    // http_server 行的句柄在 httpd 线程中取当前任务更准确
    for (size_t i = 0; i < task_metrics_count; ++i) {
        if (strcmp(task_metrics[i].name, "http_server") == 0) {
            task_metrics[i].handle = xTaskGetCurrentTaskHandle();
        }
    }

    /* CPU：只读 cpu_stats 任务的 1Hz 快照，禁止在 httpd 里 uxTaskGetSystemState */
    CpuUsageSnapshot *cpu = &s_cpu_snapshot;   // 用静态缓冲，不再每秒 calloc(1.2KB)
    CpuStats::instance().getSnapshot(*cpu);

    char *resp = s_http_json_work;
    memset(resp, 0, HTTP_JSON_WORK_CAP);
    httpd_resp_set_type(req, "application/json");

    /* ── 分块 1：核心状态 + OTA/WiFi ── */
    int len = snprintf(resp, HTTP_JSON_WORK_CAP,
        "{\"version\": \"%s\", \"build_time\": \"%s\", \"git_hash\": \"%s\", \"health\": %d, "
        "\"free_heap\": %lu, \"min_free_heap\": %lu, \"largest_free_block\": %lu, "
        "\"internal_free_heap\": %lu, \"internal_min_free_heap\": %lu, \"internal_largest_free_block\": %lu, "
        "\"internal_heap_total\": %lu, \"default_heap_total\": %lu, "
        "\"temperature\": %.1f, \"uptime_sec\": %lu, "
        "\"wifi\": {\"mode\": \"%s\", \"ip\": \"%s\", \"rssi\": %d, \"connected\": %s}, "
        "\"ota\": {\"sys_state\": \"%s\", \"sys_state_code\": %d, \"channel\": %d, \"channel_name\": \"%s\", "
        "\"relay_state\": %d, \"relay_seq\": %lu, \"relay_total_packets\": %lu, \"gd32_app_alive\": %s, "
        "\"gd32_app\": \"v%d.%d.%d.%d\", \"gd32_boot\": \"v%d.%d.%d.%d\"}, ",
        FW_VERSION, fwBuildTime(), FW_GIT_HASH, HealthMonitor::instance().getCurrent(),
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
        (int)snap.channel,
        snap.channel == OTA_CHANNEL_BLE ? "BLE" : "WIFI",
        snap.relay.state,
        (unsigned long)snap.relay.seq,
        (unsigned long)snap.relay.total_packets,
        snap.relay.gd32_app_alive ? "true" : "false",
        snap.relay.version.app_major, snap.relay.version.app_minor,
        snap.relay.version.app_patch, snap.relay.version.app_build,
        snap.relay.version.boot_major, snap.relay.version.boot_minor,
        snap.relay.version.boot_patch, snap.relay.version.boot_build);
    if (len < 0 || (size_t)len >= HTTP_JSON_WORK_CAP) {
        if (mux) {
            xSemaphoreGive(mux);
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status hdr");
        return ESP_FAIL;
    }
    if (!status_flush_chunk(req, resp, HTTP_JSON_WORK_CAP, &len)) {
        if (mux) {
            xSemaphoreGive(mux);
        }
        return ESP_FAIL;
    }

    /* ── 分块 2：CPU ── */
    len = append_cpu_status_json(resp, HTTP_JSON_WORK_CAP, 0, *cpu);
    if (!status_flush_chunk(req, resp, HTTP_JSON_WORK_CAP, &len)) {
        if (mux) {
            xSemaphoreGive(mux);
        }
        return ESP_FAIL;
    }

    /* ── 分块 3：SPI / ADC / features / boot / memory ── */
    {
        SpiMaster::SpiLinkStatus ls = SpiMaster::instance().linkStatus();
        const char *role_str = "unknown";
        switch (ls.gd32_role) {
            case 1: role_str = "app"; break;
            case 2: role_str = "boot"; break;
            default: break;
        }
        len = append_jsonf(resp, HTTP_JSON_WORK_CAP, 0,
            ",\"spi_link\":{\"version\":%u,\"gd32_role\":\"%s\",\"session_id\":\"0x%08lX\","
            "\"v2_active\":%s,\"local_credit\":%u,\"remote_credit\":%u,"
            "\"crc_errors\":%lu,\"duplicate_requests\":%lu,\"seq_errors\":%lu,"
            "\"retries\":%lu,\"ready_timeouts\":%lu,\"rx_overruns\":%lu,"
            "\"resyncs\":%lu,\"max_resp_latency_us\":%lu}",
            (unsigned)ls.version,
            role_str,
            (unsigned long)ls.session_id,
            SpiMaster::instance().v2Active() ? "true" : "false",
            (unsigned)ls.local_credit,
            (unsigned)ls.remote_credit,
            (unsigned long)ls.stats.crc_errors,
            (unsigned long)ls.stats.duplicate_requests,
            (unsigned long)ls.stats.seq_errors,
            (unsigned long)ls.stats.retries,
            (unsigned long)ls.stats.ready_timeouts,
            (unsigned long)ls.stats.rx_overruns,
            (unsigned long)ls.stats.resyncs,
            (unsigned long)ls.stats.max_resp_latency_us);
    }

    {
        gd32_adc_feedback_t ga = {};
        const bool have = getLatestGd32AdcFeedback(&ga);
        const uint32_t age = gd32AdcFeedbackAgeMs();
        const bool fresh = have && age != UINT32_MAX && age < 5000U;
        const bool feat_known = SpiMaster::instance().gd32FeatKnown();
        const bool feat_adc = SpiMaster::instance().gd32FeatAdcFeedback();
        const char *en = feat_known ? (feat_adc ? "true" : "false")
                                    : (have ? "true" : "null");
        if (have) {
            len = append_jsonf(resp, HTTP_JSON_WORK_CAP, len,
                ",\"gd32_adc\":{\"enabled\":%s,\"feat_known\":%s,\"fresh\":%s,\"valid\":%s,"
                "\"temperature\":%.2f,\"vdda\":%.3f,\"vin_adc\":%.3f,\"vcc\":%.3f,"
                "\"temp_raw\":%u,\"vref_raw\":%u,\"seq\":%lu,"
                "\"uptime_sec\":%lu,\"age_ms\":%lu}",
                en, feat_known ? "true" : "false",
                fresh ? "true" : "false", ga.valid ? "true" : "false",
                (double)ga.temperature_c, (double)ga.vdda_v, (double)ga.vin_adc_v,
                (double)ga.vcc_v,
                (unsigned)ga.temp_raw, (unsigned)ga.vref_raw,
                (unsigned long)ga.seq, (unsigned long)ga.uptime_sec,
                (unsigned long)(age == UINT32_MAX ? 0UL : (unsigned long)age));
        } else {
            len = append_jsonf(resp, HTTP_JSON_WORK_CAP, len,
                ",\"gd32_adc\":{\"enabled\":%s,\"feat_known\":%s,\"fresh\":false,\"valid\":false}",
                en, feat_known ? "true" : "false");
        }
        const uint32_t fl = SpiMaster::instance().gd32FeatFlags();
        len = append_jsonf(resp, HTTP_JSON_WORK_CAP, len,
            ",\"gd32_features\":{\"known\":%s,\"flags\":%lu,"
            "\"adc_feedback\":%s,\"uart_temp_print\":%s,"
            "\"spi_app_log\":%s,\"can_brs\":%s,"
            "\"can_ch0\":%s,\"can_ch1\":%s,\"can_ch2\":%s,\"can_ch_mask\":%u}",
            feat_known ? "true" : "false",
            (unsigned long)fl,
            (feat_known && (fl & V2_FEAT_ADC_FEEDBACK)) ? "true" : "false",
            (feat_known && (fl & V2_FEAT_UART_TEMP_PRINT)) ? "true" : "false",
            (feat_known && (fl & V2_FEAT_SPI_APP_LOG)) ? "true" : "false",
            (feat_known && (fl & V2_FEAT_CAN_BRS)) ? "true" : "false",
            (feat_known && (fl & V2_FEAT_CAN_CH0)) ? "true" : "false",
            (feat_known && (fl & V2_FEAT_CAN_CH1)) ? "true" : "false",
            (feat_known && (fl & V2_FEAT_CAN_CH2)) ? "true" : "false",
            feat_known ? (unsigned)((fl & V2_FEAT_CAN_CH_MASK) >> V2_FEAT_CAN_CH_SHIFT) : 7U);
    }

    len = SystemBoot::appendStatusJson(resp, HTTP_JSON_WORK_CAP, len);

    /* memory 动态字段：每次轮询；layout 仅 ?layout=1 时附带（链接期常量）。 */
    if (want_mem_layout) {
        len = append_jsonf(resp, HTTP_JSON_WORK_CAP, len,
            ",\"memory\":{\"layout\":{\"chip\":\"esp32c3\",\"sram_total\":%lu,\"sram_soc\":%lu,"
            "\"static_reserved\":%lu,\"reserved_tail\":%lu,"
            "\"iram\":%lu,\"data\":%lu,\"bss\":%lu,"
            "\"heap_total\":%lu,\"dma_total\":%lu,\"rtc_total\":%lu,"
            "\"task_stack_total\":%lu,\"default_heap_total\":%lu},"
            "\"heap_used\":%lu,\"heap_free\":%lu,"
            "\"heap_min_free\":%lu,\"heap_largest_free\":%lu,"
            "\"alloc_blocks\":%lu,\"free_blocks\":%lu,"
            "\"dma_free\":%lu,\"rtc_free\":%lu,\"default_heap_free\":%lu}",
            (unsigned long)sram_total,
            (unsigned long)sram_soc,
            (unsigned long)static_reserved,
            (unsigned long)reserved_tail,
            (unsigned long)iram_bytes,
            (unsigned long)data_bytes,
            (unsigned long)bss_bytes,
            (unsigned long)internal_heap_total,
            (unsigned long)dma_total,
            (unsigned long)rtc_total,
            (unsigned long)task_stack_total_bytes,
            (unsigned long)default_heap_total,
            (unsigned long)internal_heap_used,
            (unsigned long)internal_info.total_free_bytes,
            (unsigned long)internal_info.minimum_free_bytes,
            (unsigned long)internal_info.largest_free_block,
            (unsigned long)internal_info.allocated_blocks,
            (unsigned long)internal_info.free_blocks,
            (unsigned long)dma_free,
            (unsigned long)rtc_free,
            (unsigned long)free_heap);
    } else {
        len = append_jsonf(resp, HTTP_JSON_WORK_CAP, len,
            ",\"memory\":{\"heap_used\":%lu,\"heap_free\":%lu,"
            "\"heap_min_free\":%lu,\"heap_largest_free\":%lu,"
            "\"alloc_blocks\":%lu,\"free_blocks\":%lu,"
            "\"dma_free\":%lu,\"rtc_free\":%lu,\"default_heap_free\":%lu}",
            (unsigned long)internal_heap_used,
            (unsigned long)internal_info.total_free_bytes,
            (unsigned long)internal_info.minimum_free_bytes,
            (unsigned long)internal_info.largest_free_block,
            (unsigned long)internal_info.allocated_blocks,
            (unsigned long)internal_info.free_blocks,
            (unsigned long)dma_free,
            (unsigned long)rtc_free,
            (unsigned long)free_heap);
    }

    if (!status_flush_chunk(req, resp, HTTP_JSON_WORK_CAP, &len)) {
        if (mux) {
            xSemaphoreGive(mux);
        }
        return ESP_FAIL;
    }

    /* ── 分块 4：task_stack + 闭合 ── */
    len = append_jsonf(resp, HTTP_JSON_WORK_CAP, 0, ",\"task_stack\": {");
    for (size_t i = 0; i < task_metrics_count; ++i) {
        len = append_jsonf(resp, HTTP_JSON_WORK_CAP, len,
            "%s\"%s\":{\"stack_size_bytes\":%lu,\"high_watermark_bytes\":%lu,\"peak_used_bytes\":%lu}",
            i == 0 ? "" : ",",
            task_metrics[i].name,
            (unsigned long)task_metrics[i].stack_size_bytes,
            (unsigned long)get_task_stack_high_watermark_bytes(task_metrics[i].handle),
            (unsigned long)get_task_stack_peak_used_bytes(task_metrics[i].handle, task_metrics[i].stack_size_bytes));
    }
    len = append_jsonf(resp, HTTP_JSON_WORK_CAP, len, "}}");

    if (!status_flush_chunk(req, resp, HTTP_JSON_WORK_CAP, &len)) {
        if (mux) {
            xSemaphoreGive(mux);
        }
        return ESP_FAIL;
    }
    /* 结束 chunked body */
    (void)httpd_resp_send_chunk(req, NULL, 0);

    if (mux) {
        xSemaphoreGive(mux);
    }
    return ESP_OK;
}

// ============================================================
// GET /system/probe — GD32 APP 单次 SPI 探测
// ============================================================
/**
 * @brief 显式触发一次 GD32 APP 存活探测。
 *        成功时更新 OtaRelay 缓存的 alive/version 并返回版本信息。
 */
esp_err_t system_probe_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    http_set_keepalive_hdr(req);

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

// ============================================================
// GET /system/heap_low_log — 设备唯一快照槽（0 或 1 条）
// ============================================================
/**
 * @brief 返回设备侧最新一条"最低空闲创新低"上下文。
 * 多条历史由网页 IndexedDB 累积；JSON 复用 s_http_json_work（无独立 .bss）。
 */
esp_err_t system_heap_low_log_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);

    SemaphoreHandle_t mux = httpJsonWorkMutex();
    if (mux && xSemaphoreTake(mux, pdMS_TO_TICKS(200)) != pdTRUE) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"records\":[]}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char *resp = s_http_json_work;
    int len = HealthMonitor::instance().dumpHeapLowWaterJson(resp, HTTP_JSON_WORK_CAP, 0);
    if (len < 0) {
        len = 0;
    }
    if ((size_t)len >= HTTP_JSON_WORK_CAP) {
        len = (int)HTTP_JSON_WORK_CAP - 1;
    }
    resp[len] = '\0';

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, resp, len);
    if (mux) {
        xSemaphoreGive(mux);
    }
    return err;
}

/* ────────────────────────────────────────────────────────────
 * GET /motor/state — 多电机状态表（来自 GD32 0xC5 推送）
 * ──────────────────────────────────────────────────────────── */
esp_err_t motor_state_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    http_set_keepalive_hdr(req);

    motor_state_table_t tbl = {};
    const bool have = getLatestMotorState(&tbl);
    const uint32_t age = motorStateAgeMs();

    /* 二进制模式：?bin=1（前端默认走此路径，体积比 JSON 省 66%）。
     * 协议（LE，Header 12B + 每节点 Entry 44B）:
     *   Header:  magic  u16 = 0x4D53 ('MS')
     *            ver    u8  = 1
     *            flags  u8  = 0
     *            age_ms u32  (0xFFFFFFFF = 无数据)
     *            active u8  (在线电机数)
     *            count  u8  (有效 entry 数)
     *            rsvd   u16 = 0
     *   Entry:   node u8, ch u8, online u8, dlc u8,
     *            rx_count u32, seq u32, data[32]
     * 27 槽（9 node × 3 ch，node×channel 双键）最大 12 + 27*44 = 1200B。
     * 无 have_data 时仍发 header（count=0），让前端区分「无数据」与「空总线」。 */
    bool want_bin = false;
    {
        char query[32];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char val[4] = {};
            if (httpd_query_key_value(query, "bin", val, sizeof(val)) == ESP_OK) {
                want_bin = (val[0] == '1' || val[0] == 'y' || val[0] == 't');
            }
        }
    }
    if (want_bin) {
        httpd_resp_set_type(req, "application/octet-stream");
        /* 静态缓冲：27 槽扩容后 1200B 不再上栈（httpd 栈 7KB 已紧）；
         * httpd 单任务串行处理请求，静态缓冲无竞态。 */
        static uint8_t buf[12 + MOTOR_MAX_SLOTS * MOTOR_ENTRY_SIZE];
        size_t off = 0;
        /* magic 'MS' (LE: 0x53, 0x4D) */
        buf[off++] = 0x53U; buf[off++] = 0x4DU;
        buf[off++] = 1U;            /* version */
        /* flags bit0 = boot_active（总线有电机在 BOOT，0x103 心跳） */
        buf[off++] = (have && tbl.boot_active) ? 0x01U : 0x00U;
        /* age_ms LE u32 */
        const uint32_t age_send = (age == UINT32_MAX) ? 0xFFFFFFFFU : age;
        buf[off++] = (uint8_t)(age_send & 0xFF);
        buf[off++] = (uint8_t)((age_send >> 8) & 0xFF);
        buf[off++] = (uint8_t)((age_send >> 16) & 0xFF);
        buf[off++] = (uint8_t)((age_send >> 24) & 0xFF);
        buf[off++] = have ? tbl.active_count : 0U;   /* active */
        const size_t count_pos = off;                /* count，稍后回填 */
        off += 3U;                                   /* count u8 + rsvd u16 */
        /* off 现在应该是 12 */
        assert(off == 12);

        uint8_t count = 0;
        if (have) {
            for (uint8_t i = 0; i < MOTOR_MAX_SLOTS; ++i) {
                const motor_slot_t &s = tbl.slots[i];
                if (s.node_id == 0) continue;
                buf[off++] = s.node_id;
                buf[off++] = s.channel;
                buf[off++] = s.online;
                buf[off++] = s.dlc;
                /* rx_count LE */
                uint32_t r = s.rx_count;
                buf[off++] = (uint8_t)(r & 0xFF);
                buf[off++] = (uint8_t)((r >> 8) & 0xFF);
                buf[off++] = (uint8_t)((r >> 16) & 0xFF);
                buf[off++] = (uint8_t)((r >> 24) & 0xFF);
                /* seq LE */
                uint32_t sq = s.seq;
                buf[off++] = (uint8_t)(sq & 0xFF);
                buf[off++] = (uint8_t)((sq >> 8) & 0xFF);
                buf[off++] = (uint8_t)((sq >> 16) & 0xFF);
                buf[off++] = (uint8_t)((sq >> 24) & 0xFF);
                memcpy(&buf[off], s.data, MOTOR_STATUS_DLC);
                off += MOTOR_STATUS_DLC;
                count++;
            }
        }
        buf[count_pos] = count;

        return httpd_resp_send(req, (const char *)buf, off);
    }

    /* JSON 兼容路径（保留旧消费者，?bin 缺省时走此分支） */
    httpd_resp_set_type(req, "application/json");

    if (!have) {
        httpd_resp_sendstr(req, "{\"have_data\":false,\"active_count\":0,\"motors\":[]}");
        return ESP_OK;
    }

    SemaphoreHandle_t mux = httpJsonWorkMutex();
    if (mux && xSemaphoreTake(mux, pdMS_TO_TICKS(200)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "motor busy");
        return ESP_FAIL;
    }

    char *w = s_http_json_work;
    int off = snprintf(w, HTTP_JSON_WORK_CAP,
        "{\"have_data\":true,\"active_count\":%u,\"boot_active\":%s,\"age_ms\":%lu,\"motors\":[",
        (unsigned)tbl.active_count, tbl.boot_active ? "true" : "false", (unsigned long)age);

    bool first = true;
    for (uint8_t i = 0; i < MOTOR_MAX_SLOTS && off > 0 && (size_t)off < HTTP_JSON_WORK_CAP - 128; ++i) {
        const motor_slot_t &s = tbl.slots[i];
        if (s.node_id == 0) continue;

        /* data hex */
        char hex[MOTOR_STATUS_DLC * 2 + 1];
        for (uint8_t b = 0; b < MOTOR_STATUS_DLC; ++b) {
            snprintf(&hex[b * 2], 3, "%02X", s.data[b]);
        }

        off += snprintf(w + off, HTTP_JSON_WORK_CAP - off,
            "%s{\"node\":%u,\"ch\":%u,\"online\":%s,\"dlc\":%u,"
            "\"rx_count\":%lu,\"seq\":%lu,\"data\":\"%s\"}",
            first ? "" : ",",
            (unsigned)s.node_id, (unsigned)s.channel,
            s.online ? "true" : "false",
            (unsigned)s.dlc,
            (unsigned long)s.rx_count,
            (unsigned long)s.seq,
            hex);
        first = false;
    }

    if (off > 0 && (size_t)off < HTTP_JSON_WORK_CAP - 2) {
        off += snprintf(w + off, HTTP_JSON_WORK_CAP - off, "]}");
    }

    esp_err_t ret = httpd_resp_send(req, w, off > 0 ? off : 0);
    if (mux) {
        xSemaphoreGive(mux);
    }
    return ret;
}

/**
 * @brief /motor/version — 返回 GD32 缓存的电机版本（0xC6 事件推送）。
 *
 * 版本为按需查询的低频数据：前端发 0x00FF 请求触发电机应答，GD32 缓存后经
 * 0xC6 推给 ESP32，本端点从缓存读取。前端按 seq 判断是否为本次查询的新应答。
 *
 * JSON: {"have_data":bool,"nodes":[{"node":N,"seq":N,"age_ms":N,"version":"HEX(24B)"}]}
 */
esp_err_t motor_version_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    http_set_keepalive_hdr(req);

    motor_version_table_t tbl = {};
    const bool have = getLatestMotorVersion(&tbl);

    httpd_resp_set_type(req, "application/json");

    if (!have) {
        httpd_resp_sendstr(req, "{\"have_data\":false,\"nodes\":[]}");
        return ESP_OK;
    }

    SemaphoreHandle_t mux = httpJsonWorkMutex();
    if (mux && xSemaphoreTake(mux, pdMS_TO_TICKS(200)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "motor version busy");
        return ESP_FAIL;
    }

    const int64_t now_ms = esp_timer_get_time() / 1000;
    char *w = s_http_json_work;
    int off = snprintf(w, HTTP_JSON_WORK_CAP, "{\"have_data\":true,\"nodes\":[");

    bool first = true;
    for (uint8_t i = 0; i < MOTORVER_MAX_NODES && off > 0 && (size_t)off < HTTP_JSON_WORK_CAP - 128; ++i) {
        const motor_version_slot_t &s = tbl.slots[i];
        if (!s.valid || s.node_id == 0) continue;

        char hex[MOTORVER_DLC * 2 + 1];
        for (uint8_t b = 0; b < MOTORVER_DLC; ++b) {
            snprintf(&hex[b * 2], 3, "%02X", s.version[b]);
        }
        const long age = (s.recv_ms > 0 && now_ms >= s.recv_ms)
                             ? static_cast<long>(now_ms - s.recv_ms) : 0L;

        off += snprintf(w + off, HTTP_JSON_WORK_CAP - off,
            "%s{\"node\":%u,\"seq\":%lu,\"age_ms\":%ld,\"version\":\"%s\"}",
            first ? "" : ",",
            (unsigned)s.node_id,
            (unsigned long)s.seq,
            age,
            hex);
        first = false;
    }

    if (off > 0 && (size_t)off < HTTP_JSON_WORK_CAP - 2) {
        off += snprintf(w + off, HTTP_JSON_WORK_CAP - off, "]}");
    }

    esp_err_t ret = httpd_resp_send(req, w, off > 0 ? off : 0);
    if (mux) {
        xSemaphoreGive(mux);
    }
    return ret;
}

/* ═══════════════════ 电机 OTA relay ═══════════════════ */

esp_err_t motor_ota_upload_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);

    char query[16] = {};
    uint8_t channel = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[4] = {};
        if (httpd_query_key_value(query, "ch", val, sizeof(val)) == ESP_OK) {
            channel = static_cast<uint8_t>(atoi(val));
        }
    }

    const int content_len = req->content_len;
    if (content_len <= 0 || content_len > (int)MOTOR_OTA_MAX_FW_SIZE) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"firmware too large or empty\"}");
        return ESP_FAIL;
    }

    /* ── 协调：电机固件与 GD32E508 固件共用 gd32_ota 分区，先互斥 + 安全清理 ── */
    OtaCoordinator &coord = OtaCoordinator::instance();
    /* 有活跃 OTA（GD32 STORING/RELAYING、ESP32_* 等）时拒绝 */
    if (!coord.isIdleState(coord.getSystemState())) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"system busy with another OTA\"}");
        return ESP_FAIL;
    }
    if (motorOtaIsActive()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"motor OTA already in progress\"}");
        return ESP_FAIL;
    }
    /* 即将覆写 gd32_ota 分区：先失效已暂存的 GD32 固件并复位协调器到 IDLE，
       避免 /ota/relay 误把电机固件当成 GD32 固件中继刷进主控 */
    coord.abortCurrent();
    (void)coord.clearGd32Meta();

    /* ── 流式收固件写 flash（无整固件堆缓冲；复用 OTA 静态收块）──
     * ★ full_erase=true：强制擦除整个 gd32_ota 分区（252KB ~200ms），
     * 杜绝上次固件残留 + 擦除位错误导致的单 bit 翻转（0xAC20 处 0x19→0x18 根因）。 */
    Gd32FlashStore::instance().beginWriteSession(true);

    int received = 0;
    while (received < content_len) {
        size_t to_read = (size_t)content_len - received;
        if (to_read > OTA_BLOCK_SIZE) to_read = OTA_BLOCK_SIZE;

        otaUploadPaceIfHeapTight();

        int r = httpd_req_recv(req, reinterpret_cast<char *>(s_ota_upload_recv), to_read);
        if (r <= 0) {
            DEBUG_ERR("MOTOR_OTA", "upload interrupted: received=%d expected=%d", received, content_len);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }

        esp_err_t werr = Gd32FlashStore::instance().flashWriteChunk(
            (uint32_t)received, s_ota_upload_recv, (size_t)r);
        if (werr != ESP_OK) {
            DEBUG_ERR("MOTOR_OTA", "flash write fail @%d: %s", received, esp_err_to_name(werr));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
            return ESP_FAIL;
        }

        received += r;
        (void)esp_task_wdt_reset();
    }

    /* ── 收完固件写 meta（供 /motor/ota/relay 确认固件完整）──
     * 不在此处触发 SPI START + 喂数据：拆分两段式，与 BLE 刷 GD32 的
     * "prepare+收流 → /ota/relay" 对齐。收固件阶段纯写 flash，不碰 SPI，
     * 避免与 GD32 按铃推送争抢单缓冲 MISO 产生的 "SPI start failed" 竞态。 */
    ota_meta_t meta = {};
    meta.total_size  = static_cast<uint32_t>(content_len);
    meta.stored_size = static_cast<uint32_t>(received);
    meta.last_seq   = 0;
    esp_err_t merr = Gd32FlashStore::instance().metaSave(&meta);
    if (merr != ESP_OK) {
        DEBUG_ERR("MOTOR_OTA", "meta save fail after upload: %s", esp_err_to_name(merr));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"meta save failed\"}");
        return ESP_FAIL;
    }

    /* ★ 上传收尾计算 flash SHA-256 并存入 meta.checksum。
     * 电机 OTA 之前完全没有完整性校验：HTTP 流式写 flash → metaSave → relay 直接刷。
     * 一旦 flash 写入错位/损坏，ESP32 会把"一致的垃圾"喂给 GD32（CAN CRC 全过但 APP 跑飞）。
     * 这里在收流结束立即算 SHA-256，relay 触发时 metaVerifyChecksum 复核，从源头堵住坏数据。 */
    esp_err_t sha_err = Gd32FlashStore::instance().metaFinalizeChecksum();
    if (sha_err != ESP_OK) {
        DEBUG_ERR("MOTOR_OTA", "SHA-256 finalize fail after upload: %s", esp_err_to_name(sha_err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"sha256 compute failed\"}");
        return ESP_FAIL;
    }

    /* ★ 把设备端 flash SHA 直接回给前端：前端在发进 BOOT 帧之前就能比对，
     * flash 写坏时电机还在 APP 正常运行即可中止，不用等到 relay 阶段
     * （那时电机已被踢进 BOOT 白等）。relay 的三方比对仍保留作兜底。 */
    ota_meta_t sha_meta = {};
    char sha_hex[65] = {0};
    if (Gd32FlashStore::instance().metaLoad(&sha_meta) == ESP_OK) {
        for (int i = 0; i < 32; ++i) {
            snprintf(&sha_hex[i * 2], 3, "%02x", sha_meta.checksum[i]);
        }
    }

    httpd_resp_set_type(req, "application/json");
    char resp_buf[256];
    snprintf(resp_buf, sizeof(resp_buf),
        "{\"status\":\"stored\",\"message\":\"call POST /motor/ota/relay?ch=%u to trigger\","
        "\"channel\":%u,\"size\":%d,\"sha256\":\"%s\"}",
        (unsigned)channel, (unsigned)channel, received, sha_hex);
    httpd_resp_sendstr(req, resp_buf);
    return ESP_OK;
}

esp_err_t motor_ota_relay_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    http_set_keepalive_hdr(req);
    DEBUG_INFO("MOTOR_OTA", "=== HTTP: POST /motor/ota/relay ===");

    char query[16] = {};
    uint8_t channel = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[4] = {};
        if (httpd_query_key_value(query, "ch", val, sizeof(val)) == ESP_OK) {
            channel = static_cast<uint8_t>(atoi(val));
        }
    }

    /* 读 meta 拿 fw_size（motorOtaStart 参数的唯一来源）。
     * total_size==0 = 无固件可刷（未上传过，或上次 relay 成功后 metaClear 已消费）。
     * 数量完整性由上传流程保证：上传开头 clearGd32Meta，收满 content_len 才 metaSave，
     * 中断则 meta 保持空——因此这里无需再查 stored_size。 */
    ota_meta_t meta = {};
    if (Gd32FlashStore::instance().metaLoad(&meta) != ESP_OK || meta.total_size == 0) {
        return http_send_json_error(req, "400 Bad Request",
                                    "firmware_not_stored", "no firmware stored (upload first)");
    }

    /* relay 不做 SHA 校验（2026-07-27）：完整性校验已在上传阶段闭环
     *（ESP32 从 flash 读回算 SHA 随响应返回，前端比对 + 失败重传）。
     * relay 只是启动扳机；body 一律忽略，仅排空以维持 keep-alive 解析。 */
    if (req->content_len > 0) {
        char drain[128];
        int remaining = req->content_len;
        while (remaining > 0) {
            const int chunk = remaining > (int)sizeof(drain) ? (int)sizeof(drain) : remaining;
            const int r = httpd_req_recv(req, drain, (size_t)chunk);
            if (r <= 0) break;
            remaining -= r;
        }
    }

    if (motorOtaIsActive()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"motor OTA already in progress\"}");
        return ESP_FAIL;
    }

    /* 触发 SPI START + 启动 feedTask（从 flash 分块读取喂 GD32）*/
    const int result = motorOtaStart(meta.total_size, channel);

    httpd_resp_set_type(req, "application/json");
    if (result == 0) {
        /* 成功触发后清 meta：固件已交给 feedTask，避免残留导致下次误判 */
        (void)Gd32FlashStore::instance().metaClear();
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"OTA started\"}");
    } else if (result == -2) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"error\":\"OTA already in progress\"}");
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"SPI start failed\"}");
    }
    return ESP_OK;
}

esp_err_t motor_ota_status_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    http_set_keepalive_hdr(req);

    const motor_ota_status_t st = motorOtaGetStatus();

    static const char *state_names[] = {
        "idle", "handshake_req", "handshake_nonce", "handshake_confirm",
        "erase", "transfer", "finish_ota", "finished", "error",
        "pre_cancel", "wait_app_run"
    };
    const int si = (st.state <= MOTOR_OTA_WAIT_APP_RUN) ? (int)st.state : 0;

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"error\":%u,\"packets_acked\":%u,\"total_packets\":%u,"
        "\"fw_size\":%lu,\"buf_used\":%u}",
        state_names[si], st.error_code,
        st.packets_acked, st.total_packets,
        (unsigned long)st.fw_size, st.buf_used);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

esp_err_t motor_ota_cancel_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    DEBUG_WARN("MOTOR_OTA", "cancel HTTP request received, calling motorOtaCancel()");
    motorOtaCancel();
    /* 两阶段都可取消：触发前未跑 feedTask 时 motorOtaCancel 是空操作，
     * 但仍清 GD32 侧状态 + 清 flash meta，避免残留固件占分区/被误判为已存。
     * 触发后取消也清 meta 无害（feedTask 已停）。 */
    (void)Gd32FlashStore::instance().metaClear();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"cancelled\"}");
    return ESP_OK;
}
