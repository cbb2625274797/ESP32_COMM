/*
 * ble_ota.h - BLE OTA GATT 服务器
 * T5.1 - T5.5
 */
#ifndef BLE_OTA_H
#define BLE_OTA_H

#include "ota_relay.h"

/**
 * @brief BLE OTA 任务入口，负责初始化 NimBLE 并运行 GATT OTA 服务。
 * @param pvParameters FreeRTOS 任务参数，未使用。
 */
void ble_ota_task(void *pvParameters);

/**
 * @brief 启动 BLE GATT 服务器并注册 OTA 相关服务与特征。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t ble_ota_server_start(void);

/**
 * @brief 通过 BLE Notify 将 GD32 的响应数据发送给上位机 APP。
 * @param data 待推送的数据指针。
 * @param len 数据长度（字节）。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t ble_ota_notify_response(const uint8_t *data, size_t len);

/**
 * @brief 检查当前是否存在已连接的 BLE 客户端。
 * @return true 已连接，false 未连接。
 */
bool ble_ota_is_connected(void);

/**
 * @brief 在固件存储完成后通知中继模块开始向 GD32 转发。
 */
void ble_ota_signal_relay(void);

#endif // BLE_OTA_H