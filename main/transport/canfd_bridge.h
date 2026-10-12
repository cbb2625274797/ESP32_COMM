/*
 * canfd_bridge.h - CANFD 3通道通信桥接
 *
 * 封装 CANFD 帧格式构建 + SPI 传输，与 SpiMaster 的 OTA 协议职责分离。
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
    uint8_t       dlc;         ///< 数据长度 (0~64)
} can_frame_t;

/**
 * @brief 发送一帧 CANFD 数据，可选等待并读取响应。
 *        内部通过 SpiMaster 的公共 SPI 接口完成传输。
 * @param channel CAN 通道编号（0~2）。
 * @param ext_id 扩展帧 ID（29 位）。
 * @param data CAN 数据负载；dlc==0 时可为 nullptr。
 * @param dlc 数据长度（0~64）；0 表示空数据帧，仍发送 ID。
 * @param recv_buf 接收数据缓冲区（可为 nullptr）。
 * @param recv_max 接收缓冲区最大长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @param gd32_err_out 可选输出，GD32 返回的细分错误码（0xE0/0xE1/0xE2/0xE3/0xE4/0xE5/0xE6=通道静默拒发/0xE7=该通道被电机 OTA 独占）；
 *                     仅在返回 -3 时有效，其它情况清零。可为 nullptr。
 * @return 成功时返回接收数据长度，失败返回 -1/-2/-3。
 */
int canFdSend(can_channel_t channel, uint32_t ext_id,
              const uint8_t *data, uint8_t dlc,
              uint8_t *recv_buf, uint8_t recv_max,
              uint32_t timeout_ms,
              uint8_t *gd32_err_out = nullptr);

// ============================================================
// CAN 跨通道突发发送（SPI 0xC7）
// 一次 SPI 事务携带多帧，GD32 背靠背装载各通道 CAN 邮箱，
// 组内近同时上线（偏差 <1ms），用于多电机同步驱动。
// ============================================================
#define CAN_BURST_HARD_MAX   (16)   ///< GD32 单次突发硬上限（帧数）

/** @brief 突发条目：chnode=(ch<<4)|node，data 指向 dlc 字节数据。 */
typedef struct {
    uint8_t        chnode;   ///< (channel<<4) | node
    const uint8_t *data;     ///< dlc 字节数据
} can_burst_entry_t;

/**
 * @brief 给定统一 DLC，返回单次突发最多可容纳的帧数。
 *        受 236B SPI payload（4B 头 + count*(1+dlc)）与 GD32 硬上限约束。
 */
uint8_t canBurstCapacity(uint8_t dlc);

/**
 * @brief 发送一次 CAN 突发（SPI 0xC7）。count 必须 <= canBurstCapacity(dlc)。
 *        GD32 在 ISR 内对每帧调用非阻塞 transmitFrame，各通道邮箱背靠背装载。
 * @param id_suffix    CAN ID 低 16 位（CMD=0x0200）；id=(node<<16)|suffix。
 * @param dlc          所有条目统一数据长度（0~64）。
 * @param entries      count 个条目（chnode + data）。
 * @param count        条目数（1~canBurstCapacity(dlc)）。
 * @param status_out   输出每帧状态（GD32 CanTxResult 码，0x00=成功），可为 nullptr。
 * @param status_max   status_out 容量。
 * @param timeout_ms   SPI 事务超时。
 * @param gd32_err_out 返回 -3 时的 GD32 细分错误码；可为 nullptr。
 * @return 成功返回 count；-1 参数错误；-2 SPI 通信失败；-3 GD32 ERROR。
 */
int canFdSendBurst(uint16_t id_suffix, uint8_t dlc,
                   const can_burst_entry_t *entries, uint8_t count,
                   uint8_t *status_out, uint8_t status_max,
                   uint32_t timeout_ms,
                   uint8_t *gd32_err_out = nullptr);

// ============================================================
// 不重复 ID 快照表（SPI 0xC3 协议配套）
// ============================================================
#define CANFD_ID_SNAPSHOT_MAX      16    ///< 每通道最大不重复 ID 条目数
#define CANFD_ID_SNAPSHOT_DATA_MAX 64    ///< 快照条目最大数据字节数（CANFD 最大 64B，保持完整不截断）

/**
 * @brief 单个不重复 ID 的最新帧快照条目。
 *
 * 0xC1 线协议条目：id(4LE)+dlc(1)+flags(1)+rsvd(2)+data(dlc)。
 * seq / rx_count 未上线，解析后恒为 0（保留字段便于日后扩协议）。
 */
typedef struct {
    uint32_t id;        ///< CAN ID
    uint32_t seq;       ///< 线协议未携带，解析后恒 0
    uint32_t rx_count;  ///< 线协议未携带，解析后恒 0
    uint8_t  dlc;
    uint8_t  fd_flag;
    uint8_t  brs;
    uint8_t  esi;
    uint8_t  data[CANFD_ID_SNAPSHOT_DATA_MAX];
} id_snapshot_entry_t;

// ============================================================
// CAN 接收过滤模式 & 反馈快照（GD32 约 1Hz 刷新；ESP32 被动收 0xC1）
// 与 GD32 DCU_APP SPI 协议帧 0xC1/0xC2 配套
// ============================================================
typedef enum {
    CAN_RX_MODE_ALL_PASS  = 0,  ///< 默认：禁用 ID 过滤，三路全部接收
    CAN_RX_MODE_FILTER_ID = 1,  ///< 按 CONFIG_CAN_RX_FILTER_ID/MASK 做标准帧过滤
    CAN_RX_MODE_BRIDGE_0_TO_12 = 2, ///< CAN0 桥接：CAN0 收到的每一帧按原帧格式自动转发到 CAN1 与 CAN2
} can_rx_mode_t;

/**
 * @brief 单通道 CAN 反馈统计（快照字段；非 ESP32 侧 1Hz 轮询）。
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
    uint8_t   bus_state;   /**< 总线状态位图：bit0=init,bit1=控制器sleep,bit2=error_passive,bit3=bus_off,bit4=error_warning,bit5=tx_silent(收发器Silent,只听不发) */
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
 * 调用本函数解析：
 *   - page=0：清空 entries、重置 entryIdx，填固定头 + 首批条目；
 *   - page=1：校验 channel 与 page0 一致后追加；
 *   - page>1 或声明条目却截断无法解析 → 失败。
 * 成功时置 out->valid=1。
 *
 * @param payload  GD32 推送帧的 payload（[channel, page, total, count, mode, seq, ...]）。
 * @param len      payload 长度。
 * @param out      累积结果（跨 page 0/1 累积）。
 * @param entryIdx 当前已填充条目数（page 0 时被重置为 0，调用方按通道维护）。
 * @return 0 成功，<0 失败（payload 过短、页号/通道非法或条目截断）。
 */
int canFdParseFeedbackPage(const uint8_t *payload, size_t len,
                           can_feedback_result_t *out, uint8_t *entryIdx);

/**
 * @brief 主动向 GD32 查询一页 CAN 反馈（SPI 0xC1 请求/响应）。
 *
 * 2026-08 架构改造：ESP32 不再被动缓存 GD32 推送，改为前端请求时按需查询。
 * GD32 handleFeedback 接收 [channel, page] 返回 ACK + 页数据（与推送 payload 格式一致）。
 * 查询成功后调 canFdParseFeedbackPage 解析，复用现有累积逻辑。
 *
 * @param channel  目标通道 0/1/2。
 * @param page     页号 0 或 1（page 0 重置累积 + 填统计，page 1 追加 entries）。
 * @param out      累积结果（跨 page 0/1 累积，调用方按通道维护）。
 * @param entryIdx 当前已填充条目数（page 0 时被重置为 0）。
 * @return 0 成功；-1 参数/SPI 失败；-2 GD32 返回 ERROR。
 */
int canFdQueryFeedback(uint8_t channel, uint8_t page,
                        can_feedback_result_t *out, uint8_t *entryIdx);

/**
 * @brief 切换 GD32 三路 CAN 的接收过滤模式（SPI 0xC2）。
 * @param mode       目标模式（0=全收，1=按 ID 过滤，2=CAN0 桥接到 1/2）。
 * @param actual     可选输出，GD32 实际生效模式。
 * @param timeout_ms SPI 事务超时。
 * @return 0 成功，<0 失败（失败时打 WARN 日志）。
 */
int canFdSetRxMode(can_rx_mode_t mode,
                   can_rx_mode_t *actual,
                   uint32_t timeout_ms);

/**
 * @brief 切换指定 CAN 通道收发器的 Silent 模式（SPI 0xC8）：只听不发。
 *        GD32 侧把该通道 TCAN1057AV 的 S 脚置高 → 驱动器关断、**接收机照常工作**
 *        （手册 Table 8-4：RXD 仍镜像总线），所以静默期间接收与 0xC1 上报不受影响。
 *        静默期间该通道所有发送（0xC0/0xC7/桥接/relay）回 0xE6=kErrSilent，不计错误。
 * @param channel     目标通道 0/1/2。
 * @param silent      true=进 Silent，false=回 Normal。
 * @param actual      可选输出，GD32 实际生效状态（1=Silent，与其发送门控同一个值）。
 * @param timeout_ms  SPI 事务超时。
 * @return 0 成功，<0 失败（-1 参数/SPI 失败，-2 GD32 返回 ERROR）。
 */
int canFdSetTxSilent(uint8_t channel,
                     bool silent,
                     uint8_t *actual,
                     uint32_t timeout_ms);

#endif // CANFD_BRIDGE_H
