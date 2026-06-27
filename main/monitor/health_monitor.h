/*
 * health_monitor.h - 系统健康监控头文件
 *
 * HealthMonitor 类封装健康状态管理、温度监控和 WiFi RSSI 监控。
 */
#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/temperature_sensor.h"
#include "esp_timer.h"
#include "app_config.h"

// T9.3: 健康状态等级
typedef enum {
    SYS_NORMAL = 0,        // 正常
    SYS_WARNING = 1,       // 警告
    SYS_CRITICAL = 2,     // 危险
    SYS_SAFE_SHUTDOWN = 3  // 安全关机
} health_state_t;

/** @brief 健康监控模块内部状态码，用于函数返回值。 */
typedef enum {
    HEALTH_MON_OK = 0,       ///< 健康检查通过
    HEALTH_MON_WARNING = 1,  ///< 健康检查警告（可继续）
    HEALTH_MON_CRITICAL = 2, ///< 健康检查危险（建议中止）
    HEALTH_MON_SHUTDOWN = 3  ///< 需要安全关机
} health_status_t;

// WiFi RSSI 阈值（来自 app_config.h）
#define WIFI_RSSI_NORMAL   WIFI_RSSI_WARNING  ///< WiFi 信号正常阈值（别名）
#define WIFI_RSSI_WEAK     WIFI_RSSI_CRITICAL ///< WiFi 信号弱阈值（别名）

/**
 * @brief 系统健康监控类（单例）
 *
 * 封装健康状态管理、温度定时监控和 WiFi RSSI 监控。
 */
class HealthMonitor {
public:
    /**
      * @brief 获取 HealthMonitor 单例实例。
     * @return HealthMonitor 引用。
     */
    static HealthMonitor& instance();

    /** @brief 初始化健康监控模块，创建内部互斥锁与基础状态。 */
    void init();

    /**
      * @brief 反初始化：停止并删除温度监控定时器，回收 esp_timer 资源。
      *        在任务删除路径中调用；当前 HealthMonitor 任务为无限循环，
      *        无显式删除路径，本方法预留给未来停用场景使用。
      */
    void deinit();

    /** @brief 健康监控 FreeRTOS 任务入口。 */
    static void taskEntry(void *pvParameters);

    /** @brief OTA 传输前执行健康检查。 */
    health_status_t checkBeforeOta();

    /** @brief OTA 中继转发阶段执行健康检查（跳过 WiFi 检查）。 */
    health_status_t checkBeforeOtaRelay();

    /** @brief 记录一条健康事件日志。 */
    void logEvent(const char *event, int value);

    /** @brief 获取当前系统健康状态（线程安全）。 */
    health_state_t getCurrent();

    /** @brief 根据 WiFi RSSI 更新健康状态。 */
    health_status_t updateFromWifiRssi(int8_t rssi);

    /** @brief 获取最近一次芯片温度。 */
    float getLastTemperature();

    /** @brief 获取最近一次 WiFi RSSI 及其年龄。 */
    bool getLastWifiRssi(int8_t *out_rssi, uint32_t *out_age_ms);

    /** @brief 获取健康监控任务句柄。 */
    TaskHandle_t getTaskHandle() const { return m_taskHandle; }

    /**
     * @brief 由 WiFi 事件回调推送连接状态（解耦 HealthMonitor → WifiOta 依赖）。
     *        WiFi 连接/断开时由 wifi_ota 事件处理器主动调用，
     *        HealthMonitor 不再主动查询 WifiOta::isConnected()。
     * @param connected true=已连接，false=断开。
     */
    void setWifiConnected(bool connected);

private:
    /** @brief 加锁读取健康状态。 */
    health_state_t getState();

    /** @brief 加锁设置健康状态。 */
    void setState(health_state_t newState);

    HealthMonitor() = default;
    ~HealthMonitor() = default;
    HealthMonitor(const HealthMonitor&) = delete;
    HealthMonitor& operator=(const HealthMonitor&) = delete;

    SemaphoreHandle_t m_mutex = NULL;
    temperature_sensor_handle_t m_tempHandle = NULL;
    // 温度监控 esp_timer 句柄存为成员，便于 deinit() 时停止并删除定时器，
    //   避免任务被删除后定时器仍在运行、回调继续访问 instance() 成员。
    esp_timer_handle_t m_tempTimer = nullptr;
    health_state_t m_healthState = SYS_NORMAL;
    float m_lastTemperature = 0.0f;
    int8_t m_lastWifiRssi = -127;
    int64_t m_lastWifiRssiMs = 0;
    TaskHandle_t m_taskHandle = NULL;
    // m_wifiConnected 由 m_mutex 保护（setWifiConnected 写 / checkBeforeOta 读）。
    //   volatile 保留为历史兼容，实际同步由 LockGuard 提供。
    volatile bool m_wifiConnected = false;   ///< WiFi 连接状态（由 WiFi 事件推送）

    static void tempMonitorCallback(void *arg);

    // RAII 锁守卫（与 SpiMaster::LockGuard 风格一致）
    class LockGuard {
    public:
        explicit LockGuard(SemaphoreHandle_t mutex) : m_lock(mutex) {
            if (m_lock) { xSemaphoreTake(m_lock, portMAX_DELAY); }
        }
        ~LockGuard() {
            if (m_lock) { xSemaphoreGive(m_lock); }
        }
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;
    private:
        SemaphoreHandle_t m_lock;
    };
};
