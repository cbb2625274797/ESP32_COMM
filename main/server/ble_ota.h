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
     * @brief WiFi OTA 期间软挂起 BLE（停广播、断连、拒收写；不 deinit 控制器）。
     *        ESP32-C3 上 hard deinit 会打挂 WiFi 共存，导致上传立刻失联。
     * @return ESP_OK 已挂起或本就未运行。
     */
    esp_err_t suspendStack();

    /**
     * @brief WiFi OTA 结束后恢复 BLE 广播（栈仍在则只重新 adv）。
     * @return ESP_OK 成功或无需恢复。
     */
    esp_err_t resumeStack();

    /** @brief 当前是否因 WiFi OTA 挂起 BLE。 */
    bool isSuspendedForWifiOta() const { return m_suspendedForWifiOta; }

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
    /** @brief 获取 BLE OTA 守护任务句柄。 */
    TaskHandle_t getTaskHandle() const { return m_taskHandle; }

    /** @brief 获取 BLE OTA 数据工作线程句柄。 */
    TaskHandle_t getWorkerTaskHandle() const { return m_workerTaskHandle; }

    // 以下状态为 public，供 C 回调函数访问
    bool m_bleReady = false;            ///< BLE GATT 服务注册完成标志
    bool m_bleConnected = false;        ///< BLE 客户端连接状态
    uint16_t m_connHandle = 0xFFFF;     ///< BLE 连接句柄（0xFFFF=未连接）

    // BLE 传输相关（OTA 写入状态已移至 Esp32OtaCore）
    bool m_esp32RebootPending = false;   ///< 收尾任务去重标志
    uint8_t m_esp32LastCmd = 0;          ///< 最后一次数据包命令（用于补发 ACK）
    uint16_t m_esp32LastSeq = 0xFFFFU;   ///< 最后一次数据包序号（用于补发 ACK）
    TaskHandle_t m_taskHandle = nullptr;           ///< BLE OTA 守护任务句柄
    TaskHandle_t m_workerTaskHandle = nullptr;     ///< BLE OTA 数据工作线程句柄
    bool m_stackRunning = false;                   ///< NimBLE 已 init 且 host 在跑
    bool m_suspendedForWifiOta = false;            ///< 因 WiFi OTA 主动挂起
};

// ── 堆低水位快照辅助 getter（不在 BleOta 类内，file-static 资源访问） ──
/** @brief BLE OTA 数据队列当前深度（每个 item 持有 malloc 的 data 缓冲）。
 *        供 /system/heap_low_log 采样。非 OTA 期返回 0。 */
uint32_t ble_ota_get_data_queue_depth();
/** @brief BLE reorder buffer 已缓存的乱序包数（每个 slot 持有 malloc 的 payload）。
 *        供 /system/heap_low_log 采样。非 ESP32 OTA 期返回 0。 */
uint32_t ble_ota_get_reorder_cached_count();

#endif // BLE_OTA_H
