/*
 * health_monitor.cpp - 系统健康监控
 * 包含：健康状态管理 + WiFi RSSI 监控 + 温度监控
 *
 * 所有功能封装为 HealthMonitor 类方法。
 * 所有对 m_healthState / m_lastTemperature / m_lastWifiRssi 的访问
 * 均通过 m_mutex 保护，消除跨任务竞态。
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "driver/temperature_sensor.h"

#include "health_monitor.h"
#include "debug_log.h"
#include "server/wifi_ota.h"

// ============================================================
// 单例
// ============================================================
HealthMonitor& HealthMonitor::instance()
{
    static HealthMonitor inst;
    return inst;
}

// ============================================================
// 初始化
// ============================================================
void HealthMonitor::init()
{
    m_mutex = xSemaphoreCreateMutex();
    if (m_mutex == NULL) {
        DEBUG_ERR("HEALTH", "创建健康状态互斥锁失败");
    }
}

// ============================================================
// 线程安全的健康状态读写
// ============================================================
health_state_t HealthMonitor::getState()
{
    if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
    health_state_t s = m_healthState;
    if (m_mutex) xSemaphoreGive(m_mutex);
    return s;
}

void HealthMonitor::setState(health_state_t newState)
{
    if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
    m_healthState = newState;
    if (m_mutex) xSemaphoreGive(m_mutex);
}

// ============================================================
// 获取当前健康状态（public 接口）
// ============================================================
health_state_t HealthMonitor::getCurrent()
{
    return getState();
}

float HealthMonitor::getLastTemperature()
{
    if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
    float t = m_lastTemperature;
    if (m_mutex) xSemaphoreGive(m_mutex);
    return t;
}

bool HealthMonitor::getLastWifiRssi(int8_t *out_rssi, uint32_t *out_age_ms)
{
    if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
    int64_t lastMs = m_lastWifiRssiMs;
    int8_t  rssi   = m_lastWifiRssi;
    if (m_mutex) xSemaphoreGive(m_mutex);

    if (lastMs <= 0) {
        return false;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    uint32_t age_ms = (now_ms > lastMs) ? (uint32_t)(now_ms - lastMs) : 0;

    if (out_rssi)  *out_rssi  = rssi;
    if (out_age_ms) *out_age_ms = age_ms;
    return true;
}

// ============================================================
// 温度监控（定时器回调）
// ============================================================
void HealthMonitor::tempMonitorCallback(void *arg)
{
    auto& self = instance();
    float temp = 0.0f;
    esp_err_t err = temperature_sensor_get_celsius(self.m_tempHandle, &temp);
    if (err != ESP_OK) {
        DEBUG_WARN("TEMP", "读取温度失败: %s", esp_err_to_name(err));
        return;
    }

    if (self.m_mutex) xSemaphoreTake(self.m_mutex, portMAX_DELAY);
    self.m_lastTemperature = temp;
    if (self.m_mutex) xSemaphoreGive(self.m_mutex);

    if (temp >= TEMP_CRITICAL) {
        self.setState(SYS_SAFE_SHUTDOWN);
        DEBUG_ERR("TEMP", "!!! 危险 !!! 温度 %.1f°C >= %d°C，触发安全关机", temp, TEMP_CRITICAL);
        DEBUG_ERR("TEMP", "健康状态变更为: SYS_SAFE_SHUTDOWN");
    } else if (temp >= TEMP_WARNING) {
        health_state_t cur = self.getState();
        if (cur < SYS_WARNING) {
            self.setState(SYS_WARNING);
            DEBUG_WARN("TEMP", "警告: 温度 %.1f°C >= %d°C", temp, TEMP_WARNING);
            DEBUG_WARN("TEMP", "健康状态变更为: SYS_WARNING");
        }
    }
}

// ============================================================
// FreeRTOS 任务入口
// ============================================================
void HealthMonitor::taskEntry(void *pvParameters)
{
    auto& self = instance();
    DEBUG_INFO("MON", "=== 健康监控任务启动 ===");

    temperature_sensor_config_t temp_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 80);
    esp_err_t err = temperature_sensor_install(&temp_config, &self.m_tempHandle);
    if (err != ESP_OK) {
        DEBUG_ERR("MON", "温度传感器初始化失败: %s", esp_err_to_name(err));
    } else {
        ESP_ERROR_CHECK(temperature_sensor_enable(self.m_tempHandle));
        DEBUG_INFO("MON", "温度传感器已启用 (范围 20°C ~ 80°C)");
    }

    // 定时器，每1秒读取一次温度
    esp_timer_handle_t temp_timer;
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &tempMonitorCallback;
    timer_args.arg = nullptr;
    timer_args.name = "temp_monitor";
    timer_args.dispatch_method = ESP_TIMER_TASK;

    if (esp_timer_create(&timer_args, &temp_timer) == ESP_OK) {
        esp_timer_start_periodic(temp_timer, 1000000);  // 1秒
        DEBUG_INFO("MON", "温度监控定时器已启动 (1秒间隔)");
    } else {
        DEBUG_ERR("MON", "创建温度监控定时器失败");
    }

    // 周期性巡检
    while (1) {
        uint32_t free_heap = esp_get_free_heap_size();
        if (free_heap < HEAP_WARNING_MIN) {
            DEBUG_WARN("MON", "堆内存不足: %lu < %lu", free_heap, HEAP_WARNING_MIN);
        }
        health_state_t cur = self.getState();
        if (cur >= SYS_CRITICAL) {
            DEBUG_ERR("MON", "健康状态危险: %d", cur);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ============================================================
// 健康事件日志
// ============================================================
void HealthMonitor::logEvent(const char *event, int value)
{
    DEBUG_INFO("HEALTH", "[健康事件] %s: %d", event, value);
}

// ============================================================
// OTA 健康检查
// ============================================================
health_status_t HealthMonitor::checkBeforeOta()
{
    health_status_t status = HEALTH_MON_OK;
    health_state_t cur = getState();

    if (cur >= SYS_SAFE_SHUTDOWN) {
        DEBUG_WARN("HEALTH", "OTA 前检查: SYS_SAFE_SHUTDOWN，禁止传输");
        return HEALTH_MON_SHUTDOWN;
    } else if (cur >= SYS_CRITICAL) {
        DEBUG_WARN("HEALTH", "OTA 前检查: SYS_CRITICAL，禁止传输");
        return HEALTH_MON_CRITICAL;
    } else if (cur >= SYS_WARNING) {
        status = HEALTH_MON_WARNING;
        DEBUG_WARN("HEALTH", "OTA 前检查: SYS_WARNING，继续传输但需注意");
    }

    if (!WifiOta::isConnected()) {
        DEBUG_WARN("HEALTH", "OTA 前检查: WiFi 未连接");
        return HEALTH_MON_CRITICAL;
    }

    int8_t last_rssi = -127;
    uint32_t age_ms = 0;
    if (getLastWifiRssi(&last_rssi, &age_ms)) {
        if (age_ms <= OTA_WIFI_RSSI_SAMPLE_MAX_AGE_MS) {
            if (last_rssi < OTA_WIFI_RSSI_CRITICAL) {
                DEBUG_WARN("HEALTH", "OTA 前检查: 最近 RSSI %d < %d (样本年龄 %lu ms)",
                           last_rssi, OTA_WIFI_RSSI_CRITICAL, (unsigned long)age_ms);
                return HEALTH_MON_CRITICAL;
            } else if (last_rssi < WIFI_RSSI_NORMAL) {
                if (status < HEALTH_MON_WARNING) {
                    status = HEALTH_MON_WARNING;
                }
                DEBUG_WARN("HEALTH", "OTA 前检查: 最近 RSSI %d < %d (样本年龄 %lu ms)",
                           last_rssi, WIFI_RSSI_NORMAL, (unsigned long)age_ms);
            }
        }
    }

    if (status == HEALTH_MON_OK) {
        DEBUG_DBG("HEALTH", "OTA 前检查: 正常");
    }

    return status;
}

health_status_t HealthMonitor::checkBeforeOtaRelay()
{
    health_status_t status = HEALTH_MON_OK;
    health_state_t cur = getState();

    if (cur >= SYS_SAFE_SHUTDOWN) {
        DEBUG_WARN("HEALTH", "OTA 中继检查: SYS_SAFE_SHUTDOWN，禁止传输");
        return HEALTH_MON_SHUTDOWN;
    } else if (cur >= SYS_CRITICAL) {
        /* SYS_CRITICAL 可能是由 WiFi RSSI 过低导致的，
         * 但 SPI 中继阶段不依赖 WiFi，只检查温度/硬件等本地状态。
         * 温度达到 TEMP_CRITICAL(100°C) 时 m_healthState 会被设为
         * SYS_SAFE_SHUTDOWN，在上面的分支已经拦截。
         * 所以 SYS_CRITICAL 在中继阶段只可能是 WiFi 导致的，允许继续。 */
        DEBUG_WARN("HEALTH", "OTA 中继检查: SYS_CRITICAL (WiFi 相关)，SPI 中继允许继续");
    } else if (cur >= SYS_WARNING) {
        status = HEALTH_MON_WARNING;
        DEBUG_WARN("HEALTH", "OTA 中继检查: SYS_WARNING，继续传输");
    }

    /* 跳过 WiFi 连接状态和 RSSI 检查：
     * SPI 中继阶段固件已存储在 ESP32 Flash 中，
     * 不依赖 WiFi 连接，无需检查 WiFi 状态。 */

    if (status == HEALTH_MON_OK) {
        DEBUG_DBG("HEALTH", "OTA 中继检查: 正常");
    }

    return status;
}

// ============================================================
// WiFi RSSI 更新
// ============================================================
health_status_t HealthMonitor::updateFromWifiRssi(int8_t rssi)
{
    if (rssi < WIFI_RSSI_CRITICAL) {
        setState(SYS_CRITICAL);
        DEBUG_WARN("HEALTH", "WiFi RSSI 危险: %d dBm < %d dBm", rssi, WIFI_RSSI_CRITICAL);
        logEvent("WIFI危险", rssi);
        return HEALTH_MON_CRITICAL;
    } else if (rssi < WIFI_RSSI_NORMAL) {
        health_state_t cur = getState();
        if (cur < SYS_WARNING) {
            setState(SYS_WARNING);
            DEBUG_WARN("HEALTH", "WiFi RSSI 警告: %d dBm < %d dBm", rssi, WIFI_RSSI_NORMAL);
            logEvent("WIFI警告", rssi);
        }
        return HEALTH_MON_WARNING;
    }

    health_state_t cur = getState();
    if (cur == SYS_WARNING && rssi >= WIFI_RSSI_NORMAL) {
        setState(SYS_NORMAL);
        DEBUG_INFO("HEALTH", "WiFi RSSI 恢复: %d dBm >= %d dBm", rssi, WIFI_RSSI_NORMAL);
        logEvent("WIFI恢复", rssi);
    }

    return HEALTH_MON_OK;
}
