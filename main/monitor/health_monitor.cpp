/*
 * health_monitor.cpp - 系统健康监控
 * 包含：健康状态管理 + WiFi RSSI 监控 + 温度监控
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

/** @brief 全局健康状态，受互斥锁保护。 */
health_state_t g_health_state = SYS_NORMAL;

/** @brief 健康状态互斥锁。 */
static SemaphoreHandle_t g_health_mutex = NULL;
/** @brief 内置温度传感器句柄。 */
static temperature_sensor_handle_t s_temp_handle = NULL;
/** @brief 最近一次采样的芯片温度（摄氏度）。 */
static float s_last_temperature = 0.0f;
/** @brief 最近一次健康监控采样得到的 WiFi RSSI。 */
static int8_t s_last_wifi_rssi = -127;
/** @brief 最近一次 WiFi RSSI 采样时间（毫秒）。 */
static int64_t s_last_wifi_rssi_ms = 0;

// ----------------------------------------
// 健康状态管理
/**
  * @brief 初始化健康监控模块，创建互斥锁和基础状态。
 */
void health_monitor_init(void)
{
    /** @brief 健康状态互斥锁。 */
    g_health_mutex = xSemaphoreCreateMutex();
    if (g_health_mutex == NULL) {
        DEBUG_ERR("HEALTH", "创建健康状态互斥锁失败");
    }
}

/**
  * @brief 获取当前系统健康状态。
 * @return 当前健康状态枚举值。
 */
health_state_t health_get_current(void)
{
    health_state_t state;
    if (g_health_mutex) {
        xSemaphoreTake(g_health_mutex, portMAX_DELAY);
    }
    state = g_health_state;
    if (g_health_mutex) {
        xSemaphoreGive(g_health_mutex);
    }
    return state;
}

/**
  * @brief 返回最近一次采样到的芯片温度（摄氏度）。
 * @return 最近一次温度采样值（摄氏度）。
 */
float health_get_last_temperature(void)
{
    /** @brief 最近一次采样的芯片温度（摄氏度）。 */
    return s_last_temperature;
}

/**
 * @brief 获取最近一次采样到的 WiFi RSSI 及其年龄。
 * @param out_rssi 输出 RSSI 指针，可为 NULL。
 * @param out_age_ms 输出采样距今毫秒数指针，可为 NULL。
 * @return 有有效采样返回 true，否则返回 false。
 */
bool health_get_last_wifi_rssi(int8_t *out_rssi, uint32_t *out_age_ms)
{
    if (s_last_wifi_rssi_ms <= 0) {
        return false;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    uint32_t age_ms = (now_ms > s_last_wifi_rssi_ms) ? (uint32_t)(now_ms - s_last_wifi_rssi_ms) : 0;

    if (out_rssi) *out_rssi = s_last_wifi_rssi;
    if (out_age_ms) *out_age_ms = age_ms;
    return true;
}

// ----------------------------------------
// 温度监控（定时器回调）
/**
  * @brief 温度定时回调，采样并根据阈值更新健康状态。
 * @param arg
 */
static void temp_monitor_callback(void *arg)
{
    float temp = 0.0f;
    esp_err_t err = temperature_sensor_get_celsius(s_temp_handle, &temp);
    if (err != ESP_OK) {
        DEBUG_WARN("TEMP", "读取温度失败: %s", esp_err_to_name(err));
        return;
    }

    /** @brief 最近一次采样的芯片温度（摄氏度）。 */
    s_last_temperature = temp;

    if (temp >= TEMP_CRITICAL) {
        /** @brief 全局健康状态，受互斥锁保护。 */
        g_health_state = SYS_SAFE_SHUTDOWN;
        DEBUG_ERR("TEMP", "!!! 危险 !!! 温度 %.1f°C >= %d°C，触发安全关机", temp, TEMP_CRITICAL);
        DEBUG_ERR("TEMP", "健康状态变更为: SYS_SAFE_SHUTDOWN");
    } else if (temp >= TEMP_WARNING) {
        if (g_health_state < SYS_WARNING) {
            /** @brief 全局健康状态，受互斥锁保护。 */
            g_health_state = SYS_WARNING;
            DEBUG_WARN("TEMP", "警告: 温度 %.1f°C >= %d°C", temp, TEMP_WARNING);
            DEBUG_WARN("TEMP", "健康状态变更为: SYS_WARNING");
        }
    }
}

// ----------------------------------------
// 温度监控任务入口
/**
  * @brief 启动温度监控定时器，周期性巡检系统健康。
 * @param pvParameters FreeRTOS 任务参数，未使用。
 */
void health_monitor_task(void *pvParameters)
{
    DEBUG_INFO("MON", "=== 健康监控任务启动 ===");

    temperature_sensor_config_t temp_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 80);
    esp_err_t err = temperature_sensor_install(&temp_config, &s_temp_handle);
    if (err != ESP_OK) {
        DEBUG_ERR("MON", "温度传感器初始化失败: %s", esp_err_to_name(err));
    } else {
        ESP_ERROR_CHECK(temperature_sensor_enable(s_temp_handle));
        DEBUG_INFO("MON", "温度传感器已启用 (范围 20°C ~ 80°C)");
    }

    // 定时器，每1秒读取一次温度
    esp_timer_handle_t temp_timer;
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &temp_monitor_callback;
    timer_args.arg = nullptr;
    timer_args.name = "temp_monitor";
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.skip_unhandled_events = false;
    esp_timer_create(&timer_args, &temp_timer);
    esp_timer_start_periodic(temp_timer, 1 * 1000000);  // 1秒

    DEBUG_INFO("MON", "温度监控定时器已启动 (间隔 1 秒)");

    // 保活循环：定时器由 esp_timer 任务驱动，此任务只需保持存活
    // 状态汇报已由 status_monitor_task 负责，此处只在异常时告警
    while (1) {
        int8_t rssi = wifi_get_rssi();
        if (wifi_is_connected() && rssi != -127) {
            s_last_wifi_rssi = rssi;
            s_last_wifi_rssi_ms = esp_timer_get_time() / 1000;
        }

        uint32_t free_heap = esp_get_free_heap_size();
        if (free_heap < HEAP_WARNING_MIN) {
            DEBUG_WARN("MON", "空闲堆内存不足: %lu < %d", (unsigned long)free_heap, HEAP_WARNING_MIN);
        }
        if (g_health_state >= SYS_CRITICAL) {
            DEBUG_ERR("MON", "健康状态危险: %d", g_health_state);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ----------------------------------------
// 健康事件日志
/**
  * @brief 记录一条健康监控事件日志。
 * @param event 事件名称字符串。
 * @param value 事件关联的数值。
 */
void health_log_event(const char *event, int value)
{
    DEBUG_INFO("HEALTH", "[健康事件] %s: %d", event, value);
}

// ----------------------------------------
// OTA 传输前的健康检查
/**
  * @brief OTA 传输前执行健康检查，决定是否允许传输。
 * @return 健康检查结果，决定是否允许 OTA。
 */
health_status_t health_check_before_ota(void)
{
    health_status_t status = HEALTH_MON_OK;

    if (g_health_state >= SYS_SAFE_SHUTDOWN) {
        DEBUG_WARN("HEALTH", "OTA 前检查: SYS_SAFE_SHUTDOWN，禁止传输");
        return HEALTH_MON_SHUTDOWN;
    } else if (g_health_state >= SYS_CRITICAL) {
        DEBUG_WARN("HEALTH", "OTA 前检查: SYS_CRITICAL，禁止传输");
        return HEALTH_MON_CRITICAL;
    } else if (g_health_state >= SYS_WARNING) {
        status = HEALTH_MON_WARNING;
        DEBUG_WARN("HEALTH", "OTA 前检查: SYS_WARNING，继续传输但需注意");
    }

    if (!wifi_is_connected()) {
        DEBUG_WARN("HEALTH", "OTA 前检查: WiFi 未连接");
        return HEALTH_MON_CRITICAL;
    }

    int8_t last_rssi = -127;
    uint32_t age_ms = 0;
    if (health_get_last_wifi_rssi(&last_rssi, &age_ms)) {
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
  * @brief 根据 WiFi RSSI 更新健康状态并返回检查结果。
 * @param rssi 当前 WiFi 信号强度（dBm）。
 * @return 健康检查结果。
 */
health_status_t health_update_from_wifi_rssi(int8_t rssi)
{
    if (rssi < WIFI_RSSI_CRITICAL) {
        /** @brief 全局健康状态，受互斥锁保护。 */
        g_health_state = SYS_CRITICAL;
        DEBUG_WARN("HEALTH", "WiFi RSSI 危险: %d dBm < %d dBm", rssi, WIFI_RSSI_CRITICAL);
        health_log_event("WIFI危险", rssi);
        return HEALTH_MON_CRITICAL;
    } else if (rssi < WIFI_RSSI_NORMAL) {
        if (g_health_state < SYS_WARNING) {
            /** @brief 全局健康状态，受互斥锁保护。 */
            g_health_state = SYS_WARNING;
            DEBUG_WARN("HEALTH", "WiFi RSSI 警告: %d dBm < %d dBm", rssi, WIFI_RSSI_NORMAL);
            health_log_event("WIFI警告", rssi);
        }
        return HEALTH_MON_WARNING;
    }

    if (g_health_state == SYS_WARNING && rssi >= WIFI_RSSI_NORMAL) {
        /** @brief 全局健康状态，受互斥锁保护。 */
        g_health_state = SYS_NORMAL;
        DEBUG_INFO("HEALTH", "WiFi RSSI 恢复: %d dBm >= %d dBm", rssi, WIFI_RSSI_NORMAL);
        health_log_event("WIFI恢复", rssi);
    }

    return HEALTH_MON_OK;
}
