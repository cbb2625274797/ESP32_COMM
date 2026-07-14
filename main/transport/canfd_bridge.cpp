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
                               uint32_t timeout_ms,
                               uint8_t *gd32_err_out)
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
    /* 两步法：transact 发命令（丢弃延迟一拍的过期响应）+ otaPollResponse 读真实响应。
     * CAN 帧非幂等，命令只发一次，不重发。 */
    esp_err_t ret = SpiMaster::instance().transact(
        kSpiCanFrameType,
        tx_buf, tx_len,
        &resp_type, recv_buf, recv_max, &resp_len, timeout_ms);

    if (ret != ESP_OK) {
        DEBUG_WARN("CAN", "CAN_SEND SPI失败: ch=%u id=0x%08lX err=%s",
                   (unsigned)channel, (unsigned long)ext_id, esp_err_to_name(ret));
        if (gd32_err_out) *gd32_err_out = 0U;  /* SPI 失败时清零 */
        return -2;  /* SPI 通信失败 */
    }
    if (resp_type == SPI_OTA_RSP_ACK) {
        if (recv_buf == nullptr || recv_max == 0U) {
            if (gd32_err_out) *gd32_err_out = 0U;
            return static_cast<int>(resp_len);
        }
        const size_t copied_len = (resp_len < recv_max) ? resp_len : recv_max;
        if (gd32_err_out) *gd32_err_out = 0U;
        return static_cast<int>(copied_len);
    }
    /* resp_type == ERROR: 读取 GD32 返回的错误码 */
    uint8_t gd32_err = (resp_len > 0 && recv_buf) ? recv_buf[0] : 0xFF;
    if (gd32_err_out) *gd32_err_out = gd32_err;  /* 透传细分错误码 */
    DEBUG_WARN("CAN", "CAN_SEND GD32拒绝: ch=%u id=0x%08lX resp=0x%02X err=0x%02X",
               (unsigned)channel, (unsigned long)ext_id, resp_type, gd32_err);
    return -3;  /* GD32 返回 ERROR */
}

constexpr uint8_t kSpiCanFeedbackType   = 0xC1U;
constexpr uint8_t kSpiCanSetModeType    = 0xC2U;
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
 *        gd32_err_out 在返回 -3 时输出 GD32 细分错误码，其它情况清零。
 */
int canFdSend(can_channel_t channel, uint32_t ext_id,
              const uint8_t *data, uint8_t dlc,
              uint8_t *recv_buf, uint8_t recv_max,
              uint32_t timeout_ms,
              uint8_t *gd32_err_out)
{
    if (channel >= CAN_CHANNEL_MAX || dlc > CAN_MAX_DLC || !data) {
        if (gd32_err_out) *gd32_err_out = 0U;
        return -1;
    }
    return sendCanFrameOverSpi(channel, ext_id, data, dlc, recv_buf, recv_max, timeout_ms, gd32_err_out);
}

/**
 * @brief 解析 GD32 主动推送的一页 CAN 反馈 payload，累积到结果结构中。
 *
 * GD32 APP 通过 requestEsp32Read(0xC1) 主动推送每页反馈，ESP32 收到后
 * 调用本函数解析：
 *   page=0 → 重置 out，填充 28 字节固定头 + 首批条目；
 *   page=1 → 在已有条目后追加。
 * 调用方按通道维护 entryIdx，page=0 时会被重置为 0。
 */
int canFdParseFeedbackPage(const uint8_t *payload, size_t len,
                           can_feedback_result_t *out, uint8_t *entryIdx)
{
    if (payload == nullptr || out == nullptr || entryIdx == nullptr) {
        return -1;
    }

    constexpr uint8_t kFixedHeader = 28U;
    if (len < kFixedHeader) {
        return -1;
    }

    const uint8_t channel = payload[0];
    const uint8_t page    = payload[1];
    if (channel >= CAN_CHANNEL_MAX) {
        return -1;
    }

    /* page=0：重置并解析固定头 */
    if (page == 0U) {
        *entryIdx = 0U;
        out->stats.channel        = channel;
        out->stats.mode           = payload[4];
        out->stats.snapshot_seq   = readU32Le(&payload[5]);
        out->stats.rx_count       = readU32Le(&payload[9]);
        out->stats.tx_count       = readU32Le(&payload[13]);
        out->stats.rx_drop_count  = readU32Le(&payload[17]);
        out->stats.error_count    = readU32Le(&payload[21]);
        out->stats.tec            = payload[25];
        out->stats.rec            = payload[26];
        out->stats.bus_state      = payload[27];
        out->total_count          = payload[2];
    }

    /* 解析本页条目并追加 */
    const uint8_t entryCount = payload[3];
    uint16_t offset = kFixedHeader;
    for (uint8_t i = 0U; i < entryCount && *entryIdx < CANFD_ID_SNAPSHOT_MAX; ++i) {
        if (offset + 8U > len) break;

        id_snapshot_entry_t &e = out->entries[*entryIdx];
        e.id  = readU32Le(&payload[offset]);
        e.dlc = payload[offset + 4U];
        const uint8_t flags = payload[offset + 5U];
        e.fd_flag = (flags & 0x01U) ? 1U : 0U;
        e.brs     = (flags & 0x02U) ? 1U : 0U;
        e.esi     = (flags & 0x04U) ? 1U : 0U;

        const uint8_t dataLen = (e.dlc < CANFD_ID_SNAPSHOT_DATA_MAX)
                                ? e.dlc : static_cast<uint8_t>(CANFD_ID_SNAPSHOT_DATA_MAX);
        const uint16_t entrySize = static_cast<uint16_t>(8U + dataLen);
        if (offset + entrySize > len) break;

        memcpy(e.data, &payload[offset + 8U], dataLen);
        for (uint8_t d = dataLen; d < CANFD_ID_SNAPSHOT_DATA_MAX; ++d) {
            e.data[d] = 0U;
        }

        offset += entrySize;
        (*entryIdx)++;
    }

    out->valid = 1;
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

    /* 两步法破解延迟一拍：发命令一次 + poll 真实响应。
     * 0xC2 幂等，但两步法避免无效重试。 */
    esp_err_t ret = SpiMaster::instance().transact(
        kSpiCanSetModeType,
        &req, 1U,
        &resp_type, resp_buf, sizeof(resp_buf), &resp_len, timeout_ms);

    if (ret == ESP_OK && resp_type == SPI_OTA_RSP_ACK && resp_len >= 1U) {
        if (actual != nullptr) {
            *actual = static_cast<can_rx_mode_t>(resp_buf[0]);
        }
        return 0;
    }
    return -1;
}
