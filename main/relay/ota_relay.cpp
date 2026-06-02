/*
 * ota_relay.cpp - OTA 中继核心状态机实现
 * T2.3, T2.4, T3.1 - T3.7
 *
 * 所有功能封装为 OtaRelay 类方法。
 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "aes/esp_aes.h"

#include "ota_relay.h"
#include "spi_master.h"
#include "protocol.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"

// ============================================================
// 静态成员定义
// ============================================================
relay_state_t OtaRelay::relayState = RELAY_IDLE;
uint32_t OtaRelay::relaySeq = 0;
uint32_t OtaRelay::relayTotalPackets = 0;
volatile bool OtaRelay::gd32AppAlive = false;
gd32_version_t OtaRelay::gd32Version = {};
QueueHandle_t OtaRelay::dispatchQueue = NULL;
QueueHandle_t OtaRelay::resultQueue = NULL;

OtaRelay& OtaRelay::instance()
{
    static OtaRelay inst;
    return inst;
}


// ============================================================
// 内部工具函数
// ============================================================

static bool bytesAllValue(const uint8_t *buf, size_t len, uint8_t value)
{
    if (!buf || len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] != value) return false;
    }
    return true;
}

int OtaRelay::aes128EcbEncryptBlockLocal(const uint8_t *key, const uint8_t *in, uint8_t *out)
{
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    if (esp_aes_setkey(&ctx, key, 128) != 0) { esp_aes_free(&ctx); return -1; }
    int ret = esp_aes_crypt_ecb(&ctx, ESP_AES_ENCRYPT, in, out);
    esp_aes_free(&ctx);
    return ret;
}

void OtaRelay::logRespPayload(const char *stage, uint8_t resp_type, const uint8_t *payload, size_t len)
{
    char hex[193] = {0};
    size_t n = (len > 32U) ? 32U : len;
    for (size_t i = 0; i < n; ++i)
        snprintf(&hex[i * 2], sizeof(hex) - i * 2, "%02X", payload[i]);
    DEBUG_DBG("OTA", "%s resp: type=0x%02X len=%u payload[%u]=%s%s",
              stage, resp_type, (unsigned)len, (unsigned)n,
              n > 0 ? hex : "<empty>", len > n ? "..." : "");
}

esp_err_t OtaRelay::waitUntilNotWait(const char *stage, uint8_t *resp_type,
                                     uint8_t *resp_payload, size_t resp_max,
                                     size_t *resp_len, uint32_t timeout_ms, uint32_t max_wait_rounds)
{
    if (!resp_type) return ESP_ERR_INVALID_ARG;
    uint32_t round = 0;
    while (*resp_type == SPI_OTA_RSP_WAIT && round < max_wait_rounds) {
        ++round;
        DEBUG_WARN("OTA", "%s WAIT, poll #%u...", stage, round);
        esp_err_t err = SpiMaster::instance().otaPollResponse(resp_type, resp_payload, resp_max, resp_len, 100);
        if (err == ESP_ERR_TIMEOUT) { *resp_type = SPI_OTA_RSP_WAIT; continue; }
        if (err != ESP_OK) return err;
        logRespPayload(stage, *resp_type, resp_payload, resp_len ? *resp_len : 0);
    }
    return ESP_OK;
}

bool OtaRelay::isStructuredAckWithSeq(const uint8_t *payload, size_t len, uint32_t expected_seq)
{
    if (!payload || len < 6) return false;
    if (payload[0] != 0xBA || payload[1] != 0xC0) return false;
    uint32_t seq = (uint32_t)payload[2] | ((uint32_t)payload[3] << 8) |
                   ((uint32_t)payload[4] << 16) | ((uint32_t)payload[5] << 24);
    return seq == expected_seq;
}

bool OtaRelay::parseStructuredError(const uint8_t *payload, size_t len, uint32_t *seq_out, uint8_t *err_out)
{
    if (!payload || len < 7) return false;
    if (payload[0] != 0xBA || payload[1] != 0xC0) return false;
    if (seq_out) *seq_out = (uint32_t)payload[2] | ((uint32_t)payload[3] << 8) |
                   ((uint32_t)payload[4] << 16) | ((uint32_t)payload[5] << 24);
    if (err_out) *err_out = payload[6];
    return true;
}

bool OtaRelay::isHandshakeEchoAck(const uint8_t *payload, size_t len)
{ return payload && len >= 2U && payload[0] == 0x00U && payload[1] == 0xACU; }

bool OtaRelay::isChallengePayloadAck(const uint8_t *payload, size_t len)
{ return payload && len == GD32_CHALLENGE_RESPONSE_LEN; }

bool OtaRelay::isConfirmPlainAck(const uint8_t *payload, size_t len)
{ return payload && len == 2U && payload[0] == 0xBAU && payload[1] == 0xC0U; }

bool OtaRelay::isSizeSeq0Ack(const uint8_t *payload, size_t len)
{ return isStructuredAckWithSeq(payload, len, 0U); }

bool OtaRelay::isFinishPlainAck(const uint8_t *payload, size_t len)
{
    if (!payload) return false;
    if (len == 2U && payload[0] == 0x0AU && payload[1] == 0xACU) return true;
    /* structured ACK 0xBAC0 no longer accepted as FINISH success */
    return false;
}

bool OtaRelay::isStructuredAckWithPayload(const uint8_t *payload, size_t len)
{ return payload && len >= 6U && payload[0] == 0xBAU && payload[1] == 0xC0U; }

esp_err_t OtaRelay::pollUntilExpectedAckShape(const char *stage,
                                               uint8_t *resp_type, uint8_t *resp_payload,
                                               size_t resp_max, size_t *resp_len,
                                               uint32_t poll_timeout_ms, uint32_t wait_rounds,
                                               uint32_t maxExtraPolls,
                                               bool (*matcher)(const uint8_t *, size_t))
{
    if (!stage || !resp_type || !resp_payload || !resp_len || !matcher)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;
    if (*resp_type == SPI_OTA_RSP_WAIT) {
        err = instance().waitUntilNotWait(stage, resp_type, resp_payload, resp_max, resp_len, poll_timeout_ms, wait_rounds);
        if (err != ESP_OK) return err;
    }
    for (uint32_t extra = 0; extra < maxExtraPolls; ++extra) {
        if (*resp_type != SPI_OTA_RSP_ACK) return ESP_OK;
        if (matcher(resp_payload, *resp_len)) return ESP_OK;
        uint32_t err_seq = 0; uint8_t err_code = 0;
        if (parseStructuredError(resp_payload, *resp_len, &err_seq, &err_code)) return ESP_OK;
        DEBUG_WARN("OTA", "%s stale ACK, poll more: len=%u", stage, (unsigned)*resp_len);
        *resp_len = 0;
        err = SpiMaster::instance().otaPollResponse(resp_type, resp_payload, resp_max, resp_len, poll_timeout_ms);
        if (err != ESP_OK) return err;
        logRespPayload(stage, *resp_type, resp_payload, *resp_len);
        if (*resp_type == SPI_OTA_RSP_WAIT) {
            err = instance().waitUntilNotWait(stage, resp_type, resp_payload, resp_max, resp_len, poll_timeout_ms, wait_rounds);
            if (err != ESP_OK) return err;
        }
    }
    return ESP_OK;
}

bool OtaRelay::parseAndVerifyChallengePayload(const uint8_t *resp_payload, size_t resp_len,
                                               uint8_t *out_k_session, uint8_t *out_uid)
{
    if (!resp_payload || !out_k_session || !out_uid) return false;
    if (resp_len != GD32_CHALLENGE_RESPONSE_LEN) {
        DEBUG_ERR("OTA", "CHALLENGE: invalid len=%u", (unsigned)resp_len); return false;
    }
    const uint8_t *uid = resp_payload;
    const uint8_t *nonce_frag = resp_payload + 12;
    const uint8_t *encrypted_ksession = resp_payload + 20;
    if (memcmp(nonce_frag, instance().m_nonceServer + 8, 8) != 0) {
        DEBUG_ERR("OTA", "CHALLENGE: nonce mismatch"); return false;
    }
    uint8_t K_session[16] = {0};
    if (aes128EcbEncryptBlockLocal(MASTER_KEY, instance().m_nonceServer, K_session) != 0) {
        DEBUG_ERR("OTA", "CHALLENGE: derive K_session failed"); return false;
    }
    uint8_t expected_encrypted[16] = {0};
    if (aes128EcbEncryptBlockLocal(MASTER_KEY, K_session, expected_encrypted) != 0) {
        DEBUG_ERR("OTA", "CHALLENGE: encrypt K_session failed"); return false;
    }
    if (memcmp(encrypted_ksession, expected_encrypted, 16) != 0) {
        DEBUG_ERR("OTA", "CHALLENGE: K_session mismatch"); return false;
    }
    memcpy(out_k_session, K_session, 16);
    memcpy(out_uid, uid, GD32_UID_LEN);
    if (bytesAllValue(out_uid, GD32_UID_LEN, 0x00U) || bytesAllValue(out_uid, GD32_UID_LEN, 0xFFU)) {
        DEBUG_ERR("OTA", "CHALLENGE: UID all 0 or FF"); return false;
    }
    return true;
}

bool OtaRelay::aes128EcbEncrypt48Bytes(const uint8_t *key, const uint8_t *in, uint8_t *out)
{
    for (int i = 0; i < 3; ++i)
        if (aes128EcbEncryptBlockLocal(key, in + i * 16, out + i * 16) != 0) return false;
    return true;
}

void OtaRelay::calculateCrc32WithKey(const uint8_t *key, const uint8_t *data48, uint8_t *output)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (int i = 0; i < 16; ++i) { crc ^= key[i]; for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U))); }
    for (size_t i = 0; i < 48U; ++i) { crc ^= data48[i]; for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U))); }
    crc ^= 0xFFFFFFFFU;
    output[0] = (uint8_t)(crc & 0xFFU); output[1] = (uint8_t)((crc >> 8) & 0xFFU);
    output[2] = (uint8_t)((crc >> 16) & 0xFFU); output[3] = (uint8_t)((crc >> 24) & 0xFFU);
}

/**
 * @brief 从 Flash 加载元数据
 * @param meta 输出参数，加载后的元数据结构体。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t OtaRelay::metaLoad(ota_meta_t *meta)
{
    if (!meta) return ESP_ERR_INVALID_ARG;
    memset(meta, 0, sizeof(ota_meta_t));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_META_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // NVS 中没有元数据是正常状态（首次使用）
        return ESP_OK;
    }

    uint32_t val = 0;
    if (nvs_get_u32(handle, "total_size", &val) == ESP_OK) meta->total_size = val;
    if (nvs_get_u32(handle, "stored_size", &val) == ESP_OK) meta->stored_size = val;
    if (nvs_get_u32(handle, "last_seq", &val) == ESP_OK) meta->last_seq = val;
    if (nvs_get_u32(handle, "chunk_count", &val) == ESP_OK) meta->chunk_count = val;

    nvs_close(handle);
    return ESP_OK;
}

/**
 * @brief 保存元数据到 Flash
 * @param meta 待保存的元数据结构体。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t OtaRelay::metaSave(const ota_meta_t *meta)
{
    if (!meta) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_META_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "无法打开 NVS 写入: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u32(handle, "total_size", meta->total_size);
    nvs_set_u32(handle, "stored_size", meta->stored_size);
    nvs_set_u32(handle, "last_seq", meta->last_seq);
    nvs_set_u32(handle, "chunk_count", meta->chunk_count);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "NVS commit 失败: %s", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief 传输过程中更新元数据
 * @param stored_size 已存储的固件大小（字节）。
 * @param last_seq 最后成功的分块序号。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t OtaRelay::metaUpdate(uint32_t stored_size, uint32_t last_seq)
{
    ota_meta_t meta;
    esp_err_t err = OtaRelay::metaLoad(&meta);
    if (err != ESP_OK) return err;

    meta.stored_size = stored_size;
    meta.last_seq = last_seq;

    return OtaRelay::metaSave(&meta);
}

/**
 * @brief OTA 完成后或中止时清除元数据
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t OtaRelay::metaClear()
{
    DEBUG_INFO("OTA", "清除 OTA 元数据");
    ota_meta_t meta = {};
    return OtaRelay::metaSave(&meta);
}

/**
 * @brief Flash 写入接口
 * @param offset 分区内写入偏移。
 * @param data 待写入的数据指针。
 * @param len 数据长度（字节）。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t OtaRelay::flashWriteChunk(uint32_t offset, const uint8_t *data, size_t len)
{
    if (!instance().m_gd32Partition) {
        instance().m_gd32Partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "gd32_ota");
        if (!instance().m_gd32Partition) {
            DEBUG_ERR("OTA", "找不到 gd32_ota 分区");
            return ESP_ERR_NOT_FOUND;
        }
    }

    return esp_partition_write(instance().m_gd32Partition, offset, data, len);
}

/**
 * @brief 从 OTA 缓存分区读取指定偏移和长度的数据块。
 * @param offset 分区内读取偏移。
 * @param data 读取缓冲区指针。
 * @param len 读取长度（字节）。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t OtaRelay::flashReadChunk(uint32_t offset, uint8_t *data, size_t len)
{
    if (!instance().m_gd32Partition) {
        instance().m_gd32Partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "gd32_ota");
        if (!instance().m_gd32Partition) {
            DEBUG_ERR("OTA", "找不到 gd32_ota 分区");
            return ESP_ERR_NOT_FOUND;
        }
    }

    return esp_partition_read(instance().m_gd32Partition, offset, data, len);
}

/**
 * @brief 阶段1 - 发起与 GD32 的握手
 * @return 操作结果码。
 */
esp_err_t OtaRelay::requestGd32AppEnterBootloader()
{
    DEBUG_INFO("OTA", "尝试通知 GD32 APP 进入 Bootloader OTA 窗口");
    esp_err_t err = SpiMaster::instance().appRequestBootloader();
    if (err == ESP_OK) {
        /* 收到确认：可能来自 APP（即将跳转 BOOT）或 BOOT 本身（已在 BOOT）。
         * 如果来自 APP，GD32 需要复位并重新初始化 SPI 外设，需等 ~1-2s；
         * 如果已在 BOOT，SPI 已经就绪，200ms 足够。
         * 等待 1000ms 给 GD32 足够时间完成系统复位和 Bootloader 初始化。 */
        DEBUG_INFO("OTA", "GD32 已确认进入 Bootloader，等待就绪");
        vTaskDelay(pdMS_TO_TICKS(1000));
        return ESP_OK;
    }
    DEBUG_WARN("OTA", "GD32 APP 未确认进入 Bootloader: %s", esp_err_to_name(err));
    return err;
}


ota_result_t OtaRelay::probeBootHandshake()
{
    /* 0xB0 timeout: probe if GD32 is already in BOOT.
     * Send one lightweight HANDSHAKE_REQ, accept only echo ACK 0x00 0xAC
     * as "already in BOOT" signal; otherwise fail immediately. */
    DEBUG_INFO("OTA", "0xB0 timeout, probing if GD32 already in BOOT...");
    uint8_t payload[] = {0x00, 0xAC};
    uint8_t resp_type = 0U;
    uint8_t resp_payload[64] = {0};
    size_t resp_len = 0U;

    esp_err_t err = SpiMaster::instance().otaTransfer(SPI_OTA_CMD_REQ, payload, sizeof(payload),
                                     &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 500);
    if (err == ESP_OK && resp_type == SPI_OTA_RSP_ACK && isHandshakeEchoAck(resp_payload, resp_len)) {
        DEBUG_INFO("OTA", "BOOT probe success: GD32 already in BOOT, continue OTA");
        return OTA_RESULT_OK;
    }
    DEBUG_ERR("OTA", "BOOT probe failed: GD32 not in BOOT, abort (err=%d resp_type=0x%02X)", err, resp_type);
    return OTA_RESULT_ERR_TIMEOUT;
}

ota_result_t OtaRelay::sendHandshakeReq()
{
    DEBUG_INFO("OTA", "=== 阶段1: 握手开始 ===");
    DEBUG_INFO("OTA", "发送 HANDSHAKE_REQ (0x00 0xAC)");

    uint8_t payload[] = {0x00, 0xAC};
    uint8_t last_resp_type = 0U;
    size_t last_resp_len = 0U;
    esp_err_t last_err = ESP_OK;
    constexpr uint32_t kHandshakeAttempts = 10U;
    constexpr uint32_t kHandshakeReqTimeoutMs = 200U;
    constexpr uint32_t kHandshakePollTimeoutMs = 200U;
    constexpr uint32_t kHandshakeRetryGapMs = 100U;

    /** @brief 当前 OTA 中继状态机状态。 */
    relayState = RELAY_HANDSHAKE_SENT;
    DEBUG_INFO("OTA", "状态: RELAY_HANDSHAKE_SENT");

    for (uint32_t attempt = 0U; attempt < kHandshakeAttempts; ++attempt) {
        uint8_t resp_type = 0U;
        uint8_t resp_payload[64] = {0};
        size_t resp_len = 0U;

        esp_err_t err = SpiMaster::instance().otaTransfer(SPI_OTA_CMD_REQ, payload, sizeof(payload),
                                         &resp_type, resp_payload, sizeof(resp_payload), &resp_len,
                                         kHandshakeReqTimeoutMs);
        last_err = err;
        last_resp_type = resp_type;
        last_resp_len = resp_len;

        if (err == ESP_OK) {
            logRespPayload("HANDSHAKE_REQ", resp_type, resp_payload, resp_len);
            err = pollUntilExpectedAckShape("HANDSHAKE_REQ", &resp_type, resp_payload,
                                                sizeof(resp_payload), &resp_len,
                                                kHandshakePollTimeoutMs, 2, 2, OtaRelay::isHandshakeEchoAck);
            last_err = err;
            last_resp_type = resp_type;
            last_resp_len = resp_len;

            if (err == ESP_OK && resp_type == SPI_OTA_RSP_ACK && isHandshakeEchoAck(resp_payload, resp_len)) {
                DEBUG_INFO("OTA", "握手请求已确认 (attempt=%lu)", (unsigned long)(attempt + 1U));
                return OTA_RESULT_OK;
            }
        }

        DEBUG_WARN("OTA", "HANDSHAKE_REQ 未就绪，重试 %lu/%lu: err=%d resp_type=0x%02X resp_len=%u",
                   (unsigned long)(attempt + 1U),
                   (unsigned long)kHandshakeAttempts,
                   last_err,
                   last_resp_type,
                   (unsigned)last_resp_len);
        vTaskDelay(pdMS_TO_TICKS(kHandshakeRetryGapMs));
    }

    DEBUG_ERR("OTA", "HANDSHAKE_REQ 失败, err=%d, resp_type=0x%02X, resp_len=%zu",
              last_err, last_resp_type, last_resp_len);
    relayState = RELAY_ERROR;
    return OTA_RESULT_ERR_TIMEOUT;
}

/**
 * @brief 发送随机数并与 GD32 相互认证
 * @return 操作结果码。
 */
ota_result_t OtaRelay::sendNonceAndAuthenticate()
{
    uint8_t resp_type = 0;
    uint8_t resp_payload[64] = {0};
    size_t resp_len = 0;

    uint32_t timestamp = xTaskGetTickCount();
    uint32_t mem_addr = (uint32_t)(uintptr_t)m_nonceServer;
    uint32_t rand_seed = 0x12345678;

    Protocol::scrambleGenerate(timestamp, mem_addr, rand_seed, m_nonceServer);

    DEBUG_INFO("OTA", "--- 认证步骤 1: 发送 CHALLENGE ---");
    DEBUG_INFO("OTA", "发送 nonce_server (16B), timestamp=0x%08X", timestamp);
    relayState = RELAY_CHALLENGE_SENT;
    DEBUG_INFO("OTA", "状态: RELAY_CHALLENGE_SENT");

    esp_err_t err = SpiMaster::instance().otaTransfer(SPI_OTA_CMD_REQ, m_nonceServer, 16,
                                     &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 5000);
    logRespPayload("CHALLENGE", resp_type, resp_payload, resp_len);
    err = pollUntilExpectedAckShape("CHALLENGE", &resp_type, resp_payload,
                                        sizeof(resp_payload), &resp_len,
                                        1000, 12, 8, OtaRelay::isChallengePayloadAck);
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || !isChallengePayloadAck(resp_payload, resp_len)) {
        DEBUG_ERR("OTA", "CHALLENGE 发送失败, err=%d, resp_type=0x%02X, resp_len=%zu", err, resp_type, resp_len);
        relayState = RELAY_ERROR;
        return OTA_RESULT_ERR_TIMEOUT;
    }

    relayState = RELAY_CHALLENGE_RESPOND;
    DEBUG_INFO("OTA", "已收到 GD32 握手数据包");
    DEBUG_INFO("OTA", "状态: RELAY_CHALLENGE_RESPOND");

    if (!parseAndVerifyChallengePayload(resp_payload, resp_len, m_kSession, m_gd32UidRuntime)) {
        DEBUG_ERR("OTA", "CHALLENGE 解包/校验失败");
        relayState = RELAY_ERROR;
        return OTA_RESULT_ERR_AUTH;
    }

    uint8_t plaintext[16];
    for (int i = 0; i < 16; ++i) {
        plaintext[i] = m_nonceServer[i] ^ 0x55U;
    }
    uint8_t encrypted_confirm[16] = {0};
    if (aes128EcbEncryptBlockLocal(m_kSession, plaintext, encrypted_confirm) != 0) {
        DEBUG_ERR("OTA", "加密确认块失败");
        relayState = RELAY_ERROR;
        return OTA_RESULT_ERR_AUTH;
    }

    DEBUG_INFO("OTA", "--- 认证步骤 2: 发送 SESSION_KEY/CONFIRM ---");
    relayState = RELAY_SESSION_KEY_SENT;
    DEBUG_INFO("OTA", "状态: RELAY_SESSION_KEY_SENT");

    resp_len = 0;
    err = SpiMaster::instance().otaTransfer(SPI_OTA_CMD_REQ, encrypted_confirm, 16,
                           &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
    logRespPayload("CONFIRM", resp_type, resp_payload, resp_len);
    err = pollUntilExpectedAckShape("CONFIRM", &resp_type, resp_payload,
                                        sizeof(resp_payload), &resp_len,
                                        1000, 8, 8, OtaRelay::isConfirmPlainAck);
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || !isConfirmPlainAck(resp_payload, resp_len)) {
        DEBUG_ERR("OTA", "SESSION_KEY/CONFIRM 失败, err=%d, resp_type=0x%02X, resp_len=%zu", err, resp_type, resp_len);
        relayState = RELAY_ERROR;
        return OTA_RESULT_ERR_TIMEOUT;
    }

    /* 诊断: 打印 confirm 响应完整内容，确认是 2B 还是意外读到了 36B challenge 响应 */
    {
        char hex[73] = {0};
        size_t n = (resp_len > 36U) ? 36U : resp_len;
        for (size_t i = 0; i < n; ++i) {
            snprintf(&hex[i * 2], sizeof(hex) - i * 2, "%02X", resp_payload[i]);
        }
        DEBUG_INFO("OTA", "CONFIRM 验证通过, resp_len=%zu, payload=%s", resp_len, hex);
    }

    relayState = RELAY_AUTH_COMPLETE;
    DEBUG_INFO("OTA", "状态: RELAY_AUTH_COMPLETE");
    DEBUG_INFO("OTA", "=== 阶段1 认证完成: 安全通道已建立 ===");

    Protocol::sessionKeyInit(&m_sessionCtx, m_kSession);
    return OTA_RESULT_OK;
}

/**
 * @brief 向 GD32 发送固件总大小
 * @param size 固件总字节数。
 * @return 操作结果码。
 */
ota_result_t OtaRelay::sendFirmwareSize(uint32_t size)
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

    relayState = RELAY_SIZE_SENT;
    DEBUG_INFO("OTA", "状态: RELAY_SIZE_SENT");

    esp_err_t err = SpiMaster::instance().otaTransfer(SPI_OTA_CMD_DATA, size_payload, sizeof(size_payload),
                                     &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
    logRespPayload("SEND_SIZE", resp_type, resp_payload, resp_len);
    DEBUG_WARN("OTA", "SEND_SIZE poll config: poll_timeout_ms=2000 wait_rounds=300 maxExtraPolls=24");
    err = pollUntilExpectedAckShape("SEND_SIZE", &resp_type, resp_payload,
                                        sizeof(resp_payload), &resp_len,
                                        2000, 300, 24, OtaRelay::isSizeSeq0Ack);
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || !isSizeSeq0Ack(resp_payload, resp_len)) {
        DEBUG_ERR("OTA", "SEND_SIZE 失败, err=%d, resp_type=0x%02X, resp_len=%zu", err, resp_type, resp_len);
        relayState = RELAY_ERROR;
        return OTA_RESULT_ERR_TIMEOUT;
    }

    DEBUG_INFO("OTA", "大小已发送: %u 字节，等待 GD32 确认...", size);
    relayState = RELAY_SIZE_ACK;
    DEBUG_INFO("OTA", "状态: RELAY_SIZE_ACK");

    return OTA_RESULT_OK;
}

/**
 * @brief 从 Flash 向 GD32 转发存储的固件（48B/包）
 * @return 操作结果码。
 */
ota_result_t OtaRelay::relayPacketsFromFlash()
{
    DEBUG_INFO("OTA", "=== 阶段3: 从 Flash 转发固件 ===");

    ota_meta_t meta;
    if (OtaRelay::metaLoad(&meta) != ESP_OK) {
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
    relayState = RELAY_RELAY_ACTIVE;
    relayTotalPackets = total_packets;
    relaySeq = 0;

    for (uint32_t seq = 0; seq < total_packets; seq++) {
        uint32_t retry_count = 0;
        if ((seq % OTA_PACKET_HEALTH_CHECK_INTERVAL) == 0 || seq == 0 || seq == total_packets - 1) {
            health_status_t health = HealthMonitor::instance().checkBeforeOtaRelay();
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

        esp_err_t err = OtaRelay::flashReadChunk(flash_offset, plain48, chunk_size);
        if (err != ESP_OK) {
            DEBUG_ERR("OTA", "Flash 读取失败, offset=%u, err=%d", flash_offset, err);
            return OTA_RESULT_ERR_STORAGE;
        }

        tx_buf[0] = 0x00; tx_buf[1] = 0x00; tx_buf[2] = 0x00; tx_buf[3] = 0x00; /* session_num 预留 */
        tx_buf[4] = (uint8_t)(seq & 0xFFU);
        tx_buf[5] = (uint8_t)((seq >> 8) & 0xFFU);
        tx_buf[6] = 0x00;
        tx_buf[7] = 0x00;

        if (!aes128EcbEncrypt48Bytes(m_kSession, plain48, &tx_buf[8])) {
            DEBUG_ERR("OTA", "包 %u 加密失败", seq);
            return OTA_RESULT_ERR_AUTH;
        }

        uint32_t if_end = (seq == total_packets - 1) ? 0xFFFFFFFFU : 0x00000000U;
        tx_buf[56] = (uint8_t)(if_end & 0xFFU);
        tx_buf[57] = (uint8_t)((if_end >> 8) & 0xFFU);
        tx_buf[58] = (uint8_t)((if_end >> 16) & 0xFFU);
        tx_buf[59] = (uint8_t)((if_end >> 24) & 0xFFU);
        calculateCrc32WithKey(m_kSession, plain48, &tx_buf[60]);

        DEBUG_DBG("OTA", "包 %u: chunk=%u if_end=0x%08X crc32=%02X%02X%02X%02X",
                   seq, (unsigned)chunk_size, (unsigned)if_end,
                   tx_buf[60], tx_buf[61], tx_buf[62], tx_buf[63]);

        relayState = RELAY_PACKET_SENT;

        resend_packet:
        while (1) {
            resp_len = 0;
            err = SpiMaster::instance().otaTransfer(SPI_OTA_CMD_DATA, tx_buf, sizeof(tx_buf),
                                   &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
            logRespPayload("DATA_PACKET", resp_type, resp_payload, resp_len);
            if (err == ESP_OK) {
                err = waitUntilNotWait("DATA_PACKET", &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 1000, 10);
            }
            if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK) {
                DEBUG_WARN("OTA", "数据包 %u 发送失败, err=%d, resp_type=0x%02X, resp_len=%zu", seq, err, resp_type, resp_len);
                result = OTA_RESULT_ERR_TIMEOUT;
                break;
            }

            uint32_t expected_ack = (seq == total_packets - 1) ? 0xF0F0F0F0U : (seq + 1U);

            /* 跳过上一阶段残留的、seq 不匹配的结构化 ACK。
             *
             * 典型场景：SIZE 阶段的 ACK(seq=0) 仍挂在 DMA 上，
             * 包 0 发出后第一次 poll 读到 seq=0 而非期望的 seq=1。
             * 继续轮询直到拿到正确的 ACK 或超时。 */
            if (!isStructuredAckWithSeq(resp_payload, resp_len, expected_ack)) {
                uint32_t err_seq = 0;
                uint8_t err_code = 0;
                if (parseStructuredError(resp_payload, resp_len, &err_seq, &err_code)) {
                    if (err_seq == seq) {
                        DEBUG_WARN("OTA", "包 %u 收到结构化错误 err=0x%02X，准备重发", seq, err_code);
                        relayState = RELAY_PACKET_ACK_RETRY;
                        if (++retry_count >= 3U) {
                            DEBUG_ERR("OTA", "包 %u 重发次数超限", seq);
                            result = (err_code == 0x04U) ? OTA_RESULT_ERR_CRC : OTA_RESULT_ERR_GD32;
                            break;
                        }
                        continue; /* 重发本包 */
                    }
                }

                /* 非期望 ACK，也非错误帧 -> 可能是残留 ACK，循环 poll 直到拿到正确的。
                 * 超时/非ACK 退出时需要重发当前包（GD32 Flash 缓冲满时会发 WAIT 后排空，
                 * ESP32 必须重发才能让 GD32 重新处理）。 */
                bool got_expected = false;
                for (uint32_t retry = 0; retry < 10; ++retry) {
                    DEBUG_DBG("OTA", "包 %u 收到非目标ACK(seq不匹配)，继续轮询(%u/5)...", seq, retry + 1);
                    err = SpiMaster::instance().otaPollResponse(&resp_type, resp_payload, sizeof(resp_payload), &resp_len, 2000);
                    if (err == ESP_OK && resp_type == SPI_OTA_RSP_WAIT) {
                        err = waitUntilNotWait("DATA_PACKET", &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 100, 5);
                    }
                    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK) {
                        /* poll 超时或收到非 ACK（WAIT/ERROR）-> GD32 Flash 缓冲可能已满，
                         * 正在排空。重发当前包让 GD32 重新入队处理。 */
                        DEBUG_WARN("OTA", "包 %u poll 超时/非ACK，重发 (retry=%u/3)", seq, retry_count + 1);
                        relayState = RELAY_PACKET_ACK_RETRY;
                        if (++retry_count >= 3U) {
                            DEBUG_ERR("OTA", "包 %u 重发次数超限", seq);
                            result = OTA_RESULT_ERR_GD32;
                            break;
                        }
                        goto resend_packet;
                    }
                    if (isStructuredAckWithSeq(resp_payload, resp_len, expected_ack)) {
                        got_expected = true;
                        break;
                    }
                    if (parseStructuredError(resp_payload, resp_len, &err_seq, &err_code) && err_seq == seq) {
                        break; /* 下面统一处理 */
                    }
                }

                if (got_expected) {
                    /* 匹配成功 */
                    flash_offset += (uint32_t)chunk_size;
                    relayState = RELAY_PACKET_ACK_OK;
                    break;
                }

                /* 最终检查是否为错误帧 */
                if (parseStructuredError(resp_payload, resp_len, &err_seq, &err_code) && err_seq == seq) {
                    DEBUG_WARN("OTA", "包 %u 收到结构化错误 err=0x%02X，准备重发", seq, err_code);
                    relayState = RELAY_PACKET_ACK_RETRY;
                    if (++retry_count >=10U) {
                        DEBUG_ERR("OTA", "包 %u 重发次数超限", seq);
                        result = (err_code == 0x04U) ? OTA_RESULT_ERR_CRC : OTA_RESULT_ERR_GD32;
                        break;
                    }
                    continue; /* 重发本包 */
                }

                /* 轮询耗尽仍未拿到期望 ACK -> 重发当前包 */
                DEBUG_WARN("OTA", "包 %u 轮询耗尽未匹配，重发 (retry=%u/3)", seq, retry_count + 1);
                relayState = RELAY_PACKET_ACK_RETRY;
                if (++retry_count >= 10U) {
                    DEBUG_ERR("OTA", "包 %u 重发次数超限", seq);
                    result = OTA_RESULT_ERR_GD32;
                    break;
                }
                goto resend_packet;
            }

            /* 匹配成功 */
            flash_offset += (uint32_t)chunk_size;
            relayState = RELAY_PACKET_ACK_OK;
            break;
        }

        if (result != OTA_RESULT_OK) {
            break;
        }

        relaySeq = seq + 1;
        if (seq % (total_packets / 10 + 1) == 0) {
            uint32_t progress = (relaySeq * 100U) / total_packets;
            DEBUG_INFO("OTA", "转发进度: %u/%u 包 (%u%%)", relaySeq, total_packets, progress);
        }
    }
    relayState = RELAY_RECEIVE_DONE;
    DEBUG_DBG("OTA", "状态: RELAY_RECEIVE_DONE");
    return result;
}

/**
 * @brief 结束 OTA - 发送 0x0A AC 并清理
 * @return 操作结果码。
 */
ota_result_t OtaRelay::finishOta()
{
    DEBUG_INFO("OTA", "=== 阶段4: 结束 OTA ===");
    DEBUG_INFO("OTA", "发送 FINISH_OTA (0x0A 0xAC)，等待 GD32 确认即将跳转 APP");

    uint8_t finish_payload[2] = {0x0A, 0xAC};
    uint8_t resp_type = 0;
    uint8_t resp_payload[32] = {0};
    size_t resp_len = 0;

    /* FINISH_OTA 重试机制：GD32 Flash 缓冲未排空时会回 WAIT，
     * 需要重试直到收到正确的 ACK(0x0A 0xAC) 或超时。 */
    for (uint32_t retry = 0; retry < 10; ++retry) {
        resp_type = 0;
        resp_len = 0;
        memset(resp_payload, 0, sizeof(resp_payload));

        relayState = RELAY_FINISH_SENT;
        esp_err_t err = SpiMaster::instance().otaTransfer(SPI_OTA_CMD_DATA, finish_payload, sizeof(finish_payload),
                                         &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
        logRespPayload("FINISH_OTA", resp_type, resp_payload, resp_len);
        if (err != ESP_OK) {
            DEBUG_WARN("OTA", "FINISH_OTA 传输失败, err=%d, retry=%u/10", err, retry + 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        err = pollUntilExpectedAckShape("FINISH_OTA", &resp_type, resp_payload,
                                            sizeof(resp_payload), &resp_len,
                                            1000, 10, 8, OtaRelay::isFinishPlainAck);
        if (err != ESP_OK) {
            DEBUG_WARN("OTA", "FINISH_OTA poll 失败, err=%d, retry=%u/10", err, retry + 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (resp_type == SPI_OTA_RSP_ACK && isFinishPlainAck(resp_payload, resp_len)) {
            /* 成功收到 FINISH_OTA ACK */
            break;
        }

        /* 检查是否为 WAIT 帧（GD32 Flash 缓冲未排空） */
        if (resp_type == SPI_OTA_RSP_WAIT) {
            DEBUG_INFO("OTA", "FINISH_OTA 收到 WAIT, GD32 正在排空 Flash 缓冲, retry=%u/10", retry + 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* 检查是否为错误帧 */
        uint32_t err_seq = 0;
        uint8_t err_code = 0;
        if (parseStructuredError(resp_payload, resp_len, &err_seq, &err_code)) {
            DEBUG_ERR("OTA", "FINISH_OTA 收到错误帧 err=0x%02X, retry=%u/10", err_code, retry + 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        DEBUG_WARN("OTA", "FINISH_OTA 响应异常, resp_type=0x%02X, resp_len=%zu, retry=%u/10",
                   resp_type, resp_len, retry + 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (resp_type != SPI_OTA_RSP_ACK || !isFinishPlainAck(resp_payload, resp_len)) {
        DEBUG_ERR("OTA", "FINISH_OTA 失败, err=0, resp_type=0x%02X, resp_len=%zu", resp_type, resp_len);
        relayState = RELAY_ERROR;
        return OTA_RESULT_ERR_GD32;
    }

    /* isFinishPlainAck already verified: only 0x0A 0xAC (len==2) passes.
     * Structured error frames (0xBA 0xC0 ...) are rejected above. */

    relayState = RELAY_DONE;
    Protocol::sessionKeyClear(&m_sessionCtx);
    memset(&m_sessionCtx, 0, sizeof(session_ctx_t));
    memset(m_kSession, 0, sizeof(m_kSession));
    memset(m_nonceServer, 0, sizeof(m_nonceServer));
    memset(m_gd32UidRuntime, 0, sizeof(m_gd32UidRuntime));

    DEBUG_INFO("OTA", "状态: RELAY_DONE");
    DEBUG_INFO("OTA", "=== OTA 完成，等待 GD32 APP 启动 ===");

    OtaRelay::metaClear();

    /* ============================================================
     * 阶段5: APP 存活验证
     *
     * GD32 BOOT 处理完 FINISH_OTA 后会跳转到 APP。
     * APP 启动需要初始化外设和 SPI，大约需要 1-2 秒。
     * 我们以 500ms 间隔反复 ping APP，最多等 15 秒。
     * 收到 APP_ALIVE 回复才认为 OTA 真正成功。
     * ============================================================ */
    relayState = RELAY_APP_VERIFY;
    gd32AppAlive = false;

    for (uint32_t attempt = 0; attempt < 30; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(500));
        uint32_t uptime = 0;
        gd32_version_t ping_ver = {};
        esp_err_t ping_err = SpiMaster::instance().appPing(1000, &uptime, &ping_ver);
        if (ping_err == ESP_OK) {
            gd32AppAlive = true;
            gd32Version = ping_ver;
            DEBUG_INFO("OTA", "GD32 APP 存活确认: uptime=%lu sec, APP=v%d.%d.%d.%d, BOOT=v%d.%d.%d.%d",
                       (unsigned long)uptime,
                       ping_ver.app_major, ping_ver.app_minor, ping_ver.app_patch, ping_ver.app_build,
                       ping_ver.boot_major, ping_ver.boot_minor, ping_ver.boot_patch, ping_ver.boot_build);
            DEBUG_INFO("OTA", "=== OTA 全流程完成 (APP 已启动) ===");
            break;
        }
        DEBUG_INFO("OTA", "等待 GD32 APP 启动... (%u/30)", attempt + 1);
    }

    if (!gd32AppAlive) {
        DEBUG_ERR("OTA", "GD32 APP alive confirm timeout, OTA failed");
        reset();
        return OTA_RESULT_ERR_GD32;
    }

    return OTA_RESULT_OK;
}

/**
 * @brief 启动完整 OTA 流程的主入口
 * @return 操作结果码。
 */
ota_result_t OtaRelay::startHandshake()
{
    ota_result_t res;

    esp_err_t boot_err = requestGd32AppEnterBootloader();
    if (boot_err != ESP_OK) {
        res = probeBootHandshake();
        if (res != OTA_RESULT_OK) {
            reset();
            return res;
        }
    }

    res = sendHandshakeReq();
    if (res != OTA_RESULT_OK) { reset(); return res; }

    res = sendNonceAndAuthenticate();
    if (res != OTA_RESULT_OK) { reset(); return res; }

    ota_meta_t meta;
    OtaRelay::metaLoad(&meta);
    res = sendFirmwareSize(meta.total_size);
    if (res != OTA_RESULT_OK) { reset(); return res; }

    res = relayPacketsFromFlash();
    if (res != OTA_RESULT_OK) { reset(); return res; }

    res = finishOta();

    return res;
}

/**
 * @brief 从 Flash 向 GD32 转发固件
 * @return 操作结果码。
 */
ota_result_t OtaRelay::forwardFirmware()
{
    ota_meta_t meta;
    if (OtaRelay::metaLoad(&meta) != ESP_OK) {
        return OTA_RESULT_ERR_STORAGE;
    }

    DEBUG_INFO("OTA", "开始固件转发: %u 字节, %u 块",
               meta.total_size, meta.chunk_count);

    requestGd32AppEnterBootloader();

    ota_result_t res = sendHandshakeReq();
    if (res != OTA_RESULT_OK) { reset(); return res; }

    res = sendNonceAndAuthenticate();
    if (res != OTA_RESULT_OK) { reset(); return res; }

    res = sendFirmwareSize(meta.total_size);
    if (res != OTA_RESULT_OK) { reset(); return res; }

    res = relayPacketsFromFlash();
    if (res != OTA_RESULT_OK) { reset(); return res; }

    res = finishOta();

    return res;
}

/**
 * @brief 获取当前状态
 * @return 当前中继状态枚举值。
 */
relay_state_t OtaRelay::getState()
{
    /** @brief 当前 OTA 中继状态机状态。 */
    return relayState;
}

/**
 * @brief 重置状态机
 */
void OtaRelay::reset()
{
    /** @brief 当前 OTA 中继状态机状态。 */
    relayState = RELAY_IDLE;
    relaySeq = 0;
    relayTotalPackets = 0;
    Protocol::sessionKeyClear(&m_sessionCtx);
    memset(&m_sessionCtx, 0, sizeof(session_ctx_t));
    memset(m_kSession, 0, sizeof(m_kSession));
    memset(m_nonceServer, 0, sizeof(m_nonceServer));
    memset(m_gd32UidRuntime, 0, sizeof(m_gd32UidRuntime));
}

/**
 * @brief OTA 中继 FreeRTOS 任务
 * @param pvParameters FreeRTOS 任务参数，未使用。
 */
void OtaRelay::taskEntry(void *pvParameters)
{
    DEBUG_INFO("OTA", "OTA 中继任务启动");

    /** @brief OTA 命令分发队列，从 WiFi/BLE 任务接收指令。 */
    dispatchQueue = xQueueCreate(4, sizeof(uint32_t));
    /** @brief OTA 结果回传队列，将执行结果返回给调用方。 */
    resultQueue = xQueueCreate(4, sizeof(uint32_t));

    if (!dispatchQueue || !resultQueue) {
        DEBUG_ERR("OTA", "OTA 队列创建失败");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        uint32_t msg = 0;
        BaseType_t ret = xQueueReceive(dispatchQueue, &msg, portMAX_DELAY);
        if (ret != pdTRUE) {
            continue;
        }

        DEBUG_INFO("OTA", "收到 OTA 调度信号: %lu，开始转发...", (unsigned long)msg);
        ota_result_t result = instance().forwardFirmware();

        uint32_t result_msg = (uint32_t)result;
        xQueueSend(resultQueue, &result_msg, pdMS_TO_TICKS(100));

        if (result == OTA_RESULT_OK) {
            DEBUG_INFO("OTA", "OTA 转发成功完成");
            /* 保持 RELAY_DONE 状态 3 秒，让网页轮询有机会看到完成状态 */
            vTaskDelay(pdMS_TO_TICKS(3000));
        } else {
            DEBUG_ERR("OTA", "OTA 转发失败，错误码: %d", result);
        }
        instance().reset();
    }
}


