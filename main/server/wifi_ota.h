/*
 * wifi_ota.h - WiFi OTA 服务器
 * T4.1 - T4.5
 */
#ifndef WIFI_OTA_H
#define WIFI_OTA_H

#include "ota_relay.h"

// WiFi 模式: AP 或 STA
typedef enum {
    OTA_WIFI_MODE_AP,    // AP 模式（ESP32 作为热点）
    OTA_WIFI_MODE_STA    // STA 模式（连接路由器）
} ota_wifi_mode_t;

/**
 * @brief WiFi OTA 任务入口，负责 HTTP 服务、上传处理和 OTA 触发流程。
 * @param pvParameters FreeRTOS 任务参数，未使用。
 */
void wifi_ota_task(void *pvParameters);

// 在 app_main 早期执行 WiFi 初始化并尝试以 STA 模式联网。
/**
 * @brief 返回 true 表示 STA 初始化已开始，false 表示后续应回退到 AP 模式。
 * @return true STA 初始化中，false 需回退 AP。
 */
bool wifi_early_init(void);

/**
 * @brief 在固件存储完成后通知 OTA 中继开始向 GD32 转发。
 */
void wifi_ota_signal_relay(void);

/**
 * @brief 获取当前 WiFi 工作模式（AP 或 STA）。
 * @return 当前 WiFi 模式枚举值。
 */
ota_wifi_mode_t wifi_get_mode(void);

/**
 * @brief 获取当前 WiFi RSSI，未连接时通常返回无效/默认值。
 * @return 信号强度（dBm），未连接时返回无效值。
 */
int8_t wifi_get_rssi(void);

/**
 * @brief 判断当前 STA 模式下是否已经连接到上级 WiFi。
 * @return true 已连接，false 未连接。
 */
bool wifi_is_connected(void);

#endif // WIFI_OTA_H
