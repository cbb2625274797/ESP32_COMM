/*
 * canfd_bridge.cpp - CANFD 3通道通信桥接实现（M4: 从 SpiMaster 提取）
 */
#include <string.h>
#include "canfd_bridge.h"
#include "spi_master.h"
#include "monitor/debug_log.h"

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
        if (ret != ESP_OK) {
            DEBUG_WARN("CAN", "CAN_SEND SPI失败: ch=%u id=0x%08lX err=%s",
                       (unsigned)channel, (unsigned long)ext_id, esp_err_to_name(ret));
            return -2;  /* SPI 通信失败 */
        }
        /* resp_type != ACK: 读取 GD32 返回的错误码 */
        uint8_t gd32_err = (resp_len > 0 && recv_buf) ? recv_buf[0] : 0xFF;
        DEBUG_WARN("CAN", "CAN_SEND GD32拒绝: ch=%u id=0x%08lX resp=0x%02X err=0x%02X",
                   (unsigned)channel, (unsigned long)ext_id, resp_type, gd32_err);
        return -3;  /* GD32 返回 ERROR */
    }

    if (recv_buf == nullptr || recv_max == 0U) {
        return static_cast<int>(resp_len);
    }

    const size_t copied_len = (resp_len < recv_max) ? resp_len : recv_max;
    return static_cast<int>(copied_len);
}

constexpr uint8_t kSpiCanFeedbackType   = 0xC1U;
constexpr uint8_t kSpiCanSetModeType    = 0xC2U;
constexpr uint8_t kSpiIdSnapshotType    = 0xC3U;
constexpr size_t  kFeedbackPayloadLen   = 123U;  // 与 GD32 协议表对齐 (kTxBufSize=128 - overhead=5)

static inline uint32_t readU32Le(const uint8_t *p) {
    return  static_cast<uint32_t>(p[0])
          | (static_cast<uint32_t>(p[1]) << 8)
          | (static_cast<uint32_t>(p[2]) << 16)
          | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

/**
 * @brief 发送一帧 CANFD 数据，可选等待并读取响应。
 *        通过 SPI 协议帧发送给 GD32 APP，并读取 ACK/ERROR 响应。
 *        返回值: >=0 成功(接收数据长度), -1 参数错误, -2 SPI通信失败, -3 GD32拒绝。
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


/**
 * @brief 拉取一路 CAN 通道的 1Hz 反馈 + ID 快照表（GD32 SPI 帧类型 0xC1，分页）。
 *        连续拉取 page=0 和 page=1，合并为完整结果。
 */
int canFdPollFeedback(can_channel_t channel,
                      can_feedback_result_t *out,
                      uint32_t timeout_ms)
{
    if (channel >= CAN_CHANNEL_MAX || out == nullptr) {
        return -1;
    }

    out->valid = 0;
    uint8_t entryIdx = 0U;

    constexpr uint8_t kFixedHeader = 28U; /* ch+page+total+cnt+mode+seq+rx+tx+drop+err+tec+rec+bus_state */

    for (uint8_t page = 0U; page < 2U; ++page) {
        uint8_t req[2] = { static_cast<uint8_t>(channel & CAN_CHANNEL_MASK), page };
        uint8_t resp_buf[kFeedbackPayloadLen] = {};
        uint8_t resp_type = 0U;
        size_t  resp_len  = 0U;

        esp_err_t ret = SpiMaster::instance().otaTransfer(
            kSpiCanFeedbackType,
            req, 2U,
            &resp_type, resp_buf, sizeof(resp_buf), &resp_len, timeout_ms);
        if (ret != ESP_OK || resp_type != SPI_OTA_RSP_ACK || resp_len < kFixedHeader) {
            return -1;
        }

        /* 第一页时解析固定头 */
        if (page == 0U) {
            out->stats.channel        = resp_buf[0];
            out->stats.mode           = resp_buf[4];
            out->stats.snapshot_seq   = readU32Le(&resp_buf[5]);
            out->stats.rx_count       = readU32Le(&resp_buf[9]);
            out->stats.tx_count       = readU32Le(&resp_buf[13]);
            out->stats.rx_drop_count  = readU32Le(&resp_buf[17]);
            out->stats.error_count    = readU32Le(&resp_buf[21]);
            out->stats.tec            = resp_buf[25];
            out->stats.rec            = resp_buf[26];
            out->stats.bus_state      = resp_buf[27];
            out->total_count          = resp_buf[2];
        }

        const uint8_t entryCount = resp_buf[3];
        if (entryCount == 0U) break;

        uint16_t offset = kFixedHeader;
        for (uint8_t i = 0U; i < entryCount && entryIdx < CANFD_ID_SNAPSHOT_MAX; ++i) {
            if (offset + 8U > resp_len) break;

            id_snapshot_entry_t &e = out->entries[entryIdx];
            e.id  = readU32Le(&resp_buf[offset]);
            e.dlc = resp_buf[offset + 4U];
            const uint8_t flags = resp_buf[offset + 5U];
            e.fd_flag = (flags & 0x01U) ? 1U : 0U;
            e.brs     = (flags & 0x02U) ? 1U : 0U;
            e.esi     = (flags & 0x04U) ? 1U : 0U;

            const uint8_t dataLen = (e.dlc < CANFD_ID_SNAPSHOT_DATA_MAX)
                                    ? e.dlc : static_cast<uint8_t>(CANFD_ID_SNAPSHOT_DATA_MAX);
            const uint16_t entrySize = static_cast<uint16_t>(8U + dataLen);
            if (offset + entrySize > resp_len) break;

            memcpy(e.data, &resp_buf[offset + 8U], dataLen);
            for (uint8_t d = dataLen; d < CANFD_ID_SNAPSHOT_DATA_MAX; ++d) {
                e.data[d] = 0U;
            }

            offset += entrySize;
            entryIdx++;
        }

        if (entryIdx >= out->total_count) break;
    }
    out->valid = 1;
    return 0;
}

/**
 * @brief 切换 GD32 三路 CAN 接收过滤模式（GD32 SPI 帧类型 0xC2）。
 */

/**
 * @brief 拉取指定通道的不重复 ID 快照表（GD32 SPI 帧类型 0xC3）。
 *        分页拉取，每页 8 条，最多拉 2 页（16 条）。
 */
int canFdPullIdSnapshot(can_channel_t channel,
                        id_snapshot_entry_t *out,
                        uint8_t *out_count,
                        uint32_t timeout_ms)
{
    if (channel >= CAN_CHANNEL_MAX || out == nullptr || out_count == nullptr) {
        return -1;
    }

    *out_count = 0U;
    uint8_t total = 0U;

    for (uint8_t page = 0U; page < 2U; ++page) {
        uint8_t req[2] = { static_cast<uint8_t>(channel), page };
        uint8_t resp_buf[kFeedbackPayloadLen] = {};
        uint8_t resp_type = 0U;
        size_t  resp_len  = 0U;

        esp_err_t ret = SpiMaster::instance().otaTransfer(
            kSpiIdSnapshotType,
            req, 2U,
            &resp_type, resp_buf, sizeof(resp_buf), &resp_len, timeout_ms);
        if (ret != ESP_OK || resp_type != SPI_OTA_RSP_ACK || resp_len < 4U) {
            return -1;
        }

        /* resp: [ch][page][total_count][entry_count][entries...] */
        if (page == 0U) {
            total = resp_buf[2];
        }
        const uint8_t entryCount = resp_buf[3];
        if (entryCount == 0U) break;

        uint16_t offset = 4U;
        for (uint8_t i = 0U; i < entryCount && *out_count < CANFD_ID_SNAPSHOT_MAX; ++i) {
            if (offset + 16U > resp_len) break;

            id_snapshot_entry_t &e = out[*out_count];
            e.id       = readU32Le(&resp_buf[offset]);
            e.seq      = readU32Le(&resp_buf[offset + 4U]);
            e.rx_count = readU32Le(&resp_buf[offset + 8U]);
            e.dlc      = resp_buf[offset + 12U];
            const uint8_t flags = resp_buf[offset + 13U];
            e.fd_flag  = (flags & 0x01U) ? 1U : 0U;
            e.brs      = (flags & 0x02U) ? 1U : 0U;
            e.esi      = (flags & 0x04U) ? 1U : 0U;

            const uint8_t dataLen = (e.dlc < CANFD_ID_SNAPSHOT_DATA_MAX)
                                    ? e.dlc : static_cast<uint8_t>(CANFD_ID_SNAPSHOT_DATA_MAX);
            const uint16_t entrySize = 16U + dataLen;
            if (offset + entrySize > resp_len) break;

            memcpy(e.data, &resp_buf[offset + 16U], dataLen);
            /* 清零剩余字节 */
            for (uint8_t d = dataLen; d < CANFD_ID_SNAPSHOT_DATA_MAX; ++d) {
                e.data[d] = 0U;
            }

            offset += entrySize;
            (*out_count)++;
        }

        if (*out_count >= total) break; /* 已拉完 */
    }
    return 0;
}
int canFdSetRxMode(can_rx_mode_t mode,
                   can_rx_mode_t *actual,
                   uint32_t timeout_ms)
{
    if (mode != CAN_RX_MODE_ALL_PASS && mode != CAN_RX_MODE_FILTER_ID && mode != CAN_RX_MODE_BRIDGE_0_TO_12) {
        return -1;
    }

    uint8_t req = static_cast<uint8_t>(mode);
    uint8_t resp_buf[1] = {0};
    uint8_t resp_type = 0U;
    size_t  resp_len  = 0U;

    esp_err_t ret = SpiMaster::instance().otaTransfer(
        kSpiCanSetModeType,
        &req, 1U,
        &resp_type, resp_buf, sizeof(resp_buf), &resp_len, timeout_ms);
    if (ret != ESP_OK || resp_type != SPI_OTA_RSP_ACK || resp_len < 1U) {
        return -1;
    }
    if (actual != nullptr) {
        *actual = static_cast<can_rx_mode_t>(resp_buf[0]);
    }
    return 0;
}
