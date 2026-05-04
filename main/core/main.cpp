/*
 * ESP32-C3FH4 OTA 中继 - 程序入口
 * 四个 FreeRTOS 任务：System Monitor / OTA Relay / WiFi Server / BLE GATT
 * 
 * 新增：周期性状态监控任务，每 10 秒打印系统状态
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

// 状态监控任务
static void status_monitor_task(void *pvParameters);

extern "C" void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    // 初始化网络接口 + 事件循环（必须最早）
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 初始化调试日志（UART0 先就绪，UDP socket 延迟到首次发送时创建）
    // 必须在 wifi_early_init 之前，这样 WiFi 初始化日志也能输出到上位机
    debug_log_init();

    // ★ WiFi 提前初始化（调度器启动前）
    // 包括：NVS → netif 创建 → event handler 注册 → WiFi driver init → 配置 → connect
    // 连接尝试立即开始，事件排队等调度器启动后处理
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

    // 健康监控任务（温度 + 电压 + WiFi RSSI）
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

    // 状态监控任务（低优先级，周期性打印系统状态）
    xRet = xTaskCreatePinnedToCore(
        status_monitor_task,
        "status_mon",
        4096,
        NULL,
        configMAX_PRIORITIES - 5,
        NULL,
        0
    );
    if (xRet != pdPASS) {
        DEBUG_ERR("MAIN", "创建 status_monitor_task 失败");
    } else {
        DEBUG_INFO("MAIN", "状态监控任务已创建");
    }

    DEBUG_INFO("MAIN", "========================================");
    DEBUG_INFO("MAIN", "所有任务创建完成，OTA Relay 运行中");
    DEBUG_INFO("MAIN", "调试日志输出: UDP(%s:%d) + UART0(GPIO21/TX, 115200)", 
               DEBUG_LOG_IP, DEBUG_LOG_PORT);
    DEBUG_INFO("MAIN", "========================================");
}

// ============================================================
// 状态监控任务 - 每 10 秒打印系统状态
// ============================================================
static void status_monitor_task(void *pvParameters)
{
    (void)pvParameters;
    DEBUG_INFO("STAT", "状态监控任务启动");

    uint32_t loop_count = 0;

    while (1) {
        loop_count++;
        
        // 获取系统信息
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t min_free_heap = esp_get_minimum_free_heap_size();
        health_state_t health = health_get_current();
        relay_state_t relay = ota_relay_get_state();
        int8_t rssi = wifi_get_rssi();
        bool wifi_conn = wifi_is_connected();

        // 打印状态头
        DEBUG_INFO("STAT", "========== 系统状态 #%lu ==========", (unsigned long)loop_count);
        
        // 内存状态
        DEBUG_INFO("STAT", "内存: 空闲=%lu 字节, 最低=%lu 字节", 
                   (unsigned long)free_heap, (unsigned long)min_free_heap);
        
        // 健康状态
        const char *health_str;
        switch (health) {
            case SYS_NORMAL:       health_str = "正常"; break;
            case SYS_WARNING:      health_str = "警告"; break;
            case SYS_CRITICAL:     health_str = "危险"; break;
            case SYS_SAFE_SHUTDOWN: health_str = "安全关机"; break;
            default:               health_str = "未知"; break;
        }
        DEBUG_INFO("STAT", "健康状态: %s (%d)", health_str, health);

        // WiFi 状态
        if (wifi_conn) {
            DEBUG_INFO("STAT", "WiFi: 已连接, RSSI=%d dBm", rssi);
        } else {
            DEBUG_INFO("STAT", "WiFi: 未连接");
        }

        // OTA 中继状态
        const char *relay_str;
        switch (relay) {
            case RELAY_IDLE:              relay_str = "空闲"; break;
            case RELAY_HANDSHAKE_IDLE:    relay_str = "等待握手"; break;
            case RELAY_HANDSHAKE_SENT:    relay_str = "握手请求已发送"; break;
            case RELAY_CHALLENGE_SENT:    relay_str = "挑战已发送"; break;
            case RELAY_CHALLENGE_RESPOND: relay_str = "收到握手响应"; break;
            case RELAY_SESSION_KEY_SENT:  relay_str = "会话密钥已发送"; break;
            case RELAY_AUTH_COMPLETE:     relay_str = "认证完成"; break;
            case RELAY_SIZE_SENT:         relay_str = "大小已发送"; break;
            case RELAY_SIZE_ACK:          relay_str = "大小确认"; break;
            case RELAY_RECEIVING:         relay_str = "接收中"; break;
            case RELAY_RELAY_ACTIVE:      relay_str = "转发中"; break;
            case RELAY_PACKET_SENT:       relay_str = "数据包已发送"; break;
            case RELAY_PACKET_ACK_OK:     relay_str = "ACK 正常"; break;
            case RELAY_PACKET_ACK_RETRY:  relay_str = "需要重试"; break;
            case RELAY_RECEIVE_DONE:      relay_str = "接收完成"; break;
            case RELAY_TRANSFER_DONE:     relay_str = "传输完成"; break;
            case RELAY_FINISH_SENT:       relay_str = "结束已发送"; break;
            case RELAY_DONE:              relay_str = "OTA 完成"; break;
            case RELAY_ERROR:             relay_str = "错误"; break;
            default:                      relay_str = "未知"; break;
        }
        DEBUG_INFO("STAT", "OTA 中继: %s (%d)", relay_str, relay);

        DEBUG_INFO("STAT", "======================================");

        // 每 10 秒打印一次
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}