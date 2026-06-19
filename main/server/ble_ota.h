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
#include "freertos/task.h"

class BleOta {
public:
    /**
  * @brief 获取 BleOta 单例实例。
 * @return BleOta 类引用。
 */
    static BleOta& instance();

    /**
  * @brief BLE OTA FreeRTOS 任务入口。
 * @param pvParameters 任务参数，未使用。
 */
    static void taskEntry(void *pvParameters);
    /**
  * @brief 启动 BLE GATT 服务器并注册 OTA 服务。
 * @return ESP_OK 成功，其他失败。
 */
    esp_err_t serverStart();
    /**
  * @brief 通过 BLE Notify 推送响应数据。
 * @param data 待推送的数据指针。
 * @param len 数据长度。
 * @return ESP_OK 成功，其他失败。
 */
    esp_err_t notifyResponse(const uint8_t *data, size_t len);
    /**
  * @brief 检查 BLE 客户端连接状态。
 * @return 已连接返回 true，未连接返回 false。
 */
    bool isConnected();
    /**
  * @brief 触发中继模块向 GD32 转发固件。
 */
    void signalRelay();

    /** @brief 获取 BLE OTA 守护任务句柄。 */
    TaskHandle_t getTaskHandle() const { return m_taskHandle; }

    /** @brief 获取 BLE OTA 数据工作线程句柄。 */
    TaskHandle_t getWorkerTaskHandle() const { return m_workerTaskHandle; }

    // 以下状态为 public，供 C 回调函数访问
    bool m_bleReady = false;
    bool m_bleConnected = false;
    uint16_t m_connHandle = 0xFFFF;

    // BLE 传输相关（OTA 写入状态已移至 Esp32OtaCore）
    bool m_esp32RebootPending = false;   // 收尾任务去重标志
    uint8_t m_esp32LastCmd = 0;          // 最后一次数据包命令（用于补发 ACK）
    uint16_t m_esp32LastSeq = 0xFFFFU;   // 最后一次数据包序号（用于补发 ACK）
    TaskHandle_t m_taskHandle = nullptr;
    TaskHandle_t m_workerTaskHandle = nullptr;
};

#endif // BLE_OTA_H
