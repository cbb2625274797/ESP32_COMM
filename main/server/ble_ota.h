/*
 * ble_ota.h - BLE OTA GATT 服务器
 * T5.1 - T5.5
 *
 * ESP32 本机 OTA 的写入/收尾逻辑已抽离到 Esp32OtaCore（WiFi/BLE 共用）。
 * 本类仅保留 BLE 传输层相关状态（连接、收尾任务去重、ACK 窗口）。
 */
#ifndef BLE_OTA_H
#define BLE_OTA_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_ota_ops.h"

class BleOta {
public:
    static BleOta& instance();

    static void taskEntry(void *pvParameters);
    esp_err_t serverStart();
    esp_err_t notifyResponse(const uint8_t *data, size_t len);
    bool isConnected();
    void signalRelay();

    // 以下状态为 public，供 C 回调函数访问
    bool m_bleReady = false;
    bool m_bleConnected = false;
    uint16_t m_connHandle = 0xFFFF;

    // BLE 传输相关（OTA 写入状态已移至 Esp32OtaCore）
    bool m_esp32RebootPending = false;   // 收尾任务去重标志
    uint8_t m_esp32LastCmd = 0;          // 最后一次数据包命令（用于补发 ACK）
    uint16_t m_esp32LastSeq = 0xFFFFU;   // 最后一次数据包序号（用于补发 ACK）
};

#endif // BLE_OTA_H
