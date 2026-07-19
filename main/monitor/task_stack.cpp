/*
 * task_stack.cpp - 任务栈水位收集 + FreeRTOS 栈溢出钩子
 *
 * 水位表：health_mon 巡检 + /system/status 共用。
 * 溢出钩子：CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY 下内核回调；
 *   被踩栈上下文禁止 mutex/网络/格式化，仅 UART0 硬写 + abort。
 */
#include "task_stack.h"

#include <cstring>
#include "app_config.h"
#include "health_monitor.h"
#include "cpu_stats.h"
#include "ota_relay.h"
#include "wifi_ota.h"
#include "ble_ota.h"
#include "spi_master.h"
#include "driver/uart.h"
#include "esp_system.h"

// ============================================================
// 栈溢出钩子
// ============================================================
volatile bool g_stackOverflowDetected = false;

extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    g_stackOverflowDetected = true;

    static const char prefix[] = "\r\n!!! STACK OVERFLOW in task: ";
    uart_write_bytes(UART_NUM_0, prefix, sizeof(prefix) - 1);
    uart_write_bytes(UART_NUM_0, pcTaskName, strnlen(pcTaskName, configMAX_TASK_NAME_LEN));
    static const char suffix[] = " !!!\r\n";
    uart_write_bytes(UART_NUM_0, suffix, sizeof(suffix) - 1);

    /* OTA 末尾若 httpd 栈溢出却不 abort，前端会一直停在 100%。 */
    esp_system_abort("stack overflow");
}

// ============================================================
// 任务栈水位表
// ============================================================
size_t collectTaskStackMetrics(TaskStackMetric* out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }

    size_t n = 0;

    // httpd 无公开句柄；未 httpd_start 时 xTaskGetHandle 为 NULL
    if (n < max) {
        out[n++] = { "http_server", xTaskGetHandle("httpd"), HTTP_SERVER_STACK_SIZE };
    }
    if (n < max) {
        out[n++] = { "health_mon", HealthMonitor::instance().getTaskHandle(), TASK_STACK_HEALTH_MON };
    }
    if (n < max) {
        out[n++] = { "cpu_stats", CpuStats::instance().getTaskHandle(), TASK_STACK_CPU_STATS };
    }
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
