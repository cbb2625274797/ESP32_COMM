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
#include "freertos/task.h"
#include "transport/protocol.h"
#include "transport/spi_master.h"
#include "storage/gd32_flash_store.h"
#include "esp_partition.h"

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

/** @brief OTA 中继操作结果码枚举。 */
typedef enum {
    OTA_RESULT_OK = 0,             ///< 操作成功
    OTA_RESULT_ERR_TIMEOUT = -1,   ///< 等待 GD32 响应超时
    OTA_RESULT_ERR_CRC = -2,       ///< 固件 CRC/SHA-256 校验失败
    OTA_RESULT_ERR_AUTH = -3,      ///< 挑战认证失败
    OTA_RESULT_ERR_STORAGE = -4,   ///< Flash 存储错误
    OTA_RESULT_ERR_GD32 = -5,      ///< GD32 返回错误
    OTA_RESULT_ERR_HEALTH = -6,    ///< 健康检查未通过
    OTA_RESULT_ERR_USER_ABORT = -7 ///< 用户主动中止
} ota_result_t;

// ============================================================
// IRelayListener - 中继事件监听接口
// ============================================================
// OtaCoordinator 实现此接口并注册到 OtaRelay，
// OtaRelay 通过接口回调通知协调器，不再直接 #include ota_coordinator.h。
// 依赖方向为单向：OtaCoordinator → OtaRelay（通过接口）。
/**
 * @brief 中继事件监听器接口。
 *        OtaCoordinator 实现此接口，OtaRelay 通过回调通知状态变更，
 *        避免 OtaRelay → OtaCoordinator 的直接依赖。
 */
class IRelayListener {
public:
    virtual ~IRelayListener() = default;
    /** @brief 中继已开始（forwardFirmware 被调用）。 */
    virtual void onRelayStarted() = 0;
    /** @brief 中继已完成。 @param success true=成功，false=失败。 */
    virtual void onRelayComplete(bool success) = 0;
};

// ============================================================
// 中继进度快照（一次锁保护读取，消除逐 getter 不一致）
// ============================================================
/**
 * @brief GD32 中继进度只读快照。
 *        由 OtaRelay::getSnapshot() 在单次 StateLockGuard 下填充，
 *        供 OtaCoordinator::getSnapshot() 组装完整系统快照。
 */
struct RelaySnapshot {
    relay_state_t   state = RELAY_IDLE;     ///< 当前中继状态机状态
    uint32_t        seq = 0;                ///< 当前中继包序号
    uint32_t        total_packets = 0;      ///< 本次中继总包数
    bool            gd32_app_alive = false; ///< GD32 APP 存活标志
    gd32_version_t  version = {};           ///< GD32 固件版本
};

class OtaRelay {
public:
    /**
     * @brief 获取OtaRelay单例引用.
     * @return OtaRelay 单例引用。
     */
    static OtaRelay& instance();

    /**
     * @brief OTA中继FreeRTOS任务入口.
     * @param pvParameters FreeRTOS 任务参数（未使用）。
     */
    static void taskEntry(void *pvParameters);

    /**
     * @brief 从Flash向GD32转发固件（便捷入口，内部驱动状态机循环）.
     * @return 操作结果码.
     */
    ota_result_t forwardFirmware();

    // ── 状态查询（线程安全）──
    /**
     * @brief 获取当前中继状态.
     * @return 当前 relay_state_t 状态值。
     */
    static relay_state_t getState();
    /**
     * @brief 获取GD32 APP存活标志位.
     * @return true 表示 GD32 APP 存活。
     */
    static bool getGd32AppAlive();
    /**
     * @brief 一次性读取所有中继状态字段（单次锁保护）。
     *        消除逐 getter 调用之间 relay task 更新状态导致的不一致。
     * @return 中继进度快照。
     */
    static RelaySnapshot getSnapshot();
    /**
     * @brief 获取 OTA 中继任务句柄。
     * @return FreeRTOS 任务句柄。
     */
    static TaskHandle_t getTaskHandle();

    /**
     * @brief 重置本端中继状态机到 IDLE（不主动通知 GD32）。
     *        中继失败/中止时请优先用 abortRemoteAndReset()。
     */
    void reset();

    /**
     * @brief 通知 GD32 取消 OTA 会话（SPI CANCEL），再复位本端到 IDLE。
     *        失败路径必须调用，否则 GD32 可能停在 WAITING_RANDOM/CONFIRM 等半会话。
     * @param notify_remote true 时尽力发送 CANCEL（默认 true）。
     */
    void abortRemoteAndReset(bool notify_remote = true);

    // ── 中继事件监听器 ──
    /**
     * @brief 注册中继事件监听器（由 OtaCoordinator 在 init 时调用）。
     * @param listener 监听器指针，传 nullptr 取消注册。
     */
    static void setRelayListener(IRelayListener *listener);

    // ── 队列访问器 ──
    /**
     * @brief 获取事件分发队列句柄。
     *        外部模块（OtaCoordinator / BLE）通过 getter 访问，不再直接引用静态成员。
     * @return dispatchQueue 句柄，未初始化时为 NULL。
     */
    static QueueHandle_t getDispatchQueue();

    /**
     * @brief 获取结果回传队列句柄。
     * @return resultQueue 句柄，未初始化时为 NULL。
     */
    static QueueHandle_t getResultQueue();

    /**
     * @brief 探测 GD32 APP 存活并缓存版本（公开，供 OtaCoordinator / 启动探测共用）。
     * @param timeout_ms appPing 超时（毫秒）。
     * @param out_uptime 可选，输出 GD32 uptime（秒）。
     * @return true 探测成功，false 失败。
     */
    static bool probeAndCacheAppAlive(uint32_t timeout_ms, uint32_t *out_uptime = nullptr);

    /** @brief 统一 "HELLO(role=APP) → PING 探测" 循环（doWaitForAppAlive + taskEntry 共用）。 */
    static bool helloAndProbeApp(uint32_t timeout_ms, uint32_t ping_interval_ms,
                                 uint32_t max_attempts, bool allow_hello_shortcut);

    // ── 内部状态写入（仅 ota_relay.cpp 使用，无外部调用）──
    static void setState(relay_state_t s);
    static void setRelaySeq(uint32_t seq);
    static void setRelayTotalPackets(uint32_t total);
    static void setGd32AppAlive(bool alive);
    static void setGd32Version(const gd32_version_t &ver);
    static IRelayListener* getRelayListener();
    ota_result_t processEvent(relay_event_t event, const uint8_t *payload, size_t len);

    // ── 线程安全锁 ──
    static void initMutex();                  ///< 初始化状态互斥锁（taskEntry 首行调用）
    static constexpr TickType_t kLockTimeout = pdMS_TO_TICKS(50); ///< 锁获取超时时间

    /** @brief RAII 状态锁守护类，自动获取/释放 s_stateMutex。
     *         作为 public 嵌套类保留，内部代码在持锁块内可直接访问 private 状态成员。 */
    class StateLockGuard {
    public:
        explicit StateLockGuard(TickType_t timeout = kLockTimeout) {
            m_locked = (s_stateMutex && xSemaphoreTake(s_stateMutex, timeout) == pdTRUE);
        }
        ~StateLockGuard() {
            if (m_locked && s_stateMutex) { xSemaphoreGive(s_stateMutex); }
        }
        StateLockGuard(const StateLockGuard&) = delete;
        StateLockGuard& operator=(const StateLockGuard&) = delete;
    private:
        bool m_locked = false;
    };

    // ── 实例状态 ──
    session_ctx_t m_sessionCtx;                     ///< AES 会话上下文
    uint8_t m_nonceServer[16] = {0};                ///< 服务端随机数
    uint8_t m_kSession[16] = {0};                   ///< 会话密钥（挑战认证后生成）
    uint8_t m_gd32UidRuntime[12] = {0};             ///< GD32 芯片 UID（运行时握手获取）

    static constexpr size_t GD32_UID_LEN = 12;
    static constexpr size_t GD32_CHALLENGE_RESPONSE_LEN = 36;

private:
    // ── 队列（由 getter 访问）──
    static QueueHandle_t dispatchQueue;       ///< 事件分发队列
    static QueueHandle_t resultQueue;         ///< 结果回传队列

    // ── 状态成员（统一通过 public setter/getter 访问）──
    //   外部代码使用 getState()/getRelaySeq()/... 读取，setState()/setRelaySeq()/... 写入。
    //   内部代码：若已在 StateLockGuard 块内（reset/getSnapshot/启动探测），可直接访问；
    //             否则必须使用 setter/getter，禁止裸读写（消除数据竞争）。
    static relay_state_t relayState;          ///< 当前状态机状态
    static uint32_t relaySeq;                 ///< 当前中继会话序号
    static uint32_t relayTotalPackets;        ///< 本次中继总包数
    static volatile bool gd32AppAlive;        ///< GD32 APP 存活标志
    static gd32_version_t gd32Version;        ///< GD32 固件版本信息
    static IRelayListener *s_relayListener;   ///< 中继事件监听器
    static SemaphoreHandle_t s_stateMutex;    ///< 状态成员互斥锁（由 StateLockGuard 使用）

    // taskHandle 为 write-once-at-init 字段：仅 taskEntry 启动首行写入一次，
    // 之后只读。getTaskHandle() 无锁返回，因写入发生在任务创建后、任务循环开始前，
    // 与任何外部读取者构成 happens-before（任务创建同步），无数据竞争。
    static TaskHandle_t taskHandle;           ///< OTA 中继任务句柄

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
     * @return ESP_OK 成功，SPI 通信失败返回错误码。
     */
    esp_err_t requestGd32AppEnterBootloader();
    /**
     * @brief 执行握手请求（发送 HANDSHAKE_REQ）.
     * @return OTA_RESULT_OK 成功，OTA_RESULT_ERR_TIMEOUT 超时。
     */
    ota_result_t doHandshakeReq(uint32_t attempts = 20U);
    /**
     * @brief 执行挑战认证.
     * @return OTA_RESULT_OK 成功，OTA_RESULT_ERR_AUTH 认证失败。
     */
    ota_result_t doChallengeAndAuth();
    /**
     * @brief 向 GD32 发送 OTA CANCEL（0x03），请求远端完整 resetToIdle。
     * @return ESP_OK 已发出并读到响应（或可忽略的链路错误已记录）。
     */
    esp_err_t sendRemoteCancel();
    /**
     * @brief 发送固件总大小.
     * @param size 固件总大小（字节）。
     * @return OTA_RESULT_OK 成功，OTA_RESULT_ERR_TIMEOUT 超时。
     */
    ota_result_t doSendFirmwareSize(uint32_t size);
    /**
     * @brief 转发下一批数据包（由 TRANSFERRING 状态循环调用）.
     * @param deadline_tick 全局超时时刻（0=不限时）。
     * @return OTA_RESULT_OK 成功，负值表示错误。
     */
    ota_result_t doRelayPackets(TickType_t deadline_tick);
    /**
     * @brief 准备单包：读 Flash + 组装 tx_buf + 加密 + CRC（doRelayPackets 子步骤）。
     * @param seq 当前包序号。
     * @param flash_offset 本包在 gd32_ota 分区中的起始偏移（不依赖 m_transferFlashOffset，便于预组下一包）。
     * @param meta 固件元数据（用于计算末包 chunk_size）。
     * @param tx_buf 输出缓冲区（64 字节）。
     * @param out_chunk_size 输出本包有效字节数。
     * @return OTA_RESULT_OK 成功，OTA_RESULT_ERR_STORAGE 读 Flash 失败，OTA_RESULT_ERR_AUTH 加密失败。
     */
    ota_result_t prepareRelayPacket(uint32_t seq, uint32_t flash_offset, const ota_meta_t &meta,
                                    uint8_t tx_buf[64], size_t *out_chunk_size);

    /** 等 ACK 期间可选预组下一包（与 GD32 处理重叠，隐藏本端 Flash/AES 延迟）。 */
    struct NextPacketPrep {
        uint32_t next_seq = 0;
        uint32_t next_flash_offset = 0;
        const ota_meta_t *meta = nullptr;
        uint8_t *buf = nullptr;       ///< 64 字节
        size_t *chunk_size = nullptr;
        bool *ready = nullptr;
    };

    /**
     * @brief 发送单包并等待 ACK（doRelayPackets 子步骤）。
     *
     * 见 WAIT 后只 poll 不重发 DATA。可选 next_prep：在等 ACK 的 poll 间隙预组下一包。
     *
     * @param seq 当前包序号。
     * @param tx_buf 已组装好的 64 字节发送缓冲区。
     * @param chunk_size 本包有效字节数（成功时用于推进 m_transferFlashOffset）。
     * @param[out] out_result 失败时写入错误码（OK 时不修改）。
     * @param next_prep 可选；非空则在 poll 循环内至多预组一次下一包。
     * @return true 应中止外层循环（result 已设）；false 继续下一包。
     */
    bool sendRelayPacketWithRetry(uint32_t seq, const uint8_t tx_buf[64], size_t chunk_size,
                                  ota_result_t *out_result, NextPacketPrep *next_prep = nullptr);
    /**
     * @brief 发送结束指令并验证 APP 存活（顺序调用 doSendFinishOta + doWaitForAppAlive）.
     * @return OTA_RESULT_OK 成功，负值表示错误。
     */
    ota_result_t doFinishAndVerify();
    /**
     * @brief 发送 FINISH_OTA 指令并等待 GD32 ACK（10 次重试）.
     * @return OTA_RESULT_OK 成功，OTA_RESULT_ERR_GD32 失败。
     */
    ota_result_t doSendFinishOta();
    /**
     * @brief 轮询等待 GD32 APP 存活（10 次重试）.
     * @param timeout_ms 单次 appPing 超时（毫秒）。
     * @return OTA_RESULT_OK 成功，OTA_RESULT_ERR_GD32 超时失败。
     */
    ota_result_t doWaitForAppAlive(uint32_t timeout_ms);

    // ── 传输进度跟踪（状态机内部）──
    uint32_t m_transferTotalPackets = 0;
    uint32_t m_transferCurrentSeq = 0;
    uint32_t m_transferFlashOffset = 0;
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
    /** @brief 清理会话密钥/上下文/nonce/UID 运行时缓存（reset 与 doFinishAndVerify 共用）。 */
    void clearSessionCryptoState();
    ota_result_t sendAndWaitAckShape(const char *stage, uint8_t cmd,
                                     const uint8_t *payload, size_t payload_len,
                                     uint32_t transact_timeout_ms,
                                     uint32_t poll_timeout_ms, uint32_t wait_rounds, uint32_t max_extra_polls,
                                     uint8_t resp_payload[], size_t resp_max, size_t *resp_len,
                                     bool (*matcher)(const uint8_t *, size_t));
};

#endif // OTA_RELAY_H
