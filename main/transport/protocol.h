/*
 * protocol.h - OTA 握手辅助（nonce 扰码 + 会话密钥生命周期）
 *
 * 本模块刻意保持轻量（原「帧格式 / CRC16 / AES-CBC」中间件已迁出）：
 *   - 不负责 SPI Link v2 帧 / CRC32
 *   - 不负责 AES 加解密（OTA 数据包 AES-ECB 在 ota_relay 内）
 *   - 仅提供：RNG 扰码常数、server nonce 生成、session key 存清
 *
 * AES 主密钥 MASTER_KEY[] 见 app_config.h。
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "app_config.h"

/** @brief AES-128 / 会话密钥长度（字节）。 */
inline constexpr size_t SESSION_KEY_LEN = 16;

/**
 * @brief OTA 会话密钥上下文。
 *
 * 仅缓存协商得到的 K_session；真正加解密在 ota_relay 中用该 key 做 AES-ECB。
 */
typedef struct {
    uint8_t key[SESSION_KEY_LEN]; ///< 会话密钥 K_session（16 字节）
} session_ctx_t;

/**
 * @brief OTA 握手辅助类（单例）
 *
 * 持有 per-boot 扰码常数 m_scrambleConst；
 * sessionKeyInit / sessionKeyClear 为无状态静态工具，不依赖单例成员。
 */
class Protocol {
public:
    /**
     * @brief 获取 Protocol 单例实例。
     * @return Protocol 单例引用。
     */
    static Protocol& instance();

    /**
     * @brief 初始化协议层（幂等）。
     *
     * 使用硬件 RNG 填充 m_scrambleConst，供 scrambleGenerate 混合使用。
     * 可重复调用；已初始化则直接返回。
     */
    void init();

    /**
     * @brief 生成基于时间戳、地址和随机种子的 16 字节 server nonce。
     * @param timestamp 时间戳（如 FreeRTOS tick）。
     * @param mem_addr  内存地址（增加熵，通常取栈/缓冲区地址）。
     * @param rand_seed 随机种子。
     * @param nonce_out 输出缓冲区，至少 16 字节；为 nullptr 时直接返回。
     *
     * @note 非标准 KDF，仅作 OTA 握手 nonce，勿当通用密钥派生。
     * @note 若尚未 init()，内部会自动调用 init()。
     */
    void scrambleGenerate(uint32_t timestamp, uint32_t mem_addr,
                          uint32_t rand_seed, uint8_t *nonce_out);

    /**
     * @brief 会话密钥初始化，将密钥拷贝到会话上下文。
     * @param sess 会话上下文指针。
     * @param k_session 会话密钥数据（至少 SESSION_KEY_LEN 字节）。
     * @return 成功返回 0，参数无效返回 -1。
     */
    static int sessionKeyInit(session_ctx_t *sess, const uint8_t *k_session);

    /**
     * @brief 清除会话上下文中的密钥数据（抗优化擦除）。
     * @param sess 会话上下文指针；为 nullptr 时忽略。
     */
    static void sessionKeyClear(session_ctx_t *sess);

private:
    /**
     * @brief 私有默认构造函数。
     */
    Protocol() = default;
    /**
     * @brief 私有析构函数。
     */
    ~Protocol() = default;
    /**
     * @brief 禁止复制构造。
     */
    Protocol(const Protocol&) = delete;
    /**
     * @brief 禁止复制赋值。
     */
    Protocol& operator=(const Protocol&) = delete;

    /**
     * @brief 抗编译器优化的内存擦除（volatile 逐字节写 0）。
     * @param p 起始地址；nullptr 忽略。
     * @param n 字节数。
     */
    static void secureWipe(void *p, size_t n);

    uint8_t m_scrambleConst[SCRAMBLE_CONST_LEN] = {0}; ///< per-boot 扰码常数（init 时 RNG 填充）
    bool m_initialized = false;                         ///< 是否已 init
};

#endif // PROTOCOL_H
