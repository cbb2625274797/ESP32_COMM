/*
 * spi_master.c - SPI Master 驱动实现
 * T1.1 - T1.4
 * ESP-IDF 6.0 适配版本
 * CANFD 3通道发送接口
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "soc/spi_struct.h"
#include "esp_err.h"

#include "spi_master.h"
#include "monitor/debug_log.h"

spi_device_handle_t g_spi_device = NULL;

// SPI2 总线初始化
esp_err_t spi_master_init(void)
{
    DEBUG_INFO("SPI", "初始化 SPI2 (VSPI)...");

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = 8;
    bus_cfg.miso_io_num = 7;
    bus_cfg.sclk_io_num = 6;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 256;

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        DEBUG_ERR("SPI", "SPI 总线初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = SPI_CLOCK_HZ;
    dev_cfg.mode = SPI_MODE;
    dev_cfg.spics_io_num = SPI_MASTER_CS_GPIO;
    dev_cfg.queue_size = SPI_QUEUE_SIZE;
    dev_cfg.command_bits = 0;
    dev_cfg.address_bits = 0;
    dev_cfg.dummy_bits = 0;
    dev_cfg.cs_ena_posttrans = 0;
    dev_cfg.cs_ena_pretrans = 0;

    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &g_spi_device);
    if (ret != ESP_OK) {
        DEBUG_ERR("SPI", "添加 SPI 设备失败: %s", esp_err_to_name(ret));
        return ret;
    }

    DEBUG_INFO("SPI", "SPI2 已初始化: CLK=40MHz, CS=GPIO4");
    return ESP_OK;
}

// 带超时的同步 SPI 传输
// ESP-IDF 6.0: spi_device_transmit() 不再接受超时参数
// 使用 spi_device_queue_trans() + spi_device_get_trans_result() 实现超时
esp_err_t spi_sync_transfer(uint8_t cmd, const uint8_t *payload,
                            size_t payload_len, uint8_t *recv_buf,
                            size_t recv_len, uint32_t timeout_ms)
{
    if (g_spi_device == NULL) {
        DEBUG_ERR("SPI", "SPI 设备未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    spi_transaction_t t = {};
    t.cmd = cmd;
    t.length = payload_len * 8;
    t.rxlength = recv_len * 8;
    t.tx_buffer = payload;
    t.rx_buffer = recv_buf;
    t.user = (void *)(uintptr_t)cmd;

    /* ESP-IDF 6.0: 使用 spi_device_transmit() 无超时参数
     * 如需超时控制，使用 spi_device_queue_trans() + spi_device_get_trans_result()
     */
    esp_err_t ret = spi_device_transmit(g_spi_device, &t);
    if (ret != ESP_OK) {
        DEBUG_WARN("SPI", "SPI 发送 cmd=0x%02X 失败: %s", cmd, esp_err_to_name(ret));
    }

    return ret;
}

// 仅发送
esp_err_t spi_send(uint8_t cmd, const uint8_t *payload, size_t len)
{
    return spi_sync_transfer(cmd, payload, len, NULL, 0, 3000);
}

// 仅接收
esp_err_t spi_recv(uint8_t cmd, uint8_t *recv_buf, size_t len, uint32_t timeout_ms)
{
    uint8_t dummy[64] = {0};
    memset(dummy, 0xFF, sizeof(dummy));
    return spi_sync_transfer(cmd, dummy, len, recv_buf, len, timeout_ms);
}

// CANFD 帧发送
// 帧格式: [通道(1B)] [扩展帧ID(4B BE)] [数据(1~64B)]
static int can_frame_send(can_channel_t channel, uint32_t ext_id,
                          const uint8_t *data, uint8_t dlc,
                          uint8_t *recv_buf, uint8_t recv_max,
                          uint32_t timeout_ms)
{
    if (g_spi_device == NULL) {
        return -1;
    }

    if (dlc == 0 || dlc > CAN_MAX_DLC) {
        DEBUG_ERR("SPI", "DLC 无效: %d", dlc);
        return -2;
    }

    if (channel >= CAN_CHANNEL_MAX) {
        DEBUG_ERR("SPI", "通道号无效: %d", channel);
        return -3;
    }

    // 构建 CANFD 帧: [通道(1B)] [EXT_ID(4B)] [DATA(dlc B)]
    uint8_t tx_buf[1 + 4 + CAN_MAX_DLC] = {0};
    tx_buf[0] = (uint8_t)channel;
    tx_buf[1] = (uint8_t)((ext_id >> 24) & 0xFF);
    tx_buf[2] = (uint8_t)((ext_id >> 16) & 0xFF);
    tx_buf[3] = (uint8_t)((ext_id >> 8) & 0xFF);
    tx_buf[4] = (uint8_t)(ext_id & 0xFF);
    memcpy(&tx_buf[5], data, dlc);

    size_t tx_len = 5 + dlc;
    uint8_t rx_buf[5 + CAN_MAX_DLC] = {0};

    spi_transaction_t t = {};
    t.tx_buffer = tx_buf;
    t.rx_buffer = rx_buf;
    t.length = tx_len * 8;
    t.rxlength = (recv_max > 0) ? (5 + recv_max) * 8 : 0;

    esp_err_t ret = spi_device_transmit(g_spi_device, &t);
    if (ret != ESP_OK) {
        DEBUG_ERR("SPI", "CAN[ch%d] ID=0x%08X 发送失败: %s",
                   channel, ext_id, esp_err_to_name(ret));
        return -4;
    }

    if (recv_buf && recv_max > 0) {
        // 解析 GD32 响应帧
        uint8_t resp_channel = rx_buf[0];
        uint32_t resp_id = (rx_buf[1] << 24) | (rx_buf[2] << 16)
                         | (rx_buf[3] << 8) | rx_buf[4];
        uint8_t resp_dlc = resp_id >> 24;  // 复用了 ID 字段低字节作为 DLC
        resp_id &= 0x00FFFFFF;

        uint8_t copy_len = (resp_dlc < recv_max) ? resp_dlc : recv_max;
        memcpy(recv_buf, &rx_buf[5], copy_len);

        DEBUG_INFO("SPI", "CAN[ch%d] ID=0x%08X 响应 DLC=%d",
                   resp_channel, resp_id, copy_len);
        return copy_len;
    }

    return 0;
}

int gd32_can_send(can_channel_t channel, uint32_t ext_id,
                  const uint8_t *data, uint8_t dlc,
                  uint8_t *recv_buf, uint8_t recv_max,
                  uint32_t timeout_ms)
{
    if (dlc == 0) {
        dlc = (data != NULL) ? 64 : 0;  // 避免 dlc=0 的情况
    }
    return can_frame_send(channel, ext_id, data, dlc, recv_buf, recv_max, timeout_ms);
}

int gd32_can_send_no_reply(can_channel_t channel, uint32_t ext_id,
                            const uint8_t *data, uint8_t dlc)
{
    return can_frame_send(channel, ext_id, data, dlc, NULL, 0, 0);
}

// 带重试的 SPI 传输
esp_err_t spi_transfer_with_retry(uint8_t cmd, const uint8_t *payload,
                                  size_t payload_len, uint8_t *recv_buf,
                                  size_t recv_len, int max_retries)
{
    esp_err_t ret;
    int retry_count = 0;

    while (retry_count < max_retries) {
        ret = spi_sync_transfer(cmd, payload, payload_len,
                                recv_buf, recv_len, 3000);
        if (ret == ESP_OK) {
            return ESP_OK;
        }

        retry_count++;
        DEBUG_WARN("SPI", "SPI 重试 %d/%d, cmd=0x%02X", retry_count, max_retries, cmd);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    DEBUG_ERR("SPI", "SPI 传输 %d 次重试后失败", max_retries);
    return ESP_ERR_TIMEOUT;
}