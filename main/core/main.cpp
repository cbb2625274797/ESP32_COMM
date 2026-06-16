/*
 * ESP32-C3FH4 OTA 中继 - 程序入口
 * 四个 FreeRTOS 任务：健康监控 / OTA Relay / WiFi Server / BLE GATT
 *
 * 使用各模块的单例类进行初始化和任务创建。
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_ota_ops.h"

#include "monitor/health_monitor.h"
#include "relay/ota_relay.h"
#include "ota/ota_coordinator.h"
#include "transport/spi_master.h"
#include "server/wifi_ota.h"
#include "server/ble_ota.h"
#include "transport/protocol.h"
#include "monitor/debug_log.h"
#include "app_config.h"
#include "ota_mutex.h"

/**
 * @brief 程序主入口，初始化系统组件并创建各 FreeRTOS 任务。
 */
// 全局 ESP32 OTA 互斥变量（声明在 ota_mutex.h）
SemaphoreHandle_t g_esp32OtaMutex = NULL;
volatile bool g_esp32OtaOccupied = false;

extern "C" void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    // 创建 ESP32 OTA 全局互斥 Mutex
    g_esp32OtaMutex = xSemaphoreCreateMutex();
    g_esp32OtaOccupied = false;

    // 初始化网络接口 + 事件循环（必须最早）
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 初始化调试日志（UART0 先就绪，UDP socket 延迟到首次发送时创建）
    DebugLog::instance().init();

    // ★ WiFi 提前初始化（调度器启动前）
    bool wifi_sta_ok = WifiOta::instance().earlyInit();

    HealthMonitor::instance().init();
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
        HealthMonitor::taskEntry,
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

    Protocol::instance().init();
    DEBUG_INFO("MAIN", "协议层初始化完成");

    // ★ 全局 OTA 协调器初始化（在 OTA task 创建前，确保 mutex 就绪）
    OtaCoordinator::instance().init();
    DEBUG_INFO("MAIN", "OTA 全局协调器已初始化");

    esp_err_t ret = SpiMaster::instance().init();
    if (ret != ESP_OK) {
        DEBUG_ERR("MAIN", "SPI 总线初始化失败: %d", ret);
    } else {
        DEBUG_INFO("MAIN", "SPI 总线初始化完成");
    }

    // OTA 中继任务（中高优先级）
    xRet = xTaskCreatePinnedToCore(
        OtaRelay::taskEntry,
        "ota_relay",
        12288,
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
        WifiOta::taskEntry,
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
        BleOta::taskEntry,
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

    // ★ OTA 回滚保护：标记当前固件有效
    // 启用 CONFIG_APP_ROLLBACK_ENABLE 后，新固件首次启动必须调用此函数
    // 若未调用就崩溃/重启，bootloader 会自动回退到上一个有效分区
    esp_err_t ota_valid_err = esp_ota_mark_app_valid_cancel_rollback();
    if (ota_valid_err == ESP_OK) {
        DEBUG_INFO("MAIN", "OTA 回滚保护: 当前固件已标记有效");
    } else {
        DEBUG_WARN("MAIN", "OTA 回滚保护: 标记失败 (%s)，可能非 OTA 启动", esp_err_to_name(ota_valid_err));
    }

    DEBUG_INFO("MAIN", "========================================");
    DEBUG_INFO("MAIN", "所有任务创建完成，OTA Relay 运行中");
    DEBUG_INFO("MAIN", "调试日志输出: UDP(%s:%d) + UART0(GPIO21/TX, 115200)",
               DEBUG_LOG_IP, DEBUG_LOG_PORT);
    DEBUG_INFO("MAIN", "========================================");
}
