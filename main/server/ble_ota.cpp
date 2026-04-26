/*
 * ble_ota.c - BLE OTA GATT 服务器实现
 * T5.1 - T5.5
 * 
 * 注意: ESP-IDF 6.0 中蓝牙组件有较大变化
 * 此文件暂时提供空实现，待 ESP-IDF 6.0 蓝牙 API 稳定后再适配
 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"

#include "ble_ota.h"
#include "ota_relay.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"

static bool s_ble_connected = false;

/* ESP-IDF 6.0: BLE OTA 功能暂时禁用，以下函数保留供后续适配使用 */
#if 0
// 处理写入控制点
static void handle_ctrl_point_write(uint8_t *data, size_t len)
{
    if (len < 1) return;

    uint8_t cmd = data[0];
    DEBUG_DBG("BLE", "控制点写入: cmd=0x%02X, len=%d", cmd, len);

    if (cmd == 0xF7 && s_ble_connected) {
        ota_meta_t meta;
        meta_load(&meta);
        if (meta.stored_size >= meta.total_size) {
            ble_ota_signal_relay();
        }
    }
}

// 处理写入数据输入（大固件分块）
static void handle_data_in_write(uint8_t *data, size_t len)
{
    if (len < 7) return;

    uint16_t seq = data[1] | (data[2] << 8);
    uint32_t total_size = data[3] | (data[4] << 8) | (data[5] << 16) | (data[6] << 24);

    if (seq == 0 && total_size > 0) {
        ota_meta_t meta = {
            .total_size = total_size,
            .stored_size = 0,
            .last_seq = 0,
            .chunk_count = (total_size + 4095) / 4096
        };
        meta_save(&meta);
        DEBUG_INFO("BLE", "开始固件存储: %" PRIu32 " 字节", total_size);
    }

    size_t data_offset = 7;
    size_t data_len = len - data_offset - 4;

    uint32_t flash_offset = seq * 4096;
    esp_err_t err = flash_write_chunk(flash_offset, &data[data_offset], data_len);
    if (err != ESP_OK) {
        DEBUG_ERR("BLE", "Flash 写入失败 at seq %" PRIu16, seq);
    }

    meta_update(flash_offset + data_len, seq);

    DEBUG_DBG("BLE", "已存储分块 %" PRIu16 ", offset=%" PRIu32, seq, flash_offset);

    ota_meta_t meta;
    meta_load(&meta);
    if (meta.stored_size >= meta.total_size) {
        DEBUG_INFO("BLE", "全部固件已存储，触发转发");
        ble_ota_signal_relay();
    }
}
#endif /* ESP_IDF_COMPILATION */

// 启动 BLE GATT 服务器
// 注意: 此函数在 ESP-IDF 6.0 中暂时返回 ESP_ERR_NOT_SUPPORTED
esp_err_t ble_ota_server_start(void)
{
    DEBUG_WARN("BLE", "BLE OTA 功能在 ESP-IDF 6.0 中暂时不可用");
    DEBUG_WARN("BLE", "请使用 WiFi OTA 功能或等待 BLE API 适配");
    return ESP_ERR_NOT_SUPPORTED;
}

// 通过 BLE 通知 APP GD32 的响应
esp_err_t ble_ota_notify_response(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_ota_is_connected(void)
{
    return s_ble_connected;
}

// 信号通知 OTA 中继开始转发
void ble_ota_signal_relay(void)
{
    if (g_ota_dispatch_queue) {
        uint32_t msg = 1;
        BaseType_t ret = xQueueSend(g_ota_dispatch_queue, &msg, pdMS_TO_TICKS(100));
        if (ret == pdTRUE) {
            DEBUG_INFO("BLE", "OTA 调度信号已通过 BLE 发送");
        }
    }
}

// BLE OTA FreeRTOS 任务
void ble_ota_task(void *pvParameters)
{
    (void)pvParameters;
    DEBUG_WARN("BLE", "BLE OTA 任务启动，但功能暂时不可用");
    
    // 等待调度信号
    while (1) {
        uint32_t msg;
        BaseType_t ret = xQueueReceive(g_ota_dispatch_queue, &msg, pdMS_TO_TICKS(1000));
        if (ret == pdTRUE) {
            DEBUG_INFO("BLE", "收到 OTA 调度信号，但 BLE 功能不可用");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}