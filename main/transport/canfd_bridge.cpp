/*
 * canfd_bridge.cpp - CANFD 3通道通信桥接实现（M4: 从 SpiMaster 提取）
 */
#include <string.h>
#include "canfd_bridge.h"
#include "spi_master.h"

/**
 * @brief 发送一帧 CANFD 数据，可选等待并读取响应。
 *        通过 SpiMaster 的公共 syncTransfer() 完成 SPI 传输。
 *        注意: syncTransfer 内部会加 SPI 互斥锁，FreeRTOS mutex 为递归锁故安全。
 */
int canFdSend(can_channel_t channel, uint32_t ext_id,
              const uint8_t *data, uint8_t dlc,
              uint8_t *recv_buf, uint8_t recv_max,
              uint32_t timeout_ms)
{
    if (channel >= CAN_CHANNEL_MAX || dlc > CAN_MAX_DLC || !data) {
        return -1;
    }

    uint8_t tx_buf[72];
    size_t tx_len = 0;

    tx_buf[tx_len++] = (uint8_t)(channel & CAN_CHANNEL_MASK);
    tx_buf[tx_len++] = (uint8_t)((ext_id >> 24) & 0xFF);
    tx_buf[tx_len++] = (uint8_t)((ext_id >> 16) & 0xFF);
    tx_buf[tx_len++] = (uint8_t)((ext_id >> 8) & 0xFF);
    tx_buf[tx_len++] = (uint8_t)(ext_id & 0xFF);
    memcpy(&tx_buf[tx_len], data, dlc);
    tx_len += dlc;

    esp_err_t ret = SpiMaster::instance().syncTransfer(
        0, tx_buf, tx_len, recv_buf, recv_max, timeout_ms);
    if (ret != ESP_OK) {
        return -1;
    }

    return (int)recv_max;
}

/**
 * @brief 发送一帧无需响应的 CANFD 数据。
 */
int canFdSendNoReply(can_channel_t channel, uint32_t ext_id,
                     const uint8_t *data, uint8_t dlc)
{
    if (channel >= CAN_CHANNEL_MAX || dlc > CAN_MAX_DLC || !data) {
        return -1;
    }

    uint8_t tx_buf[72];
    size_t tx_len = 0;

    tx_buf[tx_len++] = (uint8_t)(channel & CAN_CHANNEL_MASK);
    tx_buf[tx_len++] = (uint8_t)((ext_id >> 24) & 0xFF);
    tx_buf[tx_len++] = (uint8_t)((ext_id >> 16) & 0xFF);
    tx_buf[tx_len++] = (uint8_t)((ext_id >> 8) & 0xFF);
    tx_buf[tx_len++] = (uint8_t)(ext_id & 0xFF);
    memcpy(&tx_buf[tx_len], data, dlc);
    tx_len += dlc;

    esp_err_t ret = SpiMaster::instance().send(0, tx_buf, tx_len);
    if (ret != ESP_OK) {
        return -1;
    }

    return 0;
}
