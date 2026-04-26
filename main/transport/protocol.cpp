/*
 * protocol.c - 协议层实现
 * T2.1, T2.2, T1.3
 * ESP-IDF 6.0 适配版本 - 使用软件 AES 实现 + 硬件 RNG
 */
#include <string.h>
#include "esp_random.h"
#include "app_config.h"
#include "protocol.h"
#include "debug_log.h"

static bool s_protocol_initialized = false;

// ========== 软件 AES-128 实现 ==========

// AES S-Box
static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

// AES 逆 S-Box
static const uint8_t rsbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

// AES Rcon
static const uint8_t Rcon[11] = {
    0x8d,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

// AES 状态操作
#define AES_BLOCK_SIZE 16

typedef struct {
    uint8_t round_key[176];  // 10 rounds * 16 bytes + initial key
} aes128_context_t;

// GF(2^8) 乘法
static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0, hbit;
    for (int i = 0; i < 8; i++) {
        if (b & 1) {
            p ^= a;
        }
        hbit = a & 0x80;
        a <<= 1;
        if (hbit) {
            a ^= 0x1b;  // 模不可约多项式 x^8 + x^4 + x^3 + x + 1
        }
        b >>= 1;
    }
    return p;
}

// 密钥扩展
static void aes128_key_expansion(const uint8_t *key, aes128_context_t *ctx)
{
    uint8_t temp[4];
    memcpy(ctx->round_key, key, 16);

    for (int i = 4; i < 44; i++) {
        for (int j = 0; j < 4; j++) {
            temp[j] = ctx->round_key[(i - 1) * 4 + j];
        }

        if (i % 4 == 0) {
            // RotWord
            uint8_t t = temp[0];
            temp[0] = temp[1];
            temp[1] = temp[2];
            temp[2] = temp[3];
            temp[3] = t;

            // SubWord
            temp[0] = sbox[temp[0]];
            temp[1] = sbox[temp[1]];
            temp[2] = sbox[temp[2]];
            temp[3] = sbox[temp[3]];

            temp[0] ^= Rcon[i / 4];
        }

        for (int j = 0; j < 4; j++) {
            ctx->round_key[i * 4 + j] = ctx->round_key[(i - 4) * 4 + j] ^ temp[j];
        }
    }
}

// 字节替换
static void sub_bytes(uint8_t *state)
{
    for (int i = 0; i < 16; i++) {
        state[i] = sbox[state[i]];
    }
}

// 逆字节替换
static void inv_sub_bytes(uint8_t *state)
{
    for (int i = 0; i < 16; i++) {
        state[i] = rsbox[state[i]];
    }
}

// 行移位
static void shift_rows(uint8_t *state)
{
    uint8_t temp;

    // 第1行左移1位
    temp = state[1];
    state[1] = state[5];
    state[5] = state[9];
    state[9] = state[13];
    state[13] = temp;

    // 第2行左移2位
    temp = state[2];
    state[2] = state[10];
    state[10] = temp;
    temp = state[6];
    state[6] = state[14];
    state[14] = temp;

    // 第3行左移3位
    temp = state[15];
    state[15] = state[11];
    state[11] = state[7];
    state[7] = state[3];
    state[3] = temp;
}

// 逆行移位
static void inv_shift_rows(uint8_t *state)
{
    uint8_t temp;

    // 第1行右移1位
    temp = state[13];
    state[13] = state[9];
    state[9] = state[5];
    state[5] = state[1];
    state[1] = temp;

    // 第2行右移2位
    temp = state[2];
    state[2] = state[10];
    state[10] = temp;
    temp = state[6];
    state[6] = state[14];
    state[14] = temp;

    // 第3行右移3位
    temp = state[3];
    state[3] = state[7];
    state[7] = state[11];
    state[11] = state[15];
    state[15] = temp;
}

// 列混合
static void mix_columns(uint8_t *state)
{
    uint8_t temp[16];
    memcpy(temp, state, 16);

    for (int i = 0; i < 4; i++) {
        int c = i * 4;
        state[c]     = gmul(2, temp[c]) ^ gmul(3, temp[c + 1]) ^ temp[c + 2] ^ temp[c + 3];
        state[c + 1] = temp[c] ^ gmul(2, temp[c + 1]) ^ gmul(3, temp[c + 2]) ^ temp[c + 3];
        state[c + 2] = temp[c] ^ temp[c + 1] ^ gmul(2, temp[c + 2]) ^ gmul(3, temp[c + 3]);
        state[c + 3] = gmul(3, temp[c]) ^ temp[c + 1] ^ temp[c + 2] ^ gmul(2, temp[c + 3]);
    }
}

// 逆列混合
static void inv_mix_columns(uint8_t *state)
{
    uint8_t temp[16];
    memcpy(temp, state, 16);

    for (int i = 0; i < 4; i++) {
        int c = i * 4;
        state[c]     = gmul(14, temp[c]) ^ gmul(11, temp[c + 1]) ^ gmul(13, temp[c + 2]) ^ gmul(9, temp[c + 3]);
        state[c + 1] = gmul(9, temp[c]) ^ gmul(14, temp[c + 1]) ^ gmul(11, temp[c + 2]) ^ gmul(13, temp[c + 3]);
        state[c + 2] = gmul(13, temp[c]) ^ gmul(9, temp[c + 1]) ^ gmul(14, temp[c + 2]) ^ gmul(11, temp[c + 3]);
        state[c + 3] = gmul(11, temp[c]) ^ gmul(13, temp[c + 1]) ^ gmul(9, temp[c + 2]) ^ gmul(14, temp[c + 3]);
    }
}

// 轮密钥加
static void add_round_key(uint8_t *state, const uint8_t *round_key)
{
    for (int i = 0; i < 16; i++) {
        state[i] ^= round_key[i];
    }
}

// AES-128 加密单个块
static void aes128_encrypt_block(const uint8_t *input, uint8_t *output, const aes128_context_t *ctx)
{
    uint8_t state[16];

    // 复制输入到状态（列优先顺序）
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            state[i * 4 + j] = input[j * 4 + i];
        }
    }

    // 初始轮密钥加
    add_round_key(state, ctx->round_key);

    // 9轮加密
    for (int round = 1; round < 10; round++) {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, ctx->round_key + round * 16);
    }

    // 最后一轮（无列混合）
    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, ctx->round_key + 10 * 16);

    // 复制状态到输出（列优先顺序）
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            output[j * 4 + i] = state[i * 4 + j];
        }
    }
}

// AES-128 解密单个块
static void aes128_decrypt_block(const uint8_t *input, uint8_t *output, const aes128_context_t *ctx)
{
    uint8_t state[16];

    // 复制输入到状态（列优先顺序）
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            state[i * 4 + j] = input[j * 4 + i];
        }
    }

    // 初始轮密钥加
    add_round_key(state, ctx->round_key + 10 * 16);

    // 9轮解密
    for (int round = 9; round >= 1; round--) {
        inv_shift_rows(state);
        inv_sub_bytes(state);
        add_round_key(state, ctx->round_key + round * 16);
        inv_mix_columns(state);
    }

    // 最后一轮（无逆列混合）
    inv_shift_rows(state);
    inv_sub_bytes(state);
    add_round_key(state, ctx->round_key);

    // 复制状态到输出（列优先顺序）
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            output[j * 4 + i] = state[i * 4 + j];
        }
    }
}

// CBC 模式加密
static void aes128_cbc_encrypt(const uint8_t *input, uint8_t *output, size_t length,
                               const uint8_t *iv, const aes128_context_t *ctx)
{
    uint8_t prev[16];
    memcpy(prev, iv, 16);

    for (size_t i = 0; i < length; i += 16) {
        uint8_t block[16];
        memcpy(block, input + i, 16);

        // XOR with previous ciphertext (or IV for first block)
        for (int j = 0; j < 16; j++) {
            block[j] ^= prev[j];
        }

        aes128_encrypt_block(block, output + i, ctx);
        memcpy(prev, output + i, 16);
    }
}

// CBC 模式解密
static void aes128_cbc_decrypt(const uint8_t *input, uint8_t *output, size_t length,
                               const uint8_t *iv, const aes128_context_t *ctx)
{
    uint8_t prev[16];
    memcpy(prev, iv, 16);

    for (size_t i = 0; i < length; i += 16) {
        aes128_decrypt_block(input + i, output + i, ctx);

        // XOR with previous ciphertext (or IV for first block)
        for (int j = 0; j < 16; j++) {
            output[i + j] ^= prev[j];
        }

        memcpy(prev, input + i, 16);
    }
}

void protocol_init(void)
{
    if (s_protocol_initialized) return;

    DEBUG_INFO("PROT", "初始化协议层（CRC16、软件AES）...");

    s_protocol_initialized = true;
    DEBUG_INFO("PROT", "协议层就绪");
}

// CRC16 Modbus（LE，低字节在前）
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

// 构建 SPI 帧: SOF + CMD + LEN(2LE) + PAYLOAD + CRC16(2LE)
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

    return idx;
}

// 解析 SPI 帧
int frame_parse(const uint8_t *frame, size_t frame_len,
                uint8_t *out_cmd, uint8_t *out_payload, size_t *out_len)
{
    if (frame_len < 6) {
        return -1;
    }

    size_t idx = 0;

    if (frame[idx++] != FRAME_SOF) {
        DEBUG_WARN("PROT", "无效 SOF: 0x%02X", frame[0]);
        return -1;
    }

    uint8_t cmd = frame[idx++];

    size_t payload_len = frame[idx] | (frame[idx + 1] << 8);
    idx += 2;

    if (payload_len > FRAME_MAX_PAYLOAD) {
        DEBUG_ERR("PROT", "Payload 长度太大: %zu", payload_len);
        return -1;
    }

    size_t expected_len = 1 + 1 + 2 + payload_len + 2;
    if (frame_len < expected_len) {
        DEBUG_WARN("PROT", "帧太短: %zu < %zu", frame_len, expected_len);
        return -1;
    }

    uint16_t received_crc = frame[idx + payload_len] | (frame[idx + payload_len + 1] << 8);
    uint16_t calc_crc = crc16_modbus(frame, idx + payload_len);

    if (received_crc != calc_crc) {
        DEBUG_WARN("PROT", "CRC 不匹配: 0x%04X vs 0x%04X", received_crc, calc_crc);
        return -1;
    }

    if (out_payload && out_len) {
        memcpy(out_payload, &frame[idx], payload_len);
        *out_len = payload_len;
    }
    if (out_cmd) {
        *out_cmd = cmd;
    }

    return 0;
}

// AES-128 CBC 加密
// 注意: 函数名带 protocol_ 前缀以避免与 ESP-IDF wpa_supplicant 组件冲突
int protocol_aes_encrypt(const uint8_t *plaintext, size_t len,
                         const uint8_t *key, const uint8_t *iv,
                         uint8_t *ciphertext_out)
{
    size_t padded_len = ((len + 15) / 16) * 16;
    uint8_t padded[256];

    memcpy(padded, plaintext, len);
    uint8_t pad = (uint8_t)(16 - (len % 16));
    memset(&padded[len], pad, pad);

    aes128_context_t ctx;
    aes128_key_expansion(key, &ctx);
    aes128_cbc_encrypt(padded, ciphertext_out, padded_len, iv, &ctx);

    return padded_len;
}

// AES-128 CBC 解密
// 注意: 函数名带 protocol_ 前缀以避免与 ESP-IDF wpa_supplicant 组件冲突
int protocol_aes_decrypt(const uint8_t *ciphertext, size_t len,
                         const uint8_t *key, const uint8_t *iv,
                         uint8_t *plaintext_out)
{
    if (len % 16 != 0) {
        DEBUG_ERR("PROT", "Ciphertext 长度不是 16 的倍数: %zu", len);
        return -1;
    }

    aes128_context_t ctx;
    aes128_key_expansion(key, &ctx);
    aes128_cbc_decrypt(ciphertext, plaintext_out, len, iv, &ctx);

    // 移除 PKCS#7 填充
    uint8_t pad = plaintext_out[len - 1];
    if (pad > 16) return -1;
    return len - pad;
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

// 会话密钥初始化
int session_key_init(session_ctx_t *sess, const uint8_t *k_session)
{
    if (!sess || !k_session) return -1;

    memset(sess, 0, sizeof(session_ctx_t));
    memcpy(sess->key, k_session, 16);
    memset(sess->iv, 0, 16);
    sess->used = 0;

    return 0;
}

void session_key_clear(session_ctx_t *sess)
{
    if (sess) {
        memset(sess->key, 0, 16);
        memset(sess->iv, 0, 16);
        sess->used = 0;
    }
}

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

// 使用 ESP-IDF 硬件 RNG 生成随机数
void generate_random_nonce(uint8_t *nonce, size_t len)
{
    esp_fill_random(nonce, len);
}