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

// 安全常量（定义在 app_config.h 中）
inline constexpr size_t MASTER_KEY_HERE_LEN = 16; ///< AES-128 主密钥长度（字节）
inline constexpr size_t CHIP_UID_LEN = 12;        ///< GD32 芯片 UID 长度（字节）

// AES 会话上下文
typedef struct {
    uint8_t key[16];          // 会话密钥 K_session
} session_ctx_t;

/**
 * @brief 协议层类（单例）
 *
 * 封装 CRC16、AES-128-CBC、帧构建/解析、会话管理等功能。
 * 大部分方法为静态工具方法，单例仅持有初始化标志。
 */
class Protocol {
public:
    /**
      * @brief 获取 Protocol 单例实例。
     * @return Protocol 单例引用。
     */
    static Protocol& instance();

    /**
      * @brief 初始化协议层。
     */
    void init();

    /**
      * @brief 生成基于时间戳、地址和随机种子的扰码 nonce。
     * @param timestamp 时间戳。
     * @param mem_addr 内存地址。
     * @param rand_seed 随机种子。
     * @param nonce_out 输出的 16 字节 nonce 缓冲区。
     */
    void scrambleGenerate(uint32_t timestamp, uint32_t mem_addr,
                                 uint32_t rand_seed, uint8_t *nonce_out);
    /**
      * @brief 会话密钥初始化，将密钥拷贝到会话上下文。
     * @param sess 会话上下文指针。
     * @param k_session 会话密钥数据。
     * @return 成功返回 0，参数无效返回 -1。
     */
    static int sessionKeyInit(session_ctx_t *sess, const uint8_t *k_session);
    /**
      * @brief 清除会话上下文中的密钥数据。
     * @param sess 会话上下文指针。
     */
    static void sessionKeyClear(session_ctx_t *sess);

private:
    Protocol() = default;
    ~Protocol() = default;
    Protocol(const Protocol&) = delete;
    Protocol& operator=(const Protocol&) = delete;

    uint8_t m_scrambleConst[SCRAMBLE_CONST_LEN] = {0};
    bool m_initialized = false;
};

#endif // PROTOCOL_H