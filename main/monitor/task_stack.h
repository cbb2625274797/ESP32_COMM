/*
 * task_stack.h - 任务栈：水位收集 + 溢出钩子
 *
 * 1) collectTaskStackMetrics：供 health_mon 巡检与 /system/status 共用任务表
 * 2) vApplicationStackOverflowHook / g_stackOverflowDetected：canary 溢出
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/** canary 触发时由 vApplicationStackOverflowHook 置位（随后 abort）。 */
extern volatile bool g_stackOverflowDetected;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

/** @brief 单个任务的栈指标描述。 */
struct TaskStackMetric {
    const char*  name;              ///< 任务名（展示用）
    TaskHandle_t handle;            ///< 句柄（可能为 NULL：任务尚未创建）
    size_t       stack_size_bytes;  ///< 创建时分配的栈大小（字节）
};

/**
 * @brief 收集受监控任务的栈指标。
 * @return 实际写入条数（<= max）。handle==NULL 的条目由调用方跳过。
 */
size_t collectTaskStackMetrics(TaskStackMetric* out, size_t max);

#endif /* __cplusplus */
