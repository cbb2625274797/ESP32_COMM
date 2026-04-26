/*
 * ble_ota.h - BLE OTA GATT 服务器
 * T5.1 - T5.5
 */
#ifndef BLE_OTA_H
#define BLE_OTA_H

#include "ota_relay.h"

// BLE OTA 任务入口
void ble_ota_task(void *pvParameters);

// 启动 BLE GATT 服务器
esp_err_t ble_ota_server_start(void);

// 通过 BLE 通知 APP GD32 的响应
esp_err_t ble_ota_notify_response(const uint8_t *data, size_t len);

// 检查 BLE 是否已连接
bool ble_ota_is_connected(void);

// 信号通知中继开始（存储完成后调用）
void ble_ota_signal_relay(void);

#endif // BLE_OTA_H