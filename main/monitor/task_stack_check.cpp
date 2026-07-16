/*
 * task_stack_check.cpp - 任务栈水位指标收集（共享）
 *
 * 实现 collectTaskStackMetrics()：复用 http_handlers 旧版 task_metrics[] 任务表，
 * 供 health_monitor 主动巡检与 /system/status 被动报告共用。
 */
#include "task_stack_check.h"

#include "app_config.h"
#include "health_monitor.h"
#include "cpu_stats.h"
#include "ota_relay.h"
#include "wifi_ota.h"
#include "ble_ota.h"
#include "spi_master.h"

size_t collectTaskStackMetrics(TaskStackMetric* out, size_t max)
{
    if (!out || max == 0) return 0;

    size_t n = 0;

    // httpd 由 esp_http_server 创建，无公开句柄；按任务名取。
    // httpd 任务在 httpd_start() 后才存在，未启动时 xTaskGetHandle 返回 NULL。
    if (n < max) {
        out[n++] = { "http_server", xTaskGetHandle("httpd"), HTTP_SERVER_STACK_SIZE };
    }
    if (n < max) {
        out[n++] = { "health_mon", HealthMonitor::instance().getTaskHandle(), TASK_STACK_HEALTH_MON };
    }
    if (n < max) {
        out[n++] = { "cpu_stats", CpuStats::instance().getTaskHandle(), TASK_STACK_CPU_STATS };
    }
    /* 转发热路径：ota_relay（业务）+ spi_worker（帧缓冲，此前监控缺口） */
    if (n < max) {
        out[n++] = { "ota_relay", OtaRelay::getTaskHandle(), TASK_STACK_OTA_RELAY };
    }
    if (n < max) {
        out[n++] = { "spi_worker", SpiMaster::instance().getWorkerTaskHandle(), TASK_STACK_SPI_WORKER };
    }
    if (n < max) {
        out[n++] = { "wifi_ota", WifiOta::instance().getTaskHandle(), TASK_STACK_WIFI_OTA };
    }
    if (n < max) {
        out[n++] = { "wifi_reconnect", WifiOta::instance().m_reconnectTask, TASK_STACK_WIFI_RECONNECT };
    }
    if (n < max) {
        out[n++] = { "ble_ota", BleOta::instance().getTaskHandle(), TASK_STACK_BLE_OTA };
    }
    if (n < max) {
        out[n++] = { "ble_ota_worker", BleOta::instance().getWorkerTaskHandle(), TASK_STACK_BLE_WORKER };
    }

    return n;
}