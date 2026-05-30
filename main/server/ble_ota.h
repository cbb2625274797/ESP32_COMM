/*
 * ble_ota.h - BLE OTA GATT 服务器
 * T5.1 - T5.5
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
    uint16_t mDataOutValHandle = 0;

    bool m_esp32OtaActive = false;
    esp_ota_handle_t m_esp32OtaHandle = 0;
    const esp_partition_t *m_esp32UpdatePartition = NULL;
    size_t m_esp32Received = 0;
    bool m_esp32RebootPending = false;
    uint8_t m_esp32LastCmd = 0;
    uint16_t m_esp32LastSeq = 0xFFFFU;
};

#endif // BLE_OTA_H