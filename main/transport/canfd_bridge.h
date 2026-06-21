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

#endif // CANFD_BRIDGE_H
