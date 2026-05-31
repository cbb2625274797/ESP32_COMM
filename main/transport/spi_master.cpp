/*
 * spi_master.cpp - SPI Master 驱动实现
 * T1.1 - T1.4
 * ESP-IDF 6.0 适配版本
 * CANFD 3通道发送接口
 *
 * 所有功能封装为 SpiMaster 类方法。
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

static constexpr size_t kSpiTransferMaxBytes = 256U;

// ============================================================
// 单例
// ============================================================
SpiMaster& SpiMaster::instance()
{
    static SpiMaster inst;
    return inst;
}

// ============================================================
// 内部 SPI 传输
// ============================================================
esp_err_t SpiMaster::transferClocked(const uint8_t *tx_buf, size_t tx_len,
                                     uint8_t *rx_buf, size_t rx_len)
{
    if (m_spiDevice == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if ((tx_len > 0U && tx_buf == NULL) || (rx_len > 0U && rx_buf == NULL)) {
        DEBUG_WARN("SPI", "transferClocked INVALID_ARG: tx_len=%u tx_buf=%p rx_len=%u rx_buf=%p dev=%p",
                   (unsigned)tx_len, tx_buf, (unsigned)rx_len, rx_buf, m_spiDevice);
        return ESP_ERR_INVALID_ARG;
    }

    const size_t total_len = (tx_len > rx_len) ? tx_len : rx_len;
    if (total_len == 0U) {
        return ESP_OK;
    }
    if (total_len > kSpiTransferMaxBytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t tx_staging[kSpiTransferMaxBytes];
    uint8_t rx_staging[kSpiTransferMaxBytes];
    const uint8_t *tx_ptr = tx_buf;
    uint8_t *rx_ptr = rx_buf;

    if (tx_len < total_len) {
        memset(tx_staging, 0xFF, total_len);
        if (tx_buf != NULL && tx_len > 0U) {
            memcpy(tx_staging, tx_buf, tx_len);
        }
        tx_ptr = tx_staging;
    }

    if (rx_len > 0U && rx_len < total_len) {
        rx_ptr = rx_staging;
    }

    spi_transaction_t t = {};
    t.length = total_len * 8U;
    t.rxlength = (rx_len > 0U) ? (total_len * 8U) : 0U;
    t.tx_buffer = tx_ptr;
    t.rx_buffer = (rx_len > 0U) ? rx_ptr : NULL;

    esp_err_t ret = spi_device_transmit(m_spiDevice, &t);
    if (ret != ESP_OK) {
        DEBUG_WARN("SPI", "spi_device_transmit fail: %s len=%u tx=%p rx=%p",
                   esp_err_to_name(ret), (unsigned)(total_len * 8U), t.tx_buffer, t.rx_buffer);
    }

    if (ret == ESP_OK && rx_len > 0U && rx_ptr != rx_buf) {
        memcpy(rx_buf, rx_ptr, rx_len);
    }
    return ret;
}

esp_err_t SpiMaster::transferRequestResponse(uint8_t cmd,
                                             const uint8_t *payload, size_t payload_len,
                                             uint8_t *rx_buf, size_t rx_len)
{
    if ((payload_len > 0U && payload == NULL) || (rx_len > 0U && rx_buf == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t cmd_len = (cmd != 0U) ? 1U : 0U;
    const size_t total_len = cmd_len + payload_len + rx_len;
    if (total_len == 0U) {
        return ESP_OK;
    }
    if (total_len > kSpiTransferMaxBytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t tx_staging[kSpiTransferMaxBytes];
    uint8_t rx_staging[kSpiTransferMaxBytes];
    size_t tx_pos = 0U;

    if (cmd_len > 0U) {
        tx_staging[tx_pos++] = cmd;
    }
    if (payload_len > 0U) {
        memcpy(&tx_staging[tx_pos], payload, payload_len);
        tx_pos += payload_len;
    }
    if (tx_pos < total_len) {
        memset(&tx_staging[tx_pos], 0xFF, total_len - tx_pos);
    }

    esp_err_t ret = transferClocked(tx_staging, total_len,
                                    (rx_len > 0U) ? rx_staging : NULL,
                                    (rx_len > 0U) ? total_len : 0U);
    if (ret == ESP_OK && rx_len > 0U) {
        memcpy(rx_buf, &rx_staging[cmd_len + payload_len], rx_len);
    }
    return ret;
}

// ============================================================
// OTA 帧工具
// ============================================================
uint8_t SpiMaster::otaCrc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
    }
    return crc;
}

bool SpiMaster::otaBuildFrame(uint8_t type, const uint8_t *payload, size_t payload_len,
                              uint8_t *out, size_t *out_len)
{
    if (!out || !out_len) return false;
    size_t total = 5 + payload_len;
    if (total > SPI_OTA_FRAME_MAX) return false;
    out[0] = SPI_OTA_SOF;
    out[1] = type;
    out[2] = (uint8_t)(payload_len & 0xFF);
    out[3] = (uint8_t)((payload_len >> 8) & 0xFF);
    if (payload && payload_len > 0) {
        memcpy(&out[4], payload, payload_len);
    }
    out[4 + payload_len] = otaCrc8(out, 4 + payload_len);
    *out_len = total;
    return true;
}

bool SpiMaster::otaParseFrame(const uint8_t *frame, size_t frame_len,
                              uint8_t *type, const uint8_t **payload, size_t *payload_len)
{
    if (!frame || frame_len < 5) return false;
    if (frame[0] != SPI_OTA_SOF) return false;

    size_t plen = (size_t)frame[2] | ((size_t)frame[3] << 8);
    size_t total = 5 + plen;
    if (frame_len < total) return false;

    uint8_t expected_crc = otaCrc8(frame, 4 + plen);
    if (frame[4 + plen] != expected_crc) return false;

    if (type) *type = frame[1];
    if (payload) *payload = (plen > 0) ? &frame[4] : nullptr;
    if (payload_len) *payload_len = plen;
    return true;
}

bool SpiMaster::otaFindFrame(const uint8_t *buf, size_t buf_len,
                             uint8_t *type, const uint8_t **payload, size_t *payload_len,
                             size_t *frame_offset)
{
    for (size_t i = 0; i + 5 <= buf_len; ++i) {
        if (buf[i] != SPI_OTA_SOF) continue;

        size_t plen = (size_t)buf[i + 2] | ((size_t)buf[i + 3] << 8);
        size_t total = 5 + plen;
        if (i + total > buf_len) continue;

        uint8_t expected_crc = otaCrc8(&buf[i], 4 + plen);
        if (buf[i + 4 + plen] != expected_crc) continue;

        if (type) *type = buf[i + 1];
        if (payload) *payload = (plen > 0) ? &buf[i + 4] : nullptr;
        if (payload_len) *payload_len = plen;
        if (frame_offset) *frame_offset = i;
        return true;
    }
    return false;
}

// ============================================================
// 初始化
// ============================================================
esp_err_t SpiMaster::init()
{
    DEBUG_INFO("SPI", "=== 初始化 SPI2 总线 ===");

    spi_bus_config_t bus_cfg = {};
    bus_cfg.miso_io_num = 7;
    bus_cfg.mosi_io_num = 5;
    bus_cfg.sclk_io_num = 6;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = kSpiTransferMaxBytes;

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
    dev_cfg.cs_ena_posttrans = 10;
    dev_cfg.cs_ena_pretrans = 20;

    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &m_spiDevice);
    if (ret != ESP_OK) {
        DEBUG_ERR("SPI", "SPI 设备添加失败: %s", esp_err_to_name(ret));
        return ret;
    }

    DEBUG_INFO("SPI", "SPI2 总线初始化完成: MOSI=GPIO5, MISO=GPIO7, SCLK=GPIO6, CS=GPIO%d, CLK=%d Hz",
               SPI_MASTER_CS_GPIO, SPI_CLOCK_HZ);

    return ESP_OK;
}

// ============================================================
// 公开 SPI 传输接口
// ============================================================
esp_err_t SpiMaster::syncTransfer(uint8_t cmd, const uint8_t *payload,
                                  size_t payload_len, uint8_t *recv_buf,
                                  size_t recv_len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    return transferRequestResponse(cmd, payload, payload_len, recv_buf, recv_len);
}

esp_err_t SpiMaster::send(uint8_t cmd, const uint8_t *payload, size_t len)
{
    return transferRequestResponse(cmd, payload, len, NULL, 0);
}

esp_err_t SpiMaster::recv(uint8_t cmd, uint8_t *recv_buf, size_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    return transferRequestResponse(cmd, NULL, 0, recv_buf, len);
}

// ============================================================
// SPI OTA 协议
// ============================================================
esp_err_t SpiMaster::otaTransfer(uint8_t type, const uint8_t *payload, size_t payload_len,
                                 uint8_t *resp_type, uint8_t *resp_payload,
                                 size_t resp_max, size_t *resp_len, uint32_t timeout_ms)
{
    uint8_t tx_frame[SPI_OTA_FRAME_MAX];
    size_t tx_len = 0;
    if (!otaBuildFrame(type, payload, payload_len, tx_frame, &tx_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    DEBUG_DBG("SPI", "OTA TX: type=0x%02X len=%u first4=%02X%02X%02X%02X",
              type, (unsigned)tx_len, tx_frame[0], tx_frame[1], tx_frame[2], tx_frame[3]);
    esp_err_t ret = transferClocked(tx_frame, tx_len, NULL, 0);
    if (ret != ESP_OK) {
        DEBUG_WARN("SPI", "OTA 请求发送失败: %s", esp_err_to_name(ret));
        return ret;
    }

    return otaPollResponse(resp_type, resp_payload, resp_max, resp_len, timeout_ms);
}

esp_err_t SpiMaster::otaPollResponse(uint8_t *resp_type, uint8_t *resp_payload,
                                     size_t resp_max, size_t *resp_len, uint32_t timeout_ms)
{
    constexpr size_t kPollBytes = 96U;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() <= deadline) {
        uint8_t rx_raw[kPollBytes];
        uint8_t dummy[kPollBytes];
        memset(dummy, 0xFF, sizeof(dummy));

        esp_err_t ret = transferClocked(dummy, sizeof(dummy), rx_raw, sizeof(rx_raw));
        if (ret != ESP_OK) {
            DEBUG_WARN("SPI", "OTA 响应读取失败: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint8_t found_type = 0;
        const uint8_t *found_payload = nullptr;
        size_t found_len = 0;
        size_t frame_offset = 0;

        if (otaFindFrame(rx_raw, sizeof(rx_raw), &found_type, &found_payload, &found_len, &frame_offset)) {
            /* 有效性检查：ACK/ERROR 帧必须有 payload，空 payload 的 ACK 是帧截断/错位，
             * 继续轮询而非当作有效响应返回。 */
            if ((found_type == SPI_OTA_RSP_ACK || found_type == SPI_OTA_RSP_ERROR) && found_len == 0) {
                DEBUG_DBG("SPI", "OTA 响应帧截断: type=0x%02X len=0 offset=%u, 继续轮询",
                          found_type, (unsigned)frame_offset);
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }

            if (resp_type) *resp_type = found_type;
            if (resp_payload && resp_max > 0) {
                size_t copy_len = (found_len < resp_max) ? found_len : resp_max;
                if (found_payload && copy_len > 0) {
                    memcpy(resp_payload, found_payload, copy_len);
                }
            }
            if (resp_len) *resp_len = found_len;

            DEBUG_DBG("SPI", "OTA 响应: type=0x%02X len=%u offset=%u",
                      found_type, (unsigned)found_len, (unsigned)frame_offset);

            if (found_type == SPI_OTA_RSP_WAIT) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    DEBUG_WARN("SPI", "OTA 响应超时: %lu ms", (unsigned long)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t SpiMaster::appRequestBootloader(uint32_t timeout_ms)
{
    DEBUG_INFO("SPI", "请求 GD32 APP 进入 Bootloader...");

    uint8_t cmd = SPI_APP_ENTER_BOOT_CMD;
    esp_err_t ret = transferClocked(&cmd, 1, NULL, 0);
    if (ret != ESP_OK) {
        DEBUG_WARN("SPI", "发送 Bootloader 请求失败: %s", esp_err_to_name(ret));
        return ret;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() <= deadline) {
        uint8_t rx = 0;
        uint8_t dummy = 0xFF;
        ret = transferClocked(&dummy, 1, &rx, 1);
        if (ret == ESP_OK && rx == SPI_APP_ENTER_BOOT_ACK_OK) {
            DEBUG_INFO("SPI", "GD32 APP 已确认进入 Bootloader");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    DEBUG_WARN("SPI", "GD32 APP 进入 Bootloader 确认超时");
    return ESP_ERR_TIMEOUT;
}

esp_err_t SpiMaster::appPing(uint32_t timeout_ms, uint32_t *uptime_sec)
{
    constexpr uint8_t kAppPingType = 0xA1U;
    constexpr size_t kAppPingPollBytes = 32U;

    uint8_t tx_frame[8] = {0};
    size_t tx_len = 0U;
    if (!otaBuildFrame(kAppPingType, NULL, 0U, tx_frame, &tx_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint32_t attempt = 0U;

    while (xTaskGetTickCount() <= deadline) {
        ++attempt;

        esp_err_t err = transferClocked(tx_frame, tx_len, NULL, 0U);
        if (err != ESP_OK) {
            DEBUG_DBG("SPI", "APP PING 请求发送失败[%lu]: %s",
                      (unsigned long)attempt, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        esp_rom_delay_us(200);

        uint8_t rx_raw[kAppPingPollBytes];
        uint8_t dummy[kAppPingPollBytes];
        memset(dummy, 0xFF, sizeof(dummy));
        memset(rx_raw, 0xFF, sizeof(rx_raw));

        err = transferClocked(dummy, sizeof(dummy), rx_raw, sizeof(rx_raw));
        if (err != ESP_OK) {
            DEBUG_DBG("SPI", "APP PING 响应读取失败[%lu]: %s",
                      (unsigned long)attempt, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        uint8_t resp_type = 0U;
        const uint8_t *resp_payload = nullptr;
        size_t resp_len = 0U;
        size_t frame_offset = 0U;
        if (otaFindFrame(rx_raw, sizeof(rx_raw),
                         &resp_type, &resp_payload, &resp_len, &frame_offset)) {
            if (resp_type == SPI_OTA_RSP_ACK && resp_len >= 3U && resp_payload[0] == kAppPingType) {
                if (uptime_sec) {
                    *uptime_sec = (uint32_t)resp_payload[1] | ((uint32_t)resp_payload[2] << 8);
                }
                DEBUG_INFO("SPI", "APP PING 成功: uptime=%lu sec, attempt=%lu, offset=%u",
                           uptime_sec ? (unsigned long)*uptime_sec : 0UL,
                           (unsigned long)attempt,
                           (unsigned)frame_offset);
                return ESP_OK;
            }

            DEBUG_DBG("SPI", "APP PING 收到无效帧[%lu]: type=0x%02X len=%u offset=%u payload0=0x%02X",
                      (unsigned long)attempt,
                      resp_type,
                      (unsigned)resp_len,
                      (unsigned)frame_offset,
                      resp_len > 0U ? resp_payload[0] : 0xFFU);
        } else {
            DEBUG_DBG("SPI", "APP PING 未找到有效帧[%lu]: first8=%02X%02X%02X%02X%02X%02X%02X%02X",
                      (unsigned long)attempt,
                      rx_raw[0], rx_raw[1], rx_raw[2], rx_raw[3],
                      rx_raw[4], rx_raw[5], rx_raw[6], rx_raw[7]);
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    DEBUG_DBG("SPI", "APP PING 超时: timeout=%ums", (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t SpiMaster::transferWithRetry(uint8_t cmd, const uint8_t *payload,
                                       size_t payload_len, uint8_t *recv_buf,
                                       size_t recv_len, int max_retries)
{
    esp_err_t ret;
    int retry_count = 0;

    DEBUG_INFO("SPI", "SPI 传输 (带重试): cmd=0x%02X, 最大重试=%d", cmd, max_retries);

    while (retry_count < max_retries) {
        ret = syncTransfer(cmd, payload, payload_len,
                           recv_buf, recv_len, 3000);
        if (ret == ESP_OK) {
            if (retry_count > 0) {
                DEBUG_INFO("SPI", "SPI 传输成功 (重试 %d 次后成功)", retry_count);
            }
            return ESP_OK;
        }

        retry_count++;
        DEBUG_WARN("SPI", "SPI 重试 %d/%d, cmd=0x%02X, err=%s",
                   retry_count, max_retries, cmd, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    DEBUG_ERR("SPI", "SPI 传输 %d 次重试后失败, cmd=0x%02X", max_retries, cmd);
    return ESP_ERR_TIMEOUT;
}

// ============================================================
// CANFD 接口
// ============================================================
int SpiMaster::canSend(can_channel_t channel, uint32_t ext_id,
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

    esp_err_t ret = syncTransfer(0, tx_buf, tx_len, recv_buf, recv_max, timeout_ms);
    if (ret != ESP_OK) {
        return -1;
    }

    return (int)recv_max;
}

int SpiMaster::canSendNoReply(can_channel_t channel, uint32_t ext_id,
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

    esp_err_t ret = send(0, tx_buf, tx_len);
    if (ret != ESP_OK) {
        return -1;
    }

    return 0;
}
