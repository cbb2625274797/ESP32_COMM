/*
 * stack_overflow_hook.h - FreeRTOS 栈溢出钩子标志声明
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* canary 触发时由 vApplicationStackOverflowHook 置位；
 * HealthMonitor 巡检循环检测此标志，置位后用 DEBUG_ERR 发详细告警。 */
extern volatile bool g_stackOverflowDetected;

#ifdef __cplusplus
}
#endif