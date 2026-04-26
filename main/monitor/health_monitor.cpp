/*
 * health_monitor.cpp - 系统健康监控
 * 包含：健康状态管理 + WiFi RSSI 监控 + 温度监控 + 电压监控
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

health_state_t g_health_state = SYS_NORMAL;

static SemaphoreHandle_t g_health_mutex = NULL;
static temperature_sensor_handle_t s_temp_handle = NULL;

// ----------------------------------------
// 健康状态管理
// ----------------------------------------
void health_monitor_init(void)
{
    g_health_mutex = xSemaphoreCreateMutex();
    if (g_health_mutex == NULL) {
        DEBUG_ERR("HEALTH", "创建健康状态互斥锁失败");
    }
}

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

// ----------------------------------------
// 温度监控（定时器回调）
// ----------------------------------------
static void temp_monitor_callback(void *arg)
{
    float temp = 0.0f;
    esp_err_t err = temperature_sensor_get_celsius(s_temp_handle, &temp);
    if (err != ESP_OK) {
        DEBUG_WARN("TEMP", "读取温度失败: %s", esp_err_to_name(err));
        return;
    }

    DEBUG_DBG("TEMP", "芯片温度: %.1f°C", temp);

    if (temp >= TEMP_CRITICAL) {
        g_health_state = SYS_SAFE_SHUTDOWN;
        DEBUG_ERR("TEMP", "危险: 温度 %.1f°C >= %d°C，触发安全关机", temp, TEMP_CRITICAL);
    } else if (temp >= TEMP_WARNING) {
        if (g_health_state < SYS_WARNING) {
            g_health_state = SYS_WARNING;
            DEBUG_WARN("TEMP", "警告: 温度 %.1f°C >= %d°C", temp, TEMP_WARNING);
        }
    }
}

// ----------------------------------------
// 电压监控（启动时调用一次）
// ----------------------------------------
static void voltage_monitor_callback(void *arg)
{
    float voltage = 3.30f;  // ESP32-C3 无内置电压传感器，使用估算
    DEBUG_DBG("VOLT", "VDD 电压: %.2fV (估算)", voltage);

    if (voltage < 2.3f) {
        g_health_state = SYS_SAFE_SHUTDOWN;
        DEBUG_ERR("VOLT", "危险: 电压 %.2fV < 2.3V，触发安全关机", voltage);
    } else if (voltage < 2.5f) {
        if (g_health_state < SYS_WARNING) {
            g_health_state = SYS_WARNING;
            DEBUG_WARN("VOLT", "警告: 电压 %.2fV < 2.5V", voltage);
        }
    }
}

// ----------------------------------------
// 温度电压监控任务入口
// ----------------------------------------
void health_monitor_task(void *pvParameters)
{
    DEBUG_INFO("MON", "健康监控任务启动");

    // 初始化内置温度传感器（测量范围 20°C ~ 80°C）
    temperature_sensor_config_t temp_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 80);
    esp_err_t err = temperature_sensor_install(&temp_config, &s_temp_handle);
    if (err != ESP_OK) {
        DEBUG_ERR("MON", "温度传感器初始化失败: %s", esp_err_to_name(err));
    } else {
        ESP_ERROR_CHECK(temperature_sensor_enable(s_temp_handle));
        DEBUG_INFO("MON", "温度传感器已启用 (范围 20°C ~ 80°C)");
    }

    // 定时器，每5秒读取一次温度
    esp_timer_handle_t temp_timer;
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &temp_monitor_callback;
    timer_args.arg = nullptr;
    timer_args.name = "temp_monitor";
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.skip_unhandled_events = false;
    esp_timer_create(&timer_args, &temp_timer);
    esp_timer_start_periodic(temp_timer, 5 * 1000000);  // 5秒

    // 启动时检查一次电压
    voltage_monitor_callback(NULL);

    // 每5秒打印一次健康状态
    while (1) {
        DEBUG_INFO("MON", "健康状态: %d, 空闲堆: %lu",
                   g_health_state, esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ----------------------------------------
// 健康事件日志
// ----------------------------------------
void health_log_event(const char *event, int value)
{
    DEBUG_WARN("HEALTH", "[健康日志] %s: %d", event, value);
}

// ----------------------------------------
// OTA 传输前的健康检查
// ----------------------------------------
health_status_t health_check_before_ota(void)
{
    health_status_t status = HEALTH_MON_OK;

    if (g_health_state >= SYS_SAFE_SHUTDOWN) {
        return HEALTH_MON_SHUTDOWN;
    } else if (g_health_state >= SYS_CRITICAL) {
        return HEALTH_MON_CRITICAL;
    } else if (g_health_state >= SYS_WARNING) {
        status = HEALTH_MON_WARNING;
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        if (ap_info.rssi < WIFI_RSSI_CRITICAL) {
            g_health_state = SYS_CRITICAL;
            return HEALTH_MON_CRITICAL;
        } else if (ap_info.rssi < WIFI_RSSI_NORMAL) {
            if (status < HEALTH_MON_WARNING) {
                status = HEALTH_MON_WARNING;
            }
        }
    }

    return status;
}

health_status_t health_update_from_wifi_rssi(int8_t rssi)
{
    if (rssi < WIFI_RSSI_CRITICAL) {
        g_health_state = SYS_CRITICAL;
        health_log_event("WIFI危险", rssi);
        return HEALTH_MON_CRITICAL;
    } else if (rssi < WIFI_RSSI_NORMAL) {
        if (g_health_state < SYS_WARNING) {
            g_health_state = SYS_WARNING;
            health_log_event("WIFI警告", rssi);
        }
        return HEALTH_MON_WARNING;
    }

    if (g_health_state == SYS_WARNING && rssi >= WIFI_RSSI_NORMAL) {
        g_health_state = SYS_NORMAL;
        health_log_event("WIFI恢复", rssi);
    }

    return HEALTH_MON_OK;
}
