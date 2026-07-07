/*
 * canfd_bridge.h - CANFD 3通道通信桥接（M4: 从 SpiMaster 提取）
 *
 * 封装 CANFD 帧格式构建 + SPI 传输，与 SpiMaster 的 OTA 协议职责解耦。
 * 通过 SpiMaster 的公共 SPI 接口完成底层传输。
 */
#ifndef CANFD_BRIDGE_H
#define CANFD_BRIDGE_H

#include <stdint.h>

// ============================================================
// CANFD 帧格式（3通道）
// [通道编号: 1B] [扩展帧ID: 4B BE] [数据: 1~64B]
// ============================================================
#define CAN_EXTENDED_ID_BITS   (29)          ///< CAN 扩展帧 ID 位数
#define CAN_MAX_DLC            (64)          ///< 最大数据长度
#define CAN_CHANNEL_MASK       (0x03)        ///< 通道号掩码 (0~2)

// 通道定义
typedef enum {
    CAN_CHANNEL_0 = 0,
    CAN_CHANNEL_1 = 1,
    CAN_CHANNEL_2 = 2,
    CAN_CHANNEL_MAX = 3,
} can_channel_t;

// CANFD 帧结构
typedef struct {
    can_channel_t channel;      ///< CAN 通道 (0~2)
    uint32_t      ext_id;      ///< 扩展帧 ID (29bit, Big-Endian 存储)
    uint8_t       data[CAN_MAX_DLC];  ///< CAN 数据负载 (最大 64 字节)
    uint8_t       dlc;         ///< 数据长度 (1~64)
} can_frame_t;

/**
 * @brief 发送一帧 CANFD 数据，可选等待并读取响应。
 *        内部通过 SpiMaster 的公共 SPI 接口完成传输。
 * @param channel CAN 通道编号（0~2）。
 * @param ext_id 扩展帧 ID（29 位）。
 * @param data CAN 数据负载。
 * @param dlc 数据长度。
 * @param recv_buf 接收数据缓冲区（可为 nullptr）。
 * @param recv_max 接收缓冲区最大长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return 成功时返回接收数据长度，失败返回 -1。
 */
int canFdSend(can_channel_t channel, uint32_t ext_id,
              const uint8_t *data, uint8_t dlc,
              uint8_t *recv_buf, uint8_t recv_max,
              uint32_t timeout_ms);

/**
 * @brief 发送一帧无需响应的 CANFD 数据。
 * @param channel CAN 通道编号（0~2）。
 * @param ext_id 扩展帧 ID（29 位）。
 * @param data CAN 数据负载。
 * @param dlc 数据长度。
 * @return 成功返回 0，失败返回 -1。
 */
int canFdSendNoReply(can_channel_t channel, uint32_t ext_id,
                     const uint8_t *data, uint8_t dlc);


// ============================================================
// 不重复 ID 快照表（SPI 0xC3 协议配套）
// ============================================================
#define CANFD_ID_SNAPSHOT_MAX      16    ///< 每通道最大不重复 ID 条目数
#define CANFD_ID_SNAPSHOT_DATA_MAX 64    ///< 快照条目最大数据字节数

/**
 * @brief 单个不重复 ID 的最新帧快照条目。
 */
typedef struct {
    uint32_t id;
    uint32_t seq;
    uint32_t rx_count;
    uint8_t  dlc;
    uint8_t  fd_flag;
    uint8_t  brs;
    uint8_t  esi;
    uint8_t  data[CANFD_ID_SNAPSHOT_DATA_MAX];
} id_snapshot_entry_t;

// ============================================================
// CAN 接收过滤模式 & 1Hz 反馈快照
// 与 GD32 DCU_APP SPI 协议帧 0xC1/0xC2 配套
// ============================================================
#define SPI_CAN_FEEDBACK_LAST_DATA_BYTES 93   ///< 反馈快照中最近一帧数据字节数（受 SPI 128B 应答帧约束（kTxBufSize=128 - overhead=5 - 前缀30 = 93））

typedef enum {
    CAN_RX_MODE_ALL_PASS  = 0,  ///< 默认：禁用 ID 过滤，三路全部接收
    CAN_RX_MODE_FILTER_ID = 1,  ///< 按 CONFIG_CAN_RX_FILTER_ID/MASK 做标准帧过滤
    CAN_RX_MODE_BRIDGE_0_TO_12 = 2, ///< CAN0 桥接：CAN0 收到的每一帧按原帧格式自动转发到 CAN1 与 CAN2
} can_rx_mode_t;

/**
 * @brief 单通道 1Hz 反馈统计。
 */
typedef struct {
    uint8_t   channel;
    uint8_t   mode;
    uint32_t  snapshot_seq;
    uint32_t  rx_count;
    uint32_t  tx_count;
    uint32_t  rx_drop_count;
    uint32_t  error_count;
    uint8_t   tec;         /**< 硬件发送错误计数器 (0~255) */
    uint8_t   rec;         /**< 硬件接收错误计数器 (0~255) */
    uint8_t   bus_state;   /**< 总线状态位图：bit0=init,bit1=sleep,bit2=error_passive,bit3=bus_off,bit4=error_warning */
} can_channel_feedback_t;

/**
 * @brief 单通道 0xC1 完整响应：统计 + ID 快照表。
 */
typedef struct {
    can_channel_feedback_t stats;
    id_snapshot_entry_t    entries[CANFD_ID_SNAPSHOT_MAX];
    uint8_t                total_count;   /**< 该通道实际 ID 条目总数 */
    uint8_t                valid;         /**< 数据是否有效 */
} can_feedback_result_t;

/**
 * @brief 解析 GD32 主动推送的一页 CAN 反馈 payload，累积到结果结构中。
 *
 * GD32 APP 通过 requestEsp32Read(0xC1) 主动推送每页反馈，ESP32 收到后
 * 调用本函数解析。page=0 时重置 out 并填充固定头 + 首批条目；
 * page=1 时在已有条目后追加。每页解析后置 out->valid=1。
 *
 * @param payload  GD32 推送帧的 payload（[channel, page, total, count, mode, seq, ...]）。
 * @param len      payload 长度。
 * @param out      累积结果（跨 page 0/1 累积）。
 * @param entryIdx 当前已填充条目数（page 0 时被重置为 0，调用方按通道维护）。
 * @return 0 成功，<0 失败（payload 过短或通道非法）。
 */
int canFdParseFeedbackPage(const uint8_t *payload, size_t len,
                           can_feedback_result_t *out, uint8_t *entryIdx);

/**
 * @brief 切换 GD32 三路 CAN 的接收过滤模式。
 * @param mode       目标模式（0=全收，1=按 ID 过滤）。
 * @param actual     可选输出，GD32 实际生效模式。
 * @param timeout_ms SPI 事务超时。
 * @return 0 成功，<0 失败。
 */
int canFdSetRxMode(can_rx_mode_t mode,
                   can_rx_mode_t *actual,
                   uint32_t timeout_ms);

/**
 * @brief 拉取指定通道的不重复 ID 快照表（SPI 帧 0xC3，分页）。
 * @param channel    目标通道 0/1/2。
 * @param out        输出条目数组（至少 CANFD_ID_SNAPSHOT_MAX 个元素）。
 * @param out_count  输出实际条目数。
 * @param timeout_ms SPI 超时。
 * @return 0 成功，<0 失败。
 */
int canFdPullIdSnapshot(can_channel_t channel,
                        id_snapshot_entry_t *out,
                        uint8_t *out_count,
                        uint32_t timeout_ms);
#endif // CANFD_BRIDGE_H
