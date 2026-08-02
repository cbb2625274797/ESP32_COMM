/*
 * ESP32-C3FH4 OTA 中继 - 程序入口
 *
 * 初始化顺序（每位成功后 SystemBoot::mark=1，末尾统一 report）：
 *   1. 冷启动延时
 *   2. netif / event_loop / debug_log
 *   3. wifi earlyInit
 *   4. health_mon + task
 *   5. cpu_stats（可选）
 *   6. protocol / ota_coord
 *   7. spi init + L0 本机自检 → mark spi；HELLO 对端轻检 → mark can_link（非关键）
 *   8. ota_relay / wifi_ota / ble_ota 任务
 *   9. can_fb 任务（依赖 spi L0）
 *  10. ota_mark_valid（可选）
 *  11. SystemBoot::report()
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_ota_ops.h"

#include "system_boot.h"
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
#include "monitor/motor_ota_relay.h"

/** @brief 创建关键任务；成功返回 true。 */
static bool createCriticalTask(TaskFunction_t entry, const char *name,
                               uint32_t stack, UBaseType_t prio)
{
    return xTaskCreatePinnedToCore(entry, name, stack, NULL, prio, NULL, 0) == pdPASS;
}

/**
 * @brief 程序主入口：按模块初始化并汇总结果。
 */
extern "C" void app_main(void)
{
    SystemBoot::reset();

    /* 上电/欠压复位后短延时再初始化子系统 */
    {
        esp_reset_reason_t rr = esp_reset_reason();
        if (rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    Esp32OtaCore::initMutex();

    if (esp_netif_init() == ESP_OK) {
        SystemBoot::mark(BOOT_NETIF);
    }

    if (esp_event_loop_create_default() == ESP_OK) {
        SystemBoot::mark(BOOT_EVENT_LOOP);
    }

    DebugLog::instance().init();
    SystemBoot::mark(BOOT_DEBUG_LOG);

    /* earlyInit：true=已启动 STA 连接流程；false=将走 AP */
    const bool wifi_early_started_sta = WifiOta::instance().earlyInit();
    SystemBoot::mark(BOOT_WIFI_EARLY);

    HealthMonitor::instance().init();
    SystemBoot::mark(BOOT_HEALTH_MON);

    if (createCriticalTask(HealthMonitor::taskEntry, "health_mon",
                           TASK_STACK_HEALTH_MON, configMAX_PRIORITIES - 4)) {
        SystemBoot::mark(BOOT_HEALTH_TASK);
    }

    CpuStats::instance().start();
    SystemBoot::mark(BOOT_CPU_STATS);

    Protocol::instance().init();
    SystemBoot::mark(BOOT_PROTOCOL);

    OtaCoordinator::instance().init();
    OtaCoordinator::instance().setBleCoexListener(&WifiOta::instance());
    SystemBoot::mark(BOOT_OTA_COORD);

    bool spi_l0_ok = false;
    {
        esp_err_t ret = SpiMaster::instance().init();
        if (ret == ESP_OK && SpiMaster::instance().selfTestL0()) {
            SystemBoot::mark(BOOT_SPI);
            spi_l0_ok = true;
            /* 对端轻检：GD32 可能仍在启动延时，失败不拉垮关键启动 */
            if (SpiMaster::instance().selfTestPeerL0()) {
                SystemBoot::mark(BOOT_CAN_LINK);
            }
        }
    }

    if (createCriticalTask(OtaRelay::taskEntry, "ota_relay",
                           TASK_STACK_OTA_RELAY, configMAX_PRIORITIES - 2)) {
        SystemBoot::mark(BOOT_OTA_RELAY_TASK);
    }

    if (createCriticalTask(WifiOta::taskEntry, "wifi_ota",
                           TASK_STACK_WIFI_OTA, configMAX_PRIORITIES - 3)) {
        SystemBoot::mark(BOOT_WIFI_TASK);
    }

    if (createCriticalTask(BleOta::taskEntry, "ble_ota",
                           TASK_STACK_BLE_OTA, configMAX_PRIORITIES - 3)) {
        SystemBoot::mark(BOOT_BLE_TASK);
    }

    /* CAN 反馈依赖 SPI 本机 L0 */
    if (spi_l0_ok && startCanFeedbackTask()) {
        SystemBoot::mark(BOOT_CAN_FB_TASK);
    }

    motorOtaRelayInit();

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        SystemBoot::mark(BOOT_OTA_VALID);
    }

    /* ★ banner 文案必须包含「启动」，前端 polling.js 靠
     *   /OTA Relay .*启动/ 匹配本行判定 ESP32 重启完成（阶段 6）。 */
    DEBUG_INFO("MAIN", "ESP32-C3 OTA Relay %s 启动, heap=%lu, wifi_early=%s",
               FW_VERSION,
               (unsigned long)esp_get_free_heap_size(),
               wifi_early_started_sta ? "STA" : "AP/other");

    const bool critical_ok = SystemBoot::report();

    if (!critical_ok) {
        DEBUG_ERR("MAIN", "critical init FAIL, restart in 3s");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
}
