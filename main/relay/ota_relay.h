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
// IRelayListener - 中继事件监听接口（解决 H4 双向耦合）
// ============================================================
// OtaCoordinator 实现此接口并注册到 OtaRelay，
// OtaRelay 通过接口回调通知协调器，不再直接 #include ota_coordinator.h。
// 依赖方向变为单向：OtaCoordinator → OtaRelay（通过接口）。
/**
 * @brief 中继事件监听器接口。
 *        OtaCoordinator 实现此接口，OtaRelay 通过回调通知状态变更，
 *        消除 OtaRelay → OtaCoordinator 的直接依赖（解决 H4 双向耦合）。
 */
class IRelayListener {
public:
    virtual ~IRelayListener() = default;
    /** @brief 中继已开始（forwardFirmware 被调用）。 */
    virtual void onRelayStarted() = 0;
    /** @brief 中继已完成。 @param success true=成功，false=失败。 */
    virtual void onRelayComplete(bool success) = 0;
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
     * @return 当前 relay_state_t 状态值。
     */
    static relay_state_t getState();
    /**
     * @brief 获取中继当前序号.
     * @return 当前中继序号。
     */
    static uint32_t getRelaySeq();
    /**
     * @brief 获取中继总包数.
     * @return 本次中继的总包数。
     */
    static uint32_t getRelayTotalPackets();
    /**
     * @brief 获取GD32 APP存活标志位.
     * @return true 表示 GD32 APP 存活。
     */
    static bool getGd32AppAlive();
    /**
     * @brief 获取GD32固件版本.
     * @param out 输出版本信息结构体指针。
     */
    static void getGd32Version(gd32_version_t *out);
    /**
     * @brief 设置中继状态.
     * @param s 目标状态。
     */
    static void setState(relay_state_t s);
    /**
     * @brief 设置中继当前序号.
     * @param seq 序号值。
     */
    static void setRelaySeq(uint32_t seq);
    /**
     * @brief 设置中继总包数.
     * @param total 总包数。
     */
    static void setRelayTotalPackets(uint32_t total);
    /**
     * @brief 设置GD32 APP存活标志位.
     * @param alive true 表示存活。
     */
    static void setGd32AppAlive(bool alive);
    /**
     * @brief 设置GD32固件版本.
     * @param ver 版本信息结构体引用。
     */
    static void setGd32Version(const gd32_version_t &ver);
    /**
     * @brief 获取 OTA 中继任务句柄。
     * @return FreeRTOS 任务句柄。
     */
    static TaskHandle_t getTaskHandle();

    /**
     * @brief 重置状态机到 IDLE.
     */
    void reset();

    // ── 中继事件监听器（解决 H4 双向耦合）──
    /**
     * @brief 注册中继事件监听器（由 OtaCoordinator 在 init 时调用）。
     * @param listener 监听器指针，传 nullptr 取消注册。
     */
    static void setRelayListener(IRelayListener *listener);
    /**
     * @brief 获取当前注册的监听器。
     * @return 监听器指针，未注册时为 nullptr。
     */
    static IRelayListener* getRelayListener();

    // ── 队列访问器（解决 M3: 封装 public static 成员）──
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

    // ── Flash 操作已迁移至 Gd32FlashStore（storage/gd32_flash_store.h）──

    // ── 公开状态成员 ──
    static relay_state_t relayState;          ///< 当前状态机状态
    static uint32_t relaySeq;                 ///< 当前中继会话序号
    static uint32_t relayTotalPackets;        ///< 本次中继总包数
    static volatile bool gd32AppAlive;        ///< GD32 APP 存活标志
    static gd32_version_t gd32Version;        ///< GD32 固件版本信息
    static TaskHandle_t taskHandle;           ///< OTA 中继任务句柄
    static IRelayListener *s_relayListener;   ///< 中继事件监听器（解决 H4）

    // ── 线程安全锁 ──
    static SemaphoreHandle_t s_stateMutex;    ///< 状态成员互斥锁
    static void initMutex();                  ///< 初始化状态互斥锁（app_main 中调用）
    static constexpr TickType_t kLockTimeout = pdMS_TO_TICKS(50); ///< 锁获取超时时间

    /** @brief RAII 状态锁守护类，自动获取/释放 s_stateMutex。 */
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
    session_ctx_t m_sessionCtx;                     ///< AES 会话上下文
    uint8_t m_nonceServer[16] = {0};                ///< 服务端随机数
    uint8_t m_kSession[16] = {0};                   ///< 会话密钥（挑战认证后生成）
    uint8_t m_gd32UidRuntime[12] = {0};             ///< GD32 芯片 UID（运行时握手获取）

    static constexpr size_t GD32_UID_LEN = 12;
    static constexpr size_t GD32_CHALLENGE_RESPONSE_LEN = 36;

private:
    // ── 队列（M3: 由 getter 访问，外部不再直接引用）──
    static QueueHandle_t dispatchQueue;       ///< 事件分发队列
    static QueueHandle_t resultQueue;         ///< 结果回传队列

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
    ota_result_t doHandshakeReq();
    /**
     * @brief 执行挑战认证.
     * @return OTA_RESULT_OK 成功，OTA_RESULT_ERR_AUTH 认证失败。
     */
    ota_result_t doChallengeAndAuth();
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
     * @brief 发送结束指令并验证 APP 存活.
     * @return OTA_RESULT_OK 成功，负值表示错误。
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
