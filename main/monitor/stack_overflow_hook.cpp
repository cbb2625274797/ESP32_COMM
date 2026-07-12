/*
 * stack_overflow_hook.cpp - FreeRTOS 栈溢出钩子
 *
 * CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY 开启后，内核在任务切换时检查
 * 栈 canary，若被踩则回调 vApplicationStackOverflowHook。
 *
 * 钩子在被踩栈的任务上下文中执行，此时该任务栈已损坏 —— 禁止任何依赖栈 /
 * mutex / 网络栈的调用（printf 格式化、UDP sendto、xSemaphoreTake 等都可能
 * 二次崩溃）。唯一安全做法：用 uart_write_bytes 往 UART0 硬写预写好的常量
 * 字符串（物理外设直写，不需要栈空间），并置一个 volatile 标志，由
 * HealthMonitor 巡检循环（运行在完好的 health_mon 栈上）后续用 DEBUG_ERR
 * 经 UDP/UART 发详细告警。
 *
 * UART0 直写不受 OTA 门控抑制，是 OTA 末尾断网阶段唯一可靠的物理通道。
 */
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_system.h"

// 供 HealthMonitor 巡检检测：canary 触发后置位，由巡检发详细告警。
volatile bool g_stackOverflowDetected = false;

extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    g_stackOverflowDetected = true;

    // 栈已损坏，只能做不依赖栈的最小操作。UART0 硬写常量字符串。
    static const char prefix[] = "\r\n!!! STACK OVERFLOW in task: ";
    uart_write_bytes(UART_NUM_0, prefix, sizeof(prefix) - 1);
    // 任务名最多 configMAX_TASK_NAME_LEN，strlen 对小已知 buffer 安全
    uart_write_bytes(UART_NUM_0, pcTaskName, strnlen(pcTaskName, configMAX_TASK_NAME_LEN));
    static const char suffix[] = " !!!\r\n";
    uart_write_bytes(UART_NUM_0, suffix, sizeof(suffix) - 1);

    /* 立刻 abort：OTA 上传 100% 后若 httpd 栈溢出却不 abort，
     * 前端会一直停在 100% 像“卡住”，且 TCB 已损坏无法恢复。 */
    esp_system_abort("stack overflow");
}