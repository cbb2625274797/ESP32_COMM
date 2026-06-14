/*
 * protocol.h - 帧格式、CRC16、AES 加密解密
 * T2.2, T1.3
 * ESP-IDF 6.0 适配版本 - 使用硬件 AES-128-CBC + 硬件 RNG
 *
 * Protocol 类封装协议层功能。
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include "app_config.h"

// 帧格式: SOF(1B) + CMD(1B) + LEN(2B LE) + PAYLOAD(LEN) + CRC16(2B LE)
#define FRAME_SOF          (0xEB)    // 帧起始标记
#define FRAME_MAX_PAYLOAD  (64)      // 安全最大 payload
#define FRAME_OVERHEAD     (6)       // SOF + CMD + LEN(2) + CRC16(2)

// 安全常量（定义在 app_config.h 中）
inline constexpr size_t MASTER_KEY_HERE_LEN = 16;
inline constexpr size_t SCRAMBLE_CONST_HERE_LEN = 16;
inline constexpr size_t CHIP_UID_LEN = 12;

// AES 会话上下文
typedef struct {
    uint8_t key[16];          // 会话密钥 K_session
    uint8_t iv[16];           // CBC 模式 IV
    int used;               // 会话是否已初始化标志
} session_ctx_t;

/**
 * @brief 协议层类（单例）
 *
 * 封装 CRC16、AES-128-CBC、帧构建/解析、会话管理等功能。
 * 大部分方法为静态工具方法，单例仅持有初始化标志。
 */
class Protocol {
public:
    static Protocol& instance();

    void init();

    static void scrambleGenerate(uint32_t timestamp, uint32_t mem_addr,
                                 uint32_t rand_seed, uint8_t *nonce_out);
    static int sessionKeyInit(session_ctx_t *sess, const uint8_t *k_session);
    static void sessionKeyClear(session_ctx_t *sess);

private:
    Protocol() = default;
    ~Protocol() = default;
    Protocol(const Protocol&) = delete;
    Protocol& operator=(const Protocol&) = delete;

    bool m_initialized = false;
};

#endif // PROTOCOL_H