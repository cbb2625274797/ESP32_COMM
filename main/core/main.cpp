/*
 * ESP32-C3FH4 OTA 中继 - 程序入口
 * FreeRTOS 任务：健康监控 / OTA Relay / WiFi Server / BLE GATT / CAN 反馈
 *
 * 使用各模块单例初始化并创建任务；入口只做装配，不实现协议。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_ota_ops.h"

#include "monitor/health_monitor.h"
#include "monitor/cpu_stats.h"
#include "relay/ota_relay.h"
#include "ota/ota_coordinator.h"
#include "transport/spi_master.h"
#include "server/wifi_ota.h"
#include "server/ble_ota.h"
#include "transport/protocol.h"
#include "monitor/debug_log.h"
#include "app_config.h"
#include "ota/esp32_ota_core.h"
#include "monitor/can_feedback_task.h"

/**
 * @brief 创建关键任务；失败则打 ERR 并 ESP_ERROR_CHECK 触发重启。
 */
static void createCriticalTask(TaskFunction_t entry, const char *name,
                               uint32_t stack, UBaseType_t prio)
{
    BaseType_t xRet = xTaskCreatePinnedToCore(
        entry, name, stack, NULL, prio, NULL, 0);
    if (xRet != pdPASS) {
        DEBUG_ERR("MAIN", "创建关键任务 %s 失败", name);
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    DEBUG_INFO("MAIN", "任务已创建: %s (prio=%u stack=%lu)",
               name, (unsigned)prio, (unsigned long)stack);
}

/**
 * @brief 程序主入口：初始化组件并创建 FreeRTOS 任务。
 */
extern "C" void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    /* 上电/欠压复位：先等电源与 GD32 侧负载稳定，再进 WiFi/SPI 重活 */
    {
        esp_reset_reason_t rr = esp_reset_reason();
        if (rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    Esp32OtaCore::initMutex();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    DebugLog::instance().init();

    /* earlyInit：true=已启动 STA 连接流程；false=将走 AP（或 CONFIG 直选 AP） */
    const bool wifi_early_started_sta = WifiOta::instance().earlyInit();

    HealthMonitor::instance().init();
    DEBUG_INFO("MAIN", "健康监控状态已初始化");
    DEBUG_INFO("MAIN", "========================================");
    DEBUG_INFO("MAIN", "ESP32-C3 OTA Relay %s 启动", FW_VERSION);
    DEBUG_INFO("MAIN", "Chip: %s, Rev %d", CONFIG_IDF_TARGET, chip_info.revision);
    DEBUG_INFO("MAIN", "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    DEBUG_INFO("MAIN", "CPU Freq: %d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    DEBUG_INFO("MAIN", "WiFi early: %s（最终 STA/AP 由 wifi_ota 任务确认）",
               wifi_early_started_sta ? "STA 连接中" : "非 STA 优先/将切 AP");
    DEBUG_INFO("MAIN", "========================================");

    /* 健康监控：温度/RSSI；缺失则运维盲区 → 关键 */
    createCriticalTask(HealthMonitor::taskEntry, "health_mon",
                       TASK_STACK_HEALTH_MON, configMAX_PRIORITIES - 4);

    /* CPU 1Hz 采样：非致命 */
    CpuStats::instance().start();

    Protocol::instance().init();
    DEBUG_INFO("MAIN", "协议层初始化完成");

    OtaCoordinator::instance().init();
    OtaCoordinator::instance().setBleCoexListener(&WifiOta::instance());
    DEBUG_INFO("MAIN", "OTA 全局协调器已初始化");

    /* SPI 是中继/CAN 反馈根基：失败则无法完成核心职责，按关键失败处理 */
    {
        esp_err_t ret = SpiMaster::instance().init();
        if (ret != ESP_OK) {
            DEBUG_ERR("MAIN", "SPI 总线初始化失败: %s", esp_err_to_name(ret));
            ESP_ERROR_CHECK(ret);
        }
        DEBUG_INFO("MAIN", "SPI 总线初始化完成");
    }

    createCriticalTask(OtaRelay::taskEntry, "ota_relay",
                       TASK_STACK_OTA_RELAY, configMAX_PRIORITIES - 2);

    createCriticalTask(WifiOta::taskEntry, "wifi_ota",
                       TASK_STACK_WIFI_OTA, configMAX_PRIORITIES - 3);

    createCriticalTask(BleOta::taskEntry, "ble_ota",
                       TASK_STACK_BLE_OTA, configMAX_PRIORITIES - 3);

    if (startCanFeedbackTask()) {
        DEBUG_INFO("MAIN", "CAN 反馈轮询任务已创建");
    } else {
        DEBUG_WARN("MAIN", "CAN 反馈轮询任务创建失败（非致命，OTA 不受影响）");
    }

    /* 任务就绪后尽快标记 app 有效，避免后续探测拖太久导致回滚窗口拉长 */
    {
        esp_err_t ota_valid_err = esp_ota_mark_app_valid_cancel_rollback();
        if (ota_valid_err == ESP_OK) {
            DEBUG_INFO("MAIN", "OTA 回滚保护: 当前固件已标记有效");
        } else {
            DEBUG_WARN("MAIN", "OTA 回滚保护: 标记失败 (%s)，可能非 OTA 启动",
                       esp_err_to_name(ota_valid_err));
        }
    }

    DEBUG_INFO("MAIN", "========================================");
    DEBUG_INFO("MAIN", "所有任务创建完成，OTA Relay 运行中");
    DEBUG_INFO("MAIN", "GD32 版本探测由 ota_relay 任务负责（hello+probe）");
    DEBUG_INFO("MAIN", "调试日志输出: UDP(%s:%d) + UART0(GPIO21/TX, 115200)",
               DEBUG_LOG_IP, DEBUG_LOG_PORT);
    DEBUG_INFO("MAIN", "========================================");
}
