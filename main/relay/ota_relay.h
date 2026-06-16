/*
 * ota_relay.h - OTA 中继事件驱动状态机
 *
 * OtaRelay 将 GD32 SPI OTA 协议封装为事件驱动的状态机：
 *   1. 每个协议阶段对应一个状态（共 6 个阶段状态 + ERROR）
 *   2. 状态通过 processEvent() 驱动转换，而非顺序赋值
 *   3. 外部（OtaCoordinator / transport layer）通过 dispatchQueue 触发中继
 *   4. forwardFirmware() 保留为便捷入口，内部驱动状态机循环
 *
 * 状态转换：
 *   IDLE → HANDSHAKING → SIZE_NEG → TRANSFERRING → FINISHING → DONE
 *   任意状态 ──(error)──→ ERROR
 *   ERROR/DONE ──(reset)──→ IDLE
 */
#ifndef OTA_RELAY_H
#define OTA_RELAY_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "transport/protocol.h"
#include "transport/spi_master.h"
#include "esp_partition.h"

#define OTA_META_NVS_NAMESPACE "ota_meta"

typedef struct {
    uint32_t total_size;
    uint32_t stored_size;
    uint32_t last_seq;
    uint8_t  checksum[32];
} ota_meta_t;

// ============================================================
// 阶段状态枚举（精简为 7 个状态，原 17 个"微状态"合并为阶段）
// ============================================================
typedef enum {
    RELAY_IDLE = 0,          // 空闲
    RELAY_HANDSHAKING,       // 阶段1: 握手 + 挑战认证
    RELAY_SIZE_NEG,          // 阶段2: 固件大小协商
    RELAY_TRANSFERRING,      // 阶段3: 数据包传输
    RELAY_FINISHING,         // 阶段4-5: 结束OTA + APP存活验证
    RELAY_DONE,              // 完成
    RELAY_ERROR = 0xFF       // 错误
} relay_state_t;

// ============================================================
// 事件类型（驱动状态转换）
// ============================================================
typedef enum {
    RELAY_EVT_START = 0,     // 启动中继流程
    RELAY_EVT_RESP_OK,       // 收到 GD32 有效响应
    RELAY_EVT_RESP_ERROR,    // 收到 GD32 错误响应
    RELAY_EVT_TIMEOUT,       // 超时
    RELAY_EVT_HEALTH_CRIT,   // 健康检查危险
    RELAY_EVT_ABORT          // 外部中止
} relay_event_t;

typedef enum {
    OTA_RESULT_OK = 0,
    OTA_RESULT_ERR_TIMEOUT = -1,
    OTA_RESULT_ERR_CRC = -2,
    OTA_RESULT_ERR_AUTH = -3,
    OTA_RESULT_ERR_STORAGE = -4,
    OTA_RESULT_ERR_GD32 = -5,
    OTA_RESULT_ERR_HEALTH = -6,
    OTA_RESULT_ERR_USER_ABORT = -7
} ota_result_t;

class OtaRelay {
public:
    /**
     * @brief 获取OtaRelay单例引用.
     */
    static OtaRelay& instance();

    /**
     * @brief OTA中继FreeRTOS任务入口.
     */
    static void taskEntry(void *pvParameters);

    /**
     * @brief 从Flash向GD32转发固件（便捷入口，内部驱动状态机循环）.
     * @return 操作结果码.
     */
    ota_result_t forwardFirmware();

    // ── 事件驱动接口 ──
    /**
     * @brief 向状态机投递事件，驱动状态转换.
     * @param event 事件类型.
     * @param payload 关联数据（响应载荷等），可为nullptr.
     * @param len 数据长度.
     * @return 当前操作结果码（OTA_RESULT_OK 表示事件已处理且可继续）.
     */
    ota_result_t processEvent(relay_event_t event, const uint8_t *payload, size_t len);

    // ── 状态查询（线程安全）──
    /**
     * @brief 获取当前中继状态.
     */
    static relay_state_t getState();
    /**
     * @brief 获取中继当前序号.
     */
    static uint32_t getRelaySeq();
    /**
     * @brief 获取中继总包数.
     */
    static uint32_t getRelayTotalPackets();
    /**
     * @brief 获取GD32 APP存活标志位.
     */
    static bool getGd32AppAlive();
    /**
     * @brief 获取GD32固件版本.
     */
    static void getGd32Version(gd32_version_t *out);
    /**
     * @brief 设置中继状态.
     */
    static void setState(relay_state_t s);
    /**
     * @brief 设置中继当前序号.
     */
    static void setRelaySeq(uint32_t seq);
    /**
     * @brief 设置中继总包数.
     */
    static void setRelayTotalPackets(uint32_t total);
    /**
     * @brief 设置GD32 APP存活标志位.
     */
    static void setGd32AppAlive(bool alive);
    /**
     * @brief 设置GD32固件版本.
     */
    static void setGd32Version(const gd32_version_t &ver);

    /**
     * @brief 重置状态机到 IDLE.
     */
    void reset();

    // ── Flash 操作 ──
    /**
     * @brief Flash写入接口（支持非4字节对齐写入）.
     */
    static esp_err_t flashWriteChunk(uint32_t offset, const uint8_t *data, size_t len);
    /**
     * @brief 从OTA缓存分区读取数据块.
     */
    static esp_err_t flashReadChunk(uint32_t offset, uint8_t *data, size_t len);
    /**
     * @brief 从Flash加载元数据.
     */
    static esp_err_t metaLoad(ota_meta_t *meta);
    /**
     * @brief 保存元数据到Flash.
     */
    static esp_err_t metaSave(const ota_meta_t *meta);
    /**
     * @brief 传输过程中更新元数据.
     */
    static esp_err_t metaUpdate(uint32_t stored_size, uint32_t last_seq);
    /**
     * @brief OTA完成或中止时清除元数据.
     */
    static esp_err_t metaClear();
    /**
     * @brief 计算gd32_ota分区中已存储固件的SHA-256.
     */
    static esp_err_t computeFlashSha256(uint8_t out_sha256[32]);
    /**
     * @brief 固件接收完毕后计算SHA-256并写入元数据.
     */
    static esp_err_t metaFinalizeChecksum();
    /**
     * @brief 校验固件完整性（SHA-256）.
     */
    static esp_err_t metaVerifyChecksum();

    // ── 公开状态成员 ──
    static relay_state_t relayState;
    static uint32_t relaySeq;
    static uint32_t relayTotalPackets;
    static volatile bool gd32AppAlive;
    static gd32_version_t gd32Version;
    static QueueHandle_t dispatchQueue;
    static QueueHandle_t resultQueue;

    // ── 线程安全锁 ──
    static SemaphoreHandle_t s_stateMutex;
    static void initMutex();
    static constexpr TickType_t kLockTimeout = pdMS_TO_TICKS(50);

    class StateLockGuard {
    public:
        explicit StateLockGuard(TickType_t timeout = kLockTimeout) {
            m_locked = (s_stateMutex && xSemaphoreTake(s_stateMutex, timeout) == pdTRUE);
        }
        ~StateLockGuard() {
            if (m_locked && s_stateMutex) { xSemaphoreGive(s_stateMutex); }
        }
        bool isLocked() const { return m_locked; }
        StateLockGuard(const StateLockGuard&) = delete;
        StateLockGuard& operator=(const StateLockGuard&) = delete;
    private:
        bool m_locked = false;
    };

    // ── 实例状态 ──
    const esp_partition_t *m_gd32Partition = NULL;
    session_ctx_t m_sessionCtx;
    uint8_t m_nonceServer[16] = {0};
    uint8_t m_kSession[16] = {0};
    uint8_t m_gd32UidRuntime[12] = {0};

    static constexpr size_t GD32_UID_LEN = 12;
    static constexpr size_t GD32_CHALLENGE_RESPONSE_LEN = 36;

private:
    OtaRelay() = default;
    ~OtaRelay() = default;
    OtaRelay(const OtaRelay&) = delete;
    OtaRelay& operator=(const OtaRelay&) = delete;

    // ── 状态机内部类型 ──
    /**
     * @brief 状态处理函数类型：接收事件 + 载荷，返回结果码.
     *        返回 OTA_RESULT_OK 表示事件已成功处理且状态机可继续；
     *        返回负值表示错误，状态机转入 RELAY_ERROR.
     */
    typedef ota_result_t (OtaRelay::*StateHandler)(relay_event_t event,
                                                    const uint8_t *payload, size_t len);

    struct RelayStateDef {
        const char *name;           // 状态名称（日志用）
        StateHandler handler;       // 事件处理器
    };

    // ── 状态表（const，编译期确定）──
    static const RelayStateDef s_stateTable[];

    // ── 各状态的事件处理器 ──
    ota_result_t handleIdle(relay_event_t event, const uint8_t *payload, size_t len);
    ota_result_t handleHandshaking(relay_event_t event, const uint8_t *payload, size_t len);
    ota_result_t handleSizeNeg(relay_event_t event, const uint8_t *payload, size_t len);
    ota_result_t handleTransferring(relay_event_t event, const uint8_t *payload, size_t len);
    ota_result_t handleFinishing(relay_event_t event, const uint8_t *payload, size_t len);
    ota_result_t handleDone(relay_event_t event, const uint8_t *payload, size_t len);
    ota_result_t handleError(relay_event_t event, const uint8_t *payload, size_t len);

    // ── 协议子步骤（由状态 handler 内部调用）──
    /**
     * @brief 请求GD32 APP进入Bootloader.
     */
    esp_err_t requestGd32AppEnterBootloader();
    /**
     * @brief 执行握手请求（发送 HANDSHAKE_REQ）.
     */
    ota_result_t doHandshakeReq();
    /**
     * @brief 执行挑战认证.
     */
    ota_result_t doChallengeAndAuth();
    /**
     * @brief 发送固件总大小.
     */
    ota_result_t doSendFirmwareSize(uint32_t size);
    /**
     * @brief 转发下一批数据包（由 TRANSFERRING 状态循环调用）.
     */
    ota_result_t doRelayPackets(TickType_t deadline_tick);
    /**
     * @brief 发送结束指令并验证 APP 存活.
     */
    ota_result_t doFinishAndVerify();

    // ── 传输进度跟踪（状态机内部）──
    uint32_t m_transferTotalPackets = 0;
    uint32_t m_transferCurrentSeq = 0;
    uint32_t m_transferFlashOffset = 0;
    uint32_t m_transferRetryCount = 0;
    TickType_t m_transferDeadline = 0;

    // ── 加密/工具方法（不变）──
    static int aes128EcbEncryptBlockLocal(const uint8_t *key, const uint8_t *in, uint8_t *out);
    static void logRespPayload(const char *stage, uint8_t resp_type, const uint8_t *payload, size_t len);
    esp_err_t waitUntilNotWait(const char *stage, uint8_t *resp_type,
                               uint8_t *resp_payload, size_t resp_max,
                               size_t *resp_len, uint32_t timeout_ms,
                               uint32_t max_wait_rounds);
    static bool isStructuredAckWithSeq(const uint8_t *payload, size_t len, uint32_t expected_seq);
    static bool parseStructuredError(const uint8_t *payload, size_t len, uint32_t *seq_out, uint8_t *err_out);
    static bool isHandshakeEchoAck(const uint8_t *payload, size_t len);
    static bool isChallengePayloadAck(const uint8_t *payload, size_t len);
    static bool isConfirmPlainAck(const uint8_t *payload, size_t len);
    static bool isSizeSeq0Ack(const uint8_t *payload, size_t len);
    static bool isFinishPlainAck(const uint8_t *payload, size_t len);
    static esp_err_t pollUntilExpectedAckShape(const char *stage,
                                               uint8_t *resp_type, uint8_t *resp_payload,
                                               size_t resp_max, size_t *resp_len,
                                               uint32_t poll_timeout_ms, uint32_t wait_rounds,
                                               uint32_t maxExtraPolls,
                                               bool (*matcher)(const uint8_t *, size_t));
    static bool parseAndVerifyChallengePayload(const uint8_t *resp_payload, size_t resp_len,
                                               uint8_t *out_k_session, uint8_t *out_uid);
    static bool aes128EcbEncrypt48Bytes(const uint8_t *key, const uint8_t *in, uint8_t *out);
    static void calculateCrc32WithKey(const uint8_t *key, const uint8_t *data48, uint8_t *output);
};

#endif // OTA_RELAY_H
