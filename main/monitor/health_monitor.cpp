/*
 * health_monitor.c - 系统健康监控实现
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"

#include "health_monitor.h"
#include "debug_log.h"

health_state_t g_health_state = SYS_NORMAL;

static SemaphoreHandle_t g_health_mutex = NULL;

void health_monitor_init(void)
{
    g_health_mutex = xSemaphoreCreateMutex();
    if (g_health_mutex == NULL) {
        DEBUG_ERR("HEALTH", "创建健康状态互斥锁失败");
    }
}

// 每次向 GD32 发送 DATA_PACKET 前的健康检查
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

// 健康事件的 Flash 日志写入
void health_log_event(const char *event, int value)
{
    DEBUG_WARN("HEALTH", "[健康日志] %s: %d", event, value);
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