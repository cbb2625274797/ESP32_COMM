/*
 * app_main.h - 应用程序初始化与健康监控
 */
#ifndef APP_MAIN_H
#define APP_MAIN_H

#include <stdint.h>

/* 系统监控任务 */
void system_monitor_task(void *pvParameters);

/* 电压读取（回退方案） */
uint32_t read_voltage_mv(void);

#endif /* APP_MAIN_H */