/**
 * @file system_boot.h
 * @brief ESP32 启动阶段模块初始化位图与汇总报告。
 *
 * 每个模块 init/建任务成功后 mark(bit)=1，启动末尾 report() 打印整体结果。
 * /system/status 通过 appendStatusJson() 把同一份结果暴露给网页。
 */
#ifndef SYSTEM_BOOT_H
#define SYSTEM_BOOT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/** 启动模块位（成功置 1） */
enum SystemBootBit : uint32_t {
    BOOT_NETIF          = 1u << 0,
    BOOT_EVENT_LOOP     = 1u << 1,
    BOOT_DEBUG_LOG      = 1u << 2,
    BOOT_WIFI_EARLY     = 1u << 3,
    BOOT_HEALTH_MON     = 1u << 4,
    BOOT_HEALTH_TASK    = 1u << 5,
    BOOT_CPU_STATS      = 1u << 6,
    BOOT_PROTOCOL       = 1u << 7,
    BOOT_OTA_COORD      = 1u << 8,
    BOOT_SPI            = 1u << 9,   ///< SPI 主机 L0（本机就绪）
    BOOT_OTA_RELAY_TASK = 1u << 10,
    BOOT_WIFI_TASK      = 1u << 11,
    BOOT_BLE_TASK       = 1u << 12,
    BOOT_CAN_LINK       = 1u << 13,  ///< SPI 对端 L0（HELLO 可达 GD32，非关键）
    BOOT_CAN_FB_TASK    = 1u << 14,  ///< CAN 反馈任务（依赖 SPI L0）
    BOOT_OTA_VALID      = 1u << 15,
};

class SystemBoot {
public:
    static void reset();
    static void mark(SystemBootBit bit);

    /** 串口汇总；返回关键模块是否全部成功 */
    static bool report();

    /**
     * @brief 追加 boot JSON 字段（含前导逗号）到 status 缓冲。
     * @return 新的写入偏移
     */
    static int appendStatusJson(char *buf, size_t cap, int offset);
};

#endif /* SYSTEM_BOOT_H */
