/*
 * ota_relay.h - OTA 中继核心状态机
 * T2.3, T3.1 - T3.7
 */
#ifndef OTA_RELAY_H
#define OTA_RELAY_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// T4.3: OTA 元数据结构，用于 NVS 存储
#define OTA_META_NVS_NAMESPACE "ota_meta"
#define OTA_META_NVS_KEY "meta"

typedef struct {
    uint32_t total_size;      // 固件总大小（字节）
    uint32_t stored_size;     // 当前已存储大小（字节）
    uint32_t last_seq;        // 最后成功存储的序列号
    uint32_t chunk_count;     // 预期总块数
    uint8_t  checksum[32];    // 固件 SHA256 校验（可选）
} ota_meta_t;

// OTA 中继状态（对应 GD32 四阶段协议）
typedef enum {
    // 阶段0: 空闲
    RELAY_IDLE = 0,

    // 阶段1: 安全握手
    RELAY_HANDSHAKE_IDLE,     // 等待 HANDSHAKE_REQ
    RELAY_HANDSHAKE_SENT,      // 已发送 0x00 AC，等待 GD32 响应
    RELAY_CHALLENGE_SENT,      // 已发送 nonce_server (16B)，等待响应
    RELAY_CHALLENGE_RESPOND,   // 已收到 GD32 握手数据包
    RELAY_SESSION_KEY_SENT,    // 已发送变换后的随机数，等待 0xBA C0
    RELAY_AUTH_COMPLETE,       // 已收到 0xBA C0，安全通道建立完成

    // 阶段2: 传输准备
    RELAY_SIZE_SENT,           // 已发送固件大小（4B LE）
    RELAY_SIZE_ACK,            // 已收到 0xBA C0 + seq

    // 阶段3: 数据传输
    RELAY_RECEIVING,           // 接收并存储固件到 Flash
    RELAY_RELAY_ACTIVE,        // 存储完成，开始向 GD32 转发
    RELAY_PACKET_SENT,         // 已发送 DATA_PACKET，等待 GD32 ACK
    RELAY_PACKET_ACK_OK,       // ACK 正常，继续下一包
    RELAY_PACKET_ACK_RETRY,   // ACK 要求重发，重新发送当前包
    RELAY_RECEIVE_DONE,        // 已发送 DATA_END，等待 TRANSFER_DONE

    // 阶段4: 结束跳转
    RELAY_TRANSFER_DONE,       // 已收到 0xBA C0 + FF FF FF FF
    RELAY_FINISH_SENT,         // 已发送 0x0A AC，等待 GD32 确认
    RELAY_DONE,                // GD32 回复 0x0A AC，OTA 完成
    RELAY_APP_VERIFY,          // 等待 GD32 APP 启动并回复存活确认

    // 错误状态
    RELAY_ERROR = 0xFF
} relay_state_t;

// OTA 结果码
typedef enum {
    OTA_RESULT_OK = 0,
    OTA_RESULT_ERR_TIMEOUT = -1,   // GD32 超时无响应
    OTA_RESULT_ERR_CRC = -2,       // CRC 校验失败
    OTA_RESULT_ERR_AUTH = -3,      // 认证失败
    OTA_RESULT_ERR_STORAGE = -4,    // Flash 存储错误
    OTA_RESULT_ERR_GD32 = -5,      // GD32 返回错误
    OTA_RESULT_ERR_HEALTH = -6,    // 健康状态异常中止
    OTA_RESULT_ERR_USER_ABORT = -7  // 用户中止
} ota_result_t;

// 外部队列句柄（在 app_main 中创建）
extern QueueHandle_t g_ota_dispatch_queue;   // 从 WiFi/BLE 到 OTA 中继
extern QueueHandle_t g_ota_result_queue;    // OTA 结果到 WiFi/BLE

/**
 * @brief OTA 中继任务入口，负责执行握手、认证、分包转发和结束流程。
 * @param pvParameters FreeRTOS 任务参数，未使用。
 */
void ota_relay_task(void *pvParameters);

/**
 * @brief 发起与 GD32 的握手流程，建立 OTA 传输前的安全会话。
 * @return 操作结果码。
 */
ota_result_t ota_relay_start_handshake(void);

/**
 * @brief 从 Flash 中读取已缓存固件并按协议转发给 GD32。
 * @return 操作结果码。
 */
ota_result_t ota_relay_forward_firmware(void);

/**
 * @brief 获取当前 OTA 中继状态机状态。
 * @return 当前中继状态枚举值。
 */
relay_state_t ota_relay_get_state(void);

/**
 * @brief 重置 OTA 中继状态机，清理当前状态以便重新开始流程。
 */
void ota_relay_reset(void);

/**
 * @brief 向 OTA 缓存分区写入一段固件数据。
 * @param offset 分区内写入偏移。
 * @param data 待写入的数据指针。
 * @param len 数据长度（字节）。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t flash_write_chunk(uint32_t offset, const uint8_t *data, size_t len);

/**
 * @brief 从 OTA 缓存分区读取一段固件数据。
 * @param offset 分区内读取偏移。
 * @param data 读取缓冲区指针。
 * @param len 读取长度（字节）。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t flash_read_chunk(uint32_t offset, uint8_t *data, size_t len);

/**
 * @brief 从持久化文件中加载 OTA 元数据。
 * @param meta 输出参数，加载后的元数据结构体。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t meta_load(ota_meta_t *meta);

/**
 * @brief 将 OTA 元数据保存到持久化文件。
 * @param meta 待保存的元数据结构体。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t meta_save(const ota_meta_t *meta);

/**
 * @brief 更新已存储大小与最后分块序号等 OTA 进度信息。
 * @param stored_size 已存储的固件大小（字节）。
 * @param last_seq 最后成功的分块序号。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t meta_update(uint32_t stored_size, uint32_t last_seq);

/**
 * @brief 清空 OTA 元数据，通常在完成或中止后调用。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t meta_clear(void);

// GD32 APP 存活标志（OTA 完成后由 APP PING 确认）
extern volatile bool g_gd32_app_alive;

// 当前中继状态（供健康监控外部访问）
extern relay_state_t g_relay_state;

#endif // OTA_RELAY_H