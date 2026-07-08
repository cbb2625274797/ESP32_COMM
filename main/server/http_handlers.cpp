/*
 * http_handlers.cpp - HTTP 请求处理器实现
 * 从 wifi_ota.cpp 提取的 HTTP handler 函数
 */
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <algorithm>
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
#include "monitor/can_feedback_task.h"
#include "ble_ota.h"

// OTA 准备状态：/ota/prepare 完成后设置，/ota/upload 消费后清除
// ★ 同时记录 prepare 时的 epoch，clear 时仅在同一会话才中止，避免误清新会话
static bool s_esp32_prepared = false;
static uint32_t s_esp32_prepare_size = 0;
static uint32_t s_esp32_prepare_epoch = 0;
static bool s_gd32_prepared = false;
static uint32_t s_gd32_prepare_size = 0;
static uint32_t s_gd32_prepare_epoch = 0;

void http_set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
}

static void clear_esp32_prepare()
{
    if (s_esp32_prepared) {
        // ★ 仅在 prepare 仍属当前会话时中止；同类型重启已发生时跳过，避免误清新会话
        OtaCoordinator::instance().abortCurrentIfEpoch(s_esp32_prepare_epoch);
        s_esp32_prepared = false;
        s_esp32_prepare_size = 0;
        s_esp32_prepare_epoch = 0;
    }
}

static void clear_gd32_prepare()
{
    if (s_gd32_prepared) {
        OtaCoordinator::instance().abortCurrentIfEpoch(s_gd32_prepare_epoch);
        s_gd32_prepared = false;
        s_gd32_prepare_size = 0;
        s_gd32_prepare_epoch = 0;
    }
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

        DEBUG_INFO("OTA", "响应已发送，等待 TCP 刷新...");
        vTaskDelay(pdMS_TO_TICKS(500));
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

esp_err_t ota_prepare_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);
    DEBUG_INFO("WIFI", "=== HTTP: POST /ota/prepare ===");

    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON parse failed");
        return ESP_FAIL;
    }

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    cJSON *size_item = cJSON_GetObjectItem(root, "total_size");
    if (!type_item || !cJSON_IsString(type_item) ||
        !size_item || !cJSON_IsNumber(size_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing type or total_size");
        return ESP_FAIL;
    }

    const char *type = cJSON_GetStringValue(type_item);
    uint32_t total_size = (uint32_t)cJSON_GetNumberValue(size_item);
    cJSON_Delete(root);

    if (type && strcmp(type, "esp32") == 0) {
        clear_esp32_prepare();

        esp_err_t err = OtaCoordinator::instance().beginEsp32OtaWithRetry(total_size, OTA_CHANNEL_WIFI);
        if (err != ESP_OK) {
            if (err == ESP_ERR_INVALID_STATE) {
                httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                    "ESP32 OTA occupied by another channel");
            } else {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
            }
            return ESP_FAIL;
        }

        s_esp32_prepared = true;
        s_esp32_prepare_size = total_size;
        s_esp32_prepare_epoch = OtaCoordinator::instance().getOtaEpoch();
        const char resp[] = "{\"status\": \"ok\", \"target\": \"esp32\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));
        DEBUG_INFO("WIFI", "ESP32 OTA 准备完成: %u 字节", total_size);
        return ESP_OK;
    }

    if (type && strcmp(type, "gd32") == 0) {
        clear_gd32_prepare();

        if (OtaRelay::getState() != RELAY_IDLE) {
            DEBUG_WARN("WIFI", "GD32 prepare 前复位中继状态: %d", OtaRelay::getState());
            OtaRelay::instance().reset();
        }

        // ★ 统一调用 prepareGd32Storage 完成：validate + begin + erase + clearMeta + saveMeta
        //   与 BLE 路径 / ota_upload_handler 共享同一序列，消除手写重复
        esp_err_t prep_err = OtaCoordinator::instance().prepareGd32Storage(total_size, OTA_CHANNEL_WIFI);
        if (prep_err == ESP_ERR_INVALID_SIZE) {
            DEBUG_ERR("WIFI", "prepare: firmware size invalid: %u", total_size);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "firmware size invalid");
            return ESP_FAIL;
        } else if (prep_err == ESP_ERR_INVALID_STATE) {
            DEBUG_ERR("WIFI", "GD32 prepare 协调器拒绝（系统忙）");
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                "system busy with another OTA operation");
            return ESP_FAIL;
        } else if (prep_err != ESP_OK) {
            DEBUG_ERR("WIFI", "GD32 prepare 失败: %s", esp_err_to_name(prep_err));
            char err_msg[96];
            snprintf(err_msg, sizeof(err_msg), "prepare gd32_ota failed: %s",
                     esp_err_to_name(prep_err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err_msg);
            return ESP_FAIL;
        }

        WifiOta::instance().stateLock();
        WifiOta::instance().setGd32StorageDone(false);
        WifiOta::instance().stateUnlock();

        s_gd32_prepared = true;
        s_gd32_prepare_size = total_size;
        s_gd32_prepare_epoch = OtaCoordinator::instance().getOtaEpoch();
        const char resp[] = "{\"status\": \"ok\", \"target\": \"gd32\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, strlen(resp));
        DEBUG_INFO("WIFI", "GD32 OTA 准备完成: %u 字节", total_size);
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown type");
    return ESP_FAIL;
}

esp_err_t ota_upload_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);

    // 每请求分配 OTA 接收缓冲区：OTA_BLOCK_SIZE=8KB > 4KB 不适合栈分配，
    // 改为 per-request malloc 避免 ESP32/GD32 两条路径共享全局 buffer，
    // max_open_sockets=4 时多连接并发也互不干扰。
    uint8_t *recv_buf = (uint8_t *)malloc(OTA_BLOCK_SIZE);
    if (!recv_buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc failed");
        return ESP_FAIL;
    }

    DEBUG_WARN("WIFI", "=== HTTP: POST /ota/upload === content_len=%d", req->content_len);
    DEBUG_INFO("WIFI", "收到固件上传请求, 内容长度: %d 字节", req->content_len);

    esp_wifi_set_ps(WIFI_PS_NONE);
    // ★ L1: WiFi OTA 开始时恢复 BLE 共存状态（通过协调器）
    OtaCoordinator::instance().notifyBleCoexEnd();

    char peek;
    int peek_ret = httpd_req_recv(req, &peek, 1);
    if (peek_ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败");
        free(recv_buf);
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
            clear_esp32_prepare();
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing total_size");
            free(recv_buf);
            return ESP_FAIL;
        }

        // ★ 若前端已通过 /ota/prepare 完成擦除，直接复用 prepared 状态，
        //   避免在大量数据传输前再次长时间阻塞 HTTP worker。
        bool used_prepare = false;
        if (s_esp32_prepared) {
            if (total_size == s_esp32_prepare_size) {
                used_prepare = true;
                s_esp32_prepared = false;
                s_esp32_prepare_size = 0;
                DEBUG_INFO("WIFI", "ESP32 OTA 使用已 prepared 状态，跳过擦除");
            } else {
                DEBUG_WARN("WIFI", "ESP32 prepared size mismatch (prepared=%u, upload=%u)，重新初始化",
                           s_esp32_prepare_size, total_size);
                clear_esp32_prepare();
            }
        }

        esp_err_t err;
        if (!used_prepare) {
            err = OtaCoordinator::instance().beginEsp32OtaWithRetry(total_size, OTA_CHANNEL_WIFI);
            if (err != ESP_OK) {
                clear_esp32_prepare();
                if (err == ESP_ERR_INVALID_STATE) {
                    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "ESP32 OTA occupied by another channel");
                } else {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
                }
                free(recv_buf);
                return ESP_FAIL;
            }
        }

        Esp32OtaCore &core = Esp32OtaCore::instance();
        uint8_t *fw_buf = recv_buf;

        // ★ 捕获当前 epoch，用于失败清理时检查会话是否仍属于本 handler
        //   若同类型重启已发生（新请求接管），旧 handler 的 abortCurrentIfEpoch 不会中止新会话
        uint32_t my_epoch = OtaCoordinator::instance().getOtaEpoch();

        size_t total_firmware_written = 0;
        size_t total_received = json_header_len;
        int last_percent = 0;

        while (total_received < req->content_len) {
            size_t to_read = req->content_len - total_received;
            if (to_read > OTA_BLOCK_SIZE) to_read = OTA_BLOCK_SIZE;

            int ret = httpd_req_recv(req, (char *)fw_buf, to_read);
            if (ret <= 0) {
                DEBUG_ERR("WIFI", "HTTP recv failed (ret=%d), aborting OTA", ret);
                // ★ 客户端断开/接收失败：用 abortCurrentIfEpoch 复位协调器状态机
                //   若 epoch 不匹配（新请求已接管），跳过中止，避免误清新会话
                OtaCoordinator::instance().abortCurrentIfEpoch(my_epoch);
                clear_esp32_prepare();
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "HTTP recv failed");
                free(recv_buf);
                return ESP_FAIL;
            }

            err = core.write(fw_buf, (size_t)ret);
            if (err != ESP_OK) {
                DEBUG_ERR("WIFI", "OTA write failed: %s", esp_err_to_name(err));
                // ★ 同上：写入失败也用 abortCurrentIfEpoch，避免误中止新会话
                OtaCoordinator::instance().abortCurrentIfEpoch(my_epoch);
                clear_esp32_prepare();
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
                free(recv_buf);
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
        free(recv_buf);
        return ESP_OK;
    }

    const uint32_t gd32_fw_size = (uint32_t)req->content_len;
    uint32_t gd32_valid_size;
    if (json_header_len > 0 && (uint32_t)json_header_len < gd32_fw_size) {
        gd32_valid_size = (total_size > 0 && total_size < gd32_fw_size) ? total_size : (gd32_fw_size - (uint32_t)json_header_len);
    } else {
        gd32_valid_size = gd32_fw_size;
    }
    if (OtaCoordinator::instance().validateGd32Size(gd32_valid_size) != ESP_OK) {
        DEBUG_ERR("WIFI", "firmware size invalid: valid=%u max=%u",
                  (unsigned)gd32_valid_size, (unsigned)OTA_MAX_FIRMWARE_SIZE);
        clear_gd32_prepare();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "firmware too large or alignment overflow");
        free(recv_buf);
        return ESP_FAIL;
    }

    // ★ 若前端已通过 /ota/prepare 完成擦除，直接复用 prepared 状态
    bool used_gd32_prepare = false;
    if (s_gd32_prepared) {
        if (gd32_valid_size == s_gd32_prepare_size) {
            used_gd32_prepare = true;
            s_gd32_prepared = false;
            s_gd32_prepare_size = 0;
            DEBUG_INFO("WIFI", "GD32 OTA 使用已 prepared 状态，跳过擦除");
        } else {
            DEBUG_WARN("WIFI", "GD32 prepared size mismatch (prepared=%u, upload=%u)，重新初始化",
                       s_gd32_prepare_size, gd32_valid_size);
            clear_gd32_prepare();
        }
    }

    if (!used_gd32_prepare) {
        if (OtaRelay::getState() != RELAY_IDLE) {
            DEBUG_WARN("WIFI", "GD32 OTA 开始前复位中继状态: %d", OtaRelay::getState());
            OtaRelay::instance().reset();
        }

        // ★ 统一调用 prepareGd32Storage 完成：begin + erase + clearMeta + saveMeta
        //   与 BLE 路径（process_gd32_packet seq=0）共用同一序列，消除隐式差异。
        esp_err_t prep_err = OtaCoordinator::instance().prepareGd32Storage(gd32_valid_size, OTA_CHANNEL_WIFI);
        if (prep_err == ESP_ERR_INVALID_STATE) {
            DEBUG_ERR("WIFI", "GD32 OTA 协调器拒绝存储（系统忙）");
            clear_gd32_prepare();
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "system busy with another OTA operation");
            free(recv_buf);
            return ESP_FAIL;
        } else if (prep_err != ESP_OK) {
            DEBUG_ERR("WIFI", "GD32 OTA 准备失败: %s", esp_err_to_name(prep_err));
            clear_gd32_prepare();
            char err_msg[96];
            snprintf(err_msg, sizeof(err_msg), "prepare gd32_ota failed: %s", esp_err_to_name(prep_err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err_msg);
            free(recv_buf);
            return ESP_FAIL;
        }

        WifiOta::instance().stateLock();
        WifiOta::instance().setGd32StorageDone(false);
        WifiOta::instance().stateUnlock();
    }

    // ★ 捕获当前 epoch，用于失败清理时检查会话是否仍属于本 handler
    uint32_t my_epoch = OtaCoordinator::instance().getOtaEpoch();

    uint8_t *buf = recv_buf;

    int received = 0;

    // expected_fw_size_raw 仅用于后续日志输出；meta 已由 prepareGd32Storage
    // （非 prepared 路径）或 /ota/prepare（prepared 路径）写入，此处不再重复保存。
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
                // ★ 首块写入失败：用 abortCurrentIfEpoch 避免误中止新会话
                OtaCoordinator::instance().abortCurrentIfEpoch(my_epoch);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write gd32_ota failed");
                free(recv_buf);
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
            // ★ 客户端断开（刷新网页/关浏览器/网络掉）：用 abortCurrentIfEpoch 避免误中止新会话
            OtaCoordinator::instance().abortCurrentIfEpoch(my_epoch);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload interrupted");
            free(recv_buf);
            return ESP_FAIL;
        }

        esp_err_t err = OtaCoordinator::instance().writeGd32Chunk(gd32_offset, buf, (size_t)ret);
        if (err != ESP_OK) {
            DEBUG_ERR("WIFI", "GD32 write failed: offset=%u len=%d err=%s",
                      (unsigned)gd32_offset, ret, esp_err_to_name(err));
            // ★ 写入失败：同上，用 abortCurrentIfEpoch
            OtaCoordinator::instance().abortCurrentIfEpoch(my_epoch);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write gd32_ota failed");
            free(recv_buf);
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
    free(recv_buf);
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


// ============================================================
// GET /can/feedback — 三路 CAN 1Hz 反馈快照
// ============================================================
esp_err_t can_feedback_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);

    can_feedback_result_t results[CAN_CHANNEL_MAX] = {};
    bool valid[CAN_CHANNEL_MAX] = {};
    for (uint8_t i = 0; i < CAN_CHANNEL_MAX; ++i) {
        valid[i] = getLatestCanFeedback(i, &results[i]);
    }

    /* 估算缓冲大小：每通道 ~2KB (统计+ID条目) */
    char *resp = (char *)calloc(1, 8192);
    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    char *p = resp;
    p += sprintf(p, "{\"channels\":[");
    for (uint8_t i = 0; i < CAN_CHANNEL_MAX; ++i) {
        const can_feedback_result_t &r = results[i];
        if (i > 0) p += sprintf(p, ",");
        p += sprintf(p, "{\"ch\":%u,\"valid\":%s", (unsigned)i,
                     valid[i] ? "true" : "false");
        if (valid[i]) {
            p += sprintf(p,
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
            for (uint8_t j = 0; j < r.total_count && j < CANFD_ID_SNAPSHOT_MAX; ++j) {
                const id_snapshot_entry_t &e = r.entries[j];
                if (j > 0) p += sprintf(p, ",");
                p += sprintf(p,
                    "{\"id\":\"0x%08lX\",\"dlc\":%u,\"fd\":%u,"
                    "\"brs\":%u,\"esi\":%u,\"data\":\"",
                    (unsigned long)e.id,
                    (unsigned)e.dlc,
                    (unsigned)e.fd_flag,
                    (unsigned)e.brs,
                    (unsigned)e.esi);
                const uint8_t dataLen = (e.dlc < CANFD_ID_SNAPSHOT_DATA_MAX)
                                        ? e.dlc : CANFD_ID_SNAPSHOT_DATA_MAX;
                for (uint8_t d = 0; d < dataLen; ++d) {
                    p += sprintf(p, "%02X", e.data[d]);
                }
                p += sprintf(p, "\"}");
            }
            p += sprintf(p, "]");
        }
        p += sprintf(p, "}");
    }
    p += sprintf(p, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, (int)(p - resp));
    free(resp);
    return ESP_OK;
}

// ============================================================
// GET/POST /can/mode — 查询或切换三路 CAN 接收模式
// ============================================================
esp_err_t can_mode_handler(httpd_req_t *req)
{
    http_set_cors_headers(req);

    if (req->method == HTTP_GET) {
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
                                   resp_data, sizeof(resp_data), 2000);

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
static constexpr size_t SYSTEM_STATUS_JSON_CAP = 8192;
static constexpr size_t CPU_TASK_SNAPSHOT_MAX = 24;

struct TaskStackMetric {
    const char* name;
    TaskHandle_t handle;
    size_t stack_size_bytes;
};

struct CpuTaskSample {
    UBaseType_t task_number;
    configRUN_TIME_COUNTER_TYPE runtime;
};

struct CpuTaskMetric {
    const char* name;
    UBaseType_t task_number;
    uint64_t runtime_delta;
    double usage_pct;
};

struct CpuUsageSnapshot {
    bool available;
    bool valid;
    uint32_t sample_ms;
    double usage_pct;
    double idle_pct;
    CpuTaskMetric tasks[CPU_TASK_SNAPSHOT_MAX];
    size_t task_count;
};

static portMUX_TYPE s_cpu_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static CpuTaskSample s_cpu_last_samples[CPU_TASK_SNAPSHOT_MAX];
static size_t s_cpu_last_count = 0;
static configRUN_TIME_COUNTER_TYPE s_cpu_last_total = 0;
static int64_t s_cpu_last_time_us = 0;
static bool s_cpu_has_last = false;

static uint64_t runtime_delta(configRUN_TIME_COUNTER_TYPE current, configRUN_TIME_COUNTER_TYPE previous)
{
    return current >= previous ? (uint64_t)(current - previous) : 0U;
}

static bool task_name_is_idle(const char* name)
{
    return name && strncmp(name, "IDLE", 4) == 0;
}

static void reset_cpu_usage_baseline_locked(const TaskStatus_t* tasks, size_t task_count,
                                            configRUN_TIME_COUNTER_TYPE total_runtime,
                                            int64_t now_us)
{
    size_t copy_count = std::min(task_count, CPU_TASK_SNAPSHOT_MAX);
    for (size_t i = 0; i < copy_count; ++i) {
        s_cpu_last_samples[i].task_number = tasks[i].xTaskNumber;
        s_cpu_last_samples[i].runtime = tasks[i].ulRunTimeCounter;
    }
    s_cpu_last_count = copy_count;
    s_cpu_last_total = total_runtime;
    s_cpu_last_time_us = now_us;
    s_cpu_has_last = true;
}

static CpuUsageSnapshot sample_cpu_usage()
{
    CpuUsageSnapshot result = {};
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
    result.available = true;

    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count == 0) {
        return result;
    }
    task_count = std::min<UBaseType_t>(task_count + 2U, CPU_TASK_SNAPSHOT_MAX);

    TaskStatus_t* tasks = (TaskStatus_t*)calloc(task_count, sizeof(TaskStatus_t));
    if (!tasks) {
        return result;
    }

    configRUN_TIME_COUNTER_TYPE total_runtime = 0;
    UBaseType_t captured_count = uxTaskGetSystemState(tasks, task_count, &total_runtime);
    int64_t now_us = esp_timer_get_time();
    if (captured_count == 0 || captured_count > task_count) {
        free(tasks);
        return result;
    }

    CpuTaskSample previous_samples[CPU_TASK_SNAPSHOT_MAX] = {};
    size_t previous_count = 0;
    configRUN_TIME_COUNTER_TYPE previous_total = 0;
    int64_t previous_time_us = 0;
    bool had_previous = false;

    portENTER_CRITICAL(&s_cpu_stats_mux);
    previous_count = s_cpu_last_count;
    previous_total = s_cpu_last_total;
    previous_time_us = s_cpu_last_time_us;
    had_previous = s_cpu_has_last;
    for (size_t i = 0; i < previous_count; ++i) {
        previous_samples[i] = s_cpu_last_samples[i];
    }
    reset_cpu_usage_baseline_locked(tasks, captured_count, total_runtime, now_us);
    portEXIT_CRITICAL(&s_cpu_stats_mux);

    bool can_compute = had_previous && total_runtime >= previous_total && now_us > previous_time_us;
    uint64_t total_delta = can_compute ? runtime_delta(total_runtime, previous_total) : 0U;
    uint64_t idle_delta = 0U;
    if (can_compute && total_delta > 0U) {
        for (UBaseType_t i = 0; i < captured_count; ++i) {
            configRUN_TIME_COUNTER_TYPE last_runtime = 0;
            bool found_previous = false;
            for (size_t j = 0; j < previous_count; ++j) {
                if (previous_samples[j].task_number == tasks[i].xTaskNumber) {
                    last_runtime = previous_samples[j].runtime;
                    found_previous = true;
                    break;
                }
            }
            if (!found_previous) {
                continue;
            }
            uint64_t delta = runtime_delta(tasks[i].ulRunTimeCounter, last_runtime);
            double pct = (double)delta * 100.0 / (double)total_delta;
            if (task_name_is_idle(tasks[i].pcTaskName)) {
                idle_delta += delta;
            }
            if (result.task_count < CPU_TASK_SNAPSHOT_MAX) {
                CpuTaskMetric& metric = result.tasks[result.task_count++];
                metric.name = tasks[i].pcTaskName ? tasks[i].pcTaskName : "";
                metric.task_number = tasks[i].xTaskNumber;
                metric.runtime_delta = delta;
                metric.usage_pct = pct;
            }
        }

        result.valid = true;
        result.sample_ms = (uint32_t)((now_us - previous_time_us) / 1000);
        result.idle_pct = std::min(100.0, (double)idle_delta * 100.0 / (double)total_delta);
        result.usage_pct = std::max(0.0, 100.0 - result.idle_pct);
    }

    free(tasks);
#else
    result.available = false;
#endif
    return result;
}

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

static int append_cpu_status_json(char* buf, size_t cap, int offset, const CpuUsageSnapshot& cpu)
{
    offset = append_jsonf(buf, cap, offset,
        "\"cpu\":{\"available\":%s,\"valid\":%s,\"usage_pct\":%.2f,\"idle_pct\":%.2f,\"sample_ms\":%lu,\"tasks\":[",
        cpu.available ? "true" : "false",
        cpu.valid ? "true" : "false",
        cpu.valid ? cpu.usage_pct : 0.0,
        cpu.valid ? cpu.idle_pct : 0.0,
        (unsigned long)(cpu.valid ? cpu.sample_ms : 0U));

    for (size_t i = 0; i < cpu.task_count; ++i) {
        const CpuTaskMetric& task = cpu.tasks[i];
        offset = append_jsonf(buf, cap, offset,
            "%s{\"name\":\"%s\",\"task_number\":%lu,\"runtime_delta\":%" PRIu64 ",\"usage_pct\":%.2f}",
            i == 0 ? "" : ",",
            task.name,
            (unsigned long)task.task_number,
            task.runtime_delta,
            task.usage_pct);
    }

    return append_jsonf(buf, cap, offset, "]},");
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
    CpuUsageSnapshot cpu = sample_cpu_usage();

    char* resp = (char*)calloc(1, SYSTEM_STATUS_JSON_CAP);
    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    int len = snprintf(resp, SYSTEM_STATUS_JSON_CAP,
        "{\"version\": \"%s\", \"build_time\": \"%s\", \"git_hash\": \"%s\", \"health\": %d, "
        "\"free_heap\": %lu, \"min_free_heap\": %lu, \"largest_free_block\": %lu, "
        "\"internal_free_heap\": %lu, \"internal_min_free_heap\": %lu, \"internal_largest_free_block\": %lu, "
        "\"internal_heap_total\": %lu, \"default_heap_total\": %lu, "
        "\"temperature\": %.1f, \"uptime_sec\": %lu, "
        "\"wifi\": {\"mode\": \"%s\", \"ip\": \"%s\", \"rssi\": %d, \"connected\": %s}, "
        "\"ota\": {\"sys_state\": \"%s\", \"sys_state_code\": %d, \"channel\": %d, \"channel_name\": \"%s\", \"total_size\": %lu, \"stored_size\": %lu, \"storage_done\": %s, "
        "\"relay_state\": %d, \"relay_seq\": %lu, \"relay_total_packets\": %lu, \"gd32_app_alive\": %s, "
        "\"gd32_app\": \"v%d.%d.%d.%d\", \"gd32_boot\": \"v%d.%d.%d.%d\"}, ",
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
        (int)snap.channel,
        snap.channel == OTA_CHANNEL_BLE ? "BLE" : "WIFI",
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

    len = append_cpu_status_json(resp, SYSTEM_STATUS_JSON_CAP, len, cpu);
    len = append_jsonf(resp, SYSTEM_STATUS_JSON_CAP, len, "\"task_stack\": {");
    for (size_t i = 0; i < sizeof(task_metrics) / sizeof(task_metrics[0]); ++i) {
        len = append_jsonf(resp, SYSTEM_STATUS_JSON_CAP, len,
            "%s\"%s\":{\"stack_size_bytes\":%lu,\"high_watermark_bytes\":%lu,\"peak_used_bytes\":%lu}",
            i == 0 ? "" : ",",
            task_metrics[i].name,
            (unsigned long)task_metrics[i].stack_size_bytes,
            (unsigned long)get_task_stack_high_watermark_bytes(task_metrics[i].handle),
            (unsigned long)get_task_stack_peak_used_bytes(task_metrics[i].handle, task_metrics[i].stack_size_bytes));
    }
    len = append_jsonf(resp, SYSTEM_STATUS_JSON_CAP, len, "}}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    free(resp);
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
