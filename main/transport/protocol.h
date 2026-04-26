/*
 * protocol.h - 帧格式、CRC16、AES 加密解密
 * T2.2, T1.3
 * ESP-IDF 6.0 适配版本 - 使用软件 AES 实现 + 硬件 RNG
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
    int used;
} session_ctx_t;

// 初始化协议层
void protocol_init(void);

// CRC16 Modbus 计算（低字节在前，LE）
uint16_t crc16_modbus(const uint8_t *data, size_t len);

// 构建帧: 返回总帧长度
// 输出格式: SOF + CMD + LEN(2) + PAYLOAD + CRC16(2)
int frame_build(uint8_t cmd, const uint8_t *payload, size_t len, uint8_t *out_frame);

// 解析帧: 返回 payload 起始偏移，失败返回 -1
// 检查 SOF，验证 CRC16，提取 CMD 和 LEN
int frame_parse(const uint8_t *frame, size_t frame_len,
                uint8_t *out_cmd, uint8_t *out_payload, size_t *out_len);

// T2.2: AES-128 CBC 加密
// 使用预设主密钥或会话密钥
// 注意: 函数名带 protocol_ 前缀以避免与 ESP-IDF wpa_supplicant 组件冲突
int protocol_aes_encrypt(const uint8_t *plaintext, size_t len,
                         const uint8_t *key, const uint8_t *iv,
                         uint8_t *ciphertext_out);

int protocol_aes_decrypt(const uint8_t *ciphertext, size_t len,
                         const uint8_t *key, const uint8_t *iv,
                         uint8_t *plaintext_out);

// T2.1: 搅伴算法 - 生成随机数
// 输入: 时间戳 + 内存地址 + rand_seed
// 输出: 16 字节 nonce_server
void scramble_generate(uint32_t timestamp, uint32_t mem_addr,
                      uint32_t rand_seed, uint8_t *nonce_out);

// T2.4: 会话密钥管理
int session_key_init(session_ctx_t *sess, const uint8_t *k_session);
void session_key_clear(session_ctx_t *sess);
int session_encrypt(session_ctx_t *sess, const uint8_t *in, size_t len, uint8_t *out);
int session_decrypt(session_ctx_t *sess, const uint8_t *in, size_t len, uint8_t *out);

// 使用 ESP-IDF 硬件 RNG 生成随机数
void generate_random_nonce(uint8_t *nonce, size_t len);

#endif // PROTOCOL_H