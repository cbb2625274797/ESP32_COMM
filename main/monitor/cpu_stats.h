/*
 * cpu_stats.h - FreeRTOS 运行时 CPU 占用采样
 *
 * 由独立低优先级任务按固定 1s 周期采样；/system/status 只读最新快照，
 * 不再在 HTTP 线程里调用 uxTaskGetSystemState（避免窗口绑浏览器轮询、减 httpd 栈压）。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** 单次快照最多跟踪的任务数。
 *  系统常驻任务 ~14（app）+ ~5（IDF: tiT/wifi/nimble/hci/ble）= ~19，
 *  按需短暂任务（ble_finish/restart/cancel/gd32_wait/ota_feed）不常驻。
 *  20 覆盖常驻集 + 余量 1；偶发按需任务多出时 uxTaskGetSystemState 截断，
 *  仅影响该拍 CPU 统计显示，不影响功能。原 32 浪费 ~1KB BSS。 */
static constexpr size_t CPU_TASK_SNAPSHOT_MAX = 20;

/** 采样周期（ms）— 窗口 sample_ms 应对齐此值 */
static constexpr uint32_t CPU_STATS_SAMPLE_MS = 1000;

struct CpuTaskMetric {
    char name[configMAX_TASK_NAME_LEN];
    UBaseType_t task_number;
    uint64_t runtime_delta;
    double usage_pct;
};

struct CpuUsageSnapshot {
    bool available;          ///< 固件是否启用 RUN_TIME_STATS
    bool valid;              ///< 是否已有完整窗口（至少两拍）
    uint32_t sample_ms;      ///< 本窗口墙钟长度
    double usage_pct;        ///< 100 - idle
    double idle_pct;
    CpuTaskMetric tasks[CPU_TASK_SNAPSHOT_MAX];
    size_t task_count;
};

/**
 * @brief CPU 运行时统计（单例 + 1Hz 采样任务）
 */
class CpuStats {
public:
    static CpuStats& instance();

    /** 创建采样任务（幂等；失败仅打日志，不阻塞启动） */
    void start();

    /** 线程安全拷贝最新快照（HTTP 用） */
    void getSnapshot(CpuUsageSnapshot& out) const;

    TaskHandle_t getTaskHandle() const { return m_taskHandle; }

private:
    CpuStats() = default;

    static void taskEntry(void* arg);
    void sampleOnce();

    mutable portMUX_TYPE m_mux = portMUX_INITIALIZER_UNLOCKED;
    CpuUsageSnapshot m_published = {};
    TaskHandle_t m_taskHandle = nullptr;
    bool m_started = false;

    /* 基线（仅采样任务写） */
    struct TaskSample {
        UBaseType_t task_number;
        configRUN_TIME_COUNTER_TYPE runtime;
    };
    TaskSample m_lastSamples[CPU_TASK_SNAPSHOT_MAX] = {};
    size_t m_lastCount = 0;
    configRUN_TIME_COUNTER_TYPE m_lastTotal = 0;
    int64_t m_lastTimeUs = 0;
    bool m_hasLast = false;

    /** uxTaskGetSystemState 目标缓冲（固定、免每拍 calloc） */
    TaskStatus_t m_taskStatusBuf[CPU_TASK_SNAPSHOT_MAX] = {};

    /** 任务数截断 WARN 节流（tick） */
    TickType_t m_lastTruncWarnTick = 0;
    /** 运行时计数回绕 WARN 节流（tick） */
    TickType_t m_lastWrapWarnTick = 0;
};
