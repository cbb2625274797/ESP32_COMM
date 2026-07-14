/*
 * spi_master.h - SPI Master 驱动头文件
 * CANFD 3通道发送接口 + 底层 SPI 总线
 *
 * SpiMaster 类封装 SPI 总线初始化、CANFD 通信和 SPI OTA 协议。
 *
 * 握手架构（线与 SPIready，GPIO10<->GD32 PB6 开漏单线）：
 * - 空闲态线 HIGH（双方释放，靠上拉）。
 * - ESP32 主动发起：下拉 SPIready -> 立刻 CS 发请求 -> CS 释放后保持下拉，
 *   GD32 在 onCsRelease() ISR 内完成帧处理+响应装载后释放 SPIready（高），
 *   ESP32 GPIO 上升沿中断唤醒后发起 CS#2 读响应。
 * - GD32 主动按铃：下拉 SPIready -> ESP32 下降沿中断唤醒后发起读事务。
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

// SPI 配置常量
#define SPI_MASTER_CS_GPIO    (4)           // CS 片选引脚
#define SPI_CLOCK_HZ          CONFIG_DCU_SPI_CLOCK_HZ  // 默认 4MHz，可在 menuconfig 手动提速
#define SPI_MODE              (0)           // CPOL=0, CPHA=0
#define SPI_QUEUE_SIZE        (4)           ///< SPI 事务队列深度
#define SPI_READY_GPIO       (10)          // 线与 SPIready 握手引脚（开漏，GPIO10<->GD32 PB6）
#define SPI_READY_TIMEOUT_MS (200)         // 等待 SPIready 上升沿超时（毫秒）
#define SPI_OTA_FRAME_MAX     (256)          ///< SPI 同步帧最大字节数

// GD32 SPI 同步帧类型
#define SPI_OTA_CMD_REQ       (0x01)
#define SPI_OTA_CMD_DATA      (0x02)
#define SPI_APP_ENTER_BOOT_CMD (0xB0)
#define SPI_APP_ENTER_BOOT_ACK_OK (0xB5)  // GD32 APP 固件须同步修改 ACK 字节为 0xB5
#define SPI_OTA_RSP_ACK       (0x80)
#define SPI_OTA_RSP_WAIT      (0x81)
#define SPI_OTA_RSP_ERROR     (0x82)

// CANFD 类型已迁移至 transport/canfd_bridge.h（M4 解耦）

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

    /**
     * @brief OTA 中继期间禁止 worker 按铃读。
     *
     * PB6 在 OTA 下也会因 WAIT/ACK 拉低。若 worker 在 job 间隙把 RESPONSE
     * 当 EVENT 偷读，业务 poll 只能空等 → 约 10s 后 DATA 重发 → GD32 DUP-SEQ，
     * 表现为刷写断断续续。中继开始设 true，结束/复位设 false。
     */
    void setSuppressRingService(bool suppress);

    /**
     * @brief OTA 业务层确认当前请求已成功（结构化 ACK 校验通过）。
     * 在 SPI 头 ack_seq 偶发失配但仍接受业务 ACK 时补齐 m_txSeq，避免后续包卡死。
     */
    void confirmTxSeqAccepted();

    /**
     * @brief L4 统一请求入口：经 SPI worker 串行执行（send+poll）。
     */
    esp_err_t transact(uint8_t type, const uint8_t *payload, size_t payload_len,
                       uint8_t *resp_type, uint8_t *resp_payload,
                       size_t resp_max, size_t *resp_len, uint32_t timeout_ms);

    /**
     * @brief L4 取出 GD32 EVENT 帧（worker 已读入事件队列）。
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

    // ── CANFD 接口已迁移至 transport/canfd_bridge.h（M4 解耦）──

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

    // ── 线与 SPIready 握手状态机 ──
    /** 握手状态机枚举。 */
    enum class HandshakeState : uint8_t {
        IDLE = 0,             ///< 空闲，线 HIGH
        MASTER_SENDING,       ///< ESP32 已下拉 SPIready，正在 CS#1 发送请求
        WAITING_RESPONSE,     ///< CS#1 完成，等待 GD32 释放 SPIready（上升沿唤醒）
        SERVING_GD32_RING,    ///< 响应 GD32 按铃，正在读 GD32 请求帧
    };

    /**
     * @brief SPI 链路状态机：封装握手状态 + 完成信号逻辑。
     *
     * 线状态语义（对齐 OTA）：
     * - 线 HIGH = 空闲（双方释放），可发起新事务
     * - 线 LOW = 忙碌（某方下拉占用），禁止发起新事务
     *
     * ESP32 作为主动方（transact/appPing）：enterBusy() 占用线 → transferClocked → exitBusy() 释放线
     * ESP32 作为按铃响应方（takeEvent）：同上
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

    /** GD32 按铃请求帧缓冲区（兼容 takeRingRx 单槽）。 */
    uint8_t m_ringRxBuf[SPI_OTA_FRAME_MAX] = {};
    size_t  m_ringRxLen = 0;
    uint8_t m_ringRxType = 0;

    // ── L4 多槽事件队列（worker 写入，业务 takeEvent 读出）──
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

    // ── L4 作业提交（调用方栈上 Job + StaticSemaphore，worker 执行）──
    enum class JobKind : uint8_t {
        Transact = 0,
        Poll,
        Hello,
        EnterBoot,
        Ping,
        WaitBoot,
        ServiceRing,
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
    };
    static constexpr UBaseType_t kJobQueueLen = 6;

    bool onWorker() const;
    esp_err_t submitJob(Job &job);
    void executeJob(Job &job);
    static void workerEntry(void *arg);
    void workerLoop();
    esp_err_t serviceRingOnce();
    bool pushEvent(uint8_t type, const uint8_t *payload, size_t len);
    bool popEvent(uint8_t *type, uint8_t *payload, size_t max_len, size_t *len);

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

    /**
     * @brief 等 GD32 释放 SPIready（上升沿），中断驱动非忙等。
     *
     * 调用前须将 m_linkState 置为 WAITING_RESPONSE（enterBusy），ISR 上升沿检测到该状态
     * 后发 0x02 通知唤醒本任务。GD32 写 Flash 期间保持 PB6 低，本函数阻塞，
     * 写完拉高 → 唤醒 → 继续，自然避免 overrun。
     *
     * @param timeout_ms 超时(毫秒)，0 表示不等待。
     * @return ESP_OK 线已高；ESP_ERR_TIMEOUT 超时。
     */
    esp_err_t waitSpiReady(uint32_t timeout_ms);

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
     * @brief L3 变长读：同 CS 先读 16B Header，再按 LEN 读 payload+CRC。
     * 调用者须已持锁；仅在 DATA_READY 已低（有数据）时调用，禁止盲读。
     * @param out_frame  完整帧输出（含头+payload+CRC）
     * @param out_max    输出缓冲容量
     * @param out_len    实际帧长
     * @param timeout_ms 单段 SPI 超时
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

    /** @brief 带重试退避的事务（SPEC §9.2）。
     *  同序号重试最多 3 次，退避 10/30/90ms；
     *  3 次失败后 HELLO 重同步，再试 1 次。
     *  调用者须已持有锁。 */
    esp_err_t transactWithRetry(uint8_t type, const uint8_t *payload, size_t payload_len,
                                 uint8_t *resp_type, uint8_t *resp_payload,
                                 size_t resp_max, size_t *resp_len, uint32_t timeout_ms);
    // OTA 帧工具（静态，无状态）
    static bool otaBuildFrame(uint8_t type, const uint8_t *payload, size_t payload_len,
                              uint8_t *out, size_t *out_len);
    static bool otaFindFrame(const uint8_t *buf, size_t buf_len,
                             uint8_t *type, const uint8_t **payload, size_t *payload_len,
                             size_t *frame_offset);
};

#endif // SPI_MASTER_H
