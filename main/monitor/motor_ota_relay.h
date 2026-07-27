/*
 * motor_ota_relay.h - 电机 OTA relay ESP32 侧（固件缓冲 + SPI 喂数据 + 进度缓存）
 *
 * 两段式流程（与 BLE 刷 GD32 对齐，避免收固件期间与 GD32 按铃争抢单缓冲 MISO）：
 *   1. 前端 POST /motor/ota 上传固件 + channel → 流式写 gd32_ota flash 分区，写 meta
 *   2. 前端 POST /motor/ota/relay?ch=X → motorOtaStart 发 SPI 0xD0(start) + 启动 feedTask
 *   3. feedTask 从 flash 分块读，经 SPI 0xD1(data) 喂 GD32
 *   4. GD32 通过 0xD2 ring push 报进度，ESP32 缓存
 *   5. 前端 GET /motor/ota/status 轮询进度
 */
#ifndef MOTOR_OTA_RELAY_H
#define MOTOR_OTA_RELAY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOTOR_OTA_MAX_FW_SIZE   0xC000U   /* 48 KB */
	/* SPI payload 上限 236 = 256 - V2_OVERHEAD(20)。前 4B 为绝对 offset LE，数据最多 232B。
	 * 无 offset 时 SPI 超时重传会被 GD32 重复追加（BLF：前 236B 循环 → 刷坏）。 */
	#define MOTOR_OTA_CHUNK_HDR     4U
	#define MOTOR_OTA_CHUNK_DATA    232U
	#define MOTOR_OTA_CHUNK_SIZE    (MOTOR_OTA_CHUNK_HDR + MOTOR_OTA_CHUNK_DATA)

typedef enum {
    MOTOR_OTA_IDLE = 0,
    MOTOR_OTA_HANDSHAKE_REQ,
    MOTOR_OTA_HANDSHAKE_NONCE,
    MOTOR_OTA_HANDSHAKE_CONFIRM,
    MOTOR_OTA_ERASE,
    MOTOR_OTA_TRANSFER,
    MOTOR_OTA_FINISH_OTA,    /* 收到 0xF0F0F0F0，已发 FINISH_OTA，等跳转 ACK */
    MOTOR_OTA_FINISHED,
    MOTOR_OTA_ERROR,
    MOTOR_OTA_PRE_CANCEL = 9,    /* 握手前预取消：发 CANCEL 清理电机残留会话（GD32 侧过渡态） */
    MOTOR_OTA_WAIT_APP_RUN = 10  /* 尾包 ACK 后等新 APP 自主发 [0A AC] 运行确认，收到才 FINISHED */
} motor_ota_state_t;

typedef struct {
    motor_ota_state_t state;
    uint8_t  error_code;
    uint16_t packets_acked;
    uint16_t total_packets;
    uint32_t fw_size;
    uint16_t buf_used;       /* GD32 侧缓冲占用 */
    int64_t  last_update_ms; /* ESP32 收到状态的时间 */
} motor_ota_status_t;

/**
 * @brief 处理 GD32 0xD2 ring push（由 can_feedback_task 分发）。
 */
void onMotorOtaStatusPush(const uint8_t *payload, size_t len);

/**
 * @brief 启动电机 OTA：发 SPI start + 启动喂数据 task。
 *
 * 固件须已由调用方（HTTP handler）流式写入 gd32_ota flash 分区
 * （Gd32FlashStore::flashWriteChunk，offset 0 起）。喂数据 task 从
 * flash 分块读取并经 SPI 0xD1 喂给 GD32，不在堆上缓冲整个固件
 * （48KB 整块 malloc 会超出 ESP32-C3 的 largest_free_block，导致 OOM）。
 *
 * @param fw_size  固件大小 (≤48KB)，须与已写入 flash 的大小一致
 * @param channel  CAN 通道 (0-2)
 * @return 0 成功, -1 参数错误, -2 已在进行中, -3 SPI 失败
 */
int motorOtaStart(uint32_t fw_size, uint8_t channel);

/**
 * @brief 电机 OTA 是否正在进行（喂数据 task 存活）。
 *        供 OtaCoordinator 在 beginGd32Storage/beginEsp32Ota 中检查：
 *        电机喂数期间禁止新的 GD32/ESP32 OTA，避免 gd32_ota 分区被并发覆写。
 */
bool motorOtaIsActive(void);

/**
 * @brief 取消电机 OTA。
 */
void motorOtaCancel(void);

/**
 * @brief 获取最新状态（线程安全）。
 */
motor_ota_status_t motorOtaGetStatus(void);

/**
 * @brief 模块初始化（创建 mutex 等）。
 */
void motorOtaRelayInit(void);

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_OTA_RELAY_H */
