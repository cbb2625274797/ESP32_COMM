/*
 * canfd_bridge.cpp - CANFD 3通道通信桥接实现
 *
 * SPI 业务类型：
 *   0xC0 发 CAN 帧（canFdSend）
 *   0xC1 反馈页解析（canFdParseFeedbackPage；由 GD32 主动推送，本文件不轮询）
 *   0xC2 设 RX 模式（canFdSetRxMode）
 *
 * 0xC1 线格式与 GD32 app_spi_cmd_can_feedback.cpp 对齐：
 *   固定头 28B + 条目 id(4LE)+dlc(1)+flags(1)+rsvd(2)+data(dlc)
 *   业务 payload 上限 256 - V2_OVERHEAD(20) = 236
 */
#include <string.h>
#include "canfd_bridge.h"
#include "spi_master.h"
#include "monitor/debug_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr uint8_t kSpiCanFrameType   = 0xC0U;
constexpr uint8_t kSpiCanSetModeType = 0xC2U;
constexpr uint8_t kFeedbackFixedHeader = 28U;
constexpr uint8_t kFeedbackMaxPage     = 1U;  /* GD32 仅 page 0/1 */

/**
 * @brief 经 SPI 发送一帧 CAN，并解析 ACK/ERROR。
 *
 * 两步法：transact 发命令（丢弃延迟一拍陈旧响应）+ poll 真实响应。
 * CAN 帧非幂等，命令只发一次，不重发。
 *
 * @return >=0 ACK 时响应负载长度；-2 SPI 失败；-3 GD32 ERROR。
 */
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
    if (dlc > 0U && data != nullptr) {
        memcpy(&tx_buf[tx_len], data, dlc);
        tx_len += dlc;
    }

    /* 独立缓冲收响应：调用方 recv_buf 为空时也能拿到 ERROR 细分码。 */
    uint8_t local_resp[72];
    uint8_t *spi_rx = (recv_buf != nullptr && recv_max > 0U) ? recv_buf : local_resp;
    const size_t spi_rx_max = (recv_buf != nullptr && recv_max > 0U)
        ? static_cast<size_t>(recv_max)
        : sizeof(local_resp);

    uint8_t resp_type = 0U;
    size_t resp_len = 0U;
    esp_err_t ret = ESP_OK;
    /* CAN 发送只会收到 ACK(0x80) 或 ERROR(0x82)。若读到 WAIT(0x81)，必是上一轮电机 OTA
     * 喂数(0xD1→WAIT)残留在 SPI 响应管道里的陈旧帧（0xC0 命令本身永不产生 WAIT）。
     * 陈旧 WAIT 会被下方误判为 ERROR → resp_len=0 → gd32_err=0xFF → 无声拒绝发送（
     * 表现为“失败一次就再也发不出去”）。此处最多重试几次排空陈旧帧，
     * 直到读到本命令真正的 ACK/ERROR。 */
    for (uint8_t attempt = 0U; attempt < 4U; ++attempt) {
        resp_type = 0U;
        resp_len = 0U;
        ret = SpiMaster::instance().transact(
            kSpiCanFrameType,
            tx_buf, tx_len,
            &resp_type, spi_rx, spi_rx_max, &resp_len, timeout_ms);
        if (ret != ESP_OK || resp_type != SPI_OTA_RSP_WAIT) {
            break;  /* SPI 失败或已拿到真正响应(ACK/ERROR) */
        }
        vTaskDelay(pdMS_TO_TICKS(3));  /* 让 GD32 更新响应，再读一次排空陈旧 WAIT */
    }

    if (ret != ESP_OK) {
        /* 同类错误节流日志，降低高频失败时的日志开销 */
        static uint32_t s_spi_fail_n = 0U;
        static int s_last_spi_err = 0;
        const int err_i = static_cast<int>(ret);
        if (s_spi_fail_n == 0U || err_i != s_last_spi_err || (s_spi_fail_n % 32U) == 0U) {
            DEBUG_WARN("CAN", "SEND SPI fail ch=%u id=%08lX %s x%lu",
                       (unsigned)channel, (unsigned long)ext_id, esp_err_to_name(ret),
                       (unsigned long)(s_spi_fail_n + 1U));
        }
        s_last_spi_err = err_i;
        ++s_spi_fail_n;
        if (gd32_err_out) {
            *gd32_err_out = 0U;
        }
        return -2;
    }
    if (resp_type == SPI_OTA_RSP_ACK) {
        if (gd32_err_out) {
            *gd32_err_out = 0U;
        }
        if (recv_buf == nullptr || recv_max == 0U) {
            return static_cast<int>(resp_len);
        }
        const size_t copied_len = (resp_len < recv_max) ? resp_len : recv_max;
        return static_cast<int>(copied_len);
    }

    /* ERROR：细分码在 payload[0]；不依赖调用方是否提供 recv_buf */
    const uint8_t gd32_err = (resp_len > 0U) ? spi_rx[0] : 0xFFU;
    if (gd32_err_out) {
        *gd32_err_out = gd32_err;
    }
    {
        static uint32_t s_rej_n = 0U;
        static uint8_t s_last_rej = 0xFFU;
        if (s_rej_n == 0U || gd32_err != s_last_rej || (s_rej_n % 32U) == 0U) {
            DEBUG_WARN("CAN", "SEND拒 ch=%u id=%08lX r=%02X e=%02X x%lu",
                       (unsigned)channel, (unsigned long)ext_id, resp_type, gd32_err,
                       (unsigned long)(s_rej_n + 1U));
        }
        s_last_rej = gd32_err;
        ++s_rej_n;
    }
    return -3;
}

/** @brief 小端读 uint32（0xC1 固定头/条目 id）。 */
static inline uint32_t readU32Le(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

/**
 * @brief 发送一帧 CANFD 数据，可选等待并读取响应。
 *
 * 返回值: >=0 成功(接收数据长度), -1 参数错误, -2 SPI通信失败, -3 GD32拒绝。
 * gd32_err_out 在返回 -3 时输出 GD32 细分错误码，其它情况清零。
 * dlc==0 时允许 data 为 nullptr（仅发 ID 的远程帧/空数据帧）。
 */
int canFdSend(can_channel_t channel, uint32_t ext_id,
              const uint8_t *data, uint8_t dlc,
              uint8_t *recv_buf, uint8_t recv_max,
              uint32_t timeout_ms,
              uint8_t *gd32_err_out)
{
    if (channel >= CAN_CHANNEL_MAX || dlc > CAN_MAX_DLC) {
        if (gd32_err_out) {
            *gd32_err_out = 0U;
        }
        return -1;
    }
    /* dlc>0 时 data 必填；dlc==0 允许空指针 */
    if (dlc > 0U && data == nullptr) {
        if (gd32_err_out) {
            *gd32_err_out = 0U;
        }
        return -1;
    }
    return sendCanFrameOverSpi(channel, ext_id, data, dlc,
                               recv_buf, recv_max, timeout_ms, gd32_err_out);
}

/**
 * @brief 解析 GD32 主动推送的一页 CAN 反馈 payload，累积到结果结构中。
 *
 *   page=0 → 清空 entries、重置 entryIdx，填充 28 字节固定头 + 首批条目；
 *   page=1 → 校验 channel 一致后在已有条目后追加。
 * 调用方按通道维护 entryIdx。
 *
 * @return 0 成功；-1 参数/页号/通道/截断导致无法解析。
 */
int canFdParseFeedbackPage(const uint8_t *payload, size_t len,
                           can_feedback_result_t *out, uint8_t *entryIdx)
{
    if (payload == nullptr || out == nullptr || entryIdx == nullptr) {
        return -1;
    }

    if (len < kFeedbackFixedHeader) {
        return -1;
    }

    const uint8_t channel = payload[0];
    const uint8_t page    = payload[1];
    if (channel >= CAN_CHANNEL_MAX) {
        return -1;
    }
    /* GD32 仅 page 0/1；非法页拒绝，避免当追加污染缓冲 */
    if (page > kFeedbackMaxPage) {
        return -1;
    }

    if (page == 0U) {
        /* 防 total 变少时 HTTP 读到上一轮残留 ID */
        *entryIdx = 0U;
        memset(out->entries, 0, sizeof(out->entries));
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
    } else {
        /* page>=1：必须与 page0 已建立的通道一致 */
        if (out->stats.channel != channel) {
            return -1;
        }
    }

    const uint8_t entryCount = payload[3];
    const uint8_t idx_before = *entryIdx;
    uint16_t offset = kFeedbackFixedHeader;

    for (uint8_t i = 0U; i < entryCount && *entryIdx < CANFD_ID_SNAPSHOT_MAX; ++i) {
        if (offset + 8U > len) {
            break;
        }

        id_snapshot_entry_t &e = out->entries[*entryIdx];
        e.id  = readU32Le(&payload[offset]);
        e.dlc = payload[offset + 4U];
        const uint8_t flags = payload[offset + 5U];
        e.fd_flag = (flags & 0x01U) ? 1U : 0U;
        e.brs     = (flags & 0x02U) ? 1U : 0U;
        e.esi     = (flags & 0x04U) ? 1U : 0U;
        /* 线协议不携带 per-ID seq/rx_count，显式置 0 避免脏读 */
        e.seq = 0U;
        e.rx_count = 0U;

        const uint8_t dataLen = (e.dlc < CANFD_ID_SNAPSHOT_DATA_MAX)
                                ? e.dlc : static_cast<uint8_t>(CANFD_ID_SNAPSHOT_DATA_MAX);
        const uint16_t entrySize = static_cast<uint16_t>(8U + dataLen);
        if (offset + entrySize > len) {
            break;
        }

        memcpy(e.data, &payload[offset + 8U], dataLen);
        for (uint8_t d = dataLen; d < CANFD_ID_SNAPSHOT_DATA_MAX; ++d) {
            e.data[d] = 0U;
        }

        offset += entrySize;
        (*entryIdx)++;
    }

    /* 声明了条目但一个都没完整解析 → 截断/坏帧 */
    if (entryCount > 0U && *entryIdx == idx_before) {
        return -1;
    }

    out->valid = 1;
    return 0;
}

/**
 * @brief 切换 GD32 三路 CAN 的接收过滤模式（SPI 0xC2）。
 * @return 0 成功；-1 参数非法或 SPI/应答失败。
 */
int canFdSetRxMode(can_rx_mode_t mode,
                   can_rx_mode_t *actual,
                   uint32_t timeout_ms)
{
    if (mode != CAN_RX_MODE_ALL_PASS && mode != CAN_RX_MODE_FILTER_ID
        && mode != CAN_RX_MODE_BRIDGE_0_TO_12) {
        DEBUG_WARN("CAN", "SET_MODE 非法 mode=%u", (unsigned)mode);
        return -1;
    }

    uint8_t req = static_cast<uint8_t>(mode);
    uint8_t resp_buf[1] = {0};
    uint8_t resp_type = 0U;
    size_t  resp_len  = 0U;

    /* 两步法：发命令一次 + poll 真实响应。0xC2 幂等，但避免无效重试。 */
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

    DEBUG_WARN("CAN", "SET_MODE fail want=%u %s r=0x%02X n=%u",
               (unsigned)mode,
               esp_err_to_name(ret),
               (unsigned)resp_type,
               (unsigned)resp_len);
    return -1;
}
