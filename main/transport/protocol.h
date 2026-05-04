/*
 * protocol.h - 帧格式、CRC16、AES 加密解密
 * T2.2, T1.3
 * ESP-IDF 6.0 适配版本 - 使用硬件 AES-128-CBC + 硬件 RNG
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
 * @brief 初始化协议层资源，建立 CRC/AES 等基础能力的可用状态。
 */
void protocol_init(void);

/**
 * @brief 计算一段数据的 CRC16-Modbus 校验值。
 * @param data 待校验的数据指针。
 * @param len 数据长度（字节）。
 * @return CRC16 校验值。
 */
uint16_t crc16_modbus(const uint8_t *data, size_t len);

/**
 * @brief 按协议格式构建一帧数据，返回生成后的总帧长度。
 * @param cmd 命令码。
 * @param payload 负载数据指针，可为 NULL。
 * @param len 负载长度（字节）。
 * @param out_frame 输出帧缓冲区。
 * @return 帧总长度，失败返回 -1。
 */
int frame_build(uint8_t cmd, const uint8_t *payload, size_t len, uint8_t *out_frame);

/**
 * @brief 解析一帧协议数据，提取命令字、负载和长度，失败返回 -1。
 * @param frame 待解析的帧数据。
 * @param frame_len 帧数据总长度。
 * @param out_cmd 输出参数，解析出的命令码。
 * @param out_payload 输出缓冲区，存放解析出的负载。
 * @param out_len 输出参数，负载实际长度。
 * @return 0 成功，-1 失败。
 */
int frame_parse(const uint8_t *frame, size_t frame_len,
                uint8_t *out_cmd, uint8_t *out_payload, size_t *out_len);

/**
 * @brief 使用 AES-128-CBC 对明文进行加密，返回密文长度，失败返回负值。
 * @param plaintext 明文数据。
 * @param len 明文长度（字节）。
 * @param key 128 位 AES 密钥。
 * @param iv 128 位 CBC 初始向量。
 * @param ciphertext_out 输出缓冲区，存放密文。
 * @return 密文长度，失败返回 -1。
 */
int protocol_aes_encrypt(const uint8_t *plaintext, size_t len,
                         const uint8_t *key, const uint8_t *iv,
                         uint8_t *ciphertext_out);

/**
 * @brief 使用 AES-128-CBC 对密文进行解密，返回去除填充后的明文长度。
 * @param ciphertext 密文数据。
 * @param len 密文长度（字节，须为 16 的倍数）。
 * @param key 128 位 AES 密钥。
 * @param iv 128 位 CBC 初始向量。
 * @param plaintext_out 输出缓冲区，存放解密后的明文。
 * @return 明文长度，失败返回 -1。
 */
int protocol_aes_decrypt(const uint8_t *ciphertext, size_t len,
                         const uint8_t *key, const uint8_t *iv,
                         uint8_t *plaintext_out);

/**
 * @brief 根据时间戳、内存地址和随机种子生成 16 字节随机数。
 * @param timestamp 时间戳种子。
 * @param mem_addr 内存地址种子。
 * @param rand_seed 随机数种子。
 * @param nonce_out 输出缓冲区，存放生成的 16 字节随机数。
 */
void scramble_generate(uint32_t timestamp, uint32_t mem_addr,
                      uint32_t rand_seed, uint8_t *nonce_out);

/**
 * @brief 用给定会话密钥初始化会话上下文。
 * @param sess 待初始化的会话上下文。
 * @param k_session 128 位会话密钥。
 * @return 0 成功，-1 失败。
 */
int session_key_init(session_ctx_t *sess, const uint8_t *k_session);

/**
 * @brief 清除会话上下文中的密钥与状态信息。
 * @param sess 待清除的会话上下文。
 */
void session_key_clear(session_ctx_t *sess);

/**
 * @brief 使用当前会话上下文对输入数据执行加密。
 * @param sess 会话上下文。
 * @param in 输入明文。
 * @param len 数据长度。
 * @param out 输出密文缓冲区。
 * @return 密文长度，失败返回 -1。
 */
int session_encrypt(session_ctx_t *sess, const uint8_t *in, size_t len, uint8_t *out);

/**
 * @brief 使用当前会话上下文对输入数据执行解密。
 * @param sess 会话上下文。
 * @param in 输入密文。
 * @param len 数据长度。
 * @param out 输出明文缓冲区。
 * @return 明文长度，失败返回 -1。
 */
int session_decrypt(session_ctx_t *sess, const uint8_t *in, size_t len, uint8_t *out);

/**
 * @brief 使用 ESP-IDF 硬件 RNG 生成指定长度的随机数。
 * @param nonce 输出缓冲区。
 * @param len 随机数字节数。
 */
void generate_random_nonce(uint8_t *nonce, size_t len);

#endif // PROTOCOL_H