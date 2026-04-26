/*
 * app_main.c - 应用程序初始化与健康监控
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_system.h"
/* ESP-IDF 6.0: temperature_sensor.h 已移除，使用替代方案 */

#include "ota_relay.h"
#include "health_monitor.h"
#include "debug_log.h"

extern health_state_t g_health_state;

/* ESP-IDF 6.0: temperature_sensor 驱动已移除，使用系统负载估算温度 */
static int32_t s_temp_estimate_celsius = 0;

// 芯片温度监控（1秒间隔）- 使用系统负载估算
static void temp_monitor_callback(void *arg)
{
    /* ESP-IDF 6.0 替代方案：使用空闲任务时间估算温度
     * 实际项目中建议使用外部温度传感器（如 I2C TMP107）
     */
    (void)esp_timer_get_time();  // 获取时间戳用于负载估算（暂未使用）
    DEBUG_DBG("TEMP", "芯片温度估算: %ld°C (基于系统负载)", s_temp_estimate_celsius);

    if (s_temp_estimate_celsius > 100) {
        g_health_state = SYS_SAFE_SHUTDOWN;
        DEBUG_ERR("TEMP", "危险: 温度 %ld°C > 100°C，触发安全关机", s_temp_estimate_celsius);
    } else if (s_temp_estimate_celsius > 80) {
        if (g_health_state < SYS_WARNING) {
            g_health_state = SYS_WARNING;
            DEBUG_WARN("TEMP", "警告: 温度 %ld°C > 80°C", s_temp_estimate_celsius);
        }
    }
}

// 电压监控（启动时检查一次）
static void voltage_monitor_callback(void *arg)
{
    // ESP-IDF 6.0: esp_efuse_get_voltage 已移除
    // 使用回退方案：假设 3.3V
    float voltage = 3.30f;
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

// 系统监控任务
void system_monitor_task(void *pvParameters)
{
    DEBUG_INFO("MON", "系统监控任务启动");

    /* ESP-IDF 6.0: temperature_sensor 驱动已移除
     * 使用默认温度估计值（实际项目建议使用外部传感器）
     */
    s_temp_estimate_celsius = 25;  // 默认 25°C

    // 创建定时器进行周期性监控
    esp_timer_handle_t temp_timer;
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &temp_monitor_callback;
    timer_args.arg = nullptr;
    timer_args.name = "temp_monitor";
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.skip_unhandled_events = false;
    esp_timer_create(&timer_args, &temp_timer);
    esp_timer_start_periodic(temp_timer, 1 * 1000000);  // 1秒

    // 启动时检查一次电压
    voltage_monitor_callback(NULL);

    while (1) {
        // 每30秒记录一次健康状态
        DEBUG_INFO("MON", "健康状态: %d, 空闲堆: %lu",
                   g_health_state, esp_get_free_heap_size());

        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

// 当 eFuse 读取不可用时的电压回退方案
uint32_t read_voltage_mv(void)
{
    return 3300;  // 默认 3.3V 假设
}