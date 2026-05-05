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

/**
 * @brief 初始化健康监控模块，创建内部互斥锁与基础状态。
 */
void health_monitor_init(void);

/**
 * @brief 健康监控任务入口，负责启动温度定时检测并周期性巡检系统状态。
 * @param pvParameters FreeRTOS 任务参数，未使用。
 */
void health_monitor_task(void *pvParameters);

/**
 * @brief 在 OTA 传输开始前执行健康检查，返回当前是否允许传输。
 * @return 健康检查结果，决定是否允许 OTA。
 */
health_status_t health_check_before_ota(void);

/**
 * @brief 记录一条健康相关事件日志，便于故障追踪与状态回溯。
 * @param event 事件名称字符串。
 * @param value 事件关联的数值。
 */
void health_log_event(const char *event, int value);

/**
 * @brief 获取当前系统健康状态。
 * @return 当前健康状态枚举值。
 */
health_state_t health_get_current(void);

/**
 * @brief 根据最新 WiFi RSSI 更新健康状态，并返回对应健康检查结果。
 * @param rssi 当前 WiFi 信号强度（dBm）。
 * @return 健康检查结果。
 */
health_status_t health_update_from_wifi_rssi(int8_t rssi);

/**
 * @brief 获取最近一次采样到的芯片温度（摄氏度）。
 * @return 最近一次温度采样值（摄氏度）。
 */
float health_get_last_temperature(void);

/**
 * @brief 获取最近一次健康监控采样得到的 WiFi RSSI。
 * @param out_rssi 输出 RSSI。
 * @param out_age_ms 输出样本距今时长（毫秒）。
 * @return true 有有效样本，false 当前无有效样本。
 */
bool health_get_last_wifi_rssi(int8_t *out_rssi, uint32_t *out_age_ms);
