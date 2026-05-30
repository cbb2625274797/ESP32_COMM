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

// 命令码（对应 GD32 OTA 协议）
#define CMD_HANDSHAKE_REQ   (0xF0)   // 握手请求
#define CMD_CHALLENGE       (0xF1)   // 挑战/随机数
#define CMD_SESSION_KEY     (0xF2)   // 会话密钥
#define CMD_SEND_SIZE       (0xF3)   // 发送固件大小
#define CMD_DATA_PACKET     (0xF4)   // 数据包
#define CMD_DATA_END        (0xF5)   // 数据结束
#define CMD_TRANSFER_DONE   (0xF6)   // 传输完成
#define CMD_FINISH_OTA      (0xF7)   // 结束 OTA
#define CMD_ACK             (0x80)   // 通用 ACK
#define CMD_ERROR           (0x81)   // 通用错误

// ACK 状态码
#define ACK_OK              (0x00)   // 成功
#define ACK_UNSUPPORTED    (0x01)   // 命令不支持
#define ACK_AUTH_FAILED    (0x02)   // 认证失败
#define ACK_SIZE_OOR       (0x03)   // 大小超出范围
#define ACK_CRC_FAILED     (0x04)   // CRC 校验失败
#define ACK_RETRY_EXCEED   (0x05)   // 重试次数超限
#define ACK_SESSION_INVALID (0x06)   // 会话密钥无效
#define ACK_FLASH_FAILED    (0x07)   // Flash 写入失败

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

    static uint16_t crc16Modbus(const uint8_t *data, size_t len);
    static int buildFrame(uint8_t cmd, const uint8_t *payload, size_t len, uint8_t *out_frame);
    static int parseFrame(const uint8_t *frame, size_t frame_len,
                          uint8_t *out_cmd, uint8_t *out_payload, size_t *out_len);
    static int aesEncrypt(const uint8_t *plaintext, size_t len,
                          const uint8_t *key, const uint8_t *iv,
                          uint8_t *ciphertext_out);
    static int aesDecrypt(const uint8_t *ciphertext, size_t len,
                          const uint8_t *key, const uint8_t *iv,
                          uint8_t *plaintext_out);
    static void scrambleGenerate(uint32_t timestamp, uint32_t mem_addr,
                                 uint32_t rand_seed, uint8_t *nonce_out);
    static int sessionKeyInit(session_ctx_t *sess, const uint8_t *k_session);
    static void sessionKeyClear(session_ctx_t *sess);
    static int sessionEncrypt(session_ctx_t *sess, const uint8_t *in, size_t len, uint8_t *out);
    static int sessionDecrypt(session_ctx_t *sess, const uint8_t *in, size_t len, uint8_t *out);
    static void generateRandomNonce(uint8_t *nonce, size_t len);

private:
    Protocol() = default;
    ~Protocol() = default;
    Protocol(const Protocol&) = delete;
    Protocol& operator=(const Protocol&) = delete;

    bool m_initialized = false;
};

#endif // PROTOCOL_H