/*
 * ota_relay.c - OTA 中继核心状态机实现
 * T2.3, T2.4, T3.1 - T3.7
 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "cJSON.h"

#include "ota_relay.h"
#include "spi_master.h"
#include "protocol.h"
#include "health_monitor.h"
#include "debug_log.h"

relay_state_t g_relay_state = RELAY_IDLE;

static const esp_partition_t *g_gd32_partition = NULL;
static session_ctx_t g_session_ctx;

QueueHandle_t g_ota_dispatch_queue = NULL;
QueueHandle_t g_ota_result_queue = NULL;

static ota_result_t send_handshake_req(void);
static ota_result_t send_nonce_and_authenticate(void);
static ota_result_t send_firmware_size(uint32_t size);
static ota_result_t relay_packets_from_flash(void);
static ota_result_t finish_ota(void);

// 从 Flash 加载元数据
esp_err_t meta_load(ota_meta_t *meta)
{
    if (!meta) return ESP_ERR_INVALID_ARG;

    memset(meta, 0, sizeof(ota_meta_t));

    FILE *f = fopen(META_FILE, "r");
    if (!f) {
        DEBUG_WARN("OTA", "元数据文件不存在，返回默认值");
        return ESP_OK;
    }

    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        DEBUG_ERR("OTA", "元数据 JSON 解析失败");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *item = cJSON_GetObjectItem(json, "total_size");
    if (item) meta->total_size = (uint32_t)item->valueint;

    item = cJSON_GetObjectItem(json, "stored_size");
    if (item) meta->stored_size = (uint32_t)item->valueint;

    item = cJSON_GetObjectItem(json, "last_seq");
    if (item) meta->last_seq = (uint32_t)item->valueint;

    item = cJSON_GetObjectItem(json, "chunk_count");
    if (item) meta->chunk_count = (uint32_t)item->valueint;

    cJSON_Delete(json);
    DEBUG_INFO("OTA", "元数据已加载: total=%u, stored=%u, seq=%u",
               meta->total_size, meta->stored_size, meta->last_seq);
    return ESP_OK;
}

// 保存元数据到 Flash
esp_err_t meta_save(const ota_meta_t *meta)
{
    if (!meta) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(META_FILE, "w");
    if (!f) {
        DEBUG_ERR("OTA", "无法打开元数据文件写入");
        return ESP_FAIL;
    }

    fprintf(f,
        "{\n"
        "  \"total_size\": %" PRIu32 ",\n"
        "  \"stored_size\": %" PRIu32 ",\n"
        "  \"last_seq\": %" PRIu32 ",\n"
        "  \"chunk_count\": %" PRIu32 "\n"
        "}",
        meta->total_size, meta->stored_size, meta->last_seq, meta->chunk_count);

    fclose(f);
    DEBUG_INFO("OTA", "元数据已保存到 Flash");
    return ESP_OK;
}

// 传输过程中更新元数据
esp_err_t meta_update(uint32_t stored_size, uint32_t last_seq)
{
    ota_meta_t meta;
    esp_err_t err = meta_load(&meta);
    if (err != ESP_OK) return err;

    meta.stored_size = stored_size;
    meta.last_seq = last_seq;

    return meta_save(&meta);
}

// OTA 完成后或中止时清除元数据
esp_err_t meta_clear(void)
{
    DEBUG_INFO("OTA", "清除 OTA 元数据");
    ota_meta_t meta = {};
    return meta_save(&meta);
}

// Flash 写入接口
esp_err_t flash_write_chunk(uint32_t offset, const uint8_t *data, size_t len)
{
    if (!g_gd32_partition) {
        g_gd32_partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "gd32_ota");
        if (!g_gd32_partition) {
            DEBUG_ERR("OTA", "找不到 gd32_ota 分区");
            return ESP_ERR_NOT_FOUND;
        }
    }

    return esp_partition_write(g_gd32_partition, offset, data, len);
}

esp_err_t flash_read_chunk(uint32_t offset, uint8_t *data, size_t len)
{
    if (!g_gd32_partition) {
        g_gd32_partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "gd32_ota");
        if (!g_gd32_partition) {
            DEBUG_ERR("OTA", "找不到 gd32_ota 分区");
            return ESP_ERR_NOT_FOUND;
        }
    }

    return esp_partition_read(g_gd32_partition, offset, data, len);
}

// 阶段1 - 发起与 GD32 的握手
static ota_result_t send_handshake_req(void)
{
    DEBUG_INFO("OTA", "=== 阶段1: 握手开始 ===");

    uint8_t tx_frame[32];
    uint8_t rx_frame[32];

    uint8_t payload[] = {0x00, 0xAC};
    frame_build(CMD_HANDSHAKE_REQ, payload, sizeof(payload), tx_frame);

    g_relay_state = RELAY_HANDSHAKE_SENT;

    esp_err_t err = spi_sync_transfer(CMD_HANDSHAKE_REQ, payload, sizeof(payload),
                                      rx_frame, sizeof(rx_frame), 3000);
    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "HANDSHAKE_REQ 失败");
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_TIMEOUT;
    }

    DEBUG_INFO("OTA", "握手请求已发送，等待 GD32 响应...");
    vTaskDelay(pdMS_TO_TICKS(100));

    return OTA_RESULT_OK;
}

// 发送随机数并与 GD32 相互认证
static ota_result_t send_nonce_and_authenticate(void)
{
    uint8_t tx_frame[64];
    uint8_t rx_frame[64];
    uint8_t nonce_server[16];

    uint32_t timestamp = xTaskGetTickCount();
    uint32_t mem_addr = (uint32_t)nonce_server;
    uint32_t rand_seed = 0x12345678;

    scramble_generate(timestamp, mem_addr, rand_seed, nonce_server);

    DEBUG_INFO("OTA", "发送 CHALLENGE，包含 nonce_server (16B)");
    g_relay_state = RELAY_CHALLENGE_SENT;

    frame_build(CMD_CHALLENGE, nonce_server, 16, tx_frame);

    esp_err_t err = spi_sync_transfer(CMD_CHALLENGE, nonce_server, 16,
                                      rx_frame, sizeof(rx_frame), 5000);
    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "CHALLENGE 发送失败");
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_TIMEOUT;
    }

    g_relay_state = RELAY_CHALLENGE_RESPOND;
    DEBUG_INFO("OTA", "已收到 GD32 握手数据包");

    // 发送 SESSION_KEY（带变换后的随机数）
    uint8_t transformed_nonce[16];
    memcpy(transformed_nonce, nonce_server, 16);

    DEBUG_INFO("OTA", "发送 SESSION_KEY（带变换后的随机数）");
    g_relay_state = RELAY_SESSION_KEY_SENT;

    frame_build(CMD_SESSION_KEY, transformed_nonce, 16, tx_frame);
    esp_err_t err2 = spi_sync_transfer(CMD_SESSION_KEY, transformed_nonce, 16,
                                       rx_frame, sizeof(rx_frame), 3000);
    if (err2 != ESP_OK) {
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_TIMEOUT;
    }

    g_relay_state = RELAY_AUTH_COMPLETE;
    DEBUG_INFO("OTA", "=== 阶段1 认证完成: 安全通道已建立 ===");

    uint8_t k_session[16] = {0};
    session_key_init(&g_session_ctx, k_session);

    return OTA_RESULT_OK;
}

// 向 GD32 发送固件总大小
static ota_result_t send_firmware_size(uint32_t size)
{
    uint8_t tx_frame[32];
    uint8_t rx_frame[32];

    DEBUG_INFO("OTA", "=== 阶段2: 发送大小 ===");

    uint8_t size_payload[4];
    size_payload[0] = (uint8_t)(size & 0xFF);
    size_payload[1] = (uint8_t)((size >> 8) & 0xFF);
    size_payload[2] = (uint8_t)((size >> 16) & 0xFF);
    size_payload[3] = (uint8_t)((size >> 24) & 0xFF);

    g_relay_state = RELAY_SIZE_SENT;

    frame_build(CMD_SEND_SIZE, size_payload, 4, tx_frame);
    esp_err_t err = spi_sync_transfer(CMD_SEND_SIZE, size_payload, 4,
                                      rx_frame, sizeof(rx_frame), 3000);
    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "SEND_SIZE 失败");
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_TIMEOUT;
    }

    DEBUG_INFO("OTA", "大小已发送: %u 字节，等待 GD32 确认...", size);
    g_relay_state = RELAY_SIZE_ACK;

    return OTA_RESULT_OK;
}

// 从 Flash 向 GD32 转发存储的固件（48B/包）
static ota_result_t relay_packets_from_flash(void)
{
    DEBUG_INFO("OTA", "=== 阶段3: 从 Flash 转发固件 ===");

    ota_meta_t meta;
    if (meta_load(&meta) != ESP_OK) {
        return OTA_RESULT_ERR_STORAGE;
    }

    uint32_t total_packets = (meta.total_size + 47) / 48;
    uint32_t flash_offset = 0;
    uint8_t tx_buf[64];
    uint8_t rx_buf[32];
    ota_result_t result = OTA_RESULT_OK;

    DEBUG_INFO("OTA", "准备转发 %u 包，每包 48 字节", total_packets);

    for (uint32_t seq = 0; seq < total_packets; seq++) {
        // 发送每个包前检查健康状态
        health_status_t health = health_check_before_ota();
        if (health == HEALTH_MON_SHUTDOWN || health == HEALTH_MON_CRITICAL) {
            DEBUG_ERR("OTA", "健康检查危险，中止 OTA");
            return OTA_RESULT_ERR_HEALTH;
        }

        size_t chunk_size = 48;
        if (seq == total_packets - 1) {
            chunk_size = meta.total_size - (seq * 48);
        }

        esp_err_t err = flash_read_chunk(flash_offset, &tx_buf[2], chunk_size);
        if (err != ESP_OK) {
            DEBUG_ERR("OTA", "Flash 读取失败，偏移=%u", flash_offset);
            return OTA_RESULT_ERR_STORAGE;
        }

        tx_buf[0] = (uint8_t)(seq & 0xFF);
        tx_buf[1] = (uint8_t)((seq >> 8) & 0xFF);

        uint16_t crc = crc16_modbus(tx_buf, 2 + chunk_size);
        tx_buf[2 + chunk_size] = (uint8_t)(crc & 0xFF);
        tx_buf[2 + chunk_size + 1] = (uint8_t)((crc >> 8) & 0xFF);

        g_relay_state = RELAY_PACKET_SENT;

        uint8_t cmd = (seq == total_packets - 1) ? CMD_DATA_END : CMD_DATA_PACKET;
        size_t tx_len = 2 + chunk_size + 2;

        err = spi_sync_transfer(cmd, tx_buf, tx_len, rx_buf, sizeof(rx_buf), 3000);
        if (err != ESP_OK) {
            DEBUG_WARN("OTA", "数据包 %u 发送失败，重试中...", seq);
            result = OTA_RESULT_ERR_TIMEOUT;
            break;
        }

        flash_offset += chunk_size;
        g_relay_state = RELAY_PACKET_ACK_OK;

        // 每 10% 记录一次进度
        if (seq % (total_packets / 10 + 1) == 0) {
            DEBUG_INFO("OTA", "转发进度: %u/%u 包 (%u%%)",
                       seq, total_packets, (seq * 100) / total_packets);
        }
    }

    g_relay_state = RELAY_RECEIVE_DONE;
    return result;
}

// 结束 OTA - 发送 0x0A AC 并清理
static ota_result_t finish_ota(void)
{
    DEBUG_INFO("OTA", "=== 阶段4: 结束 OTA ===");

    uint8_t tx_frame[16];
    uint8_t rx_frame[16];

    uint8_t payload[] = {0x0A, 0xAC};
    frame_build(CMD_FINISH_OTA, payload, sizeof(payload), tx_frame);

    g_relay_state = RELAY_FINISH_SENT;

    esp_err_t err = spi_sync_transfer(CMD_FINISH_OTA, payload, sizeof(payload),
                                      rx_frame, sizeof(rx_frame), 5000);
    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "FINISH_OTA 失败");
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_TIMEOUT;
    }

    DEBUG_INFO("OTA", "等待 GD32 跳转确认...");

    session_key_clear(&g_session_ctx);
    memset(&g_session_ctx, 0, sizeof(session_ctx_t));

    g_relay_state = RELAY_DONE;
    DEBUG_INFO("OTA", "=== OTA 完成 ===");

    meta_clear();

    return OTA_RESULT_OK;
}

// 启动完整 OTA 流程的主入口
ota_result_t ota_relay_start_handshake(void)
{
    ota_result_t res;

    res = send_handshake_req();
    if (res != OTA_RESULT_OK) return res;

    res = send_nonce_and_authenticate();
    if (res != OTA_RESULT_OK) return res;

    ota_meta_t meta;
    meta_load(&meta);
    res = send_firmware_size(meta.total_size);
    if (res != OTA_RESULT_OK) return res;

    res = relay_packets_from_flash();
    if (res != OTA_RESULT_OK) return res;

    res = finish_ota();

    return res;
}

// 从 Flash 向 GD32 转发固件
ota_result_t ota_relay_forward_firmware(void)
{
    ota_meta_t meta;
    if (meta_load(&meta) != ESP_OK) {
        return OTA_RESULT_ERR_STORAGE;
    }

    DEBUG_INFO("OTA", "开始固件转发: %u 字节, %u 块",
               meta.total_size, meta.chunk_count);

    ota_result_t res = send_handshake_req();
    if (res != OTA_RESULT_OK) return res;

    res = send_nonce_and_authenticate();
    if (res != OTA_RESULT_OK) return res;

    res = send_firmware_size(meta.total_size);
    if (res != OTA_RESULT_OK) return res;

    res = relay_packets_from_flash();
    if (res != OTA_RESULT_OK) return res;

    res = finish_ota();

    return res;
}

// 获取当前状态
relay_state_t ota_relay_get_state(void)
{
    return g_relay_state;
}

// 重置状态机
void ota_relay_reset(void)
{
    g_relay_state = RELAY_IDLE;
    session_key_clear(&g_session_ctx);
}

// OTA 中继 FreeRTOS 任务
void ota_relay_task(void *pvParameters)
{
    DEBUG_INFO("OTA", "OTA 中继任务启动");

    g_ota_dispatch_queue = xQueueCreate(4, sizeof(uint32_t));
    g_ota_result_queue = xQueueCreate(4, sizeof(uint32_t));

    uint32_t msg;
    BaseType_t ret = xQueueReceive(g_ota_dispatch_queue, &msg, portMAX_DELAY);

    if (ret == pdTRUE) {
        DEBUG_INFO("OTA", "收到 OTA 调度信号，开始转发...");
        ota_result_t result = ota_relay_forward_firmware();

        uint32_t result_msg = (uint32_t)result;
        xQueueSend(g_ota_result_queue, &result_msg, pdMS_TO_TICKS(100));

        if (result == OTA_RESULT_OK) {
            DEBUG_INFO("OTA", "OTA 转发成功完成");
        } else {
            DEBUG_ERR("OTA", "OTA 转发失败，错误码: %d", result);
        }
    }

    DEBUG_INFO("OTA", "OTA 中继任务结束");
    vTaskDelete(NULL);
}