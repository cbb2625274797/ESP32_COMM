/*
 * cpu_stats.cpp - 1Hz FreeRTOS CPU 占用采样任务
 */
#include "cpu_stats.h"

#include <algorithm>
#include <cstring>
#include <inttypes.h>
#include <stdlib.h>

#include "app_config.h"
#include "esp_timer.h"
#include "monitor/debug_log.h"

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
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count == 0) {
        return;
    }
    task_count = std::min<UBaseType_t>(task_count + 2U, (UBaseType_t)CPU_TASK_SNAPSHOT_MAX);

    TaskStatus_t* tasks = (TaskStatus_t*)calloc(task_count, sizeof(TaskStatus_t));
    if (!tasks) {
        return;
    }

    configRUN_TIME_COUNTER_TYPE total_runtime = 0;
    UBaseType_t captured = uxTaskGetSystemState(tasks, task_count, &total_runtime);
    const int64_t now_us = esp_timer_get_time();

    if (captured == 0 || captured > task_count) {
        DEBUG_WARN("CPU", "uxTaskGetSystemState 异常: captured=%lu buf=%lu",
                   (unsigned long)captured, (unsigned long)task_count);
        free(tasks);
        return;
    }

    CpuUsageSnapshot snap = {};
    snap.available = true;

    const bool had_previous = m_hasLast;
    const configRUN_TIME_COUNTER_TYPE previous_total = m_lastTotal;
    const int64_t previous_time_us = m_lastTimeUs;
    const size_t previous_count = m_lastCount;
    TaskSample previous_samples[CPU_TASK_SNAPSHOT_MAX];
    for (size_t i = 0; i < previous_count && i < CPU_TASK_SNAPSHOT_MAX; ++i) {
        previous_samples[i] = m_lastSamples[i];
    }

    /* 先更新基线，再算本窗口（与旧 HTTP 路径语义一致） */
    size_t copy_count = std::min<size_t>((size_t)captured, CPU_TASK_SNAPSHOT_MAX);
    for (size_t i = 0; i < copy_count; ++i) {
        m_lastSamples[i].task_number = tasks[i].xTaskNumber;
        m_lastSamples[i].runtime = tasks[i].ulRunTimeCounter;
    }
    m_lastCount = copy_count;
    m_lastTotal = total_runtime;
    m_lastTimeUs = now_us;
    m_hasLast = true;

    const bool can_compute = had_previous && total_runtime >= previous_total && now_us > previous_time_us;
    const uint64_t total_delta = can_compute ? runtimeDelta(total_runtime, previous_total) : 0U;
    uint64_t idle_delta = 0U;

    if (can_compute && total_delta > 0U) {
        for (UBaseType_t i = 0; i < captured; ++i) {
            configRUN_TIME_COUNTER_TYPE last_runtime = 0;
            bool found = false;
            for (size_t j = 0; j < previous_count; ++j) {
                if (previous_samples[j].task_number == tasks[i].xTaskNumber) {
                    last_runtime = previous_samples[j].runtime;
                    found = true;
                    break;
                }
            }
            if (!found) {
                continue;
            }
            uint64_t delta = runtimeDelta(tasks[i].ulRunTimeCounter, last_runtime);
            double pct = (double)delta * 100.0 / (double)total_delta;
            if (taskNameIsIdle(tasks[i].pcTaskName)) {
                idle_delta += delta;
            }
            if (snap.task_count < CPU_TASK_SNAPSHOT_MAX) {
                CpuTaskMetric& m = snap.tasks[snap.task_count++];
                const char* n = tasks[i].pcTaskName ? tasks[i].pcTaskName : "";
                strncpy(m.name, n, sizeof(m.name) - 1);
                m.name[sizeof(m.name) - 1] = '\0';
                m.task_number = tasks[i].xTaskNumber;
                m.runtime_delta = delta;
                m.usage_pct = pct;
            }
        }
        snap.valid = true;
        snap.sample_ms = (uint32_t)((now_us - previous_time_us) / 1000);
        snap.idle_pct = std::min(100.0, (double)idle_delta * 100.0 / (double)total_delta);
        snap.usage_pct = std::max(0.0, 100.0 - snap.idle_pct);
    }

    free(tasks);

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
