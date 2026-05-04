/*
 * spi_master.h - SPI Master 驱动头文件
 * CANFD 3通道发送接口 + 底层 SPI 总线
 */
#ifndef SPI_MASTER_H
#define SPI_MASTER_H

#include <stdint.h>
#include "driver/spi_master.h"

// SPI 配置常量
#define SPI_MASTER_CS_GPIO    (4)           // CS 片选引脚
#define SPI_CLOCK_HZ          (40 * 1000 * 1000)  // 40MHz
#define SPI_MODE              (0)           // CPOL=0, CPHA=0
#define SPI_QUEUE_SIZE        (1)

// SPI 设备句柄（外部引用）
extern spi_device_handle_t g_spi_device;

// ============================================================
// CANFD 帧格式（3通道）
// [通道编号: 1B] [扩展帧ID: 4B BE] [数据: 1~64B]
// 长度自动识别，无需发送 DLC
// ============================================================
#define CAN_EXTENDED_ID_BITS   (29)          // CAN 扩展帧 ID 位数
#define CAN_MAX_DLC            (64)            // 最大数据长度
#define CAN_CHANNEL_MASK        (0x03)         // 通道号掩码 (0~2)

// 通道定义
typedef enum {
    CAN_CHANNEL_0 = 0,
    CAN_CHANNEL_1 = 1,
    CAN_CHANNEL_2 = 2,
    CAN_CHANNEL_MAX = 3,
} can_channel_t;

// CANFD 帧结构
typedef struct {
    can_channel_t channel;      // CAN 通道 (0~2)
    uint32_t      ext_id;      // 扩展帧 ID (29bit, Big-Endian 存储)
    uint8_t       data[CAN_MAX_DLC];  // CAN 数据负载 (最大 64 字节)
    uint8_t       dlc;         // 数据长度 (1~64)
} can_frame_t;

/**
 * @brief 发送一帧 CANFD 数据，可选等待并读取响应内容。
 * @param channel CAN 通道（0~2）。
 * @param ext_id 29 位扩展帧 ID。
 * @param data 数据负载指针。
 * @param dlc 数据长度，0 则自动推断。
 * @param recv_buf 接收缓冲区，可为 NULL。
 * @param recv_max 期望最大接收字节数。
 * @param timeout_ms 超时时间（毫秒）。
 * @return >= 0 接收字节数，< 0 失败。
 */
int gd32_can_send(can_channel_t channel, uint32_t ext_id,
                  const uint8_t *data, uint8_t dlc,
                  uint8_t *recv_buf, uint8_t recv_max,
                  uint32_t timeout_ms);

/**
 * @brief 发送一帧无需响应的 CANFD 数据，适合广播或通知场景。
 * @param channel CAN 通道（0~2）。
 * @param ext_id 29 位扩展帧 ID。
 * @param data 数据负载指针。
 * @param dlc 数据长度。
 * @return >= 0 成功，< 0 失败。
 */
int gd32_can_send_no_reply(can_channel_t channel, uint32_t ext_id,
                            const uint8_t *data, uint8_t dlc);

/**
 * @brief 初始化 SPI2 总线与目标从设备。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t spi_master_init(void);

/**
 * @brief 执行一次同步 SPI 传输，发送命令和负载并可选接收返回数据。
 * @param cmd SPI 命令码。
 * @param payload 发送数据指针。
 * @param payload_len 发送数据长度。
 * @param recv_buf 接收缓冲区，可为 NULL。
 * @param recv_len 期望接收长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t spi_sync_transfer(uint8_t cmd, const uint8_t *payload,
                            size_t payload_len, uint8_t *recv_buf,
                            size_t recv_len, uint32_t timeout_ms);

/**
 * @brief 仅发送 SPI 数据，不关心返回内容。
 * @param cmd SPI 命令码。
 * @param payload 发送数据指针。
 * @param len 发送数据长度。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t spi_send(uint8_t cmd, const uint8_t *payload, size_t len);

/**
 * @brief 仅接收 SPI 数据，底层会发送哑数据产生时钟。
 * @param cmd SPI 命令码。
 * @param recv_buf 接收缓冲区。
 * @param len 接收长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t spi_recv(uint8_t cmd, uint8_t *recv_buf, size_t len, uint32_t timeout_ms);

#endif // SPI_MASTER_H