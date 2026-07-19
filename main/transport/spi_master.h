/*
 * spi_master.h - SPI Master 驱动头文件
 * CANFD 3通道发送接口 + 底层 SPI 总线
 *
 * SpiMaster 类封装 SPI 总线初始化、CANFD 通信和 SPI OTA 协议。
 *
 * 握手架构（v2 DATA_READY，GPIO10 ← GD32 PB6 开漏）：
 * - PB6 仅由 GD32 驱动：低 = 有待读数据，高 = 队列空。ESP32 只输入、不拉线。
 * - 帧同步/许可在帧内 SEQ/ACK_SEQ/CREDIT；CS 本身即互斥，TX 前不再等 HIGH。
 * - GD32 有数据：拉低 DATA_READY → ESP32 下降沿/轮询 → CS 读响应。
 */
#ifndef SPI_MASTER_H
#define SPI_MASTER_H

#include <stdint.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "spi_link_v2.h"
#include "app_config.h"  /* SPI_MASTER_CS_GPIO / SPI_READY_GPIO / SPI_OTA_FRAME_MAX 等 */

// SPI 时钟来自 menuconfig；其余可调项在 app_config.h
#define SPI_CLOCK_HZ          CONFIG_DCU_SPI_CLOCK_HZ

// GD32 SPI 业务 TYPE（与 SpiDefs / v2 TYPE 字段一致）
#define SPI_OTA_CMD_REQ       (0x01)
#define SPI_OTA_CMD_DATA      (0x02)
#define SPI_APP_ENTER_BOOT_CMD (0xB0)
#define SPI_APP_ENTER_BOOT_ACK_OK (0xB5)
#define SPI_OTA_RSP_ACK       (0x80)
#define SPI_OTA_RSP_WAIT      (0x81)
#define SPI_OTA_RSP_ERROR     (0x82)

// GD32 版本信息（从 APP PING 响应解析）
typedef struct {
    uint16_t app_major;
    uint16_t app_minor;
    uint16_t app_patch;
    uint16_t app_build;
    uint16_t boot_major;
    uint16_t boot_minor;
    uint16_t boot_patch;
    uint16_t boot_build;
} gd32_version_t;

/**
 * @brief SPI Master 驱动类（单例）
 *
 * 封装 SPI 总线初始化、同步传输、CANFD 通信和 SPI OTA 协议。
 */
class SpiMaster {
public:
    /**
     * @brief 获取 SpiMaster 单例实例。
     * @return SpiMaster 单例引用。
     */
    static SpiMaster& instance();

    /** @brief 初始化 SPI2 总线与目标从设备。 */
    esp_err_t init();

    // ============================================================
    // SPI OTA 协议
    // ============================================================

    /** @brief 仅执行 SPI OTA 响应读取事务，不重复发送请求帧。 */
    esp_err_t otaPollResponse(uint8_t *resp_type, uint8_t *resp_payload,
                              size_t resp_max, size_t *resp_len, uint32_t timeout_ms);

    /** @brief ESP32→GD32 发送命令 + 轮询读取真实响应（两步法，原子持锁）。
     *
     * 解决 GD32 "延迟一拍"协议：transact 首笔 CS 读到过期响应，
     * otaPollResponse 读取本笔命令的真实响应。命令只发送一次，不重发。
     * 适用于所有 ESP32→GD32 方向的命令（CAN 发送/模式切换/ID 快照等）。
     *
     * @param type        请求帧类型（如 0xC0/0xC2/0xC3）。
     * @param payload     请求负载数据。
     * @param payload_len 请求负载长度。
     * @param resp_type      输出响应类型（ACK/WAIT/ERROR）。
     * @param resp_payload   输出响应负载缓冲区。
     * @param resp_max       响应负载缓冲区最大长度。
     * @param resp_len       输出实际响应负载长度。
     * @param timeout_ms  总超时（发送 + 轮询），内部分配。
     * @return ESP_OK 成功读到响应；其他为 SPI 错误或超时。
     */
    esp_err_t otaSendAndPoll(uint8_t type, const uint8_t *payload, size_t payload_len,
                             uint8_t *resp_type, uint8_t *resp_payload,
                             size_t resp_max, size_t *resp_len, uint32_t timeout_ms);

    /** @brief 读取对端最新上报的 CREDIT（0 表示对端忙，应 POLL 等待而非重发）。 */
    uint8_t remoteCredit() const { return m_remoteCredit; }

    /** @brief 读取本端剩余可接收 credit（SPEC §10.1，/system/status 展示用）。 */
    uint8_t localCredit() const { return m_localCredit; }

    /** @brief SPI worker 任务句柄（栈水位 /system/status 与 health 巡检用）。 */
    TaskHandle_t getWorkerTaskHandle() const { return m_workerTask; }

    /** @brief SPI 任务队列当前待处理 job 数（每个堆 Job 持有 calloc 的 Job 结构）。
     *        供 /system/heap_low_log 堆低水位快照采样，定位"低谷是不是 SPI 队列堆积引起"。 */
    UBaseType_t getPendingJobCount() const { return m_jobQueue ? uxQueueMessagesWaiting(m_jobQueue) : 0; }

    /**
     * @brief OTA 中继期间禁止 worker 将 DATA_READY 当 EVENT 读取。
     * 中继开始设 true，结束/复位设 false。
     */
    void setSuppressRingService(bool suppress);

    /**
     * @brief 业务层确认当前请求已成功时推进/对齐 m_txSeq。
     */
    void confirmTxSeqAccepted();

    /**
     * @brief 统一请求入口：经 SPI worker 串行执行（send+poll）。
     */
    esp_err_t transact(uint8_t type, const uint8_t *payload, size_t payload_len,
                       uint8_t *resp_type, uint8_t *resp_payload,
                       size_t resp_max, size_t *resp_len, uint32_t timeout_ms);

    /**
     * @brief 取出 GD32 EVENT 帧（worker 已读入事件队列）。
     * @param timeout_ms  等待超时（ms）。0 = 非阻塞（原语义）。
     * @return true 取到事件；false 队列空且超时未到达。
     */
    bool takeEvent(uint8_t *type, uint8_t *payload, size_t max_len, size_t *len,
                   uint32_t timeout_ms = 0U);

    // ============================================================
    // SPI Link v2 链路层（阶段1：帧格式 + 状态成员就位）
    // ============================================================

    /** @brief v2 链路状态统计（SPEC/spi_link_v2.md §12）。 */
    struct SpiLinkStats {
        uint32_t crc_errors;            ///< CRC 校验失败计数
        uint32_t duplicate_requests;    ///< 重复请求计数（序号回退）
        uint32_t seq_errors;            ///< 序号跳变计数
        uint32_t retries;               ///< 重试总数
        uint32_t ready_timeouts;        ///< DATA_READY 超时计数
        uint32_t rx_overruns;           ///< RX overrun 计数
        uint32_t resyncs;               ///< HELLO 重同步次数
        uint32_t max_resp_latency_us;   ///< 最大响应延迟（微秒）
    };

    /** @brief v2 完整链路状态（SPEC §10.1，会话/credit + 统计）。 */
    struct SpiLinkStatus {
        uint8_t  version;               ///< 协议版本（固定 2）
        uint8_t  gd32_role;             ///< 0=unknown, 1=app, 2=boot
        uint32_t session_id;            ///< 当前会话 ID
        uint8_t  local_credit;          ///< 本端剩余 credit
        uint8_t  remote_credit;         ///< 对端剩余 credit
        SpiLinkStats stats;             ///< 8 个统计计数器
    };

    /** @brief 当前会话 ID（0 = 未建立 session）。 */
    uint32_t sessionId() const { return m_sessionId; }

    /** @brief v2 session 是否已激活。 */
    bool v2Active() const { return m_v2Active; }

    /** @brief 最近一次 HELLO 得到的 GD32 角色（0=unknown, 1=app, 2=boot）。 */
    uint8_t gd32Role() const { return m_gd32Role; }

    /**
     * @brief 是否已从 HELLO_ACK/PING 身份块拿到功能位图。
     * @note  false=尚未建连身份或旧固件。
     */
    bool gd32FeatKnown() const { return m_gd32FeatKnown; }

    /** @brief GD32 功能位图（V2_FEAT_*）；未知时为 0。 */
    uint32_t gd32FeatFlags() const { return m_gd32FeatFlags; }

    /** @brief 便捷：GD32 是否开启 ADC 0xC4（需 gd32FeatKnown）。 */
    bool gd32FeatAdcFeedback() const {
        return m_gd32FeatKnown && (m_gd32FeatFlags & V2_FEAT_ADC_FEEDBACK) != 0U;
    }

    /** @brief 是否已从 HELLO_ACK/PING 拿到 APP/BOOT 版本。 */
    bool gd32IdKnown() const { return m_gd32IdKnown; }

    /** @brief 最近身份块中的版本（HELLO 或 PING）。 */
    gd32_version_t gd32VersionCached() const { return m_gd32Version; }

    /** @brief 获取链路统计快照（线程安全，持锁读取）。 */
    SpiLinkStats linkStats();

    /** @brief 获取完整链路状态（SPEC §10.1，会话/credit + 统计）。 */
    SpiLinkStatus linkStatus();

    /** @brief 发起 HELLO 握手建立 v2 session（SPEC §5.1）。
     *  生成随机非零 SESSION_ID，发送 LINK_HELLO，等待 HELLO_ACK。
     *  成功后 m_v2Active=true，序号归零。
     *  @param timeout_ms 总超时（毫秒），默认 3000。
     *  @param role_hint  期望的对端角色（0=any, 1=app, 2=boot）。
     *  @return ESP_OK 成功；ESP_ERR_TIMEOUT 超时；ESP_ERR_INVALID_RESPONSE 响应异常。 */
    esp_err_t hello(uint32_t timeout_ms = 3000U, uint8_t role_hint = 0U);

    /** @brief 通知 GD32 DCU_APP 设置 OTA 标志并软复位进入 Bootloader。 */
    esp_err_t appRequestBootloader();

    /** @brief 通过 HELLO 轮询等待 Bootloader 就绪（v2 方式，不再依赖 PB6 序列）。
     * App→Boot 切换后，ESP32 每 20ms 发送 HELLO(role_hint=boot)，
     * 等待 HELLO_ACK 且 role=boot，最长等待 timeout_ms。
     * 超时返回 ESP_ERR_TIMEOUT。 */
    esp_err_t waitForBootloaderReady(uint32_t timeout_ms);

    /** @brief 向 GD32 APP 发送存活探测命令，等待 APP 回复 ALIVE ACK。 */
    esp_err_t appPing(uint32_t timeout_ms, uint32_t *uptime_sec, gd32_version_t *version = nullptr);

    /**
     * @brief L0 本机自检（不依赖 GD32）：SPI 设备/互斥/worker/队列已就绪。
     */
    bool selfTestL0() const;

    /**
     * @brief L0 对端轻检：短超时 HELLO（role=any），确认 SPI 链路可达 GD32。
     * @note  GD32 仍在启动延时中时可能失败，属非关键项。
     */
    bool selfTestPeerL0();

private:
    /**
     * @brief 私有默认构造函数。
     */
    SpiMaster() = default;
    /**
     * @brief 私有析构函数。
     */
    ~SpiMaster() = default;
    /**
     * @brief 禁止复制构造。
     */
    SpiMaster(const SpiMaster&) = delete;
    /**
     * @brief 禁止复制赋值。
     */
    SpiMaster& operator=(const SpiMaster&) = delete;

    spi_device_handle_t m_spiDevice = NULL;
    SemaphoreHandle_t m_mutex = NULL;

    // ── 本端事务态（非 PB6 电平门控；PB6 仅 DATA_READY 通知）──
    /** 本端 SPI 事务状态：仅用于 acceptRing（忙时不接按铃）。 */
    enum class HandshakeState : uint8_t {
        IDLE = 0,             ///< 空闲，可接受 GD32 按铃
        MASTER_SENDING,       ///< 主机正在 CS 发送
        WAITING_RESPONSE,     ///< 主机正在 poll/读响应
        SERVING_GD32_RING,    ///< 正在读 GD32 EVENT
    };

    /**
     * @brief 本端事务占用标记（v2）。
     *
     * DATA_READY（PB6）由 GD32 独占：低=有待读，高=队列空。
     * 本状态机只表示 ESP32 是否正占用 SPI 总线，避免与按铃读并发。
     */
    class SpiLinkState {
    public:
        /** 进入忙碌态。 */
        void enterBusy(HandshakeState hs) { m_hsState = hs; }

        /** 退出忙碌态，回到 IDLE。 */
        void exitBusy() { m_hsState = HandshakeState::IDLE; }

        /** 当前是否空闲（可接受新按铃下降沿）。 */
        bool isIdle() const { return m_hsState == HandshakeState::IDLE; }

        /** 当前握手状态。 */
        HandshakeState state() const { return m_hsState; }

        /** ISR 检测下降沿时调用：仅 IDLE 态接受按铃。 */
        bool acceptRing() const { return isIdle(); }

    private:
        volatile HandshakeState m_hsState = HandshakeState::IDLE;
    };

    SpiLinkState m_linkState;
    /** GD32 主动按铃标志：GPIO 下降沿 ISR 置位，仅 SPI worker 消费。 */
    volatile bool m_gd32RequestPending = false;
    /** OTA 中继时为 true：worker 不 serviceRing，避免偷读 OTA ACK/WAIT。 */
    volatile bool m_suppressRingService = false;
    /** SPI worker 任务句柄（ISR 唯一通知目标）。 */
    TaskHandle_t m_spiTaskHandle = NULL;
    TaskHandle_t m_workerTask = NULL;
    QueueHandle_t m_jobQueue = NULL;
    /** 事件计数信号量（worker push / takeEvent pop）。 */
    SemaphoreHandle_t m_eventSem = NULL;

    // ── 多槽事件队列（worker 写入，业务 takeEvent 读出）──
    static constexpr uint8_t kEventSlots = 4U;
    struct EventSlot {
        uint8_t  type;
        uint16_t len;
        uint8_t  data[SPI_OTA_FRAME_MAX];
    };
    EventSlot m_eventQ[kEventSlots] = {};
    uint8_t m_eventHead = 0;
    uint8_t m_eventTail = 0;
    uint8_t m_eventCount = 0;

    // ── 作业提交（跨任务：堆 Job + binary done；worker 串行执行）──
    enum class JobKind : uint8_t {
        Transact = 0,
        Poll,
        Hello,
        EnterBoot,
        Ping,
        WaitBoot,
    };
    struct Job {
        JobKind kind = JobKind::Transact;
        uint32_t timeout_ms = 0;
        uint8_t type = 0;
        uint8_t expected_type = 0xFFU;
        uint8_t role_hint = 0;
        size_t payload_len = 0;
        uint8_t payload[SPI_OTA_FRAME_MAX] = {};
        uint8_t *out_type = nullptr;
        uint8_t *out_payload = nullptr;
        size_t out_max = 0;
        size_t *out_len = nullptr;
        uint32_t *out_uptime = nullptr;
        gd32_version_t *out_version = nullptr;
        esp_err_t result = ESP_FAIL;
        SemaphoreHandle_t done = nullptr;
        /** submitJob 堆分配的 Job：超时放弃后由 worker 释放，成功路径由提交方释放。 */
        bool heap_allocated = false;
        volatile bool finished = false;   ///< worker 已 execute + give
        volatile bool abandoned = false;  ///< 提交方已超时返回，worker 负责 free
    };
    static constexpr UBaseType_t kJobQueueLen = 6;

    bool onWorker() const;
    esp_err_t submitJob(Job &job);
    void executeJob(Job &job);
    /** @brief worker 完成堆 Job 后的生命周期收尾（give done + 可能 free）。 */
    void finishHeapJob(Job *job);
    /** @brief FreeRTOS 任务入口，转到 workerLoop。 */
    static void workerEntry(void *arg);
    /** @brief SPI worker 主循环：EVENT 按铃 + Job 队列。 */
    void workerLoop();
    /** @brief 单次服务 GD32 按铃读帧并入 EVENT 队列。 */
    esp_err_t serviceRingOnce();
    /** @brief EVENT 入队（队列满丢最旧）。 */
    bool pushEvent(uint8_t type, const uint8_t *payload, size_t len);
    /** @brief EVENT 出队（非阻塞）。 */
    bool popEvent(uint8_t *type, uint8_t *payload, size_t max_len, size_t *len);
    /**
     * @brief 解析并缓存 APP 身份 16B（HELLO_ACK 附加块 / PING body 同布局）。
     * @param id              身份块
     * @param out_version     可选输出版本
     * @param out_uptime_sec  可选输出 uptime 秒
     */
    void applyAppIdentity16(const uint8_t *id, gd32_version_t *out_version,
                            uint32_t *out_uptime_sec);

    // ── 帧内同步状态机 ──
    volatile uint8_t  m_localCredit = 8;  ///< 本端剩余可接收帧数（信用窗口）
    volatile uint8_t  m_remoteCredit = 8; ///< 对端上报的剩余信用

    // ── SPI Link v2 会话/序号状态（阶段1 就位，阶段2 启用校验）──
    volatile uint32_t m_sessionId = 0;       ///< 会话 ID（0 = 未建立，HELLO 时由 ESP32 生成）
    volatile uint8_t  m_txSeq = 0;           ///< 本端发送序号（每发一帧递增，重发不变）
    volatile uint8_t  m_lastSentSeq = 0;     ///< 上次发送的序号（用于响应归属匹配）
    volatile uint8_t  m_rxAckedSeq = 0;     ///< 已确认的对端最新序号
    volatile uint8_t  m_expectedRxSeq = 0;  ///< 期望收到的对端序号
    volatile uint8_t  m_gd32Role = 0;       ///< GD32 角色（0=unknown, 1=app, 2=boot）
    volatile bool     m_gd32FeatKnown = false; ///< HELLO/PING 身份是否带 feat_map
    volatile uint32_t m_gd32FeatFlags = 0;  ///< V2_FEAT_*
    volatile bool     m_gd32IdKnown = false;   ///< 是否已缓存版本号
    gd32_version_t    m_gd32Version = {};      ///< HELLO/PING 缓存的版本
    volatile bool     m_v2Active = false;   ///< v2 session 是否激活
    /** 最近一次 otaFindFrame 解析到的 RESPONSE 是否与 m_lastSentSeq 归属匹配 */
    volatile bool     m_lastRxAckMatched = false;
    SpiLinkStats m_stats = {};               ///< 链路统计

    // RAII 锁守卫
    class LockGuard {
    public:
        explicit LockGuard(SemaphoreHandle_t lock) : m_lock(lock) {
            if (m_lock) { xSemaphoreTake(m_lock, portMAX_DELAY); }
        }
        ~LockGuard() {
            if (m_lock) { xSemaphoreGive(m_lock); }
        }
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;
    private:
        SemaphoreHandle_t m_lock;
    };

    /** @brief GPIO 边沿中断回调（注册到 ESP-IDF GPIO ISR 服务）。 */
    static void IRAM_ATTR onReadyEdgeISR(void *arg);

    // 内部 SPI 传输
    /**
     * @brief 执行底层 SPI 时钟同步传输。
     * @param tx_buf 发送数据缓冲区。
     * @param tx_len 发送数据长度。
     * @param rx_buf 接收数据缓冲区。
     * @param rx_len 接收数据长度。
     * @param timeout_ms 超时时间（毫秒），0 表示无限等待。
     * @return ESP_OK 表示成功，否则返回错误码。
     */
    esp_err_t transferClocked(const uint8_t *tx_buf, size_t tx_len,
                              uint8_t *rx_buf, size_t rx_len,
                              uint32_t timeout_ms = 0U);

    /**
     * @brief 底层传输，支持 SPI 事务 flags（如 SPI_TRANS_CS_KEEP_ACTIVE）。
     * 多段同 CS 变长读时：先 spi_device_acquire_bus，段间 KEEP_ACTIVE，末段清 flag 后 release_bus。
     */
    esp_err_t transferClockedFlags(const uint8_t *tx_buf, size_t tx_len,
                                   uint8_t *rx_buf, size_t rx_len,
                                   uint32_t timeout_ms, uint32_t trans_flags);

    /**
     * @brief 读一帧响应：单 CS 固定时钟预算（BOOT=128 / APP=256），再 v2_parse 截真实长度。
     * 调用者须已持锁。不用两段 KEEP_ACTIVE（Boot 中途断 CS 会清 pending → 全 0xFF）。
     * @param out_frame  完整帧输出（含头+payload+CRC）
     * @param out_max    输出缓冲容量
     * @param out_len    实际帧长
     * @param timeout_ms 单次 SPI 超时
     */
    esp_err_t readV2FrameVarlen(uint8_t *out_frame, size_t out_max, size_t *out_len,
                                uint32_t timeout_ms);

    // 无锁内部版本（供已持锁的公共方法调用，避免递归死锁）
    /**
     * @brief 无锁版本的 OTA 响应轮询（调用者须已持有锁）。
     * @param resp_type 输出响应类型。
     * @param resp_payload 输出响应负载缓冲区。
     * @param resp_max 响应负载缓冲区最大长度。
     * @param resp_len 输出实际响应长度。
     * @param timeout_ms 超时时间（毫秒）。
     * @return ESP_OK 表示成功，否则返回错误码。
     */
    esp_err_t otaPollResponseUnlocked(uint8_t *resp_type, uint8_t *resp_payload,
                                      size_t resp_max, size_t *resp_len, uint32_t timeout_ms,
                                      uint8_t expected_type = 0xFFU);
    /** @brief 无锁版本的 otaTransfer（调用者须已持有锁）。
     *  @param expect_stale true 表示调用方预期读到过期/空响应（如 otaSendAndPoll 步骤1），
     *                      找不到帧时降级为 DBG 日志，不打 WARN。 */
    esp_err_t otaTransferUnlocked(uint8_t type, const uint8_t *payload, size_t payload_len,
                                  uint8_t *resp_type, uint8_t *resp_payload,
                                  size_t resp_max, size_t *resp_len, uint32_t timeout_ms,
                                  bool expect_stale = false);

    // OTA 帧工具（静态，无状态）
    static bool otaBuildFrame(uint8_t type, const uint8_t *payload, size_t payload_len,
                              uint8_t *out, size_t *out_len);
    static bool otaFindFrame(const uint8_t *buf, size_t buf_len,
                             uint8_t *type, const uint8_t **payload, size_t *payload_len,
                             size_t *frame_offset);
};

#endif // SPI_MASTER_H
