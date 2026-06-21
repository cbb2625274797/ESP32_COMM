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

extern SemaphoreHandle_t g_esp32OtaMutex;    ///< ESP32 OTA 全局互斥锁句柄（main.cpp 中创建）
extern volatile bool g_esp32OtaOccupied;     ///< ESP32 OTA 分区占用标志（true=正在OTA）

/**
 * @brief 获取全局 OTA 占用标志（带超时）。
 * @param timeout_ms 等待互斥锁的超时时间（毫秒）。
 * @return true 成功获取并置占用标志，false 已占用或超时。
 */
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

/**
 * @brief 释放全局 OTA 占用标志。
 * @note 内部: Take Mutex → 清除标志 → Give Mutex。
 */
static inline void esp32_ota_mutex_release(void) {
    if (!g_esp32OtaMutex) return;
    xSemaphoreTake(g_esp32OtaMutex, portMAX_DELAY);
    g_esp32OtaOccupied = false;
    xSemaphoreGive(g_esp32OtaMutex);
}
