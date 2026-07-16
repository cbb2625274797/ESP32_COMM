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
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "driver/temperature_sensor.h"

#include "health_monitor.h"
#include "debug_log.h"
#include "task_stack_check.h"
#include "stack_overflow_hook.h"
#include "ota/ota_coordinator.h"

// ============================================================
// 栈水位主动巡检
// ============================================================
// 告警阈值：剩余高水位 < 栈大小 20% 即告警（复用 /system/status 的任务表）。
// 去重：每个任务只在"水位由安全跌入危险"时告警一次，恢复后再跌入才再次告警，
//   避免 5 秒巡检刷屏。health_mon 任务从不被 OTA 暂停，DEBUG_ERR 经 UDP/UART
//   直发，不受 OTA 门控抑制。
static constexpr float STACK_WARN_RATIO = 0.20f;   ///< 剩余水位告警阈值（栈大小比例）
static constexpr size_t STACK_TASK_MAX = 12;       ///< 受监控任务上限（含 spi_worker 等）
static bool s_stackWarned[STACK_TASK_MAX] = {};    ///< 各任务是否已告警（去重）

// ============================================================
// 单例
// ============================================================
/**
  * @brief 获取 HealthMonitor 单例实例。
 * @return HealthMonitor 引用。
 */
HealthMonitor& HealthMonitor::instance()
{
    static HealthMonitor inst;
    return inst;
}

// ============================================================
// 初始化
// ============================================================
/**
  * @brief 初始化健康监控模块，创建内部互斥锁与基础状态。
 */
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
/**
  * @brief 加锁读取当前健康状态。
 * @return 当前健康状态。
 */
health_state_t HealthMonitor::getState()
{
    LockGuard lock(m_mutex);
    return m_healthState;
}

/**
  * @brief 加锁设置健康状态。
 * @param newState 新的健康状态。
 */
void HealthMonitor::setState(health_state_t newState)
{
    LockGuard lock(m_mutex);
    m_healthState = newState;
}

// ============================================================
// 获取当前健康状态（public 接口）
// ============================================================
/**
  * @brief 获取当前系统健康状态（线程安全）。
 * @return 当前健康状态。
 */
health_state_t HealthMonitor::getCurrent()
{
    return getState();
}

/**
  * @brief 获取最近一次芯片温度。
 * @return 温度值（摄氏度）。
 */
float HealthMonitor::getLastTemperature()
{
    LockGuard lock(m_mutex);
    return m_lastTemperature;
}

/**
  * @brief 获取最近一次 WiFi RSSI 及其年龄。
 * @param out_rssi 输出参数，返回 RSSI 值。
 * @param out_age_ms 输出参数，返回样本年龄（毫秒）。
 * @return 是否有有效数据。
 */
bool HealthMonitor::getLastWifiRssi(int8_t *out_rssi, uint32_t *out_age_ms)
{
    int64_t lastMs;
    int8_t  rssi;
    {
        LockGuard lock(m_mutex);
        lastMs = m_lastWifiRssiMs;
        rssi   = m_lastWifiRssi;
    }

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
/**
  * @brief 温度监控定时器回调函数。
 * @param arg 用户参数（未使用）。
 */
void HealthMonitor::tempMonitorCallback(void *arg)
{
    auto& self = instance();
    float temp = 0.0f;
    esp_err_t err = temperature_sensor_get_celsius(self.m_tempHandle, &temp);
    if (err != ESP_OK) {
        DEBUG_WARN("TEMP", "读取温度失败: %s", esp_err_to_name(err));
        return;
    }

    {
        LockGuard lock(self.m_mutex);
        self.m_lastTemperature = temp;
    }

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
/**
  * @brief 健康监控 FreeRTOS 任务入口。
 * @param pvParameters 任务参数。
 */
void HealthMonitor::taskEntry(void *pvParameters)
{
    (void)pvParameters;
    auto& self = instance();
    self.m_taskHandle = xTaskGetCurrentTaskHandle();
    DEBUG_INFO("MON", "=== 健康监控任务启动 ===");

    temperature_sensor_config_t temp_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 80);
    esp_err_t err = temperature_sensor_install(&temp_config, &self.m_tempHandle);
    if (err != ESP_OK) {
        DEBUG_ERR("MON", "温度传感器初始化失败: %s", esp_err_to_name(err));
    } else {
        ESP_ERROR_CHECK(temperature_sensor_enable(self.m_tempHandle));
    }

    // 定时器，每1秒读取一次温度
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &tempMonitorCallback;
    timer_args.arg = nullptr;
    timer_args.name = "temp_monitor";
    timer_args.dispatch_method = ESP_TIMER_TASK;

    if (esp_timer_create(&timer_args, &self.m_tempTimer) == ESP_OK) {
        esp_timer_start_periodic(self.m_tempTimer, 1000000);  // 1秒
    } else {
        self.m_tempTimer = nullptr;
        DEBUG_ERR("MON", "创建温度监控定时器失败");
    }

    // 周期性巡检
    while (1) {
        // 栈溢出 canary 触发检测：vApplicationStackOverflowHook 置位后，
        //   在 health_mon 完好栈上用 DEBUG_ERR 经 UDP/UART 发详细告警。
        //   标志只在置位后告警一次，不自动清除（canary 触发即不可逆，需重启恢复）。
        if (g_stackOverflowDetected) {
            DEBUG_ERR("MON", "!!! 栈溢出已被内核 canary 检测到，请检查任务栈大小 !!!");
            // 告警一次后清标志，避免 5 秒刷屏；后续若再次触发会重新置位
            g_stackOverflowDetected = false;
        }

        uint32_t free_heap = esp_get_free_heap_size();
        if (free_heap < HEAP_WARNING_MIN) {
            /* 堆告警节流：OTA 期间阈值更低、间隔更长 */
            static TickType_t s_last_heap_warn = 0;
            TickType_t now = xTaskGetTickCount();
            const bool ota_busy = !OtaCoordinator::instance().isIdle();
            const uint32_t interval_ms = ota_busy ? 15000U
                : (free_heap < 8192U ? 10000U : 3000U);
            const uint32_t warn_floor = ota_busy ? 4096U : (uint32_t)HEAP_WARNING_MIN;
            if (free_heap < warn_floor &&
                (now - s_last_heap_warn) >= pdMS_TO_TICKS(interval_ms)) {
                s_last_heap_warn = now;
                uint32_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
                DEBUG_WARN("MON", "堆内存偏紧%s: free=%lu min=%lu largest_blk=%lu",
                           ota_busy ? "(OTA中)" : "",
                           (unsigned long)free_heap,
                           (unsigned long)esp_get_minimum_free_heap_size(),
                           (unsigned long)largest_block);
            }
        }
        health_state_t cur = self.getState();
        if (cur >= SYS_CRITICAL) {
            DEBUG_ERR("MON", "健康状态危险: %d", cur);
        }

        // 栈水位主动巡检：复用 /system/status 的任务表，低水位时 DEBUG_ERR 主动告警。
        //   health_mon 任务不被 OTA 暂停，告警经 UDP/UART 直发，穿透 OTA 门控。
        TaskStackMetric metrics[STACK_TASK_MAX];
        size_t count = collectTaskStackMetrics(metrics, STACK_TASK_MAX);
        for (size_t i = 0; i < count && i < STACK_TASK_MAX; ++i) {
            if (metrics[i].handle == NULL || metrics[i].stack_size_bytes == 0) {
                continue;
            }
            UBaseType_t hw = uxTaskGetStackHighWaterMark(metrics[i].handle);
            size_t warn_bytes = (size_t)(metrics[i].stack_size_bytes * STACK_WARN_RATIO);
            if (hw < (UBaseType_t)warn_bytes) {
                if (!s_stackWarned[i]) {
                    DEBUG_ERR("MON", "!!! 栈水位过低 !!! 任务=%s 剩余=%luB 阈值=%luB(%.0f%%) 栈大小=%luB",
                              metrics[i].name,
                              (unsigned long)hw,
                              (unsigned long)warn_bytes,
                              STACK_WARN_RATIO * 100.0f,
                              (unsigned long)metrics[i].stack_size_bytes);
                    s_stackWarned[i] = true;
                }
            } else if (s_stackWarned[i]) {
                // 水位恢复，清除告警标志，允许下次再次跌入时告警
                s_stackWarned[i] = false;
                DEBUG_INFO("MON", "栈水位恢复: 任务=%s 剩余=%luB",
                           metrics[i].name, (unsigned long)hw);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ============================================================
// OTA 健康检查
// ============================================================
/**
  * @brief OTA 传输前执行健康检查。
 * @return 健康检查状态。
 */
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
        DEBUG_DBG("HEALTH", "OTA 前检查: SYS_WARNING，继续传输但需注意");
    }

    bool wifi_connected;
    {
        LockGuard lock(m_mutex);
        wifi_connected = m_wifiConnected;
    }
    if (!wifi_connected) {
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

/**
  * @brief OTA 中继转发阶段执行健康检查（跳过 WiFi 检查）。
 * @return 健康检查状态。
 */
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
        DEBUG_DBG("HEALTH", "OTA 中继检查: SYS_WARNING，继续传输");
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
// WiFi 连接状态推送（由 WiFi 事件回调调用）
// ============================================================
/**
  * @brief 由 WiFi 事件回调推送连接状态。
 * @param connected true=已连接，false=断开。
 */
void HealthMonitor::setWifiConnected(bool connected)
{
    LockGuard lock(m_mutex);
    m_wifiConnected = connected;
}

// ============================================================
// WiFi RSSI 更新
// ============================================================
/**
  * @brief 根据 WiFi RSSI 更新健康状态。
 * @param rssi WiFi RSSI 值。
 * @return 健康检查状态。
 */
health_status_t HealthMonitor::updateFromWifiRssi(int8_t rssi)
{
    health_state_t cur = getState();
    health_state_t next = SYS_NORMAL;
    if (rssi < WIFI_RSSI_CRITICAL) {
        next = SYS_CRITICAL;
    } else if (rssi < WIFI_RSSI_NORMAL) {
        next = SYS_WARNING;
    }

    // 仅在状态发生转换时打印一次，避免同一阈值下反复刷屏。
    if (next != cur) {
        if (next == SYS_CRITICAL) {
            setState(SYS_CRITICAL);
            DEBUG_WARN("HEALTH", "WiFi RSSI 危险: %d dBm < %d dBm", rssi, WIFI_RSSI_CRITICAL);
        } else if (next == SYS_WARNING) {
            setState(SYS_WARNING);
            DEBUG_WARN("HEALTH", "WiFi RSSI 警告: %d dBm < %d dBm", rssi, WIFI_RSSI_NORMAL);
        } else { // next == SYS_NORMAL，从 WARNING/CRITICAL 恢复
            setState(SYS_NORMAL);
            DEBUG_INFO("HEALTH", "WiFi RSSI 恢复: %d dBm >= %d dBm (从 %d 降级)", rssi, WIFI_RSSI_NORMAL, (int)cur);
        }
    }

    if (next == SYS_CRITICAL) return HEALTH_MON_CRITICAL;
    if (next == SYS_WARNING) return HEALTH_MON_WARNING;

    return HEALTH_MON_OK;
}
