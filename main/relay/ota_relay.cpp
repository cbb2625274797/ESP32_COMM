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
#include "aes/esp_aes.h"

#include "ota_relay.h"
#include "spi_master.h"
#include "protocol.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"

/** @brief 当前 OTA 中继状态机状态。 */
relay_state_t g_relay_state = RELAY_IDLE;

static const esp_partition_t *g_gd32_partition = NULL;
/** @brief OTA 认证会话上下文，包含会话密钥和 IV。 */
static session_ctx_t g_session_ctx;
static uint8_t g_nonce_server[16] = {0};
static uint8_t g_k_session[16] = {0};

/** @brief OTA 命令分发队列，从 WiFi/BLE 任务接收指令。 */
QueueHandle_t g_ota_dispatch_queue = NULL;
/** @brief OTA 结果回传队列，将执行结果返回给调用方。 */
QueueHandle_t g_ota_result_queue = NULL;

/**
 * @brief OTA 握手阶段：向 GD32 发送握手请求并等待进入认证流程。
 * @return 操作结果码。
 */
static ota_result_t send_handshake_req(void);

/**
 * @brief 认证阶段：生成随机数并完成与 GD32 的双向认证。
 * @return 操作结果码。
 */
static ota_result_t send_nonce_and_authenticate(void);

/**
 * @brief 向 GD32 发送待升级固件的总大小。
 * @param size 固件总字节数。
 * @return 操作结果码。
 */
static ota_result_t send_firmware_size(uint32_t size);

/**
 * @brief 从 Flash 逐包读取固件并中继转发给 GD32。
 * @return 操作结果码。
 */
static ota_result_t relay_packets_from_flash(void);

/**
 * @brief OTA 收尾阶段：发送结束命令并等待 GD32 确认。
 * @return 操作结果码。
 */
static ota_result_t finish_ota(void);

static int aes128_ecb_encrypt_block_local(const uint8_t *key, const uint8_t *in, uint8_t *out)
{
    esp_aes_context aes;
    esp_aes_init(&aes);
    int ret = esp_aes_setkey(&aes, key, 128);
    if (ret == 0) {
        ret = esp_aes_crypt_ecb(&aes, ESP_AES_ENCRYPT, in, out);
    }
    esp_aes_free(&aes);
    return ret;
}

static int aes128_ecb_decrypt_block_local(const uint8_t *key, const uint8_t *in, uint8_t *out)
{
    esp_aes_context aes;
    esp_aes_init(&aes);
    int ret = esp_aes_setkey(&aes, key, 128);
    if (ret == 0) {
        ret = esp_aes_crypt_ecb(&aes, ESP_AES_DECRYPT, in, out);
    }
    esp_aes_free(&aes);
    return ret;
}

static bool aes128_ecb_encrypt_48bytes_local(const uint8_t *key, const uint8_t *in, uint8_t *out)
{
    for (int i = 0; i < 3; ++i) {
        if (aes128_ecb_encrypt_block_local(key, in + i * 16, out + i * 16) != 0) {
            return false;
        }
    }
    return true;
}

static void calculate_crc32_with_key_local(const uint8_t *key, const uint8_t *data48, uint8_t *output)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (int i = 0; i < 16; ++i) {
        crc ^= key[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    for (size_t i = 0; i < GD32_OTA_PACKET_SIZE; ++i) {
        crc ^= data48[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    crc ^= 0xFFFFFFFFU;
    output[0] = (uint8_t)(crc & 0xFFU);
    output[1] = (uint8_t)((crc >> 8) & 0xFFU);
    output[2] = (uint8_t)((crc >> 16) & 0xFFU);
    output[3] = (uint8_t)((crc >> 24) & 0xFFU);
}

static void log_resp_payload(const char *stage, uint8_t resp_type, const uint8_t *payload, size_t len)
{
    char hex[193] = {0};
    size_t n = (len > 32U) ? 32U : len;
    for (size_t i = 0; i < n; ++i) {
        snprintf(&hex[i * 2], sizeof(hex) - i * 2, "%02X", payload[i]);
    }
    DEBUG_INFO("OTA", "%s 响应: type=0x%02X len=%u payload[%u]=%s%s",
               stage, resp_type, (unsigned)len, (unsigned)n,
               n > 0 ? hex : "<empty>",
               len > n ? "..." : "");
}

static esp_err_t wait_until_not_wait(const char *stage, uint8_t *resp_type,
                                     uint8_t *resp_payload, size_t resp_max,
                                     size_t *resp_len, uint32_t timeout_ms, uint32_t max_wait_rounds)
{
    if (!resp_type) return ESP_ERR_INVALID_ARG;
    uint32_t round = 0;
    while (*resp_type == SPI_OTA_RSP_WAIT && round < max_wait_rounds) {
        ++round;
        DEBUG_WARN("OTA", "%s 收到 WAIT，开始第 %u 次只读轮询...", stage, round);
        esp_err_t err = spi_ota_poll_response(resp_type, resp_payload, resp_max, resp_len, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        log_resp_payload(stage, *resp_type, resp_payload, resp_len ? *resp_len : 0);
    }
    return ESP_OK;
}

/**
 * @brief 从 Flash 加载元数据
 * @param meta 输出参数，加载后的元数据结构体。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t meta_load(ota_meta_t *meta)
{
    if (!meta) return ESP_ERR_INVALID_ARG;

    memset(meta, 0, sizeof(ota_meta_t));

    FILE *f = fopen(META_FILE, "r");
    if (!f) {
        // 文件不存在是正常状态（首次使用，未刷过 GD32）
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

/**
 * @brief 保存元数据到 Flash
 * @param meta 待保存的元数据结构体。
 * @return ESP_OK 成功，其他失败。
 */
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

/**
 * @brief 传输过程中更新元数据
 * @param stored_size 已存储的固件大小（字节）。
 * @param last_seq 最后成功的分块序号。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t meta_update(uint32_t stored_size, uint32_t last_seq)
{
    ota_meta_t meta;
    esp_err_t err = meta_load(&meta);
    if (err != ESP_OK) return err;

    meta.stored_size = stored_size;
    meta.last_seq = last_seq;

    return meta_save(&meta);
}

/**
 * @brief OTA 完成后或中止时清除元数据
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t meta_clear(void)
{
    DEBUG_INFO("OTA", "清除 OTA 元数据");
    ota_meta_t meta = {};
    return meta_save(&meta);
}

/**
 * @brief Flash 写入接口
 * @param offset 分区内写入偏移。
 * @param data 待写入的数据指针。
 * @param len 数据长度（字节）。
 * @return ESP_OK 成功，其他失败。
 */
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

/**
 * @brief 从 OTA 缓存分区读取指定偏移和长度的数据块。
 * @param offset 分区内读取偏移。
 * @param data 读取缓冲区指针。
 * @param len 读取长度（字节）。
 * @return ESP_OK 成功，其他失败。
 */
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

/**
 * @brief 阶段1 - 发起与 GD32 的握手
 * @return 操作结果码。
 */
static ota_result_t send_handshake_req(void)
{
    DEBUG_INFO("OTA", "=== 阶段1: 握手开始 ===");
    DEBUG_INFO("OTA", "发送 HANDSHAKE_REQ (0x00 0xAC)");

    uint8_t payload[] = {0x00, 0xAC};
    uint8_t resp_type = 0;
    uint8_t resp_payload[64] = {0};
    size_t resp_len = 0;

    /** @brief 当前 OTA 中继状态机状态。 */
    g_relay_state = RELAY_HANDSHAKE_SENT;
    DEBUG_INFO("OTA", "状态: RELAY_HANDSHAKE_SENT");

    esp_err_t err = spi_ota_transfer(SPI_OTA_CMD_REQ, payload, sizeof(payload),
                                     &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
    log_resp_payload("HANDSHAKE_REQ", resp_type, resp_payload, resp_len);
    if (err == ESP_OK) {
        err = wait_until_not_wait("HANDSHAKE_REQ", &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 1000, 8);
    }
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || resp_len < 2 ||
        resp_payload[0] != 0x00 || resp_payload[1] != 0xAC) {
        DEBUG_ERR("OTA", "HANDSHAKE_REQ 失败, err=%d, resp_type=0x%02X, resp_len=%zu", err, resp_type, resp_len);
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_TIMEOUT;
    }

    DEBUG_INFO("OTA", "握手请求已确认");
    return OTA_RESULT_OK;
}

/**
 * @brief 发送随机数并与 GD32 相互认证
 * @return 操作结果码。
 */
static ota_result_t send_nonce_and_authenticate(void)
{
    uint8_t resp_type = 0;
    uint8_t resp_payload[64] = {0};
    size_t resp_len = 0;

    uint32_t timestamp = xTaskGetTickCount();
    uint32_t mem_addr = (uint32_t)(uintptr_t)g_nonce_server;
    uint32_t rand_seed = 0x12345678;

    scramble_generate(timestamp, mem_addr, rand_seed, g_nonce_server);

    DEBUG_INFO("OTA", "--- 认证步骤 1: 发送 CHALLENGE ---");
    DEBUG_INFO("OTA", "发送 nonce_server (16B), timestamp=0x%08X", timestamp);
    g_relay_state = RELAY_CHALLENGE_SENT;
    DEBUG_INFO("OTA", "状态: RELAY_CHALLENGE_SENT");

    esp_err_t err = spi_ota_transfer(SPI_OTA_CMD_REQ, g_nonce_server, 16,
                                     &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 5000);
    log_resp_payload("CHALLENGE", resp_type, resp_payload, resp_len);
    if (err == ESP_OK) {
        err = wait_until_not_wait("CHALLENGE", &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 1000, 12);
    }
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || resp_len != 36) {
        DEBUG_ERR("OTA", "CHALLENGE 发送失败, err=%d, resp_type=0x%02X, resp_len=%zu", err, resp_type, resp_len);
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_TIMEOUT;
    }

    g_relay_state = RELAY_CHALLENGE_RESPOND;
    DEBUG_INFO("OTA", "已收到 GD32 握手数据包");
    DEBUG_INFO("OTA", "状态: RELAY_CHALLENGE_RESPOND");

    if (aes128_ecb_encrypt_block_local(MASTER_KEY, g_nonce_server, g_k_session) != 0) {
        DEBUG_ERR("OTA", "计算 K_session 失败");
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_AUTH;
    }

    uint8_t plaintext[16];
    for (int i = 0; i < 16; ++i) {
        plaintext[i] = g_nonce_server[i] ^ 0x55U;
    }
    uint8_t encrypted_confirm[16] = {0};
    if (aes128_ecb_encrypt_block_local(g_k_session, plaintext, encrypted_confirm) != 0) {
        DEBUG_ERR("OTA", "加密确认块失败");
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_AUTH;
    }

    DEBUG_INFO("OTA", "--- 认证步骤 2: 发送 SESSION_KEY/CONFIRM ---");
    g_relay_state = RELAY_SESSION_KEY_SENT;
    DEBUG_INFO("OTA", "状态: RELAY_SESSION_KEY_SENT");

    resp_len = 0;
    err = spi_ota_transfer(SPI_OTA_CMD_REQ, encrypted_confirm, 16,
                           &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
    log_resp_payload("CONFIRM", resp_type, resp_payload, resp_len);
    if (err == ESP_OK) {
        err = wait_until_not_wait("CONFIRM", &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 1000, 8);
    }
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || resp_len < 2 ||
        resp_payload[0] != 0xBA || resp_payload[1] != 0xC0) {
        DEBUG_ERR("OTA", "SESSION_KEY/CONFIRM 失败, err=%d, resp_type=0x%02X, resp_len=%zu", err, resp_type, resp_len);
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_TIMEOUT;
    }

    g_relay_state = RELAY_AUTH_COMPLETE;
    DEBUG_INFO("OTA", "状态: RELAY_AUTH_COMPLETE");
    DEBUG_INFO("OTA", "=== 阶段1 认证完成: 安全通道已建立 ===");

    session_key_init(&g_session_ctx, g_k_session);
    return OTA_RESULT_OK;
}

/**
 * @brief 向 GD32 发送固件总大小
 * @param size 固件总字节数。
 * @return 操作结果码。
 */
static ota_result_t send_firmware_size(uint32_t size)
{
    uint8_t resp_type = 0;
    uint8_t resp_payload[32] = {0};
    size_t resp_len = 0;

    DEBUG_INFO("OTA", "=== 阶段2: 发送固件大小 ===");
    DEBUG_INFO("OTA", "固件大小: %u 字节", size);

    uint8_t size_payload[4];
    size_payload[0] = (uint8_t)(size & 0xFF);
    size_payload[1] = (uint8_t)((size >> 8) & 0xFF);
    size_payload[2] = (uint8_t)((size >> 16) & 0xFF);
    size_payload[3] = (uint8_t)((size >> 24) & 0xFF);

    g_relay_state = RELAY_SIZE_SENT;
    DEBUG_INFO("OTA", "状态: RELAY_SIZE_SENT");

    esp_err_t err = spi_ota_transfer(SPI_OTA_CMD_DATA, size_payload, sizeof(size_payload),
                                     &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
    log_resp_payload("SEND_SIZE", resp_type, resp_payload, resp_len);
    if (err == ESP_OK) {
        err = wait_until_not_wait("SEND_SIZE", &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 1500, 12);
    }
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || resp_len < 2 ||
        resp_payload[0] != 0xBA || resp_payload[1] != 0xC0) {
        DEBUG_ERR("OTA", "SEND_SIZE 失败, err=%d, resp_type=0x%02X, resp_len=%zu", err, resp_type, resp_len);
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_TIMEOUT;
    }

    DEBUG_INFO("OTA", "大小已发送: %u 字节，等待 GD32 确认...", size);
    g_relay_state = RELAY_SIZE_ACK;
    DEBUG_INFO("OTA", "状态: RELAY_SIZE_ACK");

    return OTA_RESULT_OK;
}

/**
 * @brief 从 Flash 向 GD32 转发存储的固件（48B/包）
 * @return 操作结果码。
 */
static ota_result_t relay_packets_from_flash(void)
{
    DEBUG_INFO("OTA", "=== 阶段3: 从 Flash 转发固件 ===");

    ota_meta_t meta;
    if (meta_load(&meta) != ESP_OK) {
        DEBUG_ERR("OTA", "元数据加载失败");
        return OTA_RESULT_ERR_STORAGE;
    }

    uint32_t total_packets = (meta.total_size + GD32_OTA_PACKET_SIZE - 1U) / GD32_OTA_PACKET_SIZE;
    uint32_t flash_offset = 0;
    uint8_t tx_buf[64] = {0};
    uint8_t resp_type = 0;
    uint8_t resp_payload[32] = {0};
    size_t resp_len = 0;
    ota_result_t result = OTA_RESULT_OK;

    DEBUG_INFO("OTA", "总包数: %u, 每包 %u 字节", total_packets, (unsigned)GD32_OTA_PACKET_SIZE);
    DEBUG_INFO("OTA", "状态: RELAY_RELAY_ACTIVE");
    g_relay_state = RELAY_RELAY_ACTIVE;

    for (uint32_t seq = 0; seq < total_packets; seq++) {
        if ((seq % OTA_PACKET_HEALTH_CHECK_INTERVAL) == 0 || seq == 0 || seq == total_packets - 1) {
            health_status_t health = health_check_before_ota();
            if (health == HEALTH_MON_SHUTDOWN || health == HEALTH_MON_CRITICAL) {
                DEBUG_ERR("OTA", "健康检查危险，中止 OTA");
                return OTA_RESULT_ERR_HEALTH;
            }
        }

        uint8_t plain48[GD32_OTA_PACKET_SIZE];
        memset(plain48, 0xFF, sizeof(plain48));
        size_t chunk_size = GD32_OTA_PACKET_SIZE;
        if (seq == total_packets - 1) {
            chunk_size = meta.total_size - (seq * GD32_OTA_PACKET_SIZE);
        }

        esp_err_t err = flash_read_chunk(flash_offset, plain48, chunk_size);
        if (err != ESP_OK) {
            DEBUG_ERR("OTA", "Flash 读取失败, offset=%u, err=%d", flash_offset, err);
            return OTA_RESULT_ERR_STORAGE;
        }

        tx_buf[0] = 0x00; tx_buf[1] = 0x00; tx_buf[2] = 0x00; tx_buf[3] = 0x00; /* session_num 预留 */
        tx_buf[4] = (uint8_t)(seq & 0xFFU);
        tx_buf[5] = (uint8_t)((seq >> 8) & 0xFFU);
        tx_buf[6] = 0x00;
        tx_buf[7] = 0x00;

        if (!aes128_ecb_encrypt_48bytes_local(g_k_session, plain48, &tx_buf[8])) {
            DEBUG_ERR("OTA", "包 %u 加密失败", seq);
            return OTA_RESULT_ERR_AUTH;
        }

        uint32_t if_end = (seq == total_packets - 1) ? 0xFFFFFFFFU : 0x00000000U;
        tx_buf[56] = (uint8_t)(if_end & 0xFFU);
        tx_buf[57] = (uint8_t)((if_end >> 8) & 0xFFU);
        tx_buf[58] = (uint8_t)((if_end >> 16) & 0xFFU);
        tx_buf[59] = (uint8_t)((if_end >> 24) & 0xFFU);
        calculate_crc32_with_key_local(g_k_session, plain48, &tx_buf[60]);

        DEBUG_INFO("OTA", "包 %u: chunk=%u if_end=0x%08X crc32=%02X%02X%02X%02X",
                   seq, (unsigned)chunk_size, (unsigned)if_end,
                   tx_buf[60], tx_buf[61], tx_buf[62], tx_buf[63]);

        g_relay_state = RELAY_PACKET_SENT;

        resp_len = 0;
        err = spi_ota_transfer(SPI_OTA_CMD_DATA, tx_buf, sizeof(tx_buf),
                               &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
        log_resp_payload("DATA_PACKET", resp_type, resp_payload, resp_len);
        if (err == ESP_OK) {
            err = wait_until_not_wait("DATA_PACKET", &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 1000, 10);
        }
        if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || resp_len < 4) {
            DEBUG_WARN("OTA", "数据包 %u 发送失败, err=%d, resp_type=0x%02X, resp_len=%zu", seq, err, resp_type, resp_len);
            result = OTA_RESULT_ERR_TIMEOUT;
            break;
        }

        uint32_t ack_value = (uint32_t)resp_payload[0] |
                             ((uint32_t)resp_payload[1] << 8) |
                             ((uint32_t)resp_payload[2] << 16) |
                             ((uint32_t)resp_payload[3] << 24);
        uint32_t expected_ack = (seq == total_packets - 1) ? 0xFFFFFFFFU : (seq + 1U);
        if (ack_value != expected_ack) {
            DEBUG_ERR("OTA", "包 %u ACK 不匹配: got=0x%08X expected=0x%08X", seq, ack_value, expected_ack);
            result = OTA_RESULT_ERR_GD32;
            break;
        }

        flash_offset += (uint32_t)chunk_size;
        g_relay_state = RELAY_PACKET_ACK_OK;

        if (seq % (total_packets / 10 + 1) == 0) {
            uint32_t progress = (seq * 100U) / total_packets;
            DEBUG_INFO("OTA", "转发进度: %u/%u 包 (%u%%)", seq, total_packets, progress);
        }
    }

    g_relay_state = RELAY_RECEIVE_DONE;
    DEBUG_INFO("OTA", "状态: RELAY_RECEIVE_DONE");
    return result;
}

/**
 * @brief 结束 OTA - 发送 0x0A AC 并清理
 * @return 操作结果码。
 */
static ota_result_t finish_ota(void)
{
    DEBUG_INFO("OTA", "=== 阶段4: 结束 OTA ===");
    DEBUG_INFO("OTA", "GD32 当前实现已在最后一个 DATA 包后自行 ACK 并跳转，无需额外 FINISH 命令");

    session_key_clear(&g_session_ctx);
    memset(&g_session_ctx, 0, sizeof(session_ctx_t));
    memset(g_k_session, 0, sizeof(g_k_session));
    memset(g_nonce_server, 0, sizeof(g_nonce_server));

    g_relay_state = RELAY_DONE;
    DEBUG_INFO("OTA", "状态: RELAY_DONE");
    DEBUG_INFO("OTA", "=== OTA 完成 ===");

    meta_clear();
    return OTA_RESULT_OK;
}

/**
 * @brief 启动完整 OTA 流程的主入口
 * @return 操作结果码。
 */
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

/**
 * @brief 从 Flash 向 GD32 转发固件
 * @return 操作结果码。
 */
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

/**
 * @brief 获取当前状态
 * @return 当前中继状态枚举值。
 */
relay_state_t ota_relay_get_state(void)
{
    /** @brief 当前 OTA 中继状态机状态。 */
    return g_relay_state;
}

/**
 * @brief 重置状态机
 */
void ota_relay_reset(void)
{
    /** @brief 当前 OTA 中继状态机状态。 */
    g_relay_state = RELAY_IDLE;
    session_key_clear(&g_session_ctx);
}

/**
 * @brief OTA 中继 FreeRTOS 任务
 * @param pvParameters FreeRTOS 任务参数，未使用。
 */
void ota_relay_task(void *pvParameters)
{
    DEBUG_INFO("OTA", "OTA 中继任务启动");

    /** @brief OTA 命令分发队列，从 WiFi/BLE 任务接收指令。 */
    g_ota_dispatch_queue = xQueueCreate(4, sizeof(uint32_t));
    /** @brief OTA 结果回传队列，将执行结果返回给调用方。 */
    g_ota_result_queue = xQueueCreate(4, sizeof(uint32_t));

    if (!g_ota_dispatch_queue || !g_ota_result_queue) {
        DEBUG_ERR("OTA", "OTA 队列创建失败");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        uint32_t msg = 0;
        BaseType_t ret = xQueueReceive(g_ota_dispatch_queue, &msg, portMAX_DELAY);
        if (ret != pdTRUE) {
            continue;
        }

        DEBUG_INFO("OTA", "收到 OTA 调度信号: %lu，开始转发...", (unsigned long)msg);
        ota_result_t result = ota_relay_forward_firmware();

        uint32_t result_msg = (uint32_t)result;
        xQueueSend(g_ota_result_queue, &result_msg, pdMS_TO_TICKS(100));

        if (result == OTA_RESULT_OK) {
            DEBUG_INFO("OTA", "OTA 转发成功完成");
        } else {
            DEBUG_ERR("OTA", "OTA 转发失败，错误码: %d", result);
            ota_relay_reset();
        }
    }
}