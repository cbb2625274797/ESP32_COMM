/*
 * task_stack_check.h - 任务栈水位指标收集（共享）
 *
 * 将受监控任务的任务名/句柄/栈大小统一收集，供：
 *   - health_monitor 巡检主动告警（低水位时 DEBUG_ERR，突破 OTA 门控）
 *   - http_handlers /system/status 被动报告（前端每秒轮询展示）
 * 两处复用同一份任务表，避免散落维护。
 */
#pragma once

#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** @brief 单个任务的栈指标描述。 */
struct TaskStackMetric {
    const char*  name;              ///< 任务名（展示用）
    TaskHandle_t handle;            ///< FreeRTOS 任务句柄（可能为 NULL：任务尚未创建）
    size_t       stack_size_bytes;  ///< 创建该任务时分配的栈大小（字节）
};

/**
 * @brief 收集所有受监控任务的栈指标。
 * @param out  输出数组。
 * @param max  out 数组容量。
 * @return 实际写入条数（<= max）。
 *
 * 注意：部分任务句柄在任务创建前为 NULL，调用方应跳过 handle==NULL 的条目。
 */
size_t collectTaskStackMetrics(TaskStackMetric* out, size_t max);