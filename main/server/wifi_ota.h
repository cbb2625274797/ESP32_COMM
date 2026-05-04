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

// WiFi OTA 任务入口
void wifi_ota_task(void *pvParameters);

// ★ WiFi 早期初始化（app_main 调度前调用）
// 包括 NVS、WiFi driver、netif、event handler、配置、连接尝试
// 返回 true = STA 模式启动中，false = 需要在 task 中切换 AP
bool wifi_early_init(void);

// 信号通知 OTA 中继开始（存储完成后调用）
void wifi_ota_signal_relay(void);

// 获取当前 WiFi 模式
ota_wifi_mode_t wifi_get_mode(void);

// 获取 WiFi RSSI
int8_t wifi_get_rssi(void);

// STA 是否已连接
bool wifi_is_connected(void);

#endif // WIFI_OTA_H
