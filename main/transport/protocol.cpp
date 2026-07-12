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
/**
  * @brief 获取 Protocol 单例实例。
 * @return Protocol 单例引用。
 */
Protocol& Protocol::instance()
{
    static Protocol inst;
    return inst;
}

/**
  * @brief 初始化协议层，打印初始化信息并设置初始化标志。
 */
void Protocol::init()
{
    if (m_initialized) return;

    DEBUG_INFO("PROT", "=== 初始化协议层 ===");
    DEBUG_INFO("PROT", "协议层功能: nonce 扰码 + 会话密钥管理");

    esp_fill_random(m_scrambleConst, SCRAMBLE_CONST_LEN);

    {
        char hex[SCRAMBLE_CONST_LEN * 2 + 1] = {0};
        for (size_t i = 0; i < SCRAMBLE_CONST_LEN; ++i)
            snprintf(&hex[i * 2], sizeof(hex) - i * 2, "%02X", m_scrambleConst[i]);
        DEBUG_INFO("PROT", "SCRAMBLE_CONST (RNG): %s", hex);
    }

    m_initialized = true;
}

/**
 * @brief 根据时间戳、地址和随机种子生成 16 字节 nonce。
 * @param timestamp 当前时间戳（tick）。
 * @param mem_addr 内存地址（增加随机性）。
 * @param rand_seed 随机种子。
 * @param nonce_out 输出 16 字节 nonce 缓冲区。
 */
void Protocol::scrambleGenerate(uint32_t timestamp, uint32_t mem_addr,
                                uint32_t rand_seed, uint8_t *nonce_out)
{
    uint8_t state[16];

    memcpy(&state[0], &timestamp, 4);
    memcpy(&state[4], &mem_addr, 4);
    memcpy(&state[8], &rand_seed, 4);
    memcpy(&state[12], m_scrambleConst, 4);

    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 16; i++) {
            state[i] ^= m_scrambleConst[i];
            state[i] = (state[i] << 1) | (state[i] >> 7);
        }
    }

    for (int round = 0; round < 2; round++) {
        uint32_t *words = (uint32_t *)state;
        for (int i = 0; i < 4; i++) {
            words[i] ^= m_scrambleConst[i * 4 % 16] | ((uint32_t)m_scrambleConst[i * 4 + 1] << 8);
        }
    }

    memcpy(nonce_out, state, 16);
}

/**
 * @brief 会话密钥初始化。
 * @param sess 会话上下文指针。
 * @param k_session 16 字节会话密钥。
 * @return 0 成功，-1 参数无效。
 */
int Protocol::sessionKeyInit(session_ctx_t *sess, const uint8_t *k_session)
{
    if (!sess || !k_session) return -1;

    memset(sess, 0, sizeof(session_ctx_t));
    memcpy(sess->key, k_session, 16);

    return 0;
}

/**
 * @brief 清除会话上下文中的密钥数据。
 * @param sess 会话上下文指针。
 */
void Protocol::sessionKeyClear(session_ctx_t *sess)
{
    if (sess) {
        memset(sess->key, 0, 16);
    }
}
