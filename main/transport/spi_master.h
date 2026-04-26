/*
 * spi_master.h - SPI Master 驱动头文件
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