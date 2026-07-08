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

// SPI 配置常量
#define SPI_MASTER_CS_GPIO    (4)           // CS 片选引脚
#define SPI_CLOCK_HZ          CONFIG_DCU_SPI_CLOCK_HZ  // 默认 4MHz，可在 menuconfig 手动提速
#define SPI_MODE              (0)           // CPOL=0, CPHA=0
#define SPI_QUEUE_SIZE        (4)           ///< SPI 事务队列深度
#define SPI_READY_GPIO       (10)          // 线与 SPIready 握手引脚（开漏，GPIO10<->GD32 PB6）
#define SPI_READY_TIMEOUT_MS (200)         // 等待 SPIready 上升沿超时（毫秒）
#define SPI_OTA_FRAME_MAX     (256)          ///< SPI 同步帧最大字节数
// ESP32 下拉 SPIready 后到拉 CS 之间的微秒延时（零延时起步，按需测算调大）
#define SPI_READY_DRIVE_DELAY_US (0)

// GD32 SPI 同步帧类型
#define SPI_SYNC_SOF0         (0xA5)
#define SPI_SYNC_SOF1         (0x5A)
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

    /** @brief 按 GD32 SPI OTA 最小协议执行一次请求 + 响应读取。 */
    esp_err_t otaTransfer(uint8_t type, const uint8_t *payload, size_t payload_len,
                          uint8_t *resp_type, uint8_t *resp_payload,
                          size_t resp_max, size_t *resp_len, uint32_t timeout_ms);

    /** @brief 仅执行 SPI OTA 响应读取事务，不重复发送请求帧。 */
    esp_err_t otaPollResponse(uint8_t *resp_type, uint8_t *resp_payload,
                              size_t resp_max, size_t *resp_len, uint32_t timeout_ms);

    /** @brief ESP32→GD32 发送命令 + 轮询读取真实响应（两步法，原子持锁）。
     *
     * 解决 GD32 "延迟一拍"协议：otaTransfer 首笔 CS 读到过期响应，
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

    /** @brief 通知 GD32 DCU_APP 设置 OTA 标志并软复位进入 Bootloader。 */
    esp_err_t appRequestBootloader();

    /** @brief 方案A: 通过 PB6 LOW→HIGH 序列等待 Bootloader 就绪。
     * Phase1 轮询等待 LOW (APP 复位前拉低 / Bootloader 极早拉低)；
     * Phase2 轮询等待 HIGH 并确认稳定 10ms (Bootloader init 完成释放)。
     * 超时返回 ESP_ERR_TIMEOUT，调用方应退化为固定延时。 */
    esp_err_t waitForBootloaderReady(uint32_t timeout_ms);

    /** @brief 向 GD32 APP 发送存活探测命令，等待 APP 回复 ALIVE ACK。 */
    esp_err_t appPing(uint32_t timeout_ms, uint32_t *uptime_sec, gd32_version_t *version = nullptr);

    // ── CANFD 接口已迁移至 transport/canfd_bridge.h（M4 解耦）──

    /**
     * @brief 检查 GD32 是否有主动按铃请求，若有则发起读事务读取请求帧。
     *
     * GD32 下拉 SPIready 表示「我有请求帧要给你读」。此函数检测到该状态后
     * 发起一次 SPI 读事务（发 dummy 字节，读回 GD32 准备的请求帧）。
     * 读取成功后按帧 TYPE 交给上层处理（如 CAN feedback 轮询、重启请求等）。
     *
     * @param timeout_ms 等待 GD32 请求的超时（毫秒），0 表示不等待仅检查。
     * @return ESP_OK 成功读到请求帧；ESP_ERR_NOT_FOUND 当前无请求；其余为错误码。
     *
     * @note 应在主循环或 OTA 空闲时周期调用，以响应 GD32 主动上报。
     */
    esp_err_t checkGd32Request(uint32_t timeout_ms = 0U);

    /**
     * @brief 取出最近一次 GD32 按铃请求帧（线程安全，取出后清空内部缓冲）。
     *
     * checkGd32Request() 成功读取一帧后将其 type/payload 存入内部缓冲，
     * 上层调用本函数取出并消费。取出后缓冲清空，避免重复消费。
     *
     * @param[out] type    请求帧 TYPE（如 0xC1）。
     * @param[out] payload 输出 payload 拷贝缓冲。
     * @param[in]  max_len payload 缓冲最大长度。
     * @param[out] len     实际 payload 长度。
     * @return true 表示有数据被取出；false 表示当前无数据。
     */
    bool takeRingRx(uint8_t *type, uint8_t *payload, size_t max_len, size_t *len);

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
        WAITING_BOOT_READY,   ///< 方案A: 等待 Bootloader 就绪（LOW→HIGH 序列）
    };

    /**
     * @brief SPI 链路状态机：封装握手状态 + 完成信号逻辑。
     *
     * 线状态语义（对齐 OTA）：
     * - 线 HIGH = 空闲（双方释放），可发起新事务
     * - 线 LOW = 忙碌（某方下拉占用），禁止发起新事务
     *
     * ESP32 作为主动方（otaTransfer/appPing）：enterBusy() 占用线 → transferClocked → exitBusy() 释放线
     * ESP32 作为按铃响应方（checkGd32Request）：同上，releaseReady 上升沿通知 GD32 完成
     */
    class SpiLinkState {
    public:
        /** 进入忙碌态（driveReadyLow 前/后调用）。 */
        void enterBusy(HandshakeState hs) { m_hsState = hs; }

        /** 退出忙碌态（releaseReady 后调用），回到 IDLE。 */
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
    /** GD32 主动按铃标志：GPIO 下降沿 ISR 置位，SPI 任务消费。 */
    volatile bool m_gd32RequestPending = false;
    /** SPI 任务句柄（用于 ISR 唤醒）。 */
    TaskHandle_t m_spiTaskHandle = NULL;
    /** GD32 按铃请求帧缓冲区。 */
    uint8_t m_ringRxBuf[SPI_OTA_FRAME_MAX] = {};
    size_t  m_ringRxLen = 0;
    uint8_t m_ringRxType = 0;   ///< GD32 按铃请求帧 TYPE（checkGd32Request 填充）

    // ── 帧内同步状态机 ──
    volatile uint8_t  m_localCredit = 8;  ///< 本端剩余可接收帧数（信用窗口）
    volatile uint8_t  m_remoteCredit = 8; ///< 对端上报的剩余信用

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

    /** @brief 下拉 SPIready（开漏驱动低）。 */
    void driveReadyLow();
    /** @brief 释放 SPIready（开漏高阻，靠上拉拉高）。 */
    void releaseReady();
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
                                      size_t resp_max, size_t *resp_len, uint32_t timeout_ms);
    /** @brief 无锁版本的 otaTransfer（调用者须已持有锁）。
     *  @param expect_stale true 表示调用方预期读到过期/空响应（如 otaSendAndPoll 步骤1），
     *                      找不到帧时降级为 DBG 日志，不打 WARN。 */
    esp_err_t otaTransferUnlocked(uint8_t type, const uint8_t *payload, size_t payload_len,
                                  uint8_t *resp_type, uint8_t *resp_payload,
                                  size_t resp_max, size_t *resp_len, uint32_t timeout_ms,
                                  bool expect_stale = false);
    // OTA 帧工具（静态，无状态）
    static uint32_t otaCrc32(const uint8_t *data, size_t len);
    static bool otaBuildFrame(uint8_t type, const uint8_t *payload, size_t payload_len,
                              uint8_t *out, size_t *out_len);
    static bool otaFindFrame(const uint8_t *buf, size_t buf_len,
                             uint8_t *type, const uint8_t **payload, size_t *payload_len,
                             size_t *frame_offset);
};

#endif // SPI_MASTER_H
