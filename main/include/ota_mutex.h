/*
 * ota_mutex.h - ESP32 OTA 全局互斥（BLE / WiFi 通道共享）
 *
 * 同一时刻只允许一个 OTA 源（BLE 或 WiFi）操作 ESP32 OTA 分区。
 * 变量定义 + Mutex 创建在 main.cpp app_main()。
 */
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdint>

extern SemaphoreHandle_t g_esp32OtaMutex;
extern volatile bool g_esp32OtaOccupied;

// 获取全局 OTA 占用标志（带超时），成功返回 true
// 内部: Take Mutex → 检查标志 → 若空闲则置 true → Give Mutex
/** @brief 获取全局 OTA 占用标志（带超时），成功返回 true。 */
static inline bool esp32_ota_mutex_acquire(uint32_t timeout_ms) {
    if (!g_esp32OtaMutex) return false;
    if (xSemaphoreTake(g_esp32OtaMutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    if (g_esp32OtaOccupied) {
        xSemaphoreGive(g_esp32OtaMutex);
        return false;
    }
    g_esp32OtaOccupied = true;
    xSemaphoreGive(g_esp32OtaMutex);
    return true;
}

// 释放全局 OTA 占用标志
// 内部: Take Mutex → 清除标志 → Give Mutex
/** @brief 释放全局 OTA 占用标志。 */
static inline void esp32_ota_mutex_release(void) {
    if (!g_esp32OtaMutex) return;
    xSemaphoreTake(g_esp32OtaMutex, portMAX_DELAY);
    g_esp32OtaOccupied = false;
    xSemaphoreGive(g_esp32OtaMutex);
}
