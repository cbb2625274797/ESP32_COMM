/*
 * ESP32-C3FH4 OTA 中继 - 程序入口
 * 三个 FreeRTOS 任务：OTA Relay / WiFi Server / BLE GATT
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "monitor/health_monitor.h"
#include "relay/ota_relay.h"
#include "transport/spi_master.h"
#include "server/wifi_ota.h"
#include "server/ble_ota.h"
#include "transport/protocol.h"
#include "monitor/debug_log.h"

/**
  * @brief 程序主入口，初始化系统组件并创建各 FreeRTOS 任务。
 */
extern "C" void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    // 初始化网络接口 + 事件循环（必须最早）
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 初始化调试日志（UART0 先就绪，UDP socket 延迟到首次发送时创建）
    debug_log_init();

    // ★ WiFi 提前初始化（调度器启动前）
    // 包括：NVS → netif 创建 → event handler 注册 → WiFi driver init → 配置 → connect
    bool wifi_sta_ok = wifi_early_init();

    health_monitor_init();
    DEBUG_INFO("MAIN", "健康监控状态已初始化");
    DEBUG_INFO("MAIN", "========================================");
    DEBUG_INFO("MAIN", "ESP32-C3 OTA Relay v1.0 启动");
    DEBUG_INFO("MAIN", "Chip: %s, Rev %d", CONFIG_IDF_TARGET, chip_info.revision);
    DEBUG_INFO("MAIN", "Free heap: %lu bytes", esp_get_free_heap_size());
    DEBUG_INFO("MAIN", "CPU Freq: %d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    DEBUG_INFO("MAIN", "WiFi: %s", wifi_sta_ok ? "STA 连接中" : "将使用 AP 模式");
    DEBUG_INFO("MAIN", "========================================");

    // 健康监控任务（温度 + WiFi RSSI）
    BaseType_t xRet;
    xRet = xTaskCreatePinnedToCore(
        health_monitor_task,
        "health_mon",
        4096,
        NULL,
        configMAX_PRIORITIES - 4,
        NULL,
        0
    );
    if (xRet != pdPASS) {
        DEBUG_ERR("MAIN", "创建 health_monitor_task 失败");
    } else {
        DEBUG_INFO("MAIN", "健康监控任务已创建");
    }

    protocol_init();
    DEBUG_INFO("MAIN", "协议层初始化完成");

    esp_err_t ret = spi_master_init();
    if (ret != ESP_OK) {
        DEBUG_ERR("MAIN", "SPI 总线初始化失败: %d", ret);
    } else {
        DEBUG_INFO("MAIN", "SPI 总线初始化完成");
    }

    // OTA 中继任务（中高优先级）
    xRet = xTaskCreatePinnedToCore(
        ota_relay_task,
        "ota_relay",
        8192,
        NULL,
        configMAX_PRIORITIES - 2,
        NULL,
        0  // 核0
    );
    if (xRet != pdPASS) {
        DEBUG_ERR("MAIN", "创建 ota_relay_task 失败");
    } else {
        DEBUG_INFO("MAIN", "OTA 中继任务已创建");
    }

    // WiFi OTA 服务器任务
    xRet = xTaskCreatePinnedToCore(
        wifi_ota_task,
        "wifi_ota",
        8192,
        NULL,
        configMAX_PRIORITIES - 3,
        NULL,
        0
    );
    if (xRet != pdPASS) {
        DEBUG_ERR("MAIN", "创建 wifi_ota_task 失败");
    } else {
        DEBUG_INFO("MAIN", "WiFi OTA 任务已创建");
    }

    // BLE OTA 任务
    xRet = xTaskCreatePinnedToCore(
        ble_ota_task,
        "ble_ota",
        8192,
        NULL,
        configMAX_PRIORITIES - 3,
        NULL,
        0  // ESP32-C3 单核，固定核0
    );
    if (xRet != pdPASS) {
        DEBUG_ERR("MAIN", "创建 ble_ota_task 失败");
    } else {
        DEBUG_INFO("MAIN", "BLE OTA 任务已创建");
    }

    DEBUG_INFO("MAIN", "========================================");
    DEBUG_INFO("MAIN", "所有任务创建完成，OTA Relay 运行中");
    DEBUG_INFO("MAIN", "调试日志输出: UDP(%s:%d) + UART0(GPIO21/TX, 115200)", 
               DEBUG_LOG_IP, DEBUG_LOG_PORT);
    DEBUG_INFO("MAIN", "========================================");
}

