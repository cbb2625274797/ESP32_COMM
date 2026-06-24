/*
 * canfd_bridge.cpp - CANFD 3通道通信桥接实现（M4: 从 SpiMaster 提取）
 */
#include <string.h>
#include "canfd_bridge.h"
#include "spi_master.h"

namespace {

constexpr uint8_t kSpiCanFrameType = 0xC0U;

static int sendCanFrameOverSpi(can_channel_t channel, uint32_t ext_id,
                               const uint8_t *data, uint8_t dlc,
                               uint8_t *recv_buf, uint8_t recv_max,
                               uint32_t timeout_ms)
{
    uint8_t tx_buf[72];
    size_t tx_len = 0;

    tx_buf[tx_len++] = static_cast<uint8_t>(channel & CAN_CHANNEL_MASK);
    tx_buf[tx_len++] = static_cast<uint8_t>((ext_id >> 24) & 0xFFU);
    tx_buf[tx_len++] = static_cast<uint8_t>((ext_id >> 16) & 0xFFU);
    tx_buf[tx_len++] = static_cast<uint8_t>((ext_id >> 8) & 0xFFU);
    tx_buf[tx_len++] = static_cast<uint8_t>(ext_id & 0xFFU);
    if (dlc > 0U) {
        memcpy(&tx_buf[tx_len], data, dlc);
        tx_len += dlc;
    }

    uint8_t resp_type = 0U;
    size_t resp_len = 0U;
    esp_err_t ret = SpiMaster::instance().otaTransfer(
        kSpiCanFrameType,
        tx_buf,
        tx_len,
        &resp_type,
        recv_buf,
        recv_max,
        &resp_len,
        timeout_ms);
    if (ret != ESP_OK || resp_type != SPI_OTA_RSP_ACK) {
        return -1;
    }

    if (recv_buf == nullptr || recv_max == 0U) {
        return static_cast<int>(resp_len);
    }

    const size_t copied_len = (resp_len < recv_max) ? resp_len : recv_max;
    return static_cast<int>(copied_len);
}

} // namespace

/**
 * @brief 发送一帧 CANFD 数据，可选等待并读取响应。
 *        通过 SPI 协议帧发送给 GD32 APP，并读取 ACK/ERROR 响应。
 */
int canFdSend(can_channel_t channel, uint32_t ext_id,
              const uint8_t *data, uint8_t dlc,
              uint8_t *recv_buf, uint8_t recv_max,
              uint32_t timeout_ms)
{
    if (channel >= CAN_CHANNEL_MAX || dlc > CAN_MAX_DLC || !data) {
        return -1;
    }
    return sendCanFrameOverSpi(channel, ext_id, data, dlc, recv_buf, recv_max, timeout_ms);
}

/**
 * @brief 发送一帧无需上层消费响应的 CANFD 数据。
 *        GD32 APP 仍会返回 ACK/ERROR，这里主动读走响应，避免残留到下一笔 SPI 事务。
 */
int canFdSendNoReply(can_channel_t channel, uint32_t ext_id,
                     const uint8_t *data, uint8_t dlc)
{
    if (channel >= CAN_CHANNEL_MAX || dlc > CAN_MAX_DLC || !data) {
        return -1;
    }

    return (sendCanFrameOverSpi(channel, ext_id, data, dlc, nullptr, 0U, 1000U) >= 0) ? 0 : -1;
}
