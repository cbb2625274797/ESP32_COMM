/*
 * health_monitor.cpp - 系统健康监控
 * 包含：健康状态管理 + WiFi RSSI 监控 + 温度监控
 *
 * 所有功能封装为 HealthMonitor 类方法。
 * 所有对 m_healthState / m_lastTemperature / m_lastWifiRssi 的访问
 * 均通过 m_mutex 保护，消除跨任务竞态。
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "driver/temperature_sensor.h"

#include "health_monitor.h"
#include "debug_log.h"
#include "task_stack.h"
#include "ota/ota_coordinator.h"
#include "ble_ota.h"
#include "wifi_ota.h"
#include "spi_master.h"
#include "esp_event.h"
#include "esp_netif_types.h"

// ============================================================
// heap hook：捕获"破新低时的最后一次 alloc"（最后一击，未必是主因）
// ============================================================
// CONFIG_HEAP_USE_HOOKS=y 时，每次 malloc 成功后进入本弱符号。
//
// ★ 为何不能记任务名（用户看到"每次都是 WiFi"的根因）：
//   1) WiFi IRAM 路径禁止在 hook 里调 FreeRTOS/pcTaskGetName（会搞挂 STA）。
//   2) 即便能取任务名：HTTP/UDP **发送前** lwip 仍会为 pbuf/TX 再 malloc，
//      芯片 minimum free 往往在这条"最后一击"上被压低，任务名就会是 wifi/tiT/httpd，
//      看起来像"WiFi 吃堆"，实际只是发送路径的末包分配。
//   因此 hook **永不写任务名**；只记 size + caller 地址（addr2line）。
//   前端诊断应标明「最后一击 / 可能网络发送」，勿把连接列的 WiFi 当成触发任务。
//
// ★ 记录策略：health_mon 周期把真实 free 同步进 g_est_free；
//   hook 内仅做 g_est_free -= size（C3 单核，无锁可接受漂移）。
//   当 est 创新低时更新 hook 快照。free hook 不知 size，靠周期 resync 纠偏。
//
// ★ 采样协议（消除多字段撕裂读）：
//   hook 写「最后一击」快照的多个字段（min_est/alloc_size/caller/free_after/fired
//   + recent_allocs[]）虽各自是字宽 volatile，但 reader 跨字段组合可撕裂
//   （fired=1 配上上一次的 alloc_size）。reader（maybeRecordHeapLowWater）在
//   portENTER_CRITICAL(&s_hookMux) 内一次性拷出全部字段——单核禁中断即与 hook
//   （在 heap 临界区内写）互斥。m_mutex 不参与此互斥（hook 不获取它）。
//   reader 临界区仅做 volatile 拷贝，不分配、不调 FreeRTOS/flash，保持极短。
namespace {
/** 最近若干次「值得关注」的 alloc（size≥阈值），供破新低时对照。 */
struct RecentAllocSnap {
    uint32_t size;
    uint32_t caller;
};
static constexpr uint32_t kHookInterestingMin = 48;   ///< 忽略极小碎片分配
static constexpr size_t   kRecentAllocCap     = 4;

volatile uint32_t g_hook_min_est        = 0xFFFFFFFFu;  ///< hook 见过的最低 est free
volatile uint32_t g_hook_alloc_size     = 0;            ///< 触发 est 新低的那次 alloc 大小
volatile uint32_t g_hook_caller_addr    = 0;            ///< 调用者返回地址
volatile uint32_t g_hook_free_after     = 0;            ///< 那次后的 est free（estimated，非精确实测）
volatile uint32_t g_hook_fired          = 0;
volatile uint32_t g_hook_armed          = 0;
volatile uint32_t g_est_free            = 0;            ///< estimated 近似空闲（只减不增），health_mon 周期用 esp_get_free_heap_size() 实测 resync 纠偏；误差上界 ≈ resync 周期(5s)内总 alloc 字节
volatile uint32_t g_ip_event_count      = 0;

RecentAllocSnap           g_recent_allocs[kRecentAllocCap] = {};
volatile uint32_t         g_recent_alloc_w    = 0;
volatile uint32_t         g_recent_alloc_n    = 0;

/* s_hookMux：reader（maybeRecordHeapLowWater，任务上下文）取多字段一致快照用的
 * portMUX（单核 = 禁中断）。hook 端不加锁——它在 heap 临界区内写，与 reader 的
 * 禁中断互斥。仅用于消除「最后一击」快照多字段撕裂读，不保护 m_heapLowWater
 * （后者由 m_mutex 保护）。 */
static portMUX_TYPE s_hookMux = portMUX_INITIALIZER_UNLOCKED;

void hookResyncEstFree(uint32_t real_free)
{
    g_est_free = real_free;
}

/** @brief reader 一次性拷出的 hook 共享状态一致快照（任务上下文，禁中断极短临界区）。 */
struct HookSnap {
    uint32_t        alloc_size;
    uint32_t        caller_addr;
    uint32_t        free_after;     ///< estimated（非实测）
    uint32_t        fired;
    uint32_t        est_free;
    uint32_t        min_est;
    uint32_t        recent_n;
    RecentAllocSnap recent[kRecentAllocCap];
};

/** @brief 任务上下文用：禁中断拷出 hook 共享状态一致快照。仅 volatile 拷贝，不分配/不调 FreeRTOS。 */
HookSnap captureHookSnapshot()
{
    HookSnap s = {};
    portENTER_CRITICAL(&s_hookMux);
    s.alloc_size  = g_hook_alloc_size;
    s.caller_addr = g_hook_caller_addr;
    s.free_after  = g_hook_free_after;
    s.fired       = g_hook_fired;
    s.est_free    = g_est_free;
    s.min_est     = g_hook_min_est;
    s.recent_n    = g_recent_alloc_n;
    for (uint32_t i = 0; i < s.recent_n && i < (uint32_t)kRecentAllocCap; ++i) {
        s.recent[i] = g_recent_allocs[i];
    }
    portEXIT_CRITICAL(&s_hookMux);
    return s;
}

/** @brief 在 s_hookMux 临界区内调用：消费已记录的 hook 命中，重置 min_est/fired。
 *  必须由 reader 在写完 HeapLowWaterRecord 后调用，避免同一窗口重复记。 */
void consumeHookFiredInLock()
{
    /* min_est 抬到当前 est：本次已记下低谷，下次需更低才会再次触发。
     * g_est_free 在此读以与 hook 写一致（s_hookMux 内）。 */
    g_hook_min_est = g_est_free;
    g_hook_fired   = 0;
}
}  // namespace

// ESP-IDF heap alloc hook：仅 DRAM 标量 + 内联 return address，禁止 FreeRTOS/flash 长函数。
//
// ★ hook 可调用约束（IRAM 安全契约）：
//   - 禁止：FreeRTOS API、flash/rodata 访问、mutex/semaphore、长函数。
//   - 允许：DRAM volatile 标量读写、__builtin_return_address、纯算术。
//   - 写共享状态天然原子：hook 已在 heap 临界区内（禁中断），reader 用 s_hookMux
//     禁中断取一致快照，二者互斥；hook 端不加锁、保持 IRAM 安全。
//   - 嵌套分配：hook 内禁止任何可能触发 malloc 的调用（否则递归进 hook，armed
//     标志不防递归——当前 hook 体内无分配调用，保持之）。
extern "C"
void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps)
{
    (void)caps;
    if (!ptr || size == 0 || g_hook_armed == 0) {
        return;
    }
    const uint32_t sz = (uint32_t)size;

    /* 近似空闲：只减不增（free 路径不在此修正），health_mon 周期 resync。 */
    uint32_t est = g_est_free;
    if (est >= sz) {
        est -= sz;
    } else {
        est = 0;
    }
    g_est_free = est;

    /* 环形记下近期「有意义」分配，破新低时可读到发送路径小包之前的大块。 */
    if (sz >= kHookInterestingMin) {
        const uint32_t w = g_recent_alloc_w % (uint32_t)kRecentAllocCap;
        g_recent_allocs[w].size   = sz;
        g_recent_allocs[w].caller = (uint32_t)(uintptr_t)__builtin_return_address(0);
        g_recent_alloc_w = w + 1U;
        if (g_recent_alloc_n < (uint32_t)kRecentAllocCap) {
            g_recent_alloc_n = g_recent_alloc_n + 1U;
        }
    }

    /* est 创新低 → 记「最后一击」alloc（常为网络 pbuf，任务若可读会是 wifi） */
    if (est < g_hook_min_est) {
        g_hook_min_est     = est;
        g_hook_alloc_size  = sz;
        g_hook_caller_addr = (uint32_t)(uintptr_t)__builtin_return_address(0);
        g_hook_free_after  = est;
        g_hook_fired       = 1;
        /* 不采任务名（WiFi IRAM 路径禁止 FreeRTOS 调用） */
    }
}

// IP 事件 handler：STA 获得 IP（含 DHCP 续约）时计数 +1
void ipEventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg; (void)event_base; (void)event_id; (void)event_data;
    if (event_base == IP_EVENT && (event_id == IP_EVENT_STA_GOT_IP || event_id == IP_EVENT_ETH_GOT_IP)) {
        uint32_t cur = g_ip_event_count;
        g_ip_event_count = cur + 1;
    }
}

// ============================================================
// 栈水位主动巡检
// ============================================================
// 告警阈值：剩余高水位 < 栈大小 20% 即告警（复用 /system/status 的任务表）。
// 去重：每个任务只在"水位由安全跌入危险"时告警一次，恢复后再跌入才再次告警，
//   避免 5 秒巡检刷屏。health_mon 任务从不被 OTA 暂停，DEBUG_ERR 经 UDP/UART
//   直发，不受 OTA 门控抑制。
static constexpr float STACK_WARN_RATIO = 0.20f;   ///< 剩余水位告警阈值（栈大小比例）
static constexpr size_t STACK_TASK_MAX = 12;       ///< 受监控任务上限（含 spi_worker 等）
static bool s_stackWarned[STACK_TASK_MAX] = {};    ///< 各任务是否已告警（去重）

// ============================================================
// 单例
// ============================================================
/**
  * @brief 获取 HealthMonitor 单例实例。
 * @return HealthMonitor 引用。
 */
HealthMonitor& HealthMonitor::instance()
{
    static HealthMonitor inst;
    return inst;
}

// ============================================================
// 初始化
// ============================================================
/**
  * @brief 初始化健康监控模块，创建内部互斥锁与基础状态。
 */
void HealthMonitor::init()
{
    m_mutex = xSemaphoreCreateMutex();
    if (m_mutex == NULL) {
        DEBUG_ERR("HEALTH", "创建健康状态互斥锁失败");
    }

    // 注册 IP 事件 handler，统计 STA 获得 IP（含 DHCP 续约）次数。
    //   配合 heap_low_log 快照的 ip_event_count 字段，可验证"低谷是不是 DHCP 续约引起"。
    //   失败仅告警，不致命（计数器保持 0，快照里 ip_event_count 永远 0）。
    esp_err_t ev = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ipEventHandler, nullptr);
    if (ev != ESP_OK) {
        DEBUG_WARN("HEALTH", "IP 事件 handler 注册失败: %s", esp_err_to_name(ev));
    }

    // 武装 heap alloc hook：init() 在 app_main 之后调用。armed=0 时 hook 直接 return。
    // hook 本身禁止任何 FreeRTOS/flash 调用（见 esp_heap_trace_alloc_hook 注释）。
    g_hook_armed = 1;
}

// ============================================================
// 线程安全的健康状态读写
// ============================================================
/**
  * @brief 加锁读取当前健康状态。
 * @return 当前健康状态。
 */
health_state_t HealthMonitor::getState()
{
    LockGuard lock(m_mutex);
    return m_healthState;
}

/**
  * @brief 加锁设置健康状态。
 * @param newState 新的健康状态。
 */
void HealthMonitor::setState(health_state_t newState)
{
    LockGuard lock(m_mutex);
    m_healthState = newState;
}

// ============================================================
// 获取当前健康状态（public 接口）
// ============================================================
/**
  * @brief 获取当前系统健康状态（线程安全）。
 * @return 当前健康状态。
 */
health_state_t HealthMonitor::getCurrent()
{
    return getState();
}

/**
  * @brief 获取最近一次芯片温度。
 * @return 温度值（摄氏度）。
 */
float HealthMonitor::getLastTemperature()
{
    LockGuard lock(m_mutex);
    return m_lastTemperature;
}

/**
  * @brief 获取最近一次 WiFi RSSI 及其年龄。
 * @param out_rssi 输出参数，返回 RSSI 值。
 * @param out_age_ms 输出参数，返回样本年龄（毫秒）。
 * @return 是否有有效数据。
 */
bool HealthMonitor::getLastWifiRssi(int8_t *out_rssi, uint32_t *out_age_ms)
{
    int64_t lastMs;
    int8_t  rssi;
    {
        LockGuard lock(m_mutex);
        lastMs = m_lastWifiRssiMs;
        rssi   = m_lastWifiRssi;
    }

    if (lastMs <= 0) {
        return false;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    uint32_t age_ms = (now_ms > lastMs) ? (uint32_t)(now_ms - lastMs) : 0;

    if (out_rssi)  *out_rssi  = rssi;
    if (out_age_ms) *out_age_ms = age_ms;
    return true;
}

// ============================================================
// 温度监控（定时器回调）
// ============================================================
/**
  * @brief 温度监控定时器回调函数。
 * @param arg 用户参数（未使用）。
 */
void HealthMonitor::tempMonitorCallback(void *arg)
{
    auto& self = instance();
    float temp = 0.0f;
    esp_err_t err = temperature_sensor_get_celsius(self.m_tempHandle, &temp);
    if (err != ESP_OK) {
        DEBUG_WARN("TEMP", "读取温度失败: %s", esp_err_to_name(err));
        return;
    }

    {
        LockGuard lock(self.m_mutex);
        self.m_lastTemperature = temp;
    }

    if (temp >= TEMP_CRITICAL) {
        self.setState(SYS_SAFE_SHUTDOWN);
        DEBUG_ERR("TEMP", "过热 %.1fC>=%dC 安全关机", temp, TEMP_CRITICAL);
        DEBUG_ERR("TEMP", "健康状态变更为: SYS_SAFE_SHUTDOWN");
    } else if (temp >= TEMP_WARNING) {
        health_state_t cur = self.getState();
        if (cur < SYS_WARNING) {
            self.setState(SYS_WARNING);
            DEBUG_WARN("TEMP", "警告: 温度 %.1f°C >= %d°C", temp, TEMP_WARNING);
            DEBUG_WARN("TEMP", "健康状态变更为: SYS_WARNING");
        }
    }
}

// ============================================================
// FreeRTOS 任务入口
// ============================================================
/**
  * @brief 健康监控 FreeRTOS 任务入口。
 * @param pvParameters 任务参数。
 */
void HealthMonitor::taskEntry(void *pvParameters)
{
    (void)pvParameters;
    auto& self = instance();
    self.m_taskHandle = xTaskGetCurrentTaskHandle();
    DEBUG_INFO("MON", "=== 健康监控任务启动 ===");

    temperature_sensor_config_t temp_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 80);
    esp_err_t err = temperature_sensor_install(&temp_config, &self.m_tempHandle);
    if (err != ESP_OK) {
        DEBUG_ERR("MON", "温度传感器初始化失败: %s", esp_err_to_name(err));
    } else {
        ESP_ERROR_CHECK(temperature_sensor_enable(self.m_tempHandle));
    }

    // 定时器，每1秒读取一次温度
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &tempMonitorCallback;
    timer_args.arg = nullptr;
    timer_args.name = "temp_monitor";
    timer_args.dispatch_method = ESP_TIMER_TASK;

    if (esp_timer_create(&timer_args, &self.m_tempTimer) == ESP_OK) {
        esp_timer_start_periodic(self.m_tempTimer, 1000000);  // 1秒
    } else {
        self.m_tempTimer = nullptr;
        DEBUG_ERR("MON", "创建温度监控定时器失败");
    }

    // 周期性巡检
    while (1) {
        // 栈溢出 canary 触发检测：vApplicationStackOverflowHook 置位后，
        //   在 health_mon 完好栈上用 DEBUG_ERR 经 UDP/UART 发详细告警。
        //   标志只在置位后告警一次，不自动清除（canary 触发即不可逆，需重启恢复）。
        if (g_stackOverflowDetected) {
            DEBUG_ERR("MON", "栈溢出(canary),检查任务栈");
            // 告警一次后清标志，避免 5 秒刷屏；后续若再次触发会重新置位
            g_stackOverflowDetected = false;
        }

        uint32_t free_heap = esp_get_free_heap_size();
        /* 同步 hook 近似空闲：纠正 free 路径未记账造成的向下漂移。
         * 必须在 maybeRecord 之前，否则 est 长期偏低会误触发 hook 快照。 */
        hookResyncEstFree(free_heap);

        // 堆低水位快照：仅在"最低空闲"创新低时记录一条上下文（5s 检测足够）
        self.maybeRecordHeapLowWater();

        if (free_heap < HEAP_WARNING_MIN) {
            /* 堆告警节流：OTA 期间阈值更低、间隔更长 */
            static TickType_t s_last_heap_warn = 0;
            TickType_t now = xTaskGetTickCount();
            const bool ota_busy = !OtaCoordinator::instance().isIdle();
            const uint32_t interval_ms = ota_busy ? 15000U
                : (free_heap < 8192U ? 10000U : 3000U);
            const uint32_t warn_floor = ota_busy ? 4096U : (uint32_t)HEAP_WARNING_MIN;
            if (free_heap < warn_floor &&
                (now - s_last_heap_warn) >= pdMS_TO_TICKS(interval_ms)) {
                s_last_heap_warn = now;
                uint32_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
                DEBUG_WARN("MON", "堆紧%s free=%lu min=%lu big=%lu",
                           ota_busy ? "(OTA中)" : "",
                           (unsigned long)free_heap,
                           (unsigned long)esp_get_minimum_free_heap_size(),
                           (unsigned long)largest_block);
            }
        }
        health_state_t cur = self.getState();
        if (cur >= SYS_CRITICAL) {
            DEBUG_ERR("MON", "健康状态危险: %d", cur);
        }

        // 栈水位主动巡检：复用 /system/status 的任务表，低水位时 DEBUG_ERR 主动告警。
        //   health_mon 任务不被 OTA 暂停，告警经 UDP/UART 直发，穿透 OTA 门控。
        TaskStackMetric metrics[STACK_TASK_MAX];
        size_t count = collectTaskStackMetrics(metrics, STACK_TASK_MAX);
        for (size_t i = 0; i < count && i < STACK_TASK_MAX; ++i) {
            if (metrics[i].handle == NULL || metrics[i].stack_size_bytes == 0) {
                continue;
            }
            UBaseType_t hw = uxTaskGetStackHighWaterMark(metrics[i].handle);
            size_t warn_bytes = (size_t)(metrics[i].stack_size_bytes * STACK_WARN_RATIO);
            if (hw < (UBaseType_t)warn_bytes) {
                if (!s_stackWarned[i]) {
                    DEBUG_ERR("MON", "栈低 task=%s left=%lu thr=%lu sz=%lu",
                              metrics[i].name,
                              (unsigned long)hw,
                              (unsigned long)warn_bytes,
                              (unsigned long)metrics[i].stack_size_bytes);
                    s_stackWarned[i] = true;
                }
            } else if (s_stackWarned[i]) {
                // 水位恢复，清除告警标志，允许下次再次跌入时告警
                s_stackWarned[i] = false;
                DEBUG_INFO("MON", "栈水位恢复: 任务=%s 剩余=%luB",
                           metrics[i].name, (unsigned long)hw);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ============================================================
// OTA 健康检查
// ============================================================
/**
  * @brief OTA 传输前执行健康检查。
 * @return 健康检查状态。
 */
health_status_t HealthMonitor::checkBeforeOta()
{
    health_status_t status = HEALTH_MON_OK;
    health_state_t cur = getState();

    if (cur >= SYS_SAFE_SHUTDOWN) {
        DEBUG_WARN("HEALTH", "OTA 前检查: SYS_SAFE_SHUTDOWN，禁止传输");
        return HEALTH_MON_SHUTDOWN;
    } else if (cur >= SYS_CRITICAL) {
        DEBUG_WARN("HEALTH", "OTA 前检查: SYS_CRITICAL，禁止传输");
        return HEALTH_MON_CRITICAL;
    } else if (cur >= SYS_WARNING) {
        status = HEALTH_MON_WARNING;
        DEBUG_DBG("HEALTH", "OTA 前检查: SYS_WARNING，继续传输但需注意");
    }

    bool wifi_connected;
    {
        LockGuard lock(m_mutex);
        wifi_connected = m_wifiConnected;
    }
    if (!wifi_connected) {
        DEBUG_WARN("HEALTH", "OTA 前检查: WiFi 未连接");
        return HEALTH_MON_CRITICAL;
    }

    int8_t last_rssi = -127;
    uint32_t age_ms = 0;
    if (getLastWifiRssi(&last_rssi, &age_ms)) {
        if (age_ms <= OTA_WIFI_RSSI_SAMPLE_MAX_AGE_MS) {
            if (last_rssi < OTA_WIFI_RSSI_CRITICAL) {
                DEBUG_WARN("HEALTH", "OTA前 RSSI %d<%d age=%lums",
                           last_rssi, OTA_WIFI_RSSI_CRITICAL, (unsigned long)age_ms);
                return HEALTH_MON_CRITICAL;
            } else if (last_rssi < WIFI_RSSI_NORMAL) {
                if (status < HEALTH_MON_WARNING) {
                    status = HEALTH_MON_WARNING;
                }
                DEBUG_WARN("HEALTH", "OTA前 RSSI %d<%d age=%lums",
                           last_rssi, WIFI_RSSI_NORMAL, (unsigned long)age_ms);
            }
        }
    }

    if (status == HEALTH_MON_OK) {
        DEBUG_DBG("HEALTH", "OTA 前检查: 正常");
    }

    return status;
}

/**
  * @brief OTA 中继转发阶段执行健康检查（跳过 WiFi 检查）。
 * @return 健康检查状态。
 */
health_status_t HealthMonitor::checkBeforeOtaRelay()
{
    health_status_t status = HEALTH_MON_OK;
    health_state_t cur = getState();

    if (cur >= SYS_SAFE_SHUTDOWN) {
        DEBUG_WARN("HEALTH", "OTA 中继检查: SYS_SAFE_SHUTDOWN，禁止传输");
        return HEALTH_MON_SHUTDOWN;
    } else if (cur >= SYS_CRITICAL) {
        /* SYS_CRITICAL 可能是由 WiFi RSSI 过低导致的，
         * 但 SPI 中继阶段不依赖 WiFi，只检查温度/硬件等本地状态。
         * 温度达到 TEMP_CRITICAL(100°C) 时 m_healthState 会被设为
         * SYS_SAFE_SHUTDOWN，在上面的分支已经拦截。
         * 所以 SYS_CRITICAL 在中继阶段只可能是 WiFi 导致的，允许继续。 */
        DEBUG_WARN("HEALTH", "OTA 中继检查: SYS_CRITICAL (WiFi 相关)，SPI 中继允许继续");
    } else if (cur >= SYS_WARNING) {
        status = HEALTH_MON_WARNING;
        DEBUG_DBG("HEALTH", "OTA 中继检查: SYS_WARNING，继续传输");
    }

    /* 跳过 WiFi 连接状态和 RSSI 检查：
     * SPI 中继阶段固件已存储在 ESP32 Flash 中，
     * 不依赖 WiFi 连接，无需检查 WiFi 状态。 */

    if (status == HEALTH_MON_OK) {
        DEBUG_DBG("HEALTH", "OTA 中继检查: 正常");
    }

    return status;
}

// ============================================================
// WiFi 连接状态推送（由 WiFi 事件回调调用）
// ============================================================
/**
  * @brief 由 WiFi 事件回调推送连接状态。
 * @param connected true=已连接，false=断开。
 */
void HealthMonitor::setWifiConnected(bool connected)
{
    LockGuard lock(m_mutex);
    m_wifiConnected = connected;
}

// ============================================================
// WiFi RSSI 更新
// ============================================================
/**
  * @brief 根据 WiFi RSSI 更新健康状态。
 * @param rssi WiFi RSSI 值。
 * @return 健康检查状态。
 */
health_status_t HealthMonitor::updateFromWifiRssi(int8_t rssi)
{
    health_state_t cur = getState();
    health_state_t next = SYS_NORMAL;
    if (rssi < WIFI_RSSI_CRITICAL) {
        next = SYS_CRITICAL;
    } else if (rssi < WIFI_RSSI_NORMAL) {
        next = SYS_WARNING;
    }

    // 仅在状态发生转换时打印一次，避免同一阈值下反复刷屏。
    if (next != cur) {
        if (next == SYS_CRITICAL) {
            setState(SYS_CRITICAL);
            DEBUG_WARN("HEALTH", "WiFi RSSI 危险: %d dBm < %d dBm", rssi, WIFI_RSSI_CRITICAL);
        } else if (next == SYS_WARNING) {
            setState(SYS_WARNING);
            DEBUG_WARN("HEALTH", "WiFi RSSI 警告: %d dBm < %d dBm", rssi, WIFI_RSSI_NORMAL);
        } else { // next == SYS_NORMAL，从 WARNING/CRITICAL 恢复
            setState(SYS_NORMAL);
            DEBUG_INFO("HEALTH", "RSSI恢复 %d>=%d (lv%d)", rssi, WIFI_RSSI_NORMAL, (int)cur);
        }
    }

    if (next == SYS_CRITICAL) return HEALTH_MON_CRITICAL;
    if (next == SYS_WARNING) return HEALTH_MON_WARNING;

    return HEALTH_MON_OK;
}

// ============================================================
// 堆低水位快照（"最低空闲"创新低时记录上下文）
// ============================================================
namespace {
/** @brief snprintf 追加到 buf 的安全封装：截断但不越界，返回新 offset。 */
int appendJsonfSafe(char *buf, size_t cap, int offset, const char *fmt, ...)
{
    if (offset < 0 || (size_t)offset >= cap) return offset;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + offset, cap - (size_t)offset, fmt, ap);
    va_end(ap);
    if (n < 0) return offset;
    if ((size_t)n >= cap - (size_t)offset) {
        // 被截断：vsnprintf 返回的是"本应写入"的长度，按截断后修正 offset
        return (int)cap - 1;
    }
    return offset + n;
}
} // namespace

/**
 * @brief 检测"内部堆最低空闲"是否创新低，是则覆盖写入唯一快照槽。
 *
 * 调用方：health_mon 5s 巡检任务。仅在"新低"时写一条，避免每 5s 刷一条无意义记录。
 * 上下文字段：OTA 系统状态、通道、RSSI、温度、栈剩余最少的任务名+剩余字节。
 * 所有外部读取（OtaCoordinator::getSnapshot / collectTaskStackMetrics / rssi / temp）
 * 都先在锁外取，避免持锁期间嵌套调用其它模块的锁；持锁后只做字段拷贝。
 */
void HealthMonitor::maybeRecordHeapLowWater()
{
    // 与 http_handlers.cpp 同口径：内部 DRAM 数据堆
    static constexpr uint32_t CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    const uint32_t cur_min = (uint32_t)heap_caps_get_minimum_free_size(CAPS);

    // 不创新低则直接返回（不持锁）
    if (cur_min >= m_heapLowWaterLastMin) {
        return;
    }

    // ── 锁外收集上下文 ──
    const uint32_t free_heap       = (uint32_t)esp_get_free_heap_size();
    const uint32_t largest_block   = (uint32_t)heap_caps_get_largest_free_block(CAPS);

    /* hook 共享状态一致快照：禁中断一次性拷出，避免多字段撕裂读。
     * 必须在 m_mutex 外取——s_hookMux 与 m_mutex 不可嵌套持锁。 */
    const HookSnap hs = captureHookSnapshot();

    OtaSnapshot snap = OtaCoordinator::instance().getSnapshot();
    const uint8_t  sys_state  = (uint8_t)snap.sys_state;
    const uint8_t  ota_channel = (uint8_t)snap.channel;
    const uint32_t ota_stored   = (uint32_t)snap.meta.stored_size;
    const uint32_t ota_total    = (uint32_t)snap.meta.total_size;

    int8_t  rssi = -127;
    {
        int8_t  r;
        uint32_t age_ms;
        if (getLastWifiRssi(&r, &age_ms)) {
            rssi = r;
        }
    }
    const float temperature = getLastTemperature();

    /* current_task = 采样者，不是触发者。固定写 health_mon，避免误解为“谁 alloc”。 */
    char current_task[16] = "health_mon";

    // 连接状态
    const uint8_t ble_connected  = BleOta::instance().isConnected() ? 1 : 0;
    const uint8_t wifi_connected = WifiOta::instance().getStaConnected() ? 1 : 0;

    // 嫌疑资源水位：定位"哪个子系统吃堆"
    const uint16_t ble_queue_depth = (uint16_t)ble_ota_get_data_queue_depth();
    const uint16_t ble_reorder     = (uint16_t)ble_ota_get_reorder_cached_count();
    const uint16_t spi_pending     = (uint16_t)SpiMaster::instance().getPendingJobCount();

    // 内存全景：default 含 PSRAM，与 internal 对比可判断"是内部碎片 vs 系统级紧"
    const uint32_t default_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    uint32_t alloc_count = 0;
    {
        multi_heap_info_t info = {};
        heap_caps_get_info(&info, CAPS);
        alloc_count = (uint32_t)info.allocated_blocks;
    }

    // 收集所有受监控任务栈水位，按"剩余升序"取前 8 个
    HeapLowTaskStack stacks[8];
    uint8_t stack_count = 0;
    {
        TaskStackMetric metrics[STACK_TASK_MAX];
        size_t count = collectTaskStackMetrics(metrics, STACK_TASK_MAX);
        // 先全部采集
        struct Item { char name[16]; uint32_t rem; };
        Item items[STACK_TASK_MAX];
        size_t item_n = 0;
        for (size_t i = 0; i < count && i < STACK_TASK_MAX; ++i) {
            if (metrics[i].handle == NULL || metrics[i].stack_size_bytes == 0) {
                continue;
            }
            UBaseType_t hw = uxTaskGetStackHighWaterMark(metrics[i].handle);
            Item &it = items[item_n++];
            it.rem = (uint32_t)hw * sizeof(StackType_t);
            strncpy(it.name, metrics[i].name ? metrics[i].name : "?", sizeof(it.name) - 1);
            it.name[sizeof(it.name) - 1] = '\0';
        }
        // 冒泡排序按 rem 升序（item_n ≤ 9，开销小）
        for (size_t i = 0; i + 1 < item_n; ++i) {
            for (size_t j = i + 1; j < item_n; ++j) {
                if (items[j].rem < items[i].rem) {
                    Item t = items[i]; items[i] = items[j]; items[j] = t;
                }
            }
        }
        // 取前 8
        for (size_t i = 0; i < item_n && stack_count < 8; ++i) {
            strncpy(stacks[stack_count].name, items[i].name, sizeof(stacks[stack_count].name) - 1);
            stacks[stack_count].name[sizeof(stacks[stack_count].name) - 1] = '\0';
            stacks[stack_count].remaining = items[i].rem;
            ++stack_count;
        }
    }

    // ── 持锁覆盖写唯一槽 ──
    LockGuard lock(m_mutex);

    HeapLowWaterRecord &rec = m_heapLowWater;
    rec = {};
    rec.ts_ms                  = (uint64_t)(esp_timer_get_time() / 1000);
    rec.free_heap              = free_heap;
    rec.min_free_heap          = cur_min;
    rec.largest_free_block     = largest_block;
    rec.sys_state              = sys_state;
    rec.ota_channel            = ota_channel;
    rec.rssi                   = rssi;
    rec.temperature            = temperature;
    rec.task_stack_count       = stack_count;
    for (uint8_t i = 0; i < stack_count && i < 8; ++i) {
        rec.task_stacks[i] = stacks[i];
    }
    strncpy(rec.current_task, current_task, sizeof(rec.current_task) - 1);
    rec.current_task[sizeof(rec.current_task) - 1] = '\0';
    rec.ble_connected   = ble_connected;
    rec.wifi_connected  = wifi_connected;
    rec.ota_stored_size = ota_stored;
    rec.ota_total_size  = ota_total;
    rec.ble_data_queue_depth = ble_queue_depth;
    rec.ble_reorder_cached   = ble_reorder;
    rec.spi_pending_jobs     = spi_pending;
    rec.reserved             = 0;
    rec.default_free_heap    = default_free;
    rec.alloc_count          = alloc_count;
    // heap hook：「最后一击」alloc（无任务名；见文件头注释）
    // 字段取自锁外已取的一致快照 hs（captureHookSnapshot），不再裸读 volatile，
    // 消除 fired/size/caller/free_after 多字段撕裂读。
    {
        const uint32_t hook_size   = hs.alloc_size;
        const uint32_t hook_caller = hs.caller_addr;
        const uint32_t hook_after  = hs.free_after;
        const uint32_t hook_fired  = hs.fired;

        /* 窗口内更大块（若存在）：发送前小 pbuf 压低水位时，主因更可能是此前 bulk */
        uint32_t big_size = 0;
        uint32_t big_caller = 0;
        const uint32_t n = hs.recent_n;
        for (uint32_t i = 0; i < n && i < (uint32_t)kRecentAllocCap; ++i) {
            if (hs.recent[i].size > big_size) {
                big_size = hs.recent[i].size;
                big_caller = hs.recent[i].caller;
            }
        }

        rec.hook_alloc_size   = hook_size;
        rec.hook_caller_addr  = hook_caller;
        rec.hook_free_after   = hook_after;
        rec.hook_fired        = hook_fired;
        /* hook_task 存类别：net? / net?+big / bulk / tiny（非 FreeRTOS 任务名） */
        rec.hook_task[0] = '\0';
        if (hook_fired) {
            const char *cls = "tiny";
            if (hook_size >= 2048U) {
                cls = "bulk";
            } else if (hook_size >= kHookInterestingMin) {
                if (big_size > hook_size && big_size >= 2048U) {
                    cls = "net?+big";
                    rec.hook_caller_addr = big_caller;
                } else {
                    cls = "net?";
                }
            }
            strncpy(rec.hook_task, cls, sizeof(rec.hook_task) - 1);
            rec.hook_task[sizeof(rec.hook_task) - 1] = '\0';
        }

        /* 消费已记录的 hook 命中：在 s_hookMux 内重置 min_est/fired，避免同一窗口
         * 重复记。与 hook 写互斥（禁中断），不持 m_mutex 同时持 s_hookMux。 */
        portENTER_CRITICAL(&s_hookMux);
        consumeHookFiredInLock();
        portEXIT_CRITICAL(&s_hookMux);
    }
    rec.ip_event_count = g_ip_event_count;

    m_heapLowWaterValid   = true;
    m_heapLowWaterLastMin = cur_min;
}

/**
 * @brief 输出设备唯一快照槽：{"count":0|1,"records":[...]}。
 *        cap 不够装下整条时退回 count=0，避免半截 JSON。
 */
int HealthMonitor::dumpHeapLowWaterJson(char *buf, size_t cap, int offset) const
{
    LockGuard lock(m_mutex);

    if (!m_heapLowWaterValid) {
        return appendJsonfSafe(buf, cap, offset, "{\"count\":0,\"records\":[]}");
    }

    const HeapLowWaterRecord &r = m_heapLowWater;
    /* hook_free_after 是 estimated（hook 只减不增近似值），JSON 字段名显式标注 est，
     * hook_confidence 告知前端置信度：hook 命中=est_low_watermark，未命中=none。
     * 前端应据此区分 estimated 与 measured（free_heap/min_free_heap 为实测）。 */
    const char *hook_confidence = r.hook_fired ? "est_low_watermark" : "none";
    int pos = appendJsonfSafe(buf, cap, offset,
        "{\"count\":1,\"records\":[{"
        "\"ts_ms\":%llu,\"free_heap\":%lu,\"min_free_heap\":%lu,"
        "\"largest_free_block\":%lu,\"sys_state\":%u,\"ota_channel\":%u,"
        "\"rssi\":%d,\"temperature\":%.1f,"
        "\"current_task\":\"%s\",\"ble_connected\":%u,\"wifi_connected\":%u,"
        "\"ota_stored_size\":%lu,\"ota_total_size\":%lu,"
        "\"ble_data_queue_depth\":%u,\"ble_reorder_cached\":%u,"
        "\"spi_pending_jobs\":%u,\"default_free_heap\":%lu,\"alloc_count\":%lu,"
        "\"hook_task\":\"%s\",\"hook_alloc_size\":%lu,\"hook_caller_addr\":%lu,"
        "\"hook_est_free_after\":%lu,\"hook_confidence\":\"%s\",\"hook_fired\":%lu,"
        "\"ip_event_count\":%lu,"
        "\"task_stacks\":[",
        (unsigned long long)r.ts_ms,
        (unsigned long)r.free_heap,
        (unsigned long)r.min_free_heap,
        (unsigned long)r.largest_free_block,
        (unsigned)r.sys_state,
        (unsigned)r.ota_channel,
        (int)r.rssi,
        (double)r.temperature,
        r.current_task,
        (unsigned)r.ble_connected,
        (unsigned)r.wifi_connected,
        (unsigned long)r.ota_stored_size,
        (unsigned long)r.ota_total_size,
        (unsigned)r.ble_data_queue_depth,
        (unsigned)r.ble_reorder_cached,
        (unsigned)r.spi_pending_jobs,
        (unsigned long)r.default_free_heap,
        (unsigned long)r.alloc_count,
        r.hook_task,
        (unsigned long)r.hook_alloc_size,
        (unsigned long)r.hook_caller_addr,
        (unsigned long)r.hook_free_after,
        hook_confidence,
        (unsigned long)r.hook_fired,
        (unsigned long)r.ip_event_count);
    if (pos == offset) {
        return appendJsonfSafe(buf, cap, offset, "{\"count\":0,\"records\":[]}");
    }

    for (uint8_t k = 0; k < r.task_stack_count && k < 8; ++k) {
        int after = appendJsonfSafe(buf, cap, pos,
            "%s{\"name\":\"%s\",\"remaining\":%lu}",
            k == 0 ? "" : ",",
            r.task_stacks[k].name,
            (unsigned long)r.task_stacks[k].remaining);
        if (after == pos) {
            return appendJsonfSafe(buf, cap, offset, "{\"count\":0,\"records\":[]}");
        }
        pos = after;
    }

    int closed = appendJsonfSafe(buf, cap, pos, "]}]}");
    if (closed == pos) {
        return appendJsonfSafe(buf, cap, offset, "{\"count\":0,\"records\":[]}");
    }
    return closed;
}


