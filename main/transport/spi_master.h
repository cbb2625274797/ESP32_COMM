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
    uint8_t       data[CAN_MAX_DLC];
    uint8_t       dlc;         // 数据长度 (1~64)
} can_frame_t;

// ============================================================
// 发送 CANFD 帧
// channel:  CAN 通道 (0, 1, 2)
// ext_id:   扩展帧 ID (29bit)
// data:     数据内容 (1~64 字节)
// dlc:      数据长度 (自动识别，可传 0 由 data 长度决定)
// recv_buf: 接收缓冲区 (可传 NULL)
// recv_max: 期望接收最大字节数
// timeout_ms: 超时时间 (ms)
// 返回: >= 0 成功(实际接收字节数), < 0 失败
// ============================================================
int gd32_can_send(can_channel_t channel, uint32_t ext_id,
                  const uint8_t *data, uint8_t dlc,
                  uint8_t *recv_buf, uint8_t recv_max,
                  uint32_t timeout_ms);

// 快速发送接口（不带响应接收）
// 用于广播/通知类 CAN 帧
int gd32_can_send_no_reply(can_channel_t channel, uint32_t ext_id,
                            const uint8_t *data, uint8_t dlc);

// ============================================================

// 初始化 SPI2 (VSPI) 总线和设备
esp_err_t spi_master_init(void);

// 同步 SPI 传输（阻塞式）
// 发送 cmd + payload，接收响应到 recv_buf
// 返回: ESP_OK 成功, ESP_ERR_TIMEOUT 超时
esp_err_t spi_sync_transfer(uint8_t cmd, const uint8_t *payload,
                            size_t payload_len, uint8_t *recv_buf,
                            size_t recv_len, uint32_t timeout_ms);

// 仅发送（不期望接收）
esp_err_t spi_send(uint8_t cmd, const uint8_t *payload, size_t len);

// 仅接收（发送哑数据）
esp_err_t spi_recv(uint8_t cmd, uint8_t *recv_buf, size_t len, uint32_t timeout_ms);

#endif // SPI_MASTER_H