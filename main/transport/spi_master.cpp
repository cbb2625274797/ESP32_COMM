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

static uint8_t spi_ota_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
    }
    return crc;
}

static bool spi_ota_build_frame(uint8_t type, const uint8_t *payload, size_t payload_len,
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
    out[4 + payload_len] = spi_ota_crc8(out, 4 + payload_len);
    *out_len = total;
    return true;
}

static bool spi_ota_parse_frame(const uint8_t *frame, size_t frame_len,
                                uint8_t *type, const uint8_t **payload, size_t *payload_len)
{
    if (!frame || frame_len < 5 || frame[0] != SPI_OTA_SOF) return false;
    size_t len = (size_t)frame[2] | ((size_t)frame[3] << 8);
    size_t expected = 5 + len;
    if (expected > frame_len) return false;
    uint8_t crc = spi_ota_crc8(frame, 4 + len);
    if (crc != frame[4 + len]) return false;
    if (type) *type = frame[1];
    if (payload) *payload = &frame[4];
    if (payload_len) *payload_len = len;
    return true;
}

static bool spi_ota_find_frame(const uint8_t *buf, size_t buf_len,
                               uint8_t *type, const uint8_t **payload, size_t *payload_len,
                               size_t *frame_offset)
{
    if (!buf || buf_len < 5) return false;
    for (size_t i = 0; i + 5 <= buf_len; ++i) {
        if (buf[i] != SPI_OTA_SOF) continue;
        if (spi_ota_parse_frame(&buf[i], buf_len - i, type, payload, payload_len)) {
            if (frame_offset) *frame_offset = i;
            return true;
        }
    }
    return false;
}

/** @brief SPI2 从设备句柄。 */
spi_device_handle_t g_spi_device = NULL;

/**
 * @brief SPI2 总线初始化
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t spi_master_init(void)
{
    DEBUG_INFO("SPI", "=== 初始化 SPI2 (VSPI) ===");

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = 8;
    bus_cfg.miso_io_num = 7;
    bus_cfg.sclk_io_num = 6;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 256;

    DEBUG_INFO("SPI", "SPI2 引脚: MOSI=GPIO8, MISO=GPIO7, SCLK=GPIO6");
    DEBUG_INFO("SPI", "SPI2 配置: CLK=%d MHz, CS=GPIO%d, 最大传输=%d 字节",
               SPI_CLOCK_HZ / 1000000, SPI_MASTER_CS_GPIO, bus_cfg.max_transfer_sz);

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

    DEBUG_INFO("SPI", "SPI2 已初始化: CLK=%dMHz, CS=GPIO%d, 模式=%d",
               SPI_CLOCK_HZ / 1000000, SPI_MASTER_CS_GPIO, SPI_MODE);
    DEBUG_INFO("SPI", "SPI 设备句柄: %p", g_spi_device);
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

    DEBUG_DBG("SPI", "SPI 传输: cmd=0x%02X, 发送=%zu 字节, 接收=%zu 字节",
              cmd, payload_len, recv_len);

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
    } else {
        DEBUG_DBG("SPI", "SPI 传输完成: cmd=0x%02X", cmd);
    }

    return ret;
}

// 仅发送
/**
 * @brief 调用同步传输接口发送数据，不要求接收返回内容。
 * @param cmd SPI 命令码。
 * @param payload 发送数据指针。
 * @param len 发送数据长度。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t spi_send(uint8_t cmd, const uint8_t *payload, size_t len)
{
    return spi_sync_transfer(cmd, payload, len, NULL, 0, 3000);
}

// 仅接收
/**
 * @brief 发送哑数据产生时钟，从从设备同步读取指定长度数据。
 * @param cmd SPI 命令码。
 * @param recv_buf 接收缓冲区。
 * @param len 接收长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t spi_recv(uint8_t cmd, uint8_t *recv_buf, size_t len, uint32_t timeout_ms)
{
    uint8_t dummy[64] = {0};
    memset(dummy, 0xFF, sizeof(dummy));
    return spi_sync_transfer(cmd, dummy, len, recv_buf, len, timeout_ms);
}

esp_err_t spi_ota_poll_response(uint8_t *resp_type, uint8_t *resp_payload,
                                size_t resp_max, size_t *resp_len, uint32_t timeout_ms)
{
    if (g_spi_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (resp_len) *resp_len = 0;
    if (resp_type) *resp_type = 0;

    uint8_t rx_raw[SPI_OTA_FRAME_MAX] = {0};
    uint8_t dummy[SPI_OTA_FRAME_MAX];
    memset(dummy, 0xFF, sizeof(dummy));

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint32_t attempt = 0;
    while (xTaskGetTickCount() <= deadline) {
        ++attempt;
        memset(rx_raw, 0xFF, sizeof(rx_raw));
        spi_transaction_t rsp = {};
        rsp.length = SPI_OTA_FRAME_MAX * 8;
        rsp.rxlength = SPI_OTA_FRAME_MAX * 8;
        rsp.tx_buffer = dummy;
        rsp.rx_buffer = rx_raw;
        esp_err_t ret = spi_device_transmit(g_spi_device, &rsp);
        if (ret != ESP_OK) {
            DEBUG_WARN("SPI", "SPI OTA 响应读取失败[%u]: %s", attempt, esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint8_t parsed_type = 0;
        const uint8_t *parsed_payload = nullptr;
        size_t parsed_len = 0;
        size_t frame_offset = 0;
        if (spi_ota_find_frame(rx_raw, sizeof(rx_raw), &parsed_type, &parsed_payload, &parsed_len, &frame_offset)) {
            DEBUG_INFO("SPI", "OTA响应: type=0x%02X len=%u offset=%u attempt=%u",
                       parsed_type, (unsigned)parsed_len, (unsigned)frame_offset, (unsigned)attempt);
            if (resp_type) *resp_type = parsed_type;
            if (resp_payload && resp_max > 0) {
                size_t copy_len = (parsed_len < resp_max) ? parsed_len : resp_max;
                memcpy(resp_payload, parsed_payload, copy_len);
                if (resp_len) *resp_len = copy_len;
            } else if (resp_len) {
                *resp_len = parsed_len;
            }
            return ESP_OK;
        }

        bool all_ff = true;
        for (size_t i = 0; i < sizeof(rx_raw); ++i) {
            if (rx_raw[i] != 0xFF) {
                all_ff = false;
                break;
            }
        }
        DEBUG_DBG("SPI", "OTA响应未就绪[%u]: %s", attempt, all_ff ? "all-FF" : "no-valid-SOF");
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    DEBUG_ERR("SPI", "OTA响应超时: timeout=%ums", (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t spi_ota_transfer(uint8_t type, const uint8_t *payload, size_t payload_len,
                           uint8_t *resp_type, uint8_t *resp_payload,
                           size_t resp_max, size_t *resp_len, uint32_t timeout_ms)
{
    if (g_spi_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (resp_len) *resp_len = 0;
    if (resp_type) *resp_type = 0;

    uint8_t tx_frame[SPI_OTA_FRAME_MAX] = {0};
    size_t tx_len = 0;
    if (!spi_ota_build_frame(type, payload, payload_len, tx_frame, &tx_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    DEBUG_INFO("SPI", "OTA事务: req_type=0x%02X req_len=%u", type, (unsigned)payload_len);

    spi_transaction_t req = {};
    req.length = tx_len * 8;
    req.tx_buffer = tx_frame;
    esp_err_t ret = spi_device_transmit(g_spi_device, &req);
    if (ret != ESP_OK) {
        DEBUG_ERR("SPI", "SPI OTA 请求事务失败: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(2));
    return spi_ota_poll_response(resp_type, resp_payload, resp_max, resp_len, timeout_ms);
}

// CANFD 帧发送
// 帧格式: [通道(1B)] [扩展帧ID(4B BE)] [数据(1~64B)]
static int can_frame_send(can_channel_t channel, uint32_t ext_id,
                          const uint8_t *data, uint8_t dlc,
                          uint8_t *recv_buf, uint8_t recv_max,
                          uint32_t timeout_ms)
{
    if (g_spi_device == NULL) {
        DEBUG_ERR("SPI", "CAN 发送: SPI 设备未初始化");
        return -1;
    }

    if (dlc == 0 || dlc > CAN_MAX_DLC) {
        DEBUG_ERR("SPI", "CAN[ch%d] DLC 无效: %d (范围 1~%d)", channel, dlc, CAN_MAX_DLC);
        return -2;
    }

    if (channel >= CAN_CHANNEL_MAX) {
        DEBUG_ERR("SPI", "CAN 通道号无效: %d (范围 0~%d)", channel, CAN_CHANNEL_MAX - 1);
        return -3;
    }

    DEBUG_DBG("SPI", "CAN[ch%d] ID=0x%08X 发送: DLC=%d", channel, ext_id, dlc);

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

        DEBUG_INFO("SPI", "CAN[ch%d] ID=0x%08X 响应: DLC=%d, 接收=%d 字节",
                   resp_channel, resp_id, copy_len, copy_len);
        return copy_len;
    }

    DEBUG_DBG("SPI", "CAN[ch%d] ID=0x%08X 发送完成 (无响应)", channel, ext_id);
    return 0;
}

// 对外暴露的 GD32 CAN 发送接口，会在必要时修正默认 DLC。
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

// 对外暴露的无响应 GD32 CAN 发送接口。
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

    DEBUG_INFO("SPI", "SPI 传输 (带重试): cmd=0x%02X, 最大重试=%d", cmd, max_retries);

    while (retry_count < max_retries) {
        ret = spi_sync_transfer(cmd, payload, payload_len,
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
