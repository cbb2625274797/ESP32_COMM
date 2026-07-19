/*
 * cpu_stats.cpp - 1Hz FreeRTOS CPU 占用采样任务
 */
#include "cpu_stats.h"

#include <algorithm>
#include <cstring>
#include <inttypes.h>

#include "app_config.h"
#include "esp_timer.h"
#include "monitor/debug_log.h"

/** 截断/回绕 WARN 最小间隔（30s） */
static TickType_t cpuStatsWarnIntervalTicks()
{
    return pdMS_TO_TICKS(30000);
}

CpuStats& CpuStats::instance()
{
    static CpuStats inst;
    return inst;
}

void CpuStats::start()
{
    if (m_started) {
        return;
    }
    m_started = true;

#if !defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
    portENTER_CRITICAL(&m_mux);
    m_published = {};
    m_published.available = false;
    portEXIT_CRITICAL(&m_mux);
    DEBUG_WARN("CPU", "未启用 CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS，跳过采样任务");
    return;
#else
    portENTER_CRITICAL(&m_mux);
    m_published = {};
    m_published.available = true;
    m_published.valid = false;
    portEXIT_CRITICAL(&m_mux);

    BaseType_t ok = xTaskCreatePinnedToCore(
        &CpuStats::taskEntry,
        "cpu_stats",
        TASK_STACK_CPU_STATS,
        this,
        tskIDLE_PRIORITY + 1,   /* 低优先级，少搅动占用统计 */
        &m_taskHandle,
        0);
    if (ok != pdPASS) {
        m_taskHandle = nullptr;
        m_started = false;
        DEBUG_ERR("CPU", "创建 cpu_stats 任务失败");
        return;
    }
    DEBUG_INFO("CPU", "cpu_stats 任务已启动，采样周期 %" PRIu32 " ms", CPU_STATS_SAMPLE_MS);
#endif
}

void CpuStats::getSnapshot(CpuUsageSnapshot& out) const
{
    portENTER_CRITICAL(&m_mux);
    out = m_published;
    portEXIT_CRITICAL(&m_mux);
}

#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)

static uint64_t runtimeDelta(configRUN_TIME_COUNTER_TYPE cur, configRUN_TIME_COUNTER_TYPE prev)
{
    return cur >= prev ? (uint64_t)(cur - prev) : 0U;
}

static bool taskNameIsIdle(const char* name)
{
    return name && strncmp(name, "IDLE", 4) == 0;
}

void CpuStats::sampleOnce()
{
    const UBaseType_t live = uxTaskGetNumberOfTasks();
    if (live == 0) {
        return;
    }

    /* P1：任务数超出快照上限时节流告警（否则静默丢任务） */
    if (live > (UBaseType_t)CPU_TASK_SNAPSHOT_MAX) {
        const TickType_t now_tick = xTaskGetTickCount();
        if (m_lastTruncWarnTick == 0 ||
            (now_tick - m_lastTruncWarnTick) >= cpuStatsWarnIntervalTicks()) {
            m_lastTruncWarnTick = now_tick;
            DEBUG_WARN("CPU",
                       "任务数%lu>上限%u,只计前%u",
                       (unsigned long)live,
                       (unsigned)CPU_TASK_SNAPSHOT_MAX,
                       (unsigned)CPU_TASK_SNAPSHOT_MAX);
        }
    }

    /* P1：固定成员缓冲，免每拍 heap */
    const UBaseType_t buf_slots =
        std::min<UBaseType_t>(live + 2U, (UBaseType_t)CPU_TASK_SNAPSHOT_MAX);

    configRUN_TIME_COUNTER_TYPE total_runtime = 0;
    const UBaseType_t captured =
        uxTaskGetSystemState(m_taskStatusBuf, buf_slots, &total_runtime);
    const int64_t now_us = esp_timer_get_time();

    if (captured == 0 || captured > buf_slots) {
        DEBUG_WARN("CPU", "uxTaskGetSystemState 异常: captured=%lu buf=%lu",
                   (unsigned long)captured, (unsigned long)buf_slots);
        return;
    }

    CpuUsageSnapshot snap = {};
    snap.available = true;

    /*
     * P2：先用 m_lastSamples 算本窗口，再写回基线（无需栈上 previous 拷贝）。
     * total 回绕（total_runtime < m_lastTotal）时丢弃本窗，只重建基线。
     */
    const bool had_previous = m_hasLast;
    const bool wrapped = had_previous && total_runtime < m_lastTotal;
    if (wrapped) {
        const TickType_t now_tick = xTaskGetTickCount();
        if (m_lastWrapWarnTick == 0 ||
            (now_tick - m_lastWrapWarnTick) >= cpuStatsWarnIntervalTicks()) {
            m_lastWrapWarnTick = now_tick;
            DEBUG_WARN("CPU",
                       "run-time 计数回绕，丢弃本采样窗 total=%" PRIu64 " prev=%" PRIu64,
                       (uint64_t)total_runtime,
                       (uint64_t)m_lastTotal);
        }
    }

    const bool can_compute =
        had_previous && !wrapped && now_us > m_lastTimeUs;
    const uint64_t total_delta =
        can_compute ? runtimeDelta(total_runtime, m_lastTotal) : 0U;
    uint64_t idle_delta = 0U;

    if (can_compute && total_delta > 0U) {
        for (UBaseType_t i = 0; i < captured; ++i) {
            configRUN_TIME_COUNTER_TYPE last_runtime = 0;
            bool found = false;
            for (size_t j = 0; j < m_lastCount; ++j) {
                if (m_lastSamples[j].task_number == m_taskStatusBuf[i].xTaskNumber) {
                    last_runtime = m_lastSamples[j].runtime;
                    found = true;
                    break;
                }
            }
            if (!found) {
                continue; /* 新任务：本窗无 delta */
            }
            /* 单任务计数回绕时 runtimeDelta→0，本任务本窗记 0 */
            uint64_t delta = runtimeDelta(m_taskStatusBuf[i].ulRunTimeCounter, last_runtime);
            double pct = (double)delta * 100.0 / (double)total_delta;
            if (taskNameIsIdle(m_taskStatusBuf[i].pcTaskName)) {
                idle_delta += delta;
            }
            if (snap.task_count < CPU_TASK_SNAPSHOT_MAX) {
                CpuTaskMetric& m = snap.tasks[snap.task_count++];
                const char* n = m_taskStatusBuf[i].pcTaskName ? m_taskStatusBuf[i].pcTaskName : "";
                strncpy(m.name, n, sizeof(m.name) - 1);
                m.name[sizeof(m.name) - 1] = '\0';
                m.task_number = m_taskStatusBuf[i].xTaskNumber;
                m.runtime_delta = delta;
                m.usage_pct = pct;
            }
        }
        snap.valid = true;
        snap.sample_ms = (uint32_t)((now_us - m_lastTimeUs) / 1000);
        snap.idle_pct = std::min(100.0, (double)idle_delta * 100.0 / (double)total_delta);
        snap.usage_pct = std::max(0.0, 100.0 - snap.idle_pct);
    }

    /* 更新基线（首拍 / 正常窗 / 回绕丢窗后均重建） */
    const size_t copy_count = std::min<size_t>((size_t)captured, CPU_TASK_SNAPSHOT_MAX);
    for (size_t i = 0; i < copy_count; ++i) {
        m_lastSamples[i].task_number = m_taskStatusBuf[i].xTaskNumber;
        m_lastSamples[i].runtime = m_taskStatusBuf[i].ulRunTimeCounter;
    }
    m_lastCount = copy_count;
    m_lastTotal = total_runtime;
    m_lastTimeUs = now_us;
    m_hasLast = true;

    portENTER_CRITICAL(&m_mux);
    m_published = snap;
    portEXIT_CRITICAL(&m_mux);
}

void CpuStats::taskEntry(void* arg)
{
    auto* self = static_cast<CpuStats*>(arg);
    DEBUG_INFO("CPU", "=== cpu_stats 采样任务启动 ===");

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(CPU_STATS_SAMPLE_MS);

    /* 第一拍只建基线（valid=false）；此后每拍窗口 ≈ 1000ms */
    for (;;) {
        self->sampleOnce();
        vTaskDelayUntil(&last_wake, period);
    }
}

#else /* !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS */

void CpuStats::sampleOnce() {}
void CpuStats::taskEntry(void*) { vTaskDelete(nullptr); }

#endif
