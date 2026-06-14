/*
 * protocol.cpp - 协议层实现
 * T2.1, T2.2, T1.3
 * ESP-IDF 6.0 适配版本 - 使用硬件 AES-128-CBC + 硬件 RNG
 *
 * 加密实现使用 ESP32-C3 硬件 AES 加速器（通过 mbedtls port）
 *
 * 当前保留的接口：init、scrambleGenerate（nonce 生成）、
 * sessionKeyInit / sessionKeyClear（会话密钥生命周期管理）。
 * 帧构建、AES-CBC 加解密、CRC16 等中间件代码已移除。
 */
#include <string.h>
#include <stdio.h>
#include "esp_random.h"
#include "aes/esp_aes.h"
#include "app_config.h"
#include "protocol.h"
#include "monitor/debug_log.h"

// ============================================================
// Protocol 单例
// ============================================================
Protocol& Protocol::instance()
{
    static Protocol inst;
    return inst;
}

void Protocol::init()
{
    if (m_initialized) return;

    DEBUG_INFO("PROT", "=== 初始化协议层 ===");
    DEBUG_INFO("PROT", "协议层功能: nonce 扰码 + 会话密钥管理");

    m_initialized = true;
    DEBUG_INFO("PROT", "协议层就绪");
}

/**
 * @brief 根据时间戳、地址和随机种子生成 16 字节 nonce。
 */
void Protocol::scrambleGenerate(uint32_t timestamp, uint32_t mem_addr,
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
 */
int Protocol::sessionKeyInit(session_ctx_t *sess, const uint8_t *k_session)
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
 */
void Protocol::sessionKeyClear(session_ctx_t *sess)
{
    if (sess) {
        memset(sess->key, 0, 16);
        memset(sess->iv, 0, 16);
        sess->used = 0;
    }
}