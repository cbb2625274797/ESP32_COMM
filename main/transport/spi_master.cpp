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
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "soc/spi_struct.h"
#include "esp_err.h"

#include "spi_master.h"
#include "monitor/debug_log.h"

static constexpr size_t kSpiTransferMaxBytes = 256U;

// ============================================================
// 单例
// ============================================================
/**
 * @brief 获取 SpiMaster 单例实例（单例模式）。
 * @return SpiMaster 单例引用。
 */
SpiMaster& SpiMaster::instance()
{
    static SpiMaster inst;
    return inst;
}

// ============================================================
// 内部 SPI 传输
// ============================================================
/**
 * @brief 执行底层 SPI 时钟同步传输。
 * @param tx_buf 发送数据缓冲区，可为 NULL。
 * @param tx_len 发送数据长度。
 * @param rx_buf 接收数据缓冲区，可为 NULL。
 * @param rx_len 接收数据长度。
 * @param timeout_ms 超时时间（毫秒），0 表示无限等待。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::transferClocked(const uint8_t *tx_buf, size_t tx_len,
                                     uint8_t *rx_buf, size_t rx_len,
                                     uint32_t timeout_ms)
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

    TickType_t ticks = (timeout_ms == 0U) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    esp_err_t ret = spi_device_queue_trans(m_spiDevice, &t, ticks);
    if (ret != ESP_OK) {
        DEBUG_WARN("SPI", "spi_device_queue_trans fail: %s len=%u",
                   esp_err_to_name(ret), (unsigned)(total_len * 8U));
        return ret;
    }

    spi_transaction_t *ret_trans = NULL;
    ret = spi_device_get_trans_result(m_spiDevice, &ret_trans, ticks);
    (void)ret_trans;
    if (ret != ESP_OK) {
        DEBUG_WARN("SPI", "spi_device_get_trans_result timeout/fail: %s len=%u tx=%p rx=%p",
                   esp_err_to_name(ret), (unsigned)(total_len * 8U), t.tx_buffer, t.rx_buffer);
        return ret;
    }

    if (rx_len > 0U && rx_ptr != rx_buf) {
        memcpy(rx_buf, rx_ptr, rx_len);
    }
    return ESP_OK;
}

/**
 * @brief 执行 SPI 请求-响应组合传输。
 * @param cmd 命令字节（0 表示无命令）。
 * @param payload 负载数据缓冲区。
 * @param payload_len 负载数据长度。
 * @param rx_buf 接收数据缓冲区。
 * @param rx_len 期望接收的数据长度。
 * @param timeout_ms 超时时间（毫秒），0 表示无限等待。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::transferRequestResponse(uint8_t cmd,
                                             const uint8_t *payload, size_t payload_len,
                                             uint8_t *rx_buf, size_t rx_len,
                                             uint32_t timeout_ms)
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
                                    (rx_len > 0U) ? total_len : 0U,
                                    timeout_ms);
    if (ret == ESP_OK && rx_len > 0U) {
        memcpy(rx_buf, &rx_staging[cmd_len + payload_len], rx_len);
    }
    return ret;
}

// ============================================================
// OTA 帧工具
// ============================================================
/**
 * @brief 计算 OTA 帧的 8 位 CRC 校验和。
 * @param data 数据缓冲区。
 * @param len 数据长度。
 * @return 计算得到的 CRC 值。
 */
uint8_t SpiMaster::otaCrc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
    }
    return crc;
}

/**
 * @brief 构建 OTA 协议帧。
 * @param type 帧类型。
 * @param payload 负载数据缓冲区。
 * @param payload_len 负载数据长度。
 * @param out 输出帧缓冲区。
 * @param out_len 输出实际帧长度。
 * @return true 表示构建成功，false 表示失败。
 */
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

/**
 * @brief 解析 OTA 协议帧。
 * @param frame 帧数据缓冲区。
 * @param frame_len 帧数据长度。
 * @param type 输出解析得到的帧类型。
 * @param payload 输出指向负载数据的指针。
 * @param payload_len 输出负载数据长度。
 * @return true 表示解析成功，false 表示失败。
 */
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

/**
 * @brief 在数据缓冲区中搜索并解析 OTA 帧。
 * @param buf 数据缓冲区。
 * @param buf_len 数据缓冲区长度。
 * @param type 输出帧类型。
 * @param payload 输出指向负载数据的指针。
 * @param payload_len 输出负载数据长度。
 * @param frame_offset 输出帧在缓冲区中的偏移位置。
 * @return true 表示找到并成功解析，false 表示未找到。
 */
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
        if (buf[i + 4 + plen] != expected_crc) {
            DEBUG_WARN("SPI", "CRC mismatch at offset %u: plen=%u expected=0x%02X actual=0x%02X",
                       (unsigned)i, (unsigned)plen, expected_crc, buf[i + 4 + plen]);
            continue;
        }

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
/**
 * @brief 初始化 SPI2 总线与目标从设备。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::init()
{
    LockGuard lock(m_mutex);
    // 创建 SPI 总线互斥锁（必须在 SPI 设备初始化之前）
    m_mutex = xSemaphoreCreateMutex();
    if (m_mutex == NULL) {
        DEBUG_ERR("SPI", "SPI 互斥锁创建失败");
        return ESP_ERR_NO_MEM;
    }

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
/**
 * @brief 执行一次同步 SPI 事务，先发送请求，再读取响应字节。
 * @param cmd 命令字节。
 * @param payload 负载数据缓冲区。
 * @param payload_len 负载数据长度。
 * @param recv_buf 接收数据缓冲区。
 * @param recv_len 接收数据长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::syncTransfer(uint8_t cmd, const uint8_t *payload,
                                  size_t payload_len, uint8_t *recv_buf,
                                  size_t recv_len, uint32_t timeout_ms)
{
    LockGuard lock(m_mutex);
    return transferRequestResponse(cmd, payload, payload_len, recv_buf, recv_len, timeout_ms);
}

/**
 * @brief 仅发送 SPI 请求数据，不关心返回内容。
 * @param cmd 命令字节。
 * @param payload 负载数据缓冲区。
 * @param len 负载数据长度。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::send(uint8_t cmd, const uint8_t *payload, size_t len)
{
    LockGuard lock(m_mutex);
    return transferRequestResponse(cmd, payload, len, NULL, 0, 0U);
}

/**
 * @brief 仅接收 SPI 数据，必要时先发送命令字节。
 * @param cmd 命令字节。
 * @param recv_buf 接收数据缓冲区。
 * @param len 接收数据长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::recv(uint8_t cmd, uint8_t *recv_buf, size_t len, uint32_t timeout_ms)
{
    LockGuard lock(m_mutex);
    return transferRequestResponse(cmd, NULL, 0, recv_buf, len, timeout_ms);
}

// ============================================================
// SPI OTA 协议
// ============================================================
/**
 * @brief 按 GD32 SPI OTA 最小协议执行一次请求加响应读取。
 * @param type 请求帧类型。
 * @param payload 负载数据缓冲区。
 * @param payload_len 负载数据长度。
 * @param resp_type 输出响应类型。
 * @param resp_payload 输出响应负载缓冲区。
 * @param resp_max 响应负载缓冲区最大长度。
 * @param resp_len 输出实际响应长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::otaTransfer(uint8_t type, const uint8_t *payload, size_t payload_len,
                                 uint8_t *resp_type, uint8_t *resp_payload,
                                 size_t resp_max, size_t *resp_len, uint32_t timeout_ms)
{
    LockGuard lock(m_mutex);
    uint8_t tx_frame[SPI_OTA_FRAME_MAX];
    size_t tx_len = 0;
    if (!otaBuildFrame(type, payload, payload_len, tx_frame, &tx_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    DEBUG_DBG("SPI", "OTA TX: type=0x%02X len=%u first4=%02X%02X%02X%02X",
              type, (unsigned)tx_len, tx_frame[0], tx_frame[1], tx_frame[2], tx_frame[3]);
    esp_err_t ret = transferClocked(tx_frame, tx_len, NULL, 0, timeout_ms);
    if (ret != ESP_OK) {
        DEBUG_WARN("SPI", "OTA 请求发送失败: %s", esp_err_to_name(ret));
        return ret;
    }

    return otaPollResponseUnlocked(resp_type, resp_payload, resp_max, resp_len, timeout_ms);
}

/**
 * @brief 仅执行 SPI OTA 响应读取事务，不重复发送请求帧。
 * @param resp_type 输出响应类型。
 * @param resp_payload 输出响应负载缓冲区。
 * @param resp_max 响应负载缓冲区最大长度。
 * @param resp_len 输出实际响应长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::otaPollResponse(uint8_t *resp_type, uint8_t *resp_payload,
                                     size_t resp_max, size_t *resp_len, uint32_t timeout_ms)
{
    LockGuard lock(m_mutex);
    return otaPollResponseUnlocked(resp_type, resp_payload, resp_max, resp_len, timeout_ms);
}

/**
 * @brief 无锁版本的 OTA 响应轮询实现。
 * @param resp_type 输出响应类型。
 * @param resp_payload 输出响应负载缓冲区。
 * @param resp_max 响应负载缓冲区最大长度。
 * @param resp_len 输出实际响应长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::otaPollResponseUnlocked(uint8_t *resp_type, uint8_t *resp_payload,
                                             size_t resp_max, size_t *resp_len, uint32_t timeout_ms)
{
    constexpr size_t kPollBytes = 96U;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() <= deadline) {
        uint8_t rx_raw[kPollBytes];
        uint8_t dummy[kPollBytes];
        memset(dummy, 0xFF, sizeof(dummy));

        /* 用剩余时间作为单次 SPI 事务超时，避免总等待远超 deadline */
        TickType_t now = xTaskGetTickCount();
        uint32_t remain_ms = (deadline > now) ? ((deadline - now) * portTICK_PERIOD_MS) : 0U;
        if (remain_ms == 0U) break;

        esp_err_t ret = transferClocked(dummy, sizeof(dummy), rx_raw, sizeof(rx_raw), remain_ms);
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

/**
 * @brief 通知 GD32 DCU_APP 设置 OTA 标志并软复位进入 Bootloader。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::appRequestBootloader()
{
    LockGuard lock(m_mutex);
    DEBUG_INFO("SPI", "请求 GD32 APP 进入 Bootloader...");

    constexpr uint32_t kMaxRetries = 3U;
    constexpr uint32_t kPerRetryAckMs = 200U;
    constexpr uint32_t kRetryGapMs = 50U;

    for (uint32_t retry = 0U; retry < kMaxRetries; ++retry) {
        if (retry > 0U) {
            DEBUG_WARN("SPI", "Bootloader 请求重发 (%lu/%lu)", (unsigned long)retry, (unsigned long)kMaxRetries);
            vTaskDelay(pdMS_TO_TICKS(kRetryGapMs));
        }

        uint8_t cmd = SPI_APP_ENTER_BOOT_CMD;
        esp_err_t ret = transferClocked(&cmd, 1, NULL, 0);
        if (ret != ESP_OK) {
            DEBUG_WARN("SPI", "发送 Bootloader 请求失败: %s", esp_err_to_name(ret));
            continue;
        }

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kPerRetryAckMs);
        while (xTaskGetTickCount() <= deadline) {
            uint8_t rx = 0;
            uint8_t dummy = 0xFF;
            ret = transferClocked(&dummy, 1, &rx, 1);
            if (ret == ESP_OK && rx == SPI_APP_ENTER_BOOT_ACK_OK) {
                DEBUG_INFO("SPI", "GD32 APP 已确认进入 Bootloader (retry=%lu)", (unsigned long)retry);
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    DEBUG_WARN("SPI", "GD32 APP 进入 Bootloader 确认超时 (retries exhausted)");
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 向 GD32 APP 发送存活探测命令，等待 APP 回复 ALIVE ACK。
 * @param timeout_ms 超时时间（毫秒）。
 * @param uptime_sec 输出设备运行时间（秒）。
 * @param version 输出 GD32 版本信息（可选）。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::appPing(uint32_t timeout_ms, uint32_t *uptime_sec, gd32_version_t *version)
{
    LockGuard lock(m_mutex);
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

        /* 给 GD32 充足时间处理 CS 释放中断并准备响应帧 */
        vTaskDelay(pdMS_TO_TICKS(3));

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

        bool found = false;

        /* GD32 SPI 从机字节偏移不可预测，直接在 raw buffer 中
         * 搜索完整的标签化 PING 响应模式：
         *   0xA1 [0x55...] app_vers [0xAA...] boot_vers
         * 找到 0xA1 后跳过重复字节，验证 0x55 和 0xAA 标签。
         */
        for (size_t i = 0; i + 14 < sizeof(rx_raw); ++i) {
            if (rx_raw[i] != kAppPingType) continue;  /* 找 0xA1 */
            size_t p = i + 1;
            /* 跳过最多 1 个重复的 A1 字节（SPI 偏移最多产生 1 个重复） */
            if (p < sizeof(rx_raw) && rx_raw[p] == kAppPingType) ++p;
            if (p >= sizeof(rx_raw) || rx_raw[p] != 0x55U) continue;
            /* 跳过最多 1 个重复的 0x55 */
            if (p + 1 < sizeof(rx_raw) && rx_raw[p + 1] == 0x55U) ++p;
            size_t app_start = p + 1;  /* APP 版本起始 */
            if (app_start + 4 > sizeof(rx_raw)) continue;
            size_t bp = app_start + 4;
            /* 跳过最多 1 个重复字节 */
            if (bp < sizeof(rx_raw) && bp > 0 && rx_raw[bp] == rx_raw[bp - 1]) ++bp;
            if (bp >= sizeof(rx_raw) || rx_raw[bp] != 0xAAU) continue;
            /* 跳过最多 1 个重复的 0xAA */
            if (bp + 1 < sizeof(rx_raw) && rx_raw[bp + 1] == 0xAAU) ++bp;
            size_t boot_start = bp + 1;
            if (boot_start + 4 > sizeof(rx_raw)) continue;

            /* 模式匹配成功！提取版本数据 */
            found = true;

            if (version) {
                memset(version, 0, sizeof(gd32_version_t));
                version->app_major = rx_raw[app_start];
                version->app_minor = rx_raw[app_start + 1];
                version->app_patch = rx_raw[app_start + 2];
                version->app_build = rx_raw[app_start + 3];
                version->boot_major = rx_raw[boot_start];
                version->boot_minor = rx_raw[boot_start + 1];
                version->boot_patch = rx_raw[boot_start + 2];
                version->boot_build = rx_raw[boot_start + 3];
            }
            if (uptime_sec) {
                size_t ut = boot_start + 4;
                if (ut + 3 < sizeof(rx_raw)) {
                    *uptime_sec = (uint32_t)rx_raw[ut]
                                | ((uint32_t)rx_raw[ut + 1] << 8)
                                | ((uint32_t)rx_raw[ut + 2] << 16)
                                | ((uint32_t)rx_raw[ut + 3] << 24);
                } else {
                    *uptime_sec = 0;
                }
            }
            DEBUG_WARN("SPI", "APP PING 标签匹配: A1@%u APP@%u BOOT@%u",
                       (unsigned)i, (unsigned)app_start, (unsigned)boot_start);
            break;
        }

        if (found) {
            DEBUG_INFO("SPI", "APP PING 成功: uptime=%lu sec, attempt=%lu",
                       uptime_sec ? (unsigned long)*uptime_sec : 0UL,
                       (unsigned long)attempt);
            return ESP_OK;
        }

        DEBUG_WARN("SPI", "APP PING 无匹配[%lu]: first8=%02X%02X%02X%02X%02X%02X%02X%02X",
                  (unsigned long)attempt,
                  rx_raw[0], rx_raw[1], rx_raw[2], rx_raw[3],
                  rx_raw[4], rx_raw[5], rx_raw[6], rx_raw[7]);

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    DEBUG_DBG("SPI", "APP PING 超时: timeout=%ums", (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 执行带重试的 SPI 同步传输。
 * @param cmd 命令字节。
 * @param payload 负载数据缓冲区。
 * @param payload_len 负载数据长度。
 * @param recv_buf 接收数据缓冲区。
 * @param recv_len 接收数据长度。
 * @param max_retries 最大重试次数。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::transferWithRetry(uint8_t cmd, const uint8_t *payload,
                                       size_t payload_len, uint8_t *recv_buf,
                                       size_t recv_len, int max_retries)
{
    LockGuard lock(m_mutex);
    esp_err_t ret;
    int retry_count = 0;

    DEBUG_INFO("SPI", "SPI 传输 (带重试): cmd=0x%02X, 最大重试=%d", cmd, max_retries);

    while (retry_count < max_retries) {
        ret = syncTransferUnlocked(cmd, payload, payload_len,
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
/**
 * @brief 发送一帧 CANFD 数据，可选等待并读取响应内容。
 * @param channel CAN 通道编号（0~2）。
 * @param ext_id 扩展帧 ID（29 位）。
 * @param data CAN 数据负载。
 * @param dlc 数据长度。
 * @param recv_buf 接收数据缓冲区。
 * @param recv_max 接收缓冲区最大长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return 成功时返回接收数据长度，失败返回 -1。
 */
int SpiMaster::canSend(can_channel_t channel, uint32_t ext_id,
                       const uint8_t *data, uint8_t dlc,
                       uint8_t *recv_buf, uint8_t recv_max,
                       uint32_t timeout_ms)
{
    LockGuard lock(m_mutex);
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

    esp_err_t ret = syncTransferUnlocked(0, tx_buf, tx_len, recv_buf, recv_max, timeout_ms);
    if (ret != ESP_OK) {
        return -1;
    }

    return (int)recv_max;
}

/**
 * @brief 发送一帧无需响应的 CANFD 数据。
 * @param channel CAN 通道编号（0~2）。
 * @param ext_id 扩展帧 ID（29 位）。
 * @param data CAN 数据负载。
 * @param dlc 数据长度。
 * @return 成功返回 0，失败返回 -1。
 */
int SpiMaster::canSendNoReply(can_channel_t channel, uint32_t ext_id,
                              const uint8_t *data, uint8_t dlc)
{
    LockGuard lock(m_mutex);
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

    esp_err_t ret = sendUnlocked(0, tx_buf, tx_len);
    if (ret != ESP_OK) {
        return -1;
    }

    return 0;
}

// ============================================================
// 无锁内部版本实现
// ============================================================
/**
 * @brief 无锁版本的同步 SPI 传输实现。
 * @param cmd 命令字节。
 * @param payload 负载数据缓冲区。
 * @param payload_len 负载数据长度。
 * @param recv_buf 接收数据缓冲区。
 * @param recv_len 接收数据长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::syncTransferUnlocked(uint8_t cmd, const uint8_t *payload,
                                          size_t payload_len, uint8_t *recv_buf,
                                          size_t recv_len, uint32_t timeout_ms)
{
    return transferRequestResponse(cmd, payload, payload_len, recv_buf, recv_len, timeout_ms);
}

/**
 * @brief 无锁版本的 SPI 发送实现。
 * @param cmd 命令字节。
 * @param payload 负载数据缓冲区。
 * @param len 负载数据长度。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::sendUnlocked(uint8_t cmd, const uint8_t *payload, size_t len)
{
    return transferRequestResponse(cmd, payload, len, NULL, 0, 0U);
}
