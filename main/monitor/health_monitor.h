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

// ============================================================
// 堆低水位快照（"最低空闲"创新低时记录上下文，供 /system/heap_low_log 查询）
// ============================================================
/** @brief 单条任务栈水位（用于低水位快照内嵌的任务栈表）。 */
struct HeapLowTaskStack {
    char     name[16];     ///< 任务名（截断）
    uint32_t remaining;    ///< 剩余栈字节（uxTaskGetStackHighWaterMark × sizeof(StackType_t)）
};

/** @brief 单条"堆低水位"快照记录。仅当 internal_min_free 出现新低时写入。 */
struct HeapLowWaterRecord {
    uint64_t ts_ms;            ///< 启动后毫秒（esp_timer_get_time / 1000）
    uint32_t free_heap;        ///< 当时的总空闲堆（esp_get_free_heap_size）
    uint32_t min_free_heap;    ///< 当时记录到的内部堆最低水位
    uint32_t largest_free_block; ///< 最大连续空闲块
    uint8_t  sys_state;        ///< OTA 系统状态码（ota_system_state_t）
    uint8_t  ota_channel;      ///< OTA 通道（0=WIFI, 1=BLE）
    int8_t  rssi;              ///< 最近 WiFi RSSI（dBm，-127=未知）
    float   temperature;       ///< 最近芯片温度（°C）
    /* 多任务栈水位表：按"剩余升序"取前 8 个，定位"是谁的栈突然涨了"。
     * task_stack_count=0 表示未采集到任务信息。 */
    HeapLowTaskStack task_stacks[8];
    uint8_t  task_stack_count;
    /* 触发低水位的"当前任务"：取当前 TaskHandle 的名字（taskENTER_CRITICAL 后取），
     * 用于推断是谁在分配堆。空串表示未知。 */
    char     current_task[16];
    /* 连接/OTA 进度上下文：区分"无连接 vs 中继期 vs 收尾期" */
    uint8_t  ble_connected;    ///< 0/1
    uint8_t  wifi_connected;   ///< 0/1
    uint32_t ota_stored_size;  ///< OTA 已存储字节
    uint32_t ota_total_size;   ///< OTA 总字节
    /* 嫌疑资源水位：定位"OTA/IDLE 期低谷由哪个子系统吃堆"。
     * 非 OTA 期这些应为 0；非 0 时即对应子系统持有堆缓冲。 */
    uint16_t ble_data_queue_depth; ///< BLE OTA 数据队列当前条数
    uint16_t ble_reorder_cached;   ///< BLE reorder 已缓存乱序包数
    uint16_t spi_pending_jobs;     ///< SPI master 待处理 job 数
    uint16_t reserved;             ///< 对齐保留
    /* 内存全景：区分"内部碎片"vs"系统级紧"。
     * default_free 含 PSRAM；internal_free = free_heap（含 SPIRAM 时与 default 区分）。
     * alloc_count = 当前活跃 alloc 数，看"是不是小块大量未释放"。 */
    uint32_t default_free_heap;    ///< MALLOC_CAP_DEFAULT free（含 PSRAM）
    uint32_t alloc_count;          ///< 内部堆当前 allocated_blocks（heap_caps_get_info）
    /* heap hook：「破新低时的最后一击 alloc」。
     * 不记任务名（WiFi IRAM 禁止；且发送路径末包常把任务刷成 wifi，误导主因）。
     * hook_task 现为类别串：net? / bulk / tiny（非 FreeRTOS 任务名）。
     * hook_caller_addr 用 addr2line 解析。 */
    char     hook_task[16];        ///< 分配类别 net?/bulk/tiny，不是任务名
    uint32_t hook_alloc_size;      ///< 最后一击 alloc 字节数
    uint32_t hook_caller_addr;     ///< 调用者返回地址（addr2line）
    uint32_t hook_free_after;      ///< 该次后的近似 free（est，非精确实测）
    uint32_t hook_fired;           ///< 0=本窗口 hook 未命中
    /* DHCP/IP 事件计数：每次 STA GOT_IP（含续约）+1。
     * 对比两次低谷记录的计数差，确认低谷是不是 DHCP 续约引起。 */
    uint32_t ip_event_count;       ///< 累计 IP 获得/续约次数
};

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

    /** @brief 健康监控 FreeRTOS 任务入口。 */
    static void taskEntry(void *pvParameters);

    /** @brief OTA 传输前执行健康检查。 */
    health_status_t checkBeforeOta();

    /** @brief OTA 中继转发阶段执行健康检查（跳过 WiFi 检查）。 */
    health_status_t checkBeforeOtaRelay();


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

    // ── 堆低水位快照 ──
    /** @brief 检测并可能覆盖写入唯一快照槽（线程安全）。
     *         由 health_mon 5s 巡检调用，仅当 internal_min_free 出现新低时写入。
     *         设备只保留最新 1 条；多条历史由网页 IndexedDB 累积。 */
    void maybeRecordHeapLowWater();

    /** @brief 将唯一快照序列化为 JSON：{"count":0|1,"records":[...]}。线程安全。 */
    int dumpHeapLowWaterJson(char *buf, size_t cap, int offset) const;

    /**
     * @brief 由 WiFi 事件回调推送连接状态（避免 HealthMonitor → WifiOta 依赖）。
     *        WiFi 连接/断开时由 wifi_ota 事件处理器主动调用，
     *        HealthMonitor 无需主动查询 WifiOta::isConnected()。
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
    // 温度监控 esp_timer 句柄存为成员，避免任务被删除后定时器仍在运行、
    //   回调继续访问 instance() 成员。
    esp_timer_handle_t m_tempTimer = nullptr;
    health_state_t m_healthState = SYS_NORMAL;
    float m_lastTemperature = 0.0f;
    int8_t m_lastWifiRssi = -127;
    int64_t m_lastWifiRssiMs = 0;
    TaskHandle_t m_taskHandle = NULL;
    // m_wifiConnected 由 m_mutex 保护（setWifiConnected 写 / checkBeforeOta 读）。
    bool m_wifiConnected = false;   ///< WiFi 连接状态（由 WiFi 事件推送）

    // 堆低水位：设备仅 1 槽（覆盖写）。m_mutex 保护。
    HeapLowWaterRecord m_heapLowWater = {};
    bool               m_heapLowWaterValid = false; ///< 是否有一条有效快照
    uint32_t           m_heapLowWaterLastMin = 0xFFFFFFFFu; ///< 上次记下的最低值

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
