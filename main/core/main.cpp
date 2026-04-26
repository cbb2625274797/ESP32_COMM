/*
 * ESP32-C3FH4 OTA 中继 - 程序入口
 * 四个 FreeRTOS 任务：System Monitor / OTA Relay / WiFi Server / BLE GATT
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"

#include "monitor/health_monitor.h"
#include "relay/ota_relay.h"
#include "transport/spi_master.h"
#include "server/wifi_ota.h"
#include "server/ble_ota.h"
#include "transport/protocol.h"
#include "monitor/debug_log.h"

extern "C" void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    // 初始化调试日志（UDP 输出到上位机）
    debug_log_init();
    DEBUG_INFO("MAIN", "========================================");
    DEBUG_INFO("MAIN", "ESP32-C3 OTA Relay v1.0 启动");
    DEBUG_INFO("MAIN", "Chip: %s, Rev %d", CONFIG_IDF_TARGET, chip_info.revision);
    DEBUG_INFO("MAIN", "Free heap: %lu bytes", esp_get_free_heap_size());
    DEBUG_INFO("MAIN", "========================================");

    // 健康监控任务（温度 + 电压 + WiFi RSSI）
    xTaskCreatePinnedToCore(
        health_monitor_task,
        "health_mon",
        4096,
        NULL,
        configMAX_PRIORITIES - 4,
        NULL,
        0
    );

    // 初始化协议层（CRC、AES）
    protocol_init();
    DEBUG_INFO("MAIN", "协议层初始化完成");

    // 初始化 SPI 总线
    esp_err_t ret = spi_master_init();
    if (ret != ESP_OK) {
        DEBUG_ERR("MAIN", "SPI 总线初始化失败: %d", ret);
    } else {
        DEBUG_INFO("MAIN", "SPI 总线初始化完成");
    }

    // 创建 FreeRTOS 任务
    BaseType_t xRet;

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
    }

    DEBUG_INFO("MAIN", "所有任务创建完成，OTA Relay 运行中");
    DEBUG_INFO("MAIN", "调试日志输出到上位机: %s:%d", DEBUG_LOG_IP, DEBUG_LOG_PORT);
}