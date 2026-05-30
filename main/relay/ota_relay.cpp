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
#include "nvs_flash.h"
#include "nvs.h"
#include "aes/esp_aes.h"

#include "ota_relay.h"
#include "spi_master.h"
#include "protocol.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"

/** @brief 当前 OTA 中继状态机状态。 */
relay_state_t g_relay_state = RELAY_IDLE;
volatile bool g_gd32_app_alive = false;

static const esp_partition_t *g_gd32_partition = NULL;
/** @brief OTA 认证会话上下文，包含会话密钥和 IV。 */
static session_ctx_t g_session_ctx;
static uint8_t g_nonce_server[16] = {0};
static uint8_t g_k_session[16] = {0};
static uint8_t g_gd32_uid_runtime[12] = {0};

static constexpr size_t GD32_UID_LEN = 12;
static constexpr size_t GD32_CHALLENGE_RESPONSE_LEN = 36;

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
static int aes128_ecb_encrypt_block_local(const uint8_t *key, const uint8_t *in, uint8_t *out);
static int aes128_ecb_decrypt_block_local(const uint8_t *key, const uint8_t *in, uint8_t *out);
static void log_resp_payload(const char *stage, uint8_t resp_type, const uint8_t *payload, size_t len);
static esp_err_t wait_until_not_wait(const char *stage, uint8_t *resp_type,
                                     uint8_t *resp_payload, size_t resp_max,
                                     size_t *resp_len, uint32_t timeout_ms,
                                     uint32_t max_wait_rounds);

/**
 * @brief 判断响应负载是否为带序号的结构化 ACK。
 * @param payload 响应负载指针。
 * @param len 负载长度。
 * @param expected_seq 期望的序号。
 * @return 匹配返回 true，否则返回 false。
 */
static bool is_structured_ack_with_seq(const uint8_t *payload, size_t len, uint32_t expected_seq)
{
    if (!payload || len < 6) return false;
    if (payload[0] != 0xBA || payload[1] != 0xC0) return false;
    uint32_t seq = (uint32_t)payload[2] | ((uint32_t)payload[3] << 8) |
                   ((uint32_t)payload[4] << 16) | ((uint32_t)payload[5] << 24);
    return seq == expected_seq;
}

/**
 * @brief 解析结构化错误响应中的序号和错误码。
 * @param payload 响应负载指针。
 * @param len 负载长度。
 * @param seq_out 输出错误对应序号，可为 NULL。
 * @param err_out 输出错误码，可为 NULL。
 * @return 解析成功返回 true，否则返回 false。
 */
static bool parse_structured_error(const uint8_t *payload, size_t len, uint32_t *seq_out, uint8_t *err_out)
{
    if (!payload || len < 7) return false;
    if (payload[0] != 0xBA || payload[1] != 0xC0) return false;
    if (seq_out) {
        *seq_out = (uint32_t)payload[2] | ((uint32_t)payload[3] << 8) |
                   ((uint32_t)payload[4] << 16) | ((uint32_t)payload[5] << 24);
    }
    if (err_out) *err_out = payload[6];
    return true;
}

/**
 * @brief 判断缓冲区是否全部由指定字节值组成。
 * @param buf 输入缓冲区。
 * @param len 缓冲区长度。
 * @param value 目标字节值。
 * @return 全部匹配返回 true，否则返回 false。
 */
static bool is_handshake_echo_ack(const uint8_t *payload, size_t len)
{
    return payload && len >= 2U && payload[0] == 0x00U && payload[1] == 0xACU;
}

static bool is_challenge_payload_ack(const uint8_t *payload, size_t len)
{
    return payload && len == GD32_CHALLENGE_RESPONSE_LEN;
}

static bool is_confirm_plain_ack(const uint8_t *payload, size_t len)
{
    return payload && len == 2U && payload[0] == 0xBAU && payload[1] == 0xC0U;
}

static bool is_size_seq0_ack(const uint8_t *payload, size_t len)
{
    return is_structured_ack_with_seq(payload, len, 0U);
}

static bool is_finish_plain_ack(const uint8_t *payload, size_t len)
{
    return payload && len >= 2U && payload[0] == 0x0AU && payload[1] == 0xACU;
}

/**
 * @brief FINISH 阶段匹配器：接受 0x0A 0xAC 明文 ACK 或结构化错误帧。
 *
 * BOOT 在 FINISH 成功时回 2B 明文 ACK {0x0A, 0xAC}；
 * 失败时回 7B 结构化错误帧 {0xBA,0xC0, seq(4B), err_code}。
 * 两种都需要识别，否则 ESP32 会把错误帧当成残留 ACK 继续轮询。
 */
static bool is_finish_ack_or_error(const uint8_t *payload, size_t len)
{
    if (!payload) return false;
    /* 明文 ACK: 0x0A 0xAC */
    if (len >= 2U && payload[0] == 0x0AU && payload[1] == 0xACU) return true;
    /* 结构化错误帧: 0xBA 0xC0 + seq(4B) + err_code */
    if (len >= 7U && payload[0] == 0xBAU && payload[1] == 0xC0U) return true;
    return false;
}


/**
 * @brief 按小端序读取 4 字节无符号整数。
 * @param p 输入字节指针（至少 4 字节）。
 * @return 读取出的 uint32_t 数值。
 */
static bool bytes_all_value(const uint8_t *buf, size_t len, uint8_t value)
{
    if (!buf || len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] != value) return false;
    }
    return true;
}

/**
 * @brief 继续轮询，直到拿到符合当前阶段预期形状的 ACK，或遇到结构化错误/超时。
 */
static esp_err_t poll_until_expected_ack_shape(const char *stage,
                                               uint8_t *resp_type,
                                               uint8_t *resp_payload,
                                               size_t resp_max,
                                               size_t *resp_len,
                                               uint32_t poll_timeout_ms,
                                               uint32_t wait_rounds,
                                               uint32_t maxExtraPolls,
                                               bool (*matcher)(const uint8_t *, size_t))
{
    if (!stage || !resp_type || !resp_payload || !resp_len || !matcher) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    if (*resp_type == SPI_OTA_RSP_WAIT) {
        err = wait_until_not_wait(stage, resp_type, resp_payload, resp_max, resp_len,
                                  poll_timeout_ms, wait_rounds);
        if (err != ESP_OK) {
            return err;
        }
    }

    for (uint32_t extra = 0; extra < maxExtraPolls; ++extra) {
        if (*resp_type != SPI_OTA_RSP_ACK) {
            return ESP_OK;
        }
        if (matcher(resp_payload, *resp_len)) {
            return ESP_OK;
        }

        uint32_t err_seq = 0;
        uint8_t err_code = 0;
        if (parse_structured_error(resp_payload, *resp_len, &err_seq, &err_code)) {
            return ESP_OK;
        }

        DEBUG_WARN("OTA", "%s 收到上一阶段残留ACK/非目标ACK，继续轮询: len=%u", stage, (unsigned)*resp_len);
        *resp_len = 0;
        err = spi_ota_poll_response(resp_type, resp_payload, resp_max, resp_len, poll_timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        log_resp_payload(stage, *resp_type, resp_payload, *resp_len);
        if (*resp_type == SPI_OTA_RSP_WAIT) {
            err = wait_until_not_wait(stage, resp_type, resp_payload, resp_max, resp_len,
                                      poll_timeout_ms, wait_rounds);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    return ESP_OK;
}

/**
 * @brief 解析并校验 GD32 Challenge 响应，提取会话密钥和 UID。
 * @param resp_payload Challenge 响应数据。
 * @param resp_len 响应长度。
 * @param out_k_session 输出会话密钥（16 字节）。
 * @param out_uid 输出 UID（GD32_UID_LEN 字节）。
 * @return 校验通过返回 true，否则返回 false。
 */
static bool parse_and_verify_challenge_payload(const uint8_t *resp_payload, size_t resp_len,
                                               uint8_t *out_k_session, uint8_t *out_uid)
{
    if (!resp_payload || !out_k_session || !out_uid) return false;

    if (resp_len != GD32_CHALLENGE_RESPONSE_LEN) {
        DEBUG_ERR("OTA", "CHALLENGE validate failed: invalid payload length=%u", (unsigned)resp_len);
        return false;
    }

    /*
     * GD32 响应格式 (36 bytes):
     *   [0..11]  = UID (12 bytes)
     *   [12..19] = nonce_server[8:16] (8 bytes, nonce 后半段)
     *   [20..35] = encrypted_Ksession (16 bytes) = AES(K_master, K_session)
     *
     * 其中 K_session = AES(K_master, nonce_server)
     */

    const uint8_t *uid = resp_payload;
    const uint8_t *nonce_frag = resp_payload + 12;
    const uint8_t *encrypted_ksession = resp_payload + 20;

    // 校验 nonce 片段: 应与 nonce_server[8:16] 一致
    if (memcmp(nonce_frag, g_nonce_server + 8, 8) != 0) {
        DEBUG_ERR("OTA", "CHALLENGE校验失败: nonce片段不匹配");
        return false;
    }

    // 派生 K_session = AES(K_master, nonce_server)
    uint8_t K_session[16] = {0};
    if (aes128_ecb_encrypt_block_local(MASTER_KEY, g_nonce_server, K_session) != 0) {
        DEBUG_ERR("OTA", "CHALLENGE校验失败: 派生K_session失败");
        return false;
    }

    // 验证 encrypted_Ksession: AES(K_master, K_session) 应匹配
    uint8_t expected_encrypted[16] = {0};
    if (aes128_ecb_encrypt_block_local(MASTER_KEY, K_session, expected_encrypted) != 0) {
        DEBUG_ERR("OTA", "CHALLENGE校验失败: 加密K_session失败");
        return false;
    }

    if (memcmp(encrypted_ksession, expected_encrypted, 16) != 0) {
        DEBUG_ERR("OTA", "CHALLENGE校验失败: encrypted_Ksession不匹配");
        return false;
    }

    // 校验通过，输出 K_session 和 UID
    memcpy(out_k_session, K_session, 16);
    memcpy(out_uid, uid, GD32_UID_LEN);

    if (bytes_all_value(out_uid, GD32_UID_LEN, 0x00U) ||
        bytes_all_value(out_uid, GD32_UID_LEN, 0xFFU)) {
        DEBUG_ERR("OTA", "CHALLENGE校验失败: UID无效(全00/全FF)");
        return false;
    }

    char uid_hex[GD32_UID_LEN * 2 + 1] = {0};
    for (size_t i = 0; i < GD32_UID_LEN; ++i) {
        snprintf(&uid_hex[i * 2], sizeof(uid_hex) - i * 2, "%02X", out_uid[i]);
    }
    DEBUG_INFO("OTA",
               "GD32 UID(%uB @0x%08" PRIX32 "/0x%08" PRIX32 "/0x%08" PRIX32 "): %s",
               (unsigned)GD32_UID_LEN,
               DEVICE_SERIAL_ADDR_LSB, DEVICE_SERIAL_ADDR_MID, DEVICE_SERIAL_ADDR_MSB,
               uid_hex);

    return true;
}

/**
 * @brief 使用 AES-128 ECB 加密单个 16 字节分组。
 * @param key 16 字节密钥。
 * @param in 输入明文分组（16 字节）。
 * @param out 输出密文分组（16 字节）。
 * @return 0 表示成功，非 0 表示失败。
 */
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

/**
 * @brief 使用 AES-128 ECB 解密单个 16 字节分组。
 * @param key 16 字节密钥。
 * @param in 输入密文分组（16 字节）。
 * @param out 输出明文分组（16 字节）。
 * @return 0 表示成功，非 0 表示失败。
 */
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

/**
 * @brief 按 3 个分组对 48 字节数据执行 AES-128 ECB 加密。
 * @param key 16 字节密钥。
 * @param in 输入明文（48 字节）。
 * @param out 输出密文（48 字节）。
 * @return 全部分组加密成功返回 true，否则返回 false。
 */
static bool aes128_ecb_encrypt_48bytes_local(const uint8_t *key, const uint8_t *in, uint8_t *out)
{
    for (int i = 0; i < 3; ++i) {
        if (aes128_ecb_encrypt_block_local(key, in + i * 16, out + i * 16) != 0) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 计算带 key 前缀的 CRC32（key + 48B 数据），输出小端 4 字节。
 * @param key 16 字节会话密钥。
 * @param data48 输入数据（48 字节）。
 * @param output 输出 CRC32（4 字节）。
 */
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

/**
 * @brief 记录响应类型和负载十六进制摘要，便于 OTA 阶段诊断。
 * @param stage 当前阶段名称。
 * @param resp_type 响应类型。
 * @param payload 响应负载。
 * @param len 响应负载长度。
 */
static void log_resp_payload(const char *stage, uint8_t resp_type, const uint8_t *payload, size_t len)
{
    char hex[193] = {0};
    size_t n = (len > 32U) ? 32U : len;
    for (size_t i = 0; i < n; ++i) {
        snprintf(&hex[i * 2], sizeof(hex) - i * 2, "%02X", payload[i]);
    }
    DEBUG_DBG("OTA", "%s 响应: type=0x%02X len=%u payload[%u]=%s%s",
               stage, resp_type, (unsigned)len, (unsigned)n,
               n > 0 ? hex : "<empty>",
               len > n ? "..." : "");
}

/**
 * @brief 当收到 WAIT 响应时进行只读轮询，直到响应变为非 WAIT 或达到轮询上限。
 * @param stage 当前阶段名称（用于日志）。
 * @param resp_type 输入/输出响应类型。
 * @param resp_payload 输出响应负载缓冲区。
 * @param resp_max 响应缓冲区大小。
 * @param resp_len 输出响应长度指针，可为 NULL。
 * @param timeout_ms 每次轮询的超时毫秒。
 * @param max_wait_rounds 最多轮询次数。
 * @return ESP_OK 或底层 SPI 轮询错误码。
 */
static esp_err_t wait_until_not_wait(const char *stage, uint8_t *resp_type,
                                     uint8_t *resp_payload, size_t resp_max,
                                     size_t *resp_len, uint32_t timeout_ms, uint32_t max_wait_rounds)
{
    if (!resp_type) return ESP_ERR_INVALID_ARG;
    uint32_t round = 0;
    while (*resp_type == SPI_OTA_RSP_WAIT && round < max_wait_rounds) {
        ++round;
        DEBUG_WARN("OTA", "%s 收到 WAIT，开始第 %u 次只读轮询...", stage, round);
        /* GD32 擦除 Flash 期间 (__disable_irq) 无法响应 SPI，
         * ESP32 的 poll 会持续拿到 0xFF 或 WAIT 帧残留字节，
         * 导致 spi_ota_poll_response 超时返回 ESP_ERR_TIMEOUT。
         *
         * 这是正常现象——只要还在 WAIT 轮询范围内，超时不代表协议失败，
         * 只是 GD32 还在忙。继续重试即可。
         *
         * 用 100ms 短超时做每次 poll，避免擦除期间长阻塞。
         * 300 轮 × 100ms = 30s，远超 2-3 秒的 Flash 擦除时间。
         * 实际上擦除完成后 ACK 会立即返回，不会真的等 30s。 */
        esp_err_t err = spi_ota_poll_response(resp_type, resp_payload, resp_max, resp_len, 100);
        if (err == ESP_ERR_TIMEOUT) {
            /* 擦除期间 SPI 无响应 → 保持 WAIT 状态继续轮询 */
            *resp_type = SPI_OTA_RSP_WAIT;
            continue;
        }
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
esp_err_t meta_save(const ota_meta_t *meta)
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
static void request_gd32_app_enter_bootloader(void)
{
    DEBUG_INFO("OTA", "尝试通知 GD32 APP 进入 Bootloader OTA 窗口");
    esp_err_t err = gd32_app_request_bootloader(500);
    if (err == ESP_OK) {
        /* 收到确认：可能来自 APP（即将跳转 BOOT）或 BOOT 本身（已在 BOOT）。
         * 如果来自 APP，GD32 需要复位并重新初始化 SPI 外设，需等 ~1-2s；
         * 如果已在 BOOT，SPI 已经就绪，200ms 足够。
         * 等待 1000ms 给 GD32 足够时间完成系统复位和 Bootloader 初始化。 */
        DEBUG_INFO("OTA", "GD32 已确认进入 Bootloader，等待就绪");
        vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
        DEBUG_WARN("OTA", "GD32 APP 未确认进入 Bootloader，继续尝试直接握手: %s", esp_err_to_name(err));
    }
}

static ota_result_t send_handshake_req(void)
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
    g_relay_state = RELAY_HANDSHAKE_SENT;
    DEBUG_INFO("OTA", "状态: RELAY_HANDSHAKE_SENT");

    for (uint32_t attempt = 0U; attempt < kHandshakeAttempts; ++attempt) {
        uint8_t resp_type = 0U;
        uint8_t resp_payload[64] = {0};
        size_t resp_len = 0U;

        esp_err_t err = spi_ota_transfer(SPI_OTA_CMD_REQ, payload, sizeof(payload),
                                         &resp_type, resp_payload, sizeof(resp_payload), &resp_len,
                                         kHandshakeReqTimeoutMs);
        last_err = err;
        last_resp_type = resp_type;
        last_resp_len = resp_len;

        if (err == ESP_OK) {
            log_resp_payload("HANDSHAKE_REQ", resp_type, resp_payload, resp_len);
            err = poll_until_expected_ack_shape("HANDSHAKE_REQ", &resp_type, resp_payload,
                                                sizeof(resp_payload), &resp_len,
                                                kHandshakePollTimeoutMs, 2, 2, is_handshake_echo_ack);
            last_err = err;
            last_resp_type = resp_type;
            last_resp_len = resp_len;

            if (err == ESP_OK && resp_type == SPI_OTA_RSP_ACK && is_handshake_echo_ack(resp_payload, resp_len)) {
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
    g_relay_state = RELAY_ERROR;
    return OTA_RESULT_ERR_TIMEOUT;
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
    err = poll_until_expected_ack_shape("CHALLENGE", &resp_type, resp_payload,
                                        sizeof(resp_payload), &resp_len,
                                        1000, 12, 8, is_challenge_payload_ack);
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || !is_challenge_payload_ack(resp_payload, resp_len)) {
        DEBUG_ERR("OTA", "CHALLENGE 发送失败, err=%d, resp_type=0x%02X, resp_len=%zu", err, resp_type, resp_len);
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_TIMEOUT;
    }

    g_relay_state = RELAY_CHALLENGE_RESPOND;
    DEBUG_INFO("OTA", "已收到 GD32 握手数据包");
    DEBUG_INFO("OTA", "状态: RELAY_CHALLENGE_RESPOND");

    if (!parse_and_verify_challenge_payload(resp_payload, resp_len, g_k_session, g_gd32_uid_runtime)) {
        DEBUG_ERR("OTA", "CHALLENGE 解包/校验失败");
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
    err = poll_until_expected_ack_shape("CONFIRM", &resp_type, resp_payload,
                                        sizeof(resp_payload), &resp_len,
                                        1000, 8, 8, is_confirm_plain_ack);
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || !is_confirm_plain_ack(resp_payload, resp_len)) {
        DEBUG_ERR("OTA", "SESSION_KEY/CONFIRM 失败, err=%d, resp_type=0x%02X, resp_len=%zu", err, resp_type, resp_len);
        g_relay_state = RELAY_ERROR;
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
    DEBUG_WARN("OTA", "SEND_SIZE poll config: poll_timeout_ms=2000 wait_rounds=300 maxExtraPolls=24");
    err = poll_until_expected_ack_shape("SEND_SIZE", &resp_type, resp_payload,
                                        sizeof(resp_payload), &resp_len,
                                        2000, 300, 24, is_size_seq0_ack);
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || !is_size_seq0_ack(resp_payload, resp_len)) {
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
        uint32_t retry_count = 0;
        if ((seq % OTA_PACKET_HEALTH_CHECK_INTERVAL) == 0 || seq == 0 || seq == total_packets - 1) {
            health_status_t health = health_check_before_ota_relay();
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

        DEBUG_DBG("OTA", "包 %u: chunk=%u if_end=0x%08X crc32=%02X%02X%02X%02X",
                   seq, (unsigned)chunk_size, (unsigned)if_end,
                   tx_buf[60], tx_buf[61], tx_buf[62], tx_buf[63]);

        g_relay_state = RELAY_PACKET_SENT;

        while (1) {
            resp_len = 0;
            err = spi_ota_transfer(SPI_OTA_CMD_DATA, tx_buf, sizeof(tx_buf),
                                   &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
            log_resp_payload("DATA_PACKET", resp_type, resp_payload, resp_len);
            if (err == ESP_OK) {
                err = wait_until_not_wait("DATA_PACKET", &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 1000, 10);
            }
            if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK) {
                DEBUG_WARN("OTA", "数据包 %u 发送失败, err=%d, resp_type=0x%02X, resp_len=%zu", seq, err, resp_type, resp_len);
                result = OTA_RESULT_ERR_TIMEOUT;
                break;
            }

            uint32_t expected_ack = (seq == total_packets - 1) ? 0xFFFFFFFFU : (seq + 1U);

            /* 跳过上一阶段残留的、seq 不匹配的结构化 ACK。
             *
             * 典型场景：SIZE 阶段的 ACK(seq=0) 仍挂在 DMA 上，
             * 包 0 发出后第一次 poll 读到 seq=0 而非期望的 seq=1。
             * 继续轮询直到拿到正确的 ACK 或超时。 */
            if (!is_structured_ack_with_seq(resp_payload, resp_len, expected_ack)) {
                uint32_t err_seq = 0;
                uint8_t err_code = 0;
                if (parse_structured_error(resp_payload, resp_len, &err_seq, &err_code)) {
                    if (err_seq == seq) {
                        DEBUG_WARN("OTA", "包 %u 收到结构化错误 err=0x%02X，准备重发", seq, err_code);
                        g_relay_state = RELAY_PACKET_ACK_RETRY;
                        if (++retry_count >= 3U) {
                            DEBUG_ERR("OTA", "包 %u 重发次数超限", seq);
                            result = (err_code == 0x04U) ? OTA_RESULT_ERR_CRC : OTA_RESULT_ERR_GD32;
                            break;
                        }
                        continue; /* 重发本包 */
                    }
                }

                /* 非期望 ACK，也非错误帧 → 可能是残留 ACK，循环 poll 直到拿到正确的 */
                bool got_expected = false;
                for (uint32_t retry = 0; retry < 5; ++retry) {
                    DEBUG_WARN("OTA", "包 %u 收到非目标ACK(seq不匹配)，继续轮询(%u/5)...", seq, retry + 1);
                    err = spi_ota_poll_response(&resp_type, resp_payload, sizeof(resp_payload), &resp_len, 2000);
                    if (err == ESP_OK && resp_type == SPI_OTA_RSP_WAIT) {
                        err = wait_until_not_wait("DATA_PACKET", &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 1000, 10);
                    }
                    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK) {
                        break;
                    }
                    if (is_structured_ack_with_seq(resp_payload, resp_len, expected_ack)) {
                        got_expected = true;
                        break;
                    }
                    if (parse_structured_error(resp_payload, resp_len, &err_seq, &err_code) && err_seq == seq) {
                        break; /* 下面统一处理 */
                    }
                }

                if (got_expected) {
                    /* 匹配成功 */
                    flash_offset += (uint32_t)chunk_size;
                    g_relay_state = RELAY_PACKET_ACK_OK;
                    break;
                }

                /* 最终检查是否为错误帧 */
                if (parse_structured_error(resp_payload, resp_len, &err_seq, &err_code) && err_seq == seq) {
                    DEBUG_WARN("OTA", "包 %u 收到结构化错误 err=0x%02X，准备重发", seq, err_code);
                    g_relay_state = RELAY_PACKET_ACK_RETRY;
                    if (++retry_count >= 3U) {
                        DEBUG_ERR("OTA", "包 %u 重发次数超限", seq);
                        result = (err_code == 0x04U) ? OTA_RESULT_ERR_CRC : OTA_RESULT_ERR_GD32;
                        break;
                    }
                    continue; /* 重发本包 */
                }

                DEBUG_ERR("OTA", "包 %u ACK/ERROR 不匹配，len=%zu", seq, resp_len);
                result = OTA_RESULT_ERR_GD32;
                break;
            }

            /* 匹配成功 */
            flash_offset += (uint32_t)chunk_size;
            g_relay_state = RELAY_PACKET_ACK_OK;
            break;
        }

        if (result != OTA_RESULT_OK) {
            break;
        }

        if (seq % (total_packets / 10 + 1) == 0) {
            uint32_t progress = (seq * 100U) / total_packets;
            DEBUG_INFO("OTA", "转发进度: %u/%u 包 (%u%%)", seq, total_packets, progress);
        }
    }

    g_relay_state = RELAY_RECEIVE_DONE;
    DEBUG_DBG("OTA", "状态: RELAY_RECEIVE_DONE");
    return result;
}

/**
 * @brief 结束 OTA - 发送 0x0A AC 并清理
 * @return 操作结果码。
 */
static ota_result_t finish_ota(void)
{
    DEBUG_INFO("OTA", "=== 阶段4: 结束 OTA ===");
    DEBUG_INFO("OTA", "发送 FINISH_OTA (0x0A 0xAC)，等待 GD32 确认即将跳转 APP");

    uint8_t resp_type = 0;
    uint8_t resp_payload[32] = {0};
    size_t resp_len = 0;
    uint8_t finish_payload[2] = {0x0A, 0xAC};

    g_relay_state = RELAY_FINISH_SENT;
    esp_err_t err = spi_ota_transfer(SPI_OTA_CMD_DATA, finish_payload, sizeof(finish_payload),
                                     &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
    log_resp_payload("FINISH_OTA", resp_type, resp_payload, resp_len);
    err = poll_until_expected_ack_shape("FINISH_OTA", &resp_type, resp_payload,
                                        sizeof(resp_payload), &resp_len,
                                        1000, 10, 8, is_finish_ack_or_error);
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK ||
        !is_finish_ack_or_error(resp_payload, resp_len)) {
        DEBUG_ERR("OTA", "FINISH_OTA 失败, err=%d, resp_type=0x%02X, resp_len=%zu", err, resp_type, resp_len);
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_GD32;
    }

    /* 检查是否为结构化错误帧（BOOT validateApplication 失败等） */
    uint32_t err_seq = 0;
    uint8_t err_code = 0;
    if (parse_structured_error(resp_payload, resp_len, &err_seq, &err_code)) {
        DEBUG_ERR("OTA", "FINISH_OTA BOOT 返回错误: seq=0x%08lX err=0x%02X (%s)",
                  (unsigned long)err_seq, err_code,
                  err_code == 0x02U ? "FLASH_ERROR/validateApp失败" :
                  err_code == 0x04U ? "INVALID_PACKET" :
                  err_code == 0x03U ? "INVALID_KEY" :
                  "未知错误");
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_GD32;
    }

    /* 正常明文 ACK: 0x0A 0xAC */
    if (!is_finish_plain_ack(resp_payload, resp_len)) {
        DEBUG_ERR("OTA", "FINISH_OTA 响应格式异常: len=%zu payload[0]=0x%02X", resp_len, resp_payload[0]);
        g_relay_state = RELAY_ERROR;
        return OTA_RESULT_ERR_GD32;
    }

    g_relay_state = RELAY_DONE;
    session_key_clear(&g_session_ctx);
    memset(&g_session_ctx, 0, sizeof(session_ctx_t));
    memset(g_k_session, 0, sizeof(g_k_session));
    memset(g_nonce_server, 0, sizeof(g_nonce_server));
    memset(g_gd32_uid_runtime, 0, sizeof(g_gd32_uid_runtime));

    DEBUG_INFO("OTA", "状态: RELAY_DONE");
    DEBUG_INFO("OTA", "=== OTA 完成，等待 GD32 APP 启动 ===");

    meta_clear();

    /* ============================================================
     * 阶段5: APP 存活验证
     *
     * GD32 BOOT 处理完 FINISH_OTA 后会跳转到 APP。
     * APP 启动需要初始化外设和 SPI，大约需要 1-2 秒。
     * 我们以 500ms 间隔反复 ping APP，最多等 15 秒。
     * 收到 APP_ALIVE 回复才认为 OTA 真正成功。
     * ============================================================ */
    g_relay_state = RELAY_APP_VERIFY;
    g_gd32_app_alive = false;

    for (uint32_t attempt = 0; attempt < 30; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(500));
        uint32_t uptime = 0;
        esp_err_t ping_err = gd32_app_ping(1000, &uptime);
        if (ping_err == ESP_OK) {
            g_gd32_app_alive = true;
            DEBUG_INFO("OTA", "GD32 APP 存活确认: uptime=%lu sec", (unsigned long)uptime);
            DEBUG_INFO("OTA", "=== OTA 全流程完成 (APP 已启动) ===");
            break;
        }
        DEBUG_INFO("OTA", "等待 GD32 APP 启动... (%u/30)", attempt + 1);
    }

    if (!g_gd32_app_alive) {
        DEBUG_WARN("OTA", "GD32 APP 存活确认超时，OTA 传输已完成但 APP 未启动");
    }

    return OTA_RESULT_OK;
}

/**
 * @brief 启动完整 OTA 流程的主入口
 * @return 操作结果码。
 */
ota_result_t ota_relay_start_handshake(void)
{
    ota_result_t res;

    request_gd32_app_enter_bootloader();

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

    request_gd32_app_enter_bootloader();

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
    memset(&g_session_ctx, 0, sizeof(session_ctx_t));
    memset(g_k_session, 0, sizeof(g_k_session));
    memset(g_nonce_server, 0, sizeof(g_nonce_server));
    memset(g_gd32_uid_runtime, 0, sizeof(g_gd32_uid_runtime));
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
            ota_relay_reset();
        } else {
            DEBUG_ERR("OTA", "OTA 转发失败，错误码: %d", result);
            ota_relay_reset();
        }
    }
}
