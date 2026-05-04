/*
 * protocol.c - 协议层实现
 * T2.1, T2.2, T1.3
 * ESP-IDF 6.0 适配版本 - 使用硬件 AES-128-CBC + 硬件 RNG
 *
 * 加密实现使用 ESP32-C3 硬件 AES 加速器（通过 mbedtls port）
 */
#include <string.h>
#include <stdio.h>
#include "esp_random.h"
#include "aes/esp_aes.h"
#include "app_config.h"
#include "protocol.h"
#include "monitor/debug_log.h"

/** @brief 协议层初始化标志。 */
static bool s_protocol_initialized = false;

/**
  * @brief 初始化协议层，准备 CRC 和 AES 模块。
 */
void protocol_init(void)
{
    if (s_protocol_initialized) return;

    DEBUG_INFO("PROT", "=== 初始化协议层 ===");
    DEBUG_INFO("PROT", "协议层功能: CRC16 Modbus + 硬件 AES-128-CBC");

    /** @brief 协议层初始化标志。 */
    s_protocol_initialized = true;
    DEBUG_INFO("PROT", "协议层就绪");
}

/**
 * @brief CRC16 Modbus（LE，低字节在前）
 * @param data 待校验的数据指针。
 * @param len 数据长度（字节）。
 * @return CRC16 校验值。
 */
uint16_t crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return (crc >> 8) | (crc << 8);
}

/**
 * @brief 构建 SPI 帧: SOF + CMD + LEN(2LE) + PAYLOAD + CRC16(2LE)
 * @param cmd 命令码。
 * @param payload 负载数据指针，可为 NULL。
 * @param len 负载长度（字节）。
 * @param out_frame 输出帧缓冲区。
 * @return 帧总长度，失败返回 -1。
 */
int frame_build(uint8_t cmd, const uint8_t *payload, size_t len, uint8_t *out_frame)
{
    if (len > FRAME_MAX_PAYLOAD) {
        DEBUG_ERR("PROT", "Payload 太大: %zu > %d", len, FRAME_MAX_PAYLOAD);
        return -1;
    }

    size_t idx = 0;
    out_frame[idx++] = FRAME_SOF;
    out_frame[idx++] = cmd;
    out_frame[idx++] = (uint8_t)(len & 0xFF);
    out_frame[idx++] = (uint8_t)((len >> 8) & 0xFF);

    if (payload && len > 0) {
        memcpy(&out_frame[idx], payload, len);
        idx += len;
    }

    uint16_t crc = crc16_modbus(out_frame, idx);
    out_frame[idx++] = (uint8_t)(crc & 0xFF);
    out_frame[idx++] = (uint8_t)((crc >> 8) & 0xFF);

    DEBUG_DBG("PROT", "帧构建: cmd=0x%02X, len=%zu, crc=0x%04X", cmd, len, crc);

    return idx;
}

// 解析 SPI 帧
int frame_parse(const uint8_t *frame, size_t frame_len,
                uint8_t *out_cmd, uint8_t *out_payload, size_t *out_len)
{
    if (frame_len < 6) {
        DEBUG_WARN("PROT", "帧太短: %zu < 6", frame_len);
        return -1;
    }

    size_t idx = 0;

    if (frame[idx++] != FRAME_SOF) {
        DEBUG_WARN("PROT", "无效 SOF: 0x%02X (期望 0x%02X)", frame[0], FRAME_SOF);
        return -1;
    }

    uint8_t cmd = frame[idx++];

    size_t payload_len = frame[idx] | (frame[idx + 1] << 8);
    idx += 2;

    if (payload_len > FRAME_MAX_PAYLOAD) {
        DEBUG_ERR("PROT", "Payload 长度太大: %zu > %d", payload_len, FRAME_MAX_PAYLOAD);
        return -1;
    }

    size_t expected_len = 1 + 1 + 2 + payload_len + 2;
    if (frame_len < expected_len) {
        DEBUG_WARN("PROT", "帧太短: %zu < %zu (期望)", frame_len, expected_len);
        return -1;
    }

    uint16_t received_crc = frame[idx + payload_len] | (frame[idx + payload_len + 1] << 8);
    uint16_t calc_crc = crc16_modbus(frame, idx + payload_len);

    if (received_crc != calc_crc) {
        DEBUG_WARN("PROT", "CRC 不匹配: 收到=0x%04X, 计算=0x%04X", received_crc, calc_crc);
        return -1;
    }

    DEBUG_DBG("PROT", "帧解析: cmd=0x%02X, len=%zu, crc=0x%04X OK", cmd, payload_len, calc_crc);

    if (out_payload && out_len) {
        memcpy(out_payload, &frame[idx], payload_len);
        *out_len = payload_len;
    }
    if (out_cmd) {
        *out_cmd = cmd;
    }

    return 0;
}

// AES-128 CBC 加密（使用 ESP32-C3 硬件 AES）
int protocol_aes_encrypt(const uint8_t *plaintext, size_t len,
                         const uint8_t *key, const uint8_t *iv,
                         uint8_t *ciphertext_out)
{
    if (!plaintext || !key || !iv || !ciphertext_out) {
        DEBUG_ERR("PROT", "AES 加密参数无效");
        return -1;
    }

    // PKCS#7 填充
    size_t padded_len = ((len + 15) / 16) * 16;
    uint8_t padded[256];
    memset(padded, 0, sizeof(padded));
    memcpy(padded, plaintext, len);
    uint8_t pad = (uint8_t)(16 - (len % 16));
    if (pad == 0) pad = 16;
    memset(&padded[len], pad, pad);

    DEBUG_DBG("PROT", "AES 加密: 明文=%zu 字节, 填充后=%zu 字节, 填充值=0x%02X", len, padded_len, pad);

    // 硬件 AES-CBC 加密（使用 ESP32-C3 硬件 AES 加速器）
    esp_aes_context aes_ctx;
    esp_aes_init(&aes_ctx);

    int ret = esp_aes_setkey(&aes_ctx, key, 128);
    if (ret != 0) {
        DEBUG_ERR("PROT", "esp_aes_setkey 失败: %d", ret);
        esp_aes_free(&aes_ctx);
        return -1;
    }

    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);

    ret = esp_aes_crypt_cbc(&aes_ctx, ESP_AES_ENCRYPT, padded_len, iv_copy, padded, ciphertext_out);

    esp_aes_free(&aes_ctx);

    if (ret == 0) {
        DEBUG_DBG("PROT", "AES 加密成功: %zu -> %zu 字节", len, padded_len);
        return padded_len;
    } else {
        DEBUG_ERR("PROT", "esp_aes_crypt_cbc 加密失败: %d", ret);
        return -1;
    }
}

// AES-128 CBC 解密（使用 ESP32-C3 硬件 AES）
int protocol_aes_decrypt(const uint8_t *ciphertext, size_t len,
                         const uint8_t *key, const uint8_t *iv,
                         uint8_t *plaintext_out)
{
    if (!ciphertext || !key || !iv || !plaintext_out) {
        DEBUG_ERR("PROT", "AES 解密参数无效");
        return -1;
    }

    if (len % 16 != 0 || len == 0) {
        DEBUG_ERR("PROT", "Ciphertext 长度无效: %zu (必须为 16 的倍数)", len);
        return -1;
    }

    DEBUG_DBG("PROT", "AES 解密: 密文=%zu 字节", len);

    // 硬件 AES-CBC 解密（使用 ESP32-C3 硬件 AES 加速器）
    esp_aes_context aes_ctx;
    esp_aes_init(&aes_ctx);

    int ret = esp_aes_setkey(&aes_ctx, key, 128);
    if (ret != 0) {
        DEBUG_ERR("PROT", "esp_aes_setkey 失败: %d", ret);
        esp_aes_free(&aes_ctx);
        return -1;
    }

    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);

    ret = esp_aes_crypt_cbc(&aes_ctx, ESP_AES_DECRYPT, len, iv_copy, ciphertext, plaintext_out);

    esp_aes_free(&aes_ctx);

    if (ret != 0) {
        DEBUG_ERR("PROT", "esp_aes_crypt_cbc 解密失败: %d", ret);
        return -1;
    }

    // 移除 PKCS#7 填充
    uint8_t pad = plaintext_out[len - 1];
    if (pad > 16) {
        DEBUG_ERR("PROT", "无效填充: %d", pad);
        return -1;
    }

    size_t orig_len = len - pad;
    DEBUG_DBG("PROT", "AES 解密成功: %zu -> %zu 字节 (填充=%d)", len, orig_len, pad);

    return orig_len;
}

// 搅伴算法 - 生成随机数
void scramble_generate(uint32_t timestamp, uint32_t mem_addr,
                      uint32_t rand_seed, uint8_t *nonce_out)
{
    uint8_t state[16];

    memcpy(&state[0], &timestamp, 4);
    memcpy(&state[4], &mem_addr, 4);
    memcpy(&state[8], &rand_seed, 4);
    memcpy(&state[12], SCRAMBLE_CONST, 4);

    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 16; i++) {
            state[i] ^= SCRAMBLE_CONST[i];
            state[i] = (state[i] << 1) | (state[i] >> 7);
        }
    }

    for (int round = 0; round < 2; round++) {
        uint32_t *words = (uint32_t *)state;
        for (int i = 0; i < 4; i++) {
            words[i] ^= SCRAMBLE_CONST[i * 4 % 16] | ((uint32_t)SCRAMBLE_CONST[i * 4 + 1] << 8);
        }
    }

    memcpy(nonce_out, state, 16);
}

/**
 * @brief 会话密钥初始化
 * @param sess 待初始化的会话上下文。
 * @param k_session 128 位会话密钥。
 * @return 0 成功，-1 失败。
 */
int session_key_init(session_ctx_t *sess, const uint8_t *k_session)
{
    if (!sess || !k_session) return -1;

    memset(sess, 0, sizeof(session_ctx_t));
    memcpy(sess->key, k_session, 16);
    memset(sess->iv, 0, 16);
    sess->used = 0;

    return 0;
}

/**
  * @brief 清除会话上下文中的密钥和状态。
 * @param sess 待清除的会话上下文。
 */
void session_key_clear(session_ctx_t *sess)
{
    if (sess) {
        memset(sess->key, 0, 16);
        memset(sess->iv, 0, 16);
        sess->used = 0;
    }
}

/**
  * @brief 使用会话上下文加密数据。
 * @param sess 会话上下文。
 * @param in 输入明文。
 * @param len 数据长度。
 * @param out 输出密文缓冲区。
 * @return 密文长度，失败返回 -1。
 */
int session_encrypt(session_ctx_t *sess, const uint8_t *in, size_t len, uint8_t *out)
{
    if (!sess || !in || !out) return -1;

    uint8_t iv_copy[16];
    memcpy(iv_copy, sess->iv, 16);

    int ret = protocol_aes_encrypt(in, len, sess->key, iv_copy, out);

    if (ret > 0) {
        memcpy(sess->iv, &out[ret - 16], 16);
    }

    return ret;
}

/**
  * @brief 使用会话上下文解密数据。
 * @param sess 会话上下文。
 * @param in 输入密文。
 * @param len 数据长度。
 * @param out 输出明文缓冲区。
 * @return 明文长度，失败返回 -1。
 */
int session_decrypt(session_ctx_t *sess, const uint8_t *in, size_t len, uint8_t *out)
{
    if (!sess || !in || !out) return -1;

    uint8_t iv_copy[16];
    memcpy(iv_copy, sess->iv, 16);

    int ret = protocol_aes_decrypt(in, len, sess->key, iv_copy, out);

    if (ret > 0) {
        memcpy(sess->iv, &in[len - 16], 16);
    }

    return ret;
}

/**
 * @brief 使用 ESP-IDF 硬件 RNG 生成随机数
 * @param nonce 输出缓冲区。
 * @param len 随机数字节数。
 */
void generate_random_nonce(uint8_t *nonce, size_t len)
{
    esp_fill_random(nonce, len);
}