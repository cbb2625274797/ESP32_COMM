/*
 * motor_version.h - 被动接收 GD32 多电机版本推送（SPI TYPE 0xC6）
 *
 * 与 0xC5 电机状态同模型：GD32 收到电机版本应答(0x00FF)后事件驱动按铃推送，
 * ESP32 takeEvent 侧解析入库。版本为按需查询的低频数据，独立于 0xC5 周期推送。
 * 是否推送由 GD32 CONFIG_MOTOR_VERSION_ENABLE 决定；ESP32 无本地开关，有帧即解析。
 *
 * payload 格式（单帧，Header 2B + 每 entry 29B）：
 *   [0] count  本帧节点数
 *   [1] rsvd   保留
 *   [2..] 每个 entry 29B:
 *     [0] node_id, [1..4] seq (LE), [5..28] version[24] (原始版本 payload)
 */
#ifndef MOTOR_VERSION_H
#define MOTOR_VERSION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOTORVER_MAX_NODES   9
#define MOTORVER_DLC         24
#define MOTORVER_ENTRY_SIZE  29   /* node(1) + seq(4) + version(24) */
#define MOTORVER_HEADER_LEN  2

/** @brief 单电机版本快照。 */
typedef struct {
    uint8_t  node_id;                    ///< 1-9, 0=空槽
    uint32_t seq;                        ///< GD32 侧更新序号（判新鲜用）
    uint8_t  version[MOTORVER_DLC];      ///< 原始版本 payload
    int64_t  recv_ms;                    ///< ESP32 收到时 esp_timer ms
    bool     valid;                      ///< 该槽已收到过版本
} motor_version_slot_t;

/** @brief 全局电机版本表。 */
typedef struct {
    motor_version_slot_t slots[MOTORVER_MAX_NODES]; ///< 按 node_id-1 索引
    bool have_data;                                 ///< 至少收到过一帧
} motor_version_table_t;

/**
 * @brief 处理一帧 0xC6 EVENT payload（由 can_fb 任务 takeEvent 分发）。
 * @return true 表示已接受并更新。
 */
bool onMotorVersionEvent(const uint8_t *payload, size_t len);

/**
 * @brief 拷贝最新电机版本表（线程安全）。
 * @param[out] out  输出缓冲
 * @return true 表示至少收到过一帧
 */
bool getLatestMotorVersion(motor_version_table_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_VERSION_H */
