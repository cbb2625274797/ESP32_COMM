/*
 * protocol.cpp - OTA 握手辅助实现
 *
 * 当前保留的接口：
 *   - init              ：硬件 RNG 填充扰码常数
 *   - scrambleGenerate  ：server nonce 生成
 *   - sessionKeyInit    ：会话密钥写入
 *   - sessionKeyClear   ：会话密钥安全擦除
 *
 * 帧构建、AES-ECB/CBC 加解密、CRC16 等中间件代码已迁出（见 ota_relay / spi_link_v2）。
 */
#include <string.h>
#include "esp_random.h"
#include "app_config.h"
#include "protocol.h"
#include "monitor/debug_log.h"

// ============================================================
// Protocol 单例
// ============================================================

/**
 * @brief 获取 Protocol 单例实例（Meyers 单例）。
 * @return Protocol 单例引用。
 */
Protocol& Protocol::instance()
{
    static Protocol inst;
    return inst;
}

/**
 * @brief 抗优化密钥/敏感缓冲擦除。
 *
 * 普通 memset 可能被编译器当作「无后续读取」而消除；
 * 通过 volatile 逐字节写 0，保证运行时真正清零。
 *
 * @param p 起始地址；nullptr 忽略。
 * @param n 字节数。
 */
void Protocol::secureWipe(void *p, size_t n)
{
    if (p == nullptr || n == 0U) {
        return;
    }
    volatile uint8_t *v = static_cast<volatile uint8_t *>(p);
    while (n > 0U) {
        *v++ = 0;
        --n;
    }
}

/**
 * @brief 初始化协议层，打印初始化信息并设置初始化标志。
 *
 * 使用硬件 RNG 填充 m_scrambleConst。完整常数不再以 INFO 打 hex，
 * 避免 UDP/串口日志侧信道；需要核对时开 DEBUG_DBG。
 */
void Protocol::init()
{
    if (m_initialized) {
        return;
    }

    DEBUG_INFO("PROT", "=== 初始化协议层（nonce 扰码 + 会话密钥管理）===");

    esp_fill_random(m_scrambleConst, SCRAMBLE_CONST_LEN);

    /* 不把完整扰码常数打到 INFO/UDP：减小日志侧信道；需要调试时开 DBG。 */
    DEBUG_DBG("PROT", "SCRAMBLE_CONST ready (%u B from HW RNG)",
              (unsigned)SCRAMBLE_CONST_LEN);

    m_initialized = true;
}

/**
 * @brief 根据时间戳、地址和随机种子生成 16 字节 nonce。
 * @param timestamp 当前时间戳（tick）。
 * @param mem_addr 内存地址（增加随机性）。
 * @param rand_seed 随机种子。
 * @param nonce_out 输出 16 字节 nonce 缓冲区；为 nullptr 时直接返回。
 *
 * 混合步骤概要：
 *   1) state = timestamp|mem_addr|rand_seed|scrambleConst[0..3]（小端）
 *   2) 3 轮：逐字节 XOR 全表 scrambleConst + 循环左移 1
 *   3) 2 轮：按 32-bit 字 XOR 完整 4B scrambleConst（memcpy 避免未对齐访问）
 *
 * @note 非标准 KDF；仅 OTA 握手用。未 init 时自动 init()。
 */
void Protocol::scrambleGenerate(uint32_t timestamp, uint32_t mem_addr,
                                uint32_t rand_seed, uint8_t *nonce_out)
{
    if (nonce_out == nullptr) {
        return;
    }
    if (!m_initialized) {
        init();
    }

    uint8_t state[16];

    /* 宿主小端布局；ESP32 / GD32 均为 LE，与对端一致。 */
    memcpy(&state[0], &timestamp, 4);
    memcpy(&state[4], &mem_addr, 4);
    memcpy(&state[8], &rand_seed, 4);
    memcpy(&state[12], m_scrambleConst, 4);

    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 16; i++) {
            state[i] ^= m_scrambleConst[i];
            state[i] = (uint8_t)((state[i] << 1) | (state[i] >> 7));
        }
    }

    /* 按 32-bit 字再混一轮完整 4B 扰码（memcpy 避免未对齐 uint32_t*）。 */
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 4; i++) {
            uint32_t w = 0;
            memcpy(&w, &state[i * 4], 4);
            const uint32_t mix =
                (uint32_t)m_scrambleConst[i * 4]
                | ((uint32_t)m_scrambleConst[i * 4 + 1] << 8)
                | ((uint32_t)m_scrambleConst[i * 4 + 2] << 16)
                | ((uint32_t)m_scrambleConst[i * 4 + 3] << 24);
            w ^= mix;
            memcpy(&state[i * 4], &w, 4);
        }
    }

    memcpy(nonce_out, state, 16);
}

/**
 * @brief 会话密钥初始化。
 * @param sess 会话上下文指针。
 * @param k_session 16 字节会话密钥。
 * @return 0 成功，-1 参数无效。
 *
 * 先 secureWipe 再拷贝，避免旧 key 残留。
 */
int Protocol::sessionKeyInit(session_ctx_t *sess, const uint8_t *k_session)
{
    if (sess == nullptr || k_session == nullptr) {
        return -1;
    }

    secureWipe(sess, sizeof(*sess));
    memcpy(sess->key, k_session, SESSION_KEY_LEN);
    return 0;
}

/**
 * @brief 清除会话上下文中的密钥数据。
 * @param sess 会话上下文指针；为 nullptr 时忽略。
 *
 * 使用 secureWipe，避免编译器优化掉「无读回的」密钥清零。
 */
void Protocol::sessionKeyClear(session_ctx_t *sess)
{
    if (sess == nullptr) {
        return;
    }
    secureWipe(sess->key, SESSION_KEY_LEN);
}
