/*
 * motor_state.h - 被动接收 GD32 多电机状态推送（SPI TYPE 0xC5）
 *
 * 与 CAN 0xC1 / ADC 0xC4 同模型：GD32 250ms 主动按铃推送（独立任务位），ESP32 takeEvent 侧解析入库。
 * 是否推送由 GD32 CONFIG_MOTOR_STATE_ENABLE 决定；ESP32 无本地开关，有帧即解析。
 *
 * payload 格式（每页）：
 *   [0] page, [1] total_pages, [2] entry_count, [3] active_count
 *   [4..] 每个 entry 92B (v0.74: STATUS DLC 48):
 *     [0] node_id, [1] channel, [2] online, [3] dlc,
 *     [4..7] rx_count (LE), [8..11] seq (LE),
 *     [12..59] data[48] (原始 STATUS payload, 字节 32~35 = InitStatus)
 *     [60..63] config_rx_count (LE), [64..67] config_seq (LE),
 *     [68..91] config_data[24] (原始 CONFIG payload, 目标值回报)
 *
 * 注意：STATUS DLC 48 与页大小/页数变更（76→92、9→14 页）需 DCU GD32 与
 * ESP32 固件成对升级，旧解析方会因长度校验失败拒收（不误解析）。
 *
 * 同一节点号可能同时挂在多个 CAN 通道（两台电机同号异线），
 * 缓存表与 GD32 端一致按 node×channel 双键存储（27 槽）。
 */
#ifndef MOTOR_STATE_H
#define MOTOR_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOTOR_MAX_NODES      9
#define MOTOR_CHANNELS       3
#define MOTOR_MAX_SLOTS      (MOTOR_MAX_NODES * MOTOR_CHANNELS)
#define MOTOR_STATUS_DLC     48
#define MOTOR_CONFIG_DLC     24
#define MOTOR_ENTRY_SIZE     92
#define MOTOR_PAGE_HEADER    4
#define MOTOR_TOTAL_PAGES    14

/** @brief 单电机状态快照。 */
typedef struct {
    uint8_t  node_id;                    ///< 1-9, 0=空槽
    uint8_t  channel;                    ///< CAN 通道 (0/1/2)
    uint8_t  online;                     ///< 1=在线, 0=离线
    uint8_t  dlc;                        ///< STATUS 帧 DLC (正常=48)
    uint32_t rx_count;                   ///< 累计收到帧数
    uint32_t seq;                        ///< GD32 侧更新序号
    uint8_t  data[MOTOR_STATUS_DLC];     ///< 原始 STATUS payload
    uint32_t config_rx_count;            ///< 累计收到 CONFIG 帧数
    uint32_t config_seq;                 ///< CONFIG 更新序号
    uint8_t  config_data[MOTOR_CONFIG_DLC]; ///< 原始 CONFIG payload (目标值回报)
} motor_slot_t;

/** @brief 全局电机状态表。 */
typedef struct {
    motor_slot_t slots[MOTOR_MAX_SLOTS]; ///< 按 (node_id-1)*MOTOR_CHANNELS+channel 索引
    uint8_t  active_count;               ///< 在线电机数
    uint8_t  boot_active;                ///< 1=总线上有电机在 BOOT（0x103 心跳，来自 0xC5 头 bit7）
    uint8_t  valid_slots;                ///< 有效槽位数（node_id!=0）
    int64_t  recv_ms;                    ///< ESP32 最近收到时 esp_timer ms
    bool     have_data;                  ///< 至少收到过一帧
} motor_state_table_t;

/**
 * @brief 处理一帧 0xC5 EVENT payload（由 can_fb 任务 takeEvent 分发）。
 * @return true 表示已接受并更新。
 */
bool onMotorStateEvent(const uint8_t *payload, size_t len);

/**
 * @brief 拷贝最新电机状态表（线程安全）。
 * @param[out] out  输出缓冲
 * @return true 表示至少收到过一帧
 */
bool getLatestMotorState(motor_state_table_t *out);

/** @brief 距离上次收到的毫秒数；无数据返回 UINT32_MAX。 */
uint32_t motorStateAgeMs(void);

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_STATE_H */
