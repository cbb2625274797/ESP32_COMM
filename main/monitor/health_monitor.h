/*
 * health_monitor.h - 系统健康监控头文件
 */
#pragma once

#include <stdint.h>
#include "app_config.h"

// T9.3: 健康状态等级
typedef enum {
    SYS_NORMAL = 0,        // 正常
    SYS_WARNING = 1,       // 警告
    SYS_CRITICAL = 2,     // 危险
    SYS_SAFE_SHUTDOWN = 3  // 安全关机
} health_state_t;

typedef enum {
    HEALTH_MON_OK = 0,
    HEALTH_MON_WARNING = 1,
    HEALTH_MON_CRITICAL = 2,
    HEALTH_MON_SHUTDOWN = 3
} health_status_t;

// 外部共享状态（互斥锁保护）
extern health_state_t g_health_state;

// WiFi RSSI 阈值（来自 app_config.h）
#define WIFI_RSSI_NORMAL   WIFI_RSSI_WARNING
#define WIFI_RSSI_WEAK     WIFI_RSSI_CRITICAL

// 函数声明
void health_monitor_init(void);
void health_monitor_task(void *pvParameters);
health_status_t health_check_before_ota(void);
void health_log_event(const char *event, int value);
health_state_t health_get_current(void);
health_status_t health_update_from_wifi_rssi(int8_t rssi);
