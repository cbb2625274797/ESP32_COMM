/*
 * spi_master.cpp - SPI Master 驱动实现
 * T1.1 - T1.4
 * ESP-IDF 6.0 适配版本
 * CANFD 3通道发送接口
 *
 * 所有功能封装为 SpiMaster 类方法。
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/spi_struct.h"
#include "esp_err.h"
#include "esp_log.h"
#include "rom/ets_sys.h"  // esp_rom_delay_us (ets_delay_us)
#include "esp_random.h"   // esp_random() for HELLO session_id
#include "esp_intr_alloc.h"  // ESP_INTR_FLAG_IRAM

#include "spi_master.h"
#include "spi_link_v2.h"
#include "monitor/debug_log.h"


static constexpr size_t kSpiTransferMaxBytes = 256U;

/**
 * @brief 计算本笔 CS 实际时钟字节数。
 * 不再固定打满 256B：GD32 BOOT 旧 RBNE 路径会 RX-OVR；即使 RX-DMA 后也浪费带宽。
 * 取 max(帧长, 最小响应窗口)，上限 SPI_OTA_FRAME_MAX。
 */
static size_t spiXferBytes(size_t frame_len)
{
    /* HELLO_ACK ~26B；OTA data ~84B。与 BOOT kTxBufSize=128 对齐，默认 128。
     * 过短会截断响应；过长（256）曾导致从机 RX-OVR。 */
    size_t n = frame_len;
    if (n < 128U) {
        n = 128U;
    }
    if (n > SPI_OTA_FRAME_MAX) {
        n = SPI_OTA_FRAME_MAX;
    }
    return n;
}

/** @brief ISR-safe GPIO 电平读取（不调用 Flash 中的 gpio_get_level）。
 *  用于 SPIready 边沿中断，避免在 cache 被禁用期间访问 Flash 代码。
 */
static int IRAM_ATTR readReadyLevelRaw(void)
{
    return (REG_READ(GPIO_IN_REG) >> SPI_READY_GPIO) & 0x1;
}

// ============================================================
// 单例
// ============================================================
/**
 * @brief 获取 SpiMaster 单例实例（单例模式）。
 * @return SpiMaster 单例引用。
 */
SpiMaster& SpiMaster::instance()
{
    static SpiMaster inst;
    return inst;
}

/**
 * @brief 下拉 SPIready（v2 DATA_READY 模式下 ESP32 不再操作 PB6）。
 *        保留函数体兼容调用方，实际为空操作。
 */
void SpiMaster::driveReadyLow()
{
    /* v2 DATA_READY: PB6 仅 GD32 下拉，ESP32 GPIO10 只输入。 */
}

/**
 * @brief 释放 SPIready（v2 DATA_READY 模式下空操作）。
 */
void SpiMaster::releaseReady()
{
    /* v2 DATA_READY: ESP32 不操作 PB6。 */
}

/**
 * @brief GPIO 边沿中断回调（SPIready 线跳变）。
 *
 * - 下降沿：GD32 主动按铃（请求 ESP32 来读）。若当前空闲则置
 *   m_gd32RequestPending 并唤醒 SPI 任务。
 * - 上升沿：Bootloader init 完成（释放 SPIready），唤醒等待
 *   WAITING_BOOT_READY 的任务。
 *
 * ISR 内仅做标志翻转 + 任务通知，不做 SPI 传输。
 */
void IRAM_ATTR SpiMaster::onReadyEdgeISR(void *arg)
{
    SpiMaster *self = static_cast<SpiMaster *>(arg);
    if (!self) { return; }
    int level = readReadyLevelRaw();
    if (level == 0) {
        /* 下降沿 */
        if (self->m_linkState.acceptRing()) {
            /* GD32 按铃：仅 IDLE 态接受 */
            self->m_gd32RequestPending = true;
            if (self->m_spiTaskHandle) {
                BaseType_t hp = pdFALSE;
                xTaskNotifyFromISR(self->m_spiTaskHandle, 0x01U,
                                   eSetBits, &hp);
                portYIELD_FROM_ISR(hp);
            }
        } else if (self->m_linkState.state() == HandshakeState::WAITING_BOOT_READY) {
            /* 方案A: Bootloader 已接管（APP 拉低或 BOOT 极早拉低） */
            if (self->m_spiTaskHandle) {
                BaseType_t hp = pdFALSE;
                xTaskNotifyFromISR(self->m_spiTaskHandle, 0x04U,
                                   eSetBits, &hp);
                portYIELD_FROM_ISR(hp);
            }
        }
    } else {
        /* 上升沿 */
        if (self->m_linkState.state() == HandshakeState::WAITING_RESPONSE) {
            /* GD32 释放 SPIready，可发起新 CS */
            if (self->m_spiTaskHandle) {
                BaseType_t hp = pdFALSE;
                xTaskNotifyFromISR(self->m_spiTaskHandle, 0x02U,
                                   eSetBits, &hp);
                portYIELD_FROM_ISR(hp);
            }
        } else if (self->m_linkState.state() == HandshakeState::WAITING_BOOT_READY) {
            /* 方案A: Bootloader init 完成，释放 SPIready */
            if (self->m_spiTaskHandle) {
                BaseType_t hp = pdFALSE;
                xTaskNotifyFromISR(self->m_spiTaskHandle, 0x08U,
                                   eSetBits, &hp);
                portYIELD_FROM_ISR(hp);
            }
        }
    }
}

/**
 * @brief 等 SPIready 适合发起下一笔主机事务（v2 DATA_READY 语义）。
 *
 * v2：PB6 低 = GD32 有待读数据；高 = 队列空/空闲。
 * - 发新命令前：若 PB6 已高，立即返回；若仍低，可能是未读走的响应，
 *   短暂等待其升高（BOOT 交付后应拉高）。超时不阻塞——CS 本身互斥。
 * - 注意：poll 读响应路径不应调用本函数等 HIGH；应等 LOW 或直接读。
 */
esp_err_t SpiMaster::waitSpiReady(uint32_t timeout_ms)
{
    if (timeout_ms == 0U) { return ESP_OK; }

    /* 已高，立即返回 */
    if (readReadyLevelRaw() == 1) { return ESP_OK; }

    /* PB6 低：可能有未读响应。短等升高；BOOT 修好交付清 pending 后应很快。
     * 超时仍 OK，避免卡死（随后 CS 本身可互斥推进）。 */
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        if (readReadyLevelRaw() == 1) { return ESP_OK; }
        vTaskDelay(pdMS_TO_TICKS(1) > 0 ? pdMS_TO_TICKS(1) : 1);
    }
    return ESP_OK;
}

/**
 * @brief 等 GD32 拉低 PB6（有待读响应）。
 * v2 DATA_READY：低 = 有数据，与 waitSpiReady（等高）语义相反。
 * @return ESP_OK 已低；ESP_ERR_TIMEOUT 超时仍为高（调用方可再决定是否盲 poll）。
 */
static esp_err_t waitSpiDataReady(uint32_t timeout_ms)
{
    if (readReadyLevelRaw() == 0) { return ESP_OK; }  /* 已低 = 有数据 */
    if (timeout_ms == 0U) { return ESP_ERR_TIMEOUT; }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        if (readReadyLevelRaw() == 0) { return ESP_OK; }
        vTaskDelay(pdMS_TO_TICKS(1) > 0 ? pdMS_TO_TICKS(1) : 1);
    }
    return ESP_ERR_TIMEOUT;
}

// ============================================================
// 内部 SPI 传输
// ============================================================
/**
 * @brief 执行底层 SPI 时钟同步传输。
 * @param tx_buf 发送数据缓冲区，可为 NULL。
 * @param tx_len 发送数据长度。
 * @param rx_buf 接收数据缓冲区，可为 NULL。
 * @param rx_len 接收数据长度。
 * @param timeout_ms 超时时间（毫秒），0 表示无限等待。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::transferClocked(const uint8_t *tx_buf, size_t tx_len,
                                     uint8_t *rx_buf, size_t rx_len,
                                     uint32_t timeout_ms)
{
    if (m_spiDevice == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if ((tx_len > 0U && tx_buf == NULL) || (rx_len > 0U && rx_buf == NULL)) {
        DEBUG_WARN("SPI", "transferClocked INVALID_ARG: tx_len=%u tx_buf=%p rx_len=%u rx_buf=%p dev=%p",
                   (unsigned)tx_len, tx_buf, (unsigned)rx_len, rx_buf, m_spiDevice);
        return ESP_ERR_INVALID_ARG;
    }

    const size_t total_len = (tx_len > rx_len) ? tx_len : rx_len;
    if (total_len == 0U) {
        return ESP_OK;
    }
    if (total_len > kSpiTransferMaxBytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t tx_staging[kSpiTransferMaxBytes];
    uint8_t rx_staging[kSpiTransferMaxBytes];
    const uint8_t *tx_ptr = tx_buf;
    uint8_t *rx_ptr = rx_buf;

    if (tx_len < total_len) {
        memset(tx_staging, 0xFF, total_len);
        if (tx_buf != NULL && tx_len > 0U) {
            memcpy(tx_staging, tx_buf, tx_len);
        }
        tx_ptr = tx_staging;
    }

    if (rx_len > 0U && rx_len < total_len) {
        rx_ptr = rx_staging;
    }

    spi_transaction_t t = {};
    t.length = total_len * 8U;
    t.rxlength = (rx_len > 0U) ? (total_len * 8U) : 0U;
    t.tx_buffer = tx_ptr;
    t.rx_buffer = (rx_len > 0U) ? rx_ptr : NULL;

    /* timeout handled via spi_device_get_trans_result below */
    esp_err_t ret = spi_device_queue_trans(m_spiDevice, &t, portMAX_DELAY)  /* always wait for queue slot */;
    if (ret != ESP_OK) {
        DEBUG_WARN("SPI", "spi_device_queue_trans fail: %s len=%u",
                   esp_err_to_name(ret), (unsigned)(total_len * 8U));
        return ret;
    }

    spi_transaction_t *ret_trans = NULL;
    ret = spi_device_get_trans_result(m_spiDevice, &ret_trans, pdMS_TO_TICKS(500))  /* DMA completion timeout */;
    if (ret != ESP_OK) {
        /* Queue timeout: transaction may still be in DMA. Must retrieve with
         * infinite wait, otherwise ret_queue residual blocks future queue_trans. */
        DEBUG_WARN("SPI", "spi_device_get_trans_result timeout: %s len=%u, retrying",
                   esp_err_to_name(ret), (unsigned)(total_len * 8U));
        ret = spi_device_get_trans_result(m_spiDevice, &ret_trans, portMAX_DELAY);
        if (ret != ESP_OK) {
            DEBUG_ERR("SPI", "spi_device_get_trans_result fail with infinite wait: %s",
                      esp_err_to_name(ret));
            return ret;
        }
        (void)ret_trans;
        return ESP_ERR_TIMEOUT;
    }
    (void)ret_trans;

    if (rx_len > 0U && rx_ptr != rx_buf) {
        memcpy(rx_buf, rx_ptr, rx_len);
    }
    return ESP_OK;
}

// ============================================================
// OTA 帧工具
// ============================================================
/**
 * @brief 计算 CRC32（MPEG-2，与 GD32 三端一致）。
 *        委托给 v2_crc32，算法完全相同。
 */
uint32_t SpiMaster::otaCrc32(const uint8_t *data, size_t len)
{
    return v2_crc32(data, len);
}

/* v1 同步帧头已升级为 v2 16B 头（见 spi_link_v2.h）。
 * 旧 6B 头常量保留为兼容注释，不再用于构建/解析。 */
static constexpr size_t kSyncHeaderLen = V2_HEADER_LEN;  /* 16 */
static constexpr size_t kSyncCrcLen     = V2_CRC_LEN;    /* 4 */
static constexpr size_t kSyncOverhead   = V2_OVERHEAD;   /* 20 */

/* v1 LE 工具已迁移至 spi_link_v2.h 的 v2_put/v2_get 内联函数。 */

/*
 * v2 shim：旧 otaBuildFrame 接口委托给 v2_build_frame。
 * 使用当前 session/seq 成员填充 v2 帧头，FLAGS=REQUEST。
 * 记录 m_lastSentSeq 供响应归属匹配。
 */
bool SpiMaster::otaBuildFrame(uint8_t type, const uint8_t *payload, size_t payload_len,
                              uint8_t *out, size_t *out_len)
{
    if (!out || !out_len) return false;
    if (payload_len > 0U && payload == NULL) return false;

    SpiMaster &self = SpiMaster::instance();
    const uint8_t flags = V2_FLAG_REQUEST;
    const uint8_t tx_seq = self.m_txSeq;
    const uint8_t ack_seq = self.m_rxAckedSeq;
    const uint8_t credit = self.m_localCredit;
    self.m_lastSentSeq = tx_seq;  /* 记录发送序号供响应归属匹配 */

    return v2_build_frame(type, flags, self.m_sessionId,
                         tx_seq, ack_seq, credit,
                         payload, payload_len, out, out_len);
}

/*
 * v2 shim：旧 otaFindFrame 接口委托给 v2_parse_frame + SESSION/SEQ 校验。
 * ESP32 是主机，收到 GD32 的 RESPONSE/EVENT 帧：
 *   - ack_seq 携带"GD32 确认收到的 ESP32 请求序号"，用于响应归属匹配
 *   - tx_seq 是 GD32 的发送序号
 */
bool SpiMaster::otaFindFrame(const uint8_t *buf, size_t buf_len,
                             uint8_t *type, const uint8_t **payload, size_t *payload_len,
                             size_t *frame_offset)
{
    if (!buf) return false;

    v2_header_t hdr;
    const uint8_t *pload = nullptr;
    if (!v2_parse_frame(buf, buf_len, &hdr, &pload)) {
        return false;
    }

    SpiMaster &self = SpiMaster::instance();

    /* 更新对端 credit */
    self.m_remoteCredit = hdr.credit;

    if (self.m_v2Active) {
        /* SESSION_ID 校验 */
        if (hdr.session_id != self.m_sessionId) {
            self.m_stats.seq_errors++;
            const bool is_event = (hdr.flags & V2_FLAG_EVENT) != 0U;
            /* EVENT 按铃帧在 GD32 session 未激活时 session_id=0（见 requestEsp32Read）。
             * 若此处直接拒绝：按铃被读走却解析失败 → 狂刷 WARN，CAN 反馈全丢。
             * 策略：EVENT 一律放行；REQUEST/RESPONSE 仍拒绝错 session。 */
            if (!is_event) {
                static uint32_t s_sess_mm_logs = 0U;
                if ((++s_sess_mm_logs % 32U) == 1U) {
                    DEBUG_WARN("SPI", "v2 SESSION mismatch: got=0x%08lX expected=0x%08lX type=0x%02X (log#%lu)",
                               (unsigned long)hdr.session_id, (unsigned long)self.m_sessionId,
                               hdr.type, (unsigned long)s_sess_mm_logs);
                }
                return false;
            }
            static uint32_t s_evt_sess0_logs = 0U;
            if ((++s_evt_sess0_logs % 64U) == 1U) {
                DEBUG_DBG("SPI", "EVENT session mismatch accepted: got=0x%08lX exp=0x%08lX type=0x%02X",
                          (unsigned long)hdr.session_id, (unsigned long)self.m_sessionId, hdr.type);
            }
        }

        /* 响应归属匹配：ack_seq 应等于 ESP32 刚发的 m_lastSentSeq。
         * 兼容旧 GD32 固件：业务响应曾写死 ack_seq=0。在 session 已匹配时
         * 接受 ack_seq==0，否则无法在“只刷 ESP32+BOOT、APP 仍旧”时完成 ENTER_BOOT。 */
        if (hdr.flags & V2_FLAG_RESPONSE) {
            const bool ack_match = (hdr.ack_seq == self.m_lastSentSeq);
            const bool legacy_zero_ack = (hdr.ack_seq == 0U);
            if (!ack_match && !legacy_zero_ack) {
                /* 延迟一拍下常见：上一帧 ACK 对不上当前 m_lastSentSeq。
                 * 属协议常态，不刷 WARN（前端会当警告刷屏）；仅 DBG 且限频。 */
                static uint32_t s_stale_logs = 0U;
                if ((++s_stale_logs % 64U) == 1U) {
                    DEBUG_DBG("SPI", "v2 stale response: type=0x%02X ack_seq=%u expected=%u (log#%lu)",
                              hdr.type, hdr.ack_seq, self.m_lastSentSeq,
                              (unsigned long)s_stale_logs);
                }
                return false;
            }
            if (!ack_match && legacy_zero_ack) {
                DEBUG_DBG("SPI", "v2 legacy ack_seq=0 accepted: type=0x%02X expected=%u",
                          hdr.type, self.m_lastSentSeq);
            }
            /* 响应归属匹配成功，更新已确认序号 */
            self.m_rxAckedSeq = self.m_lastSentSeq;
        }

        /* 更新期望的对端序号（EVENT 帧的 tx_seq） */
        if (hdr.flags & V2_FLAG_EVENT) {
            self.m_expectedRxSeq = hdr.tx_seq + 1U;
        }
    }

    if (type)         *type = hdr.type;
    if (payload)      *payload = pload;
    if (payload_len)  *payload_len = hdr.payload_len;
    if (frame_offset) *frame_offset = hdr.frame_offset;
    return true;
}

// ============================================================
// 初始化
// ============================================================
/**
 * @brief 初始化 SPI2 总线与目标从设备。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::init()
{
    // ★ 修复：先创建 mutex，再使用 LockGuard。
    //   旧代码在 LockGuard 构造时 m_mutex 仍为 NULL，
    //   虽然 LockGuard 会跳过 NULL take/give，但在极端情况下
    //   其他任务可能在 mutex 创建后立即尝试 take，产生竞态。
    if (!m_mutex) {
        m_mutex = xSemaphoreCreateMutex();
        if (m_mutex == NULL) {
            DEBUG_ERR("SPI", "SPI 互斥锁创建失败");
            return ESP_ERR_NO_MEM;
        }
    }
    LockGuard lock(m_mutex);

    DEBUG_INFO("SPI", "=== 初始化 SPI2 总线 ===");

    spi_bus_config_t bus_cfg = {};
    bus_cfg.miso_io_num = 7;
    bus_cfg.mosi_io_num = 5;
    bus_cfg.sclk_io_num = 6;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = kSpiTransferMaxBytes * SPI_QUEUE_SIZE;

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        DEBUG_ERR("SPI", "SPI 总线初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = SPI_CLOCK_HZ;
    dev_cfg.mode = SPI_MODE;
    dev_cfg.spics_io_num = SPI_MASTER_CS_GPIO;
    dev_cfg.queue_size = SPI_QUEUE_SIZE;
    dev_cfg.command_bits = 0;
    dev_cfg.address_bits = 0;
    dev_cfg.dummy_bits = 0;
    /* 加长 CS 建立时间，给 GD32 EXTI/预装 DMA 留余量（单位：SPI 半周期相关） */
    dev_cfg.cs_ena_posttrans = 32;
    dev_cfg.cs_ena_pretrans = 64;

    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &m_spiDevice);
    if (ret != ESP_OK) {
        DEBUG_ERR("SPI", "SPI 设备添加失败: %s", esp_err_to_name(ret));
        return ret;
    }

    DEBUG_INFO("SPI", "SPI2 总线初始化完成: MOSI=GPIO5, MISO=GPIO7, SCLK=GPIO6, CS=GPIO%d, CLK=%d Hz",
               SPI_MASTER_CS_GPIO, SPI_CLOCK_HZ);

    /* 配置 SPI Ready 握手引脚(GPIO10)，v2 DATA_READY 模式：仅输入。
     * PB6 仅由 GD32 下拉（有数据待发时拉低，队列空时释放高）。
     * ESP32 不再下拉 PB6，只检测边沿：下降沿=GD32 有数据按铃。 */
    gpio_config_t ready_cfg = {};
    ready_cfg.pin_bit_mask = (1ULL << SPI_READY_GPIO);
    ready_cfg.mode = GPIO_MODE_INPUT;             /* v2: 仅输入，不再开漏输出 */
    ready_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    ready_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ready_cfg.intr_type = GPIO_INTR_ANYEDGE;       /* 双边沿中断 */
    esp_err_t gpio_ret = gpio_config(&ready_cfg);
    if (gpio_ret != ESP_OK) {
        DEBUG_WARN("SPI", "SPI Ready 握手引脚 GPIO%d 配置失败: %s", SPI_READY_GPIO, esp_err_to_name(gpio_ret));
    } else {
        DEBUG_INFO("SPI", "SPI Ready 开漏握手已配置: GPIO%d (线与 GD32 PB6)", SPI_READY_GPIO);
    }

    /* 释放 SPIready（开漏高阻），确保空闲态线 HIGH */
    releaseReady();

    /* 安装 GPIO ISR 服务（全局仅安装一次）并注册边沿回调 */
    static bool s_isrInstalled = false;
    if (!s_isrInstalled) {
        gpio_ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        if (gpio_ret != ESP_OK && gpio_ret != ESP_ERR_INVALID_STATE) {
            DEBUG_WARN("SPI", "gpio_install_isr_service 失败: %s", esp_err_to_name(gpio_ret));
        } else {
            s_isrInstalled = true;
        }
    }
    if (s_isrInstalled) {
        gpio_ret = gpio_isr_handler_add((gpio_num_t)SPI_READY_GPIO,
                                        onReadyEdgeISR, this);
        if (gpio_ret != ESP_OK) {
            DEBUG_WARN("SPI", "gpio_isr_handler_add GPIO%d 失败: %s",
                       SPI_READY_GPIO, esp_err_to_name(gpio_ret));
        }
    }

    /* 记录当前任务句柄，供 GPIO ISR 唤醒 */
    m_spiTaskHandle = xTaskGetCurrentTaskHandle();
    m_linkState.exitBusy();
    m_gd32RequestPending = false;
    m_localCredit = 8;
    m_remoteCredit = 8;
    /* v2 链路状态初始化：session 未建立，序号从 0 开始 */
    m_sessionId = 0;
    m_txSeq = 0;
    m_lastSentSeq = 0;
    m_rxAckedSeq = 0;
    m_expectedRxSeq = 0;
    m_gd32Role = 0;
    m_v2Active = false;
    m_stats = SpiLinkStats{};

    return ESP_OK;
}

// ============================================================
// SPI OTA 协议
// ============================================================
/**
 * @brief 按 GD32 SPI OTA 最小协议执行一次请求加响应读取。
 * @param type 请求帧类型。
 * @param payload 负载数据缓冲区。
 * @param payload_len 负载数据长度。
 * @param resp_type 输出响应类型。
 * @param resp_payload 输出响应负载缓冲区。
 * @param resp_max 响应负载缓冲区最大长度。
 * @param resp_len 输出实际响应长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::otaTransfer(uint8_t type, const uint8_t *payload, size_t payload_len,
                                 uint8_t *resp_type, uint8_t *resp_payload,
                                 size_t resp_max, size_t *resp_len, uint32_t timeout_ms)
{
    /* 公开 otaTransfer 固定走两步法：GD32 延迟一拍，首笔 CS 读到的是旧响应。
     * 与 otaSendAndPoll 同语义，保证 OTA 握手/数据包调用方无需各自再拼 poll。 */
    return otaSendAndPoll(type, payload, payload_len,
                          resp_type, resp_payload, resp_max, resp_len, timeout_ms);
}

/**
 * @brief 无锁版本的 otaTransfer（调用者须已持有锁）。
 */
esp_err_t SpiMaster::otaTransferUnlocked(uint8_t type, const uint8_t *payload, size_t payload_len,
                                         uint8_t *resp_type, uint8_t *resp_payload,
                                         size_t resp_max, size_t *resp_len, uint32_t timeout_ms,
                                         bool expect_stale)
{
    uint8_t tx_frame[SPI_OTA_FRAME_MAX];
    size_t tx_len = 0;
    if (!otaBuildFrame(type, payload, payload_len, tx_frame, &tx_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    DEBUG_DBG("SPI", "OTA TX: type=0x%02X len=%u", type, (unsigned)tx_len);

    if (tx_len > SPI_OTA_FRAME_MAX) {
        DEBUG_ERR("SPI", "OTA frame too large: %u", (unsigned)tx_len);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 同步单 CS 传输：拉低 SPIready 后，在单次 SPI 事务中同时发送请求并读回响应。
     * 注意：GD32 BOOT 采用"延迟一拍"协议——响应在 onCsAssert() 装载 DMA，
     * queueTxFrame() 只缓冲不启动 DMA。因此本笔 CS 内读到的响应属于上一笔请求，
     * 首笔请求必读 0xFF。握手请求幂等可通过重试匹配；唯一性请求需配合
     * otaPollResponse() 读取延迟响应。详见 SpiOta.cpp queueTxFrame 注释。 */
    m_linkState.enterBusy(HandshakeState::WAITING_RESPONSE);
    /* 等 GD32 释放 PB6（上一笔 onCsRelease 处理完）再发起新 CS。
     * 中断驱动：GD32 拉高 → ISR 0x02 → 唤醒。GD32 写 Flash 期间
     * PB6 保持低，本函数阻塞直到写完，自然避免 overrun。
     * 0xB0 不走 otaTransfer，不受此等待影响。 */
    (void)waitSpiReady(SPI_READY_TIMEOUT_MS);
    m_linkState.enterBusy(HandshakeState::MASTER_SENDING);

    driveReadyLow();
    if (SPI_READY_DRIVE_DELAY_US > 0U) {
        esp_rom_delay_us(SPI_READY_DRIVE_DELAY_US);
    }

    const size_t xfer_len = spiXferBytes(tx_len);
    uint8_t tx_padded[SPI_OTA_FRAME_MAX];
    if (tx_len > sizeof(tx_padded) || xfer_len > sizeof(tx_padded)) {
        DEBUG_ERR("SPI", "OTA frame %u exceeds tx_padded %u", (unsigned)tx_len, (unsigned)sizeof(tx_padded));
        releaseReady();
        m_linkState.exitBusy();
        return ESP_ERR_INVALID_SIZE;
    }
    memset(tx_padded, 0xFF, xfer_len);
    memcpy(tx_padded, tx_frame, tx_len);

    uint8_t rx_raw[SPI_OTA_FRAME_MAX];
    memset(rx_raw, 0xFF, xfer_len);

    esp_err_t ret = transferClocked(tx_padded, xfer_len, rx_raw, xfer_len, timeout_ms);
    releaseReady();
    m_linkState.exitBusy();
    if (ret != ESP_OK) {
        DEBUG_WARN("SPI", "OTA sync transfer failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t found_type = 0;
    const uint8_t *found_payload = nullptr;
    size_t found_len = 0;
    size_t frame_offset = 0;
    if (otaFindFrame(rx_raw, xfer_len, &found_type, &found_payload, &found_len, &frame_offset)) {
        if (resp_type) *resp_type = found_type;
        if (resp_payload && resp_max > 0) {
            size_t copy_len = (found_len < resp_max) ? found_len : resp_max;
            if (found_payload && copy_len > 0) {
                memcpy(resp_payload, found_payload, copy_len);
            }
        }
        if (resp_len) *resp_len = found_len;
        /* 成功收到响应，递增发送序号（重发时调用方在调用前已保存原序号） */
        m_txSeq = (uint8_t)(m_txSeq + 1U);
        return ESP_OK;
    }

    if (expect_stale) {
        DEBUG_DBG("SPI", "OTA step1 stale response (expected, delayed-by-one)");
    } else {
        DEBUG_WARN("SPI", "OTA sync transfer response not found");
    }
    return ESP_ERR_INVALID_RESPONSE;
}
/**
 * @brief 仅执行 SPI OTA 响应读取事务，不重复发送请求帧。
 * @param resp_type 输出响应类型。
 * @param resp_payload 输出响应负载缓冲区。
 * @param resp_max 响应负载缓冲区最大长度。
 * @param resp_len 输出实际响应长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::otaPollResponse(uint8_t *resp_type, uint8_t *resp_payload,
                                     size_t resp_max, size_t *resp_len, uint32_t timeout_ms)
{
    LockGuard lock(m_mutex);
    return otaPollResponseUnlocked(resp_type, resp_payload, resp_max, resp_len, timeout_ms);
}

/**
 * @brief 无锁版本的 OTA 响应轮询实现。
 * @param resp_type 输出响应类型。
 * @param resp_payload 输出响应负载缓冲区。
 * @param resp_max 响应负载缓冲区最大长度。
 * @param resp_len 输出实际响应长度。
 * @param timeout_ms 超时时间（毫秒）。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::otaPollResponseUnlocked(uint8_t *resp_type, uint8_t *resp_payload,
                                             size_t resp_max, size_t *resp_len, uint32_t timeout_ms,
                                             uint8_t expected_type)
{
    constexpr size_t kPollBytes = 128U;  /* 与 spiXferBytes 默认窗口一致 */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint32_t attempt = 0;

    while (xTaskGetTickCount() <= deadline) {
        ++attempt;

        /* v2 DATA_READY：poll 是读响应。PB6 低 = 有待读数据，应立即 CS；
         * 不要 waitSpiReady(等 HIGH)。首轮多等一会，给 GD32 主循环 AES 时间。 */
        m_linkState.enterBusy(HandshakeState::WAITING_RESPONSE);
        (void)waitSpiDataReady(attempt == 1U ? 100U : 20U);

        uint8_t rx_raw[kPollBytes];
        uint8_t dummy[kPollBytes];
        memset(dummy, 0xFF, sizeof(dummy));

        TickType_t now = xTaskGetTickCount();
        uint32_t remain_ms = (deadline > now) ? ((deadline - now) * portTICK_PERIOD_MS) : 0U;
        if (remain_ms == 0U) break;
        uint32_t spi_timeout = (remain_ms > 500U) ? 500U : remain_ms;
        /* 与发送路径一致：先 driveReady 再 CS，避免 GD32 侧时序偶发丢响应 */
        driveReadyLow();
        esp_err_t ret = transferClocked(dummy, sizeof(dummy), rx_raw, sizeof(rx_raw), spi_timeout);
        releaseReady();
        if (ret != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        /* Check if all 0xFF (slave not driving MISO) */
        bool all_ff = true;
        for (size_t k = 0; k < 8 && k < sizeof(rx_raw); ++k) {
            if (rx_raw[k] != 0xFF) { all_ff = false; break; }
        }
        if (all_ff) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_DCU_OTA_POLL_BACKOFF_MS));
            continue;
        }

        uint8_t found_type = 0;
        const uint8_t *found_payload = nullptr;
        size_t found_len = 0;
        size_t frame_offset = 0;

        if (otaFindFrame(rx_raw, sizeof(rx_raw), &found_type, &found_payload, &found_len, &frame_offset)) {
            /* v2: 不再跳过 found_len==0 的 ACK/ERROR。
             * 延迟一拍协议下首次 poll 可能读到上一笔的过期响应，
             * 调用方通过 resp_type 和 payload 内容自行判断是否为期望响应。
             * WAIT 仍需继续轮询。 */

            /* 类型过滤：如果指定了 expected_type 且不匹配，丢弃继续轮询。
             * 避免消费掉不相关的帧（如 CAN feedback 事件帧被 PING poll 吃掉）。 */
            if (expected_type != 0xFFU && found_type != expected_type &&
                found_type != SPI_OTA_RSP_WAIT) {
                DEBUG_DBG("SPI", "poll type mismatch: got=0x%02X want=0x%02X, discarding",
                          found_type, expected_type);
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }

            if (resp_type) *resp_type = found_type;
            if (resp_payload && resp_max > 0) {
                size_t copy_len = (found_len < resp_max) ? found_len : resp_max;
                if (found_payload && copy_len > 0) {
                    memcpy(resp_payload, found_payload, copy_len);
                }
            }
            if (resp_len) *resp_len = found_len;

            if (found_type == SPI_OTA_RSP_WAIT) {
                vTaskDelay(pdMS_TO_TICKS(CONFIG_DCU_OTA_POLL_BACKOFF_MS));
                continue;
            }
            /* 真实响应匹配成功：推进发送序号。
             * expect_stale 的 step1 若读到 0xFF 不会自增 m_txSeq，
             * 必须在 poll 成功时补齐，避免下一命令复用同一 TX_SEQ。 */
            m_txSeq = (uint8_t)(m_lastSentSeq + 1U);
            m_linkState.exitBusy();
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }

    m_linkState.exitBusy();
    DEBUG_WARN("SPI", "OTA response timeout: %lu ms", (unsigned long)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief ESP32→GD32 发送命令 + 轮询读取真实响应（两步法，原子持锁）。
 *
 * 解决 GD32 "延迟一拍"协议：otaTransfer 首笔 CS 读到过期响应，
 * otaPollResponse 读取本笔命令的真实响应。命令只发送一次，不重发。
 * 适用于所有 ESP32→GD32 方向的命令（CAN 发送/模式切换/ID 快照等）。
 */
esp_err_t SpiMaster::otaSendAndPoll(uint8_t type, const uint8_t *payload, size_t payload_len,
                                    uint8_t *resp_type, uint8_t *resp_payload,
                                    size_t resp_max, size_t *resp_len, uint32_t timeout_ms)
{
    LockGuard lock(m_mutex);

    /* 步骤 1: 发送命令（忽略延迟一拍的过期响应）。
     * otaTransferUnlocked 可能返回:
     *   ESP_OK                    - 读到过期帧（正常，忽略）
     *   ESP_ERR_INVALID_RESPONSE  - 读到 0xFF（正常，延迟一拍）
     *   其他                      - SPI 真实错误 */
    const uint32_t send_timeout = (timeout_ms > 500U) ? 500U : timeout_ms;
    esp_err_t send_ret = otaTransferUnlocked(type, payload, payload_len,
                                             nullptr, nullptr, 0U, nullptr, send_timeout,
                                             /*expect_stale=*/true);
    if (send_ret != ESP_OK && send_ret != ESP_ERR_INVALID_RESPONSE) {
        return send_ret;
    }

    /* 等 GD32 完成 onCsRelease，并优先等 PB6 变低（响应/WAIT 已装载）。
     * OTA DATA 在 GD32 主循环做 AES，可能数 ms；不可固定短 delay 后盲 poll。 */
    {
        const TickType_t gap = (pdMS_TO_TICKS(1) > 0) ? pdMS_TO_TICKS(1) : 1;
        vTaskDelay(gap);
    }
    m_linkState.enterBusy(HandshakeState::WAITING_RESPONSE);
    const uint32_t ready_wait_ms = (timeout_ms > 800U) ? 300U : 80U;
    if (waitSpiDataReady(ready_wait_ms) != ESP_OK) {
        DEBUG_DBG("SPI", "DATA_READY 未拉低(%u ms)，继续 poll", (unsigned)ready_wait_ms);
    }
    m_linkState.exitBusy();

    /* 步骤 2: 轮询读取真实响应（dummy TX，不重发命令） */
    const uint32_t poll_timeout = (timeout_ms > send_timeout)
                                  ? (timeout_ms - send_timeout) : 500U;
    return otaPollResponseUnlocked(resp_type, resp_payload, resp_max, resp_len, poll_timeout,
                                    /*expected_type=*/0xFFU);
}

/**
 * @brief 带重试退避的事务（SPEC §9.2）。
 *
 * 同序号重试最多 3 次，退避 10/30/90ms；
 * 3 次失败后 HELLO 重同步，再试 1 次。
 * 调用者须已持有锁。
 */
esp_err_t SpiMaster::transactWithRetry(uint8_t type, const uint8_t *payload, size_t payload_len,
                                        uint8_t *resp_type, uint8_t *resp_payload,
                                        size_t resp_max, size_t *resp_len, uint32_t timeout_ms)
{
    /* 保存原始序号用于重发时恢复 */
    const uint8_t saved_tx_seq = m_txSeq;
    static constexpr uint32_t kBackoffMs[] = { 10U, 30U, 90U };
    static constexpr uint32_t kMaxRetries = 3U;

    for (uint32_t retry = 0U; retry <= kMaxRetries; ++retry) {
        if (retry > 0U) {
            const uint32_t backoff = kBackoffMs[(retry - 1U) < 3U ? (retry - 1U) : 2U];
            vTaskDelay(pdMS_TO_TICKS(backoff));
            m_stats.retries++;
            /* 重发时恢复原序号 */
            m_txSeq = saved_tx_seq;
        }

        /* 使用两步法（持锁已由调用者完成，用 Unlocked 版本） */
        const uint32_t send_timeout = (timeout_ms > 500U) ? 500U : timeout_ms;
        esp_err_t send_ret = otaTransferUnlocked(type, payload, payload_len,
                                                  nullptr, nullptr, 0U, nullptr, send_timeout,
                                                  /*expect_stale=*/true);
        if (send_ret != ESP_OK && send_ret != ESP_ERR_INVALID_RESPONSE) {
            if (retry < kMaxRetries) { continue; }
            return send_ret;
        }

        const uint32_t poll_timeout = (timeout_ms > send_timeout)
                                      ? (timeout_ms - send_timeout) : 500U;
        esp_err_t ret = otaPollResponseUnlocked(resp_type, resp_payload,
                                                 resp_max, resp_len, poll_timeout,
                                                 /*expected_type=*/0xFFU);
        if (ret == ESP_OK) {
            return ESP_OK;
        }

        if (ret == ESP_ERR_TIMEOUT) {
            m_stats.ready_timeouts++;
        }

        /* 3 次重试后尝试 HELLO 重同步（最后一次循环） */
        if (retry == kMaxRetries - 1U) {
            DEBUG_WARN("SPI", "transactWithRetry: %u 次失败, HELLO 重同步...", (unsigned)retry);
            m_v2Active = false;
            /* HELLO 需要锁——但调用者已持锁。
             * hello() 内部有 LockGuard，会递归死锁。
             * 因此用 hello 的核心逻辑但不加锁：两阶段（发 + dummy 收）。 */
            m_sessionId = esp_random() | 0x00000001U;
            m_txSeq = 0;
            m_lastSentSeq = 0;
            m_rxAckedSeq = 0;
            m_expectedRxSeq = 0;

            uint8_t hello_frame[V2_MAX_FRAME];
            size_t hello_len = 0;
            if (v2_build_hello_frame(m_sessionId, 0U, hello_frame, &hello_len)) {
                /* 同 session 连发 2 次 HELLO，覆盖延迟一拍（勿用紧贴 dummy） */
                for (uint32_t hi = 0U; hi < 2U; ++hi) {
                    m_linkState.enterBusy(HandshakeState::WAITING_RESPONSE);
                    (void)waitSpiReady(SPI_READY_TIMEOUT_MS);
                    m_linkState.enterBusy(HandshakeState::MASTER_SENDING);
                    driveReadyLow();

                    const size_t xfer_len = spiXferBytes(hello_len);
                    uint8_t tx_p[SPI_OTA_FRAME_MAX];
                    memset(tx_p, 0xFF, xfer_len);
                    memcpy(tx_p, hello_frame, hello_len);
                    uint8_t rx_r[SPI_OTA_FRAME_MAX];
                    memset(rx_r, 0xFF, xfer_len);
                    esp_err_t hr = transferClocked(tx_p, xfer_len, rx_r, xfer_len, 2000U);
                    releaseReady();
                    m_linkState.exitBusy();
                    if (hr != ESP_OK) {
                        continue;
                    }
                    v2_header_t hdr;
                    const uint8_t *pl = nullptr;
                    if (v2_parse_frame(rx_r, xfer_len, &hdr, &pl) &&
                        hdr.type == V2_TYPE_LINK_HELLO &&
                        (hdr.flags & V2_FLAG_RESPONSE) &&
                        hdr.payload_len >= 6U && pl &&
                        v2_get_u32le(&pl[0]) == m_sessionId) {
                        m_gd32Role = pl[4];
                        m_localCredit = pl[5];
                        m_v2Active = true;
                        m_stats.resyncs++;
                        DEBUG_INFO("SPI", "transactWithRetry: HELLO 重同步成功 role=%u", m_gd32Role);
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
        }
    }

    return ESP_ERR_TIMEOUT;
}


/**
 * @brief 通知 GD32 APP 设置 OTA 标志并软复位进入 Bootloader（v2 标准帧）。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::appRequestBootloader()
{
    LockGuard lock(m_mutex);
    DEBUG_INFO("SPI", "请求 GD32 APP 进入 Bootloader (v2)...");

    m_gd32RequestPending = false;

    /* 先空读 1～2 次，尽量吞掉残留 0xC1 按铃帧，避免盖住 0xB0 的延迟一拍响应。 */
    for (uint32_t d = 0U; d < 2U; ++d) {
        constexpr size_t kPoll = 128U;
        uint8_t rx_raw[kPoll];
        uint8_t dummy[kPoll];
        memset(dummy, 0xFF, sizeof(dummy));
        memset(rx_raw, 0xFF, sizeof(rx_raw));
        m_linkState.enterBusy(HandshakeState::WAITING_RESPONSE);
        driveReadyLow();
        (void)transferClocked(dummy, sizeof(dummy), rx_raw, sizeof(rx_raw), 50U);
        releaseReady();
        m_linkState.exitBusy();
        vTaskDelay((pdMS_TO_TICKS(5) > 0) ? pdMS_TO_TICKS(5) : 1);
    }

    /* 最多 3 次 0xB0。只认 type=0xB0 + status=0xB5 为真 ACK。
     * 勿把 session=0 的 0x80 当成功（常见为过期/错帧，导致“假 ack、真没进 BOOT”）。 */
    constexpr uint32_t kMaxSends = 3U;
    bool command_sent = false;
    bool got_ack = false;
    const TickType_t gap = (pdMS_TO_TICKS(10) > 0) ? pdMS_TO_TICKS(10) : 1;

    for (uint32_t retry = 0U; retry < kMaxSends; ++retry) {
        if (retry > 0U) {
            vTaskDelay(pdMS_TO_TICKS(50));
            /* 序号/会话错乱时重同步一次再发 0xB0 */
            if (retry == 1U) {
                m_v2Active = false;
                m_sessionId = esp_random() | 0x00000001U;
                m_txSeq = 0;
                m_lastSentSeq = 0;
                m_rxAckedSeq = 0;
                m_expectedRxSeq = 0;
                uint8_t hf[V2_MAX_FRAME];
                size_t hl = 0;
                if (v2_build_hello_frame(m_sessionId, V2_ROLE_APP, hf, &hl)) {
                    for (uint32_t hi = 0U; hi < 2U; ++hi) {
                        const size_t xlen = spiXferBytes(hl);
                        uint8_t tx_p[SPI_OTA_FRAME_MAX];
                        uint8_t rx_r[SPI_OTA_FRAME_MAX];
                        memset(tx_p, 0xFF, xlen);
                        memcpy(tx_p, hf, hl);
                        memset(rx_r, 0xFF, xlen);
                        m_linkState.enterBusy(HandshakeState::MASTER_SENDING);
                        driveReadyLow();
                        (void)transferClocked(tx_p, xlen, rx_r, xlen, 200U);
                        releaseReady();
                        m_linkState.exitBusy();
                        v2_header_t h;
                        const uint8_t *pl = nullptr;
                        if (v2_parse_frame(rx_r, xlen, &h, &pl) &&
                            h.type == V2_TYPE_LINK_HELLO && (h.flags & V2_FLAG_RESPONSE) &&
                            pl && h.payload_len >= 6U &&
                            v2_get_u32le(&pl[0]) == m_sessionId) {
                            m_gd32Role = pl[4];
                            m_v2Active = true;
                            DEBUG_INFO("SPI", "ENTER_BOOT 前 HELLO 重同步 ok role=%u", m_gd32Role);
                            break;
                        }
                        vTaskDelay(gap);
                    }
                }
            }
        }

        esp_err_t ret = otaTransferUnlocked(SPI_APP_ENTER_BOOT_CMD, nullptr, 0U,
                                             nullptr, nullptr, 0U, nullptr, 400U,
                                             /*expect_stale=*/true);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_RESPONSE) {
            DEBUG_WARN("SPI", "ENTER_BOOT transfer failed: %s", esp_err_to_name(ret));
            continue;
        }
        command_sent = true;
        m_txSeq = (uint8_t)(m_lastSentSeq + 1U);
        vTaskDelay(gap);

        /* poll 最多 ~150ms：只认 0xB0+0xB5；0xC1/0x82/过期 0x80 都忽略 */
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(150U);
        uint8_t last_type = 0xFFU;
        while (xTaskGetTickCount() <= deadline) {
            m_linkState.enterBusy(HandshakeState::WAITING_RESPONSE);
            (void)waitSpiDataReady(30U);

            constexpr size_t kPoll = 128U;
            uint8_t rx_raw[kPoll];
            uint8_t dummy[kPoll];
            memset(dummy, 0xFF, sizeof(dummy));
            memset(rx_raw, 0xFF, sizeof(rx_raw));
            driveReadyLow();
            ret = transferClocked(dummy, sizeof(dummy), rx_raw, sizeof(rx_raw), 80U);
            releaseReady();
            m_linkState.exitBusy();
            if (ret != ESP_OK) {
                vTaskDelay(gap);
                continue;
            }

            v2_header_t hdr;
            const uint8_t *pload = nullptr;
            if (!v2_parse_frame(rx_raw, sizeof(rx_raw), &hdr, &pload) ||
                !(hdr.flags & V2_FLAG_RESPONSE)) {
                vTaskDelay(gap);
                continue;
            }
            last_type = hdr.type;

            if (hdr.type == SPI_APP_ENTER_BOOT_CMD) {
                const uint8_t status = (hdr.payload_len > 0U && pload)
                    ? pload[0] : 0x00U;
                if (status == SPI_APP_ENTER_BOOT_ACK_OK) {
                    DEBUG_INFO("SPI", "ENTER_BOOT ACK type=0xB0 status=0xB5 sess=0x%08lX",
                               (unsigned long)hdr.session_id);
                    got_ack = true;
                    break;
                }
                DEBUG_WARN("SPI", "ENTER_BOOT ACK status=0x%02X (want 0xB5)", status);
            } else if (hdr.type == V2_TYPE_LINK_ERROR) {
                const uint8_t errc = (hdr.payload_len > 0U && pload) ? pload[0] : 0xFFU;
                const uint8_t exp  = (hdr.payload_len > 1U && pload) ? pload[1] : 0xFFU;
                DEBUG_WARN("SPI", "ENTER_BOOT 被拒 LINK_ERROR code=0x%02X expected_seq=%u last_sent=%u",
                           errc, exp, (unsigned)m_lastSentSeq);
                break;
            } else if (hdr.type == SPI_OTA_RSP_ERROR) {
                DEBUG_WARN("SPI", "ENTER_BOOT 收到 RSP_ERROR(0x82)，将重发");
                break;
            } else {
                /* 0xC1 事件 / 过期 0x80 等：继续 poll，不记成功 */
                DEBUG_DBG("SPI", "ENTER_BOOT poll 忽略 type=0x%02X flags=0x%02X",
                          hdr.type, hdr.flags);
            }
            vTaskDelay(gap);
        }

        if (got_ack) {
            break;
        }
        if (last_type != 0xFFU) {
            DEBUG_INFO("SPI", "ENTER_BOOT poll 结束 last_type=0x%02X (未获 0xB0/0xB5)", last_type);
        }
    }

    if (command_sent) {
        /* 静默：APP 擦配置页(可达数十 ms) + 软复位 + BOOT 起机。
         * 此间禁止 HELLO，否则 role 会一直像卡在 APP。 */
        const uint32_t kQuietMs = got_ack ? 900U : 500U;
        DEBUG_INFO("SPI", "ENTER_BOOT 已上总线 (ack=%s)，静默 %lums 等待 APP 复位...",
                   got_ack ? "yes" : "no", (unsigned long)kQuietMs);
        vTaskDelay(pdMS_TO_TICKS(kQuietMs));
        return got_ack ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    DEBUG_WARN("SPI", "ENTER_BOOT 未能发出 SPI 帧");
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 通过 HELLO 轮询等待 Bootloader 就绪（v2 方式）。
 *
 * App→Boot 切换后轮询 HELLO，直到 role=boot 或超时。
 * APP 仍在线时拉大间隔、降噪日志，避免 role=1 刷屏。
 */
esp_err_t SpiMaster::waitForBootloaderReady(uint32_t timeout_ms)
{
    DEBUG_INFO("SPI", "等待 Bootloader 就绪 (HELLO 轮询, timeout=%lums)...",
               (unsigned long)timeout_ms);

    /* 复位/起机再留白；未走 appRequestBootloader 的调用方也需要 */
    {
        const TickType_t settle = (pdMS_TO_TICKS(200) > 0) ? pdMS_TO_TICKS(200) : 1;
        vTaskDelay(settle);
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    constexpr uint32_t kHelloAttemptMs = 400U;
    uint32_t app_hits = 0U;
    uint32_t rounds = 0U;

    while (xTaskGetTickCount() < deadline) {
        TickType_t now = xTaskGetTickCount();
        uint32_t remain_ms = (deadline > now)
            ? (uint32_t)((deadline - now) * portTICK_PERIOD_MS) : 0U;
        if (remain_ms == 0U) {
            break;
        }
        const uint32_t attempt_ms = (remain_ms < kHelloAttemptMs) ? remain_ms : kHelloAttemptMs;
        ++rounds;

        esp_err_t ret = hello(attempt_ms, V2_ROLE_BOOT);
        if (ret == ESP_OK && m_gd32Role == V2_ROLE_BOOT) {
            DEBUG_INFO("SPI", "Bootloader 就绪 (HELLO role=boot, round=%lu)",
                       (unsigned long)rounds);
            return ESP_OK;
        }

        if (ret == ESP_OK && m_gd32Role == V2_ROLE_APP) {
            ++app_hits;
            /* 仍在 APP：复位未完成或 ENTER_BOOT 未生效；放慢 HELLO 给 Flash/复位时间 */
            if (app_hits == 1U || (app_hits % 5U) == 0U) {
                DEBUG_INFO("SPI", "仍为 APP (role=1)，等待复位进 BOOT... (app_hits=%lu)",
                           (unsigned long)app_hits);
            }
            const uint32_t app_gap_ms = (app_hits < 3U) ? 200U : 150U;
            const TickType_t app_gap = (pdMS_TO_TICKS(app_gap_ms) > 0)
                ? pdMS_TO_TICKS(app_gap_ms) : 1;
            vTaskDelay(app_gap);
            continue;
        }

        /* 无响应 / 0xC1 噪声：中等等待 */
        const TickType_t miss_gap = (pdMS_TO_TICKS(60) > 0) ? pdMS_TO_TICKS(60) : 1;
        vTaskDelay(miss_gap);
    }

    DEBUG_WARN("SPI", "Bootloader 就绪超时 (%lums, rounds=%lu, app_hits=%lu)",
               (unsigned long)timeout_ms, (unsigned long)rounds, (unsigned long)app_hits);
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 向 GD32 APP 发送存活探测命令（v2 标准帧，单 CS 同步传输）。
 *
 * 与旧版一致采用单 CS 事务：发送 PING 帧同时读回响应，
 * 不走 otaPollResponseUnlocked（避免消费不相关帧）。
 * 用 v2_parse_frame 在 RX buffer 里定位响应帧。
 *
 * GD32 App PING handler 用 kRspAck(0x80) 作为响应 type，
 * payload(16B) = APP version(4B) + BOOT version(4B) + uptime_ms(4B) + build_hash(4B)。
 *
 * @param timeout_ms 超时时间（毫秒）。
 * @param uptime_sec 输出设备运行时间（秒）。
 * @param version 输出 GD32 版本信息（可选）。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::appPing(uint32_t timeout_ms, uint32_t *uptime_sec, gd32_version_t *version)
{
    LockGuard lock(m_mutex);
    constexpr uint8_t kAppPingType = 0xA1U;

    /* 构建 v2 PING 请求帧 */
    uint8_t tx_frame[V2_MAX_FRAME];
    size_t tx_len = 0;
    if (!v2_build_frame(kAppPingType, V2_FLAG_REQUEST, m_sessionId,
                         m_txSeq, m_rxAckedSeq, m_localCredit,
                         nullptr, 0, tx_frame, &tx_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint32_t attempt = 0U;

    while (xTaskGetTickCount() <= deadline) {
        ++attempt;

        /* 单 CS 同步传输：下拉 SPIready 后，在单次 SPI 事务中发送 PING 并读回响应。
         * GD32 延迟一拍协议下，本次 CS 读到的是上一笔的响应（首笔为 0xFF）。
         * 发送 v2 帧后 GD32 在 CS 释放后处理，响应在下一次 CS 返回。
         * 因此需要多次循环：第一次发 PING + 读 0xFF，第二次发 PING + 读上一笔 PING 的 ACK。 */
        const size_t xfer_len = spiXferBytes(tx_len);
        uint8_t tx_padded[SPI_OTA_FRAME_MAX];
        memset(tx_padded, 0xFF, xfer_len);
        memcpy(tx_padded, tx_frame, tx_len);

        m_linkState.enterBusy(HandshakeState::MASTER_SENDING);
        driveReadyLow();
        if (SPI_READY_DRIVE_DELAY_US > 0U) {
            esp_rom_delay_us(SPI_READY_DRIVE_DELAY_US);
        }

        uint8_t rx_raw[SPI_OTA_FRAME_MAX];
        memset(rx_raw, 0xFF, xfer_len);

        esp_err_t err = transferClocked(tx_padded, xfer_len, rx_raw, xfer_len, 0U);
        releaseReady();
        m_linkState.exitBusy();
        if (err != ESP_OK) {
            DEBUG_DBG("SPI", "APP PING transfer failed[%lu]: %s",
                      (unsigned long)attempt, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /* 在 RX buffer 中用 v2_parse_frame 定位响应帧 */
        v2_header_t hdr;
        const uint8_t *pload = nullptr;
        if (v2_parse_frame(rx_raw, xfer_len, &hdr, &pload) &&
            hdr.type == SPI_OTA_RSP_ACK && hdr.payload_len >= 12U && pload) {
            /* GD32 APP 重启后 g_v2Active=false，响应 session_id=0。
             * appPing 不经 otaFindFrame，故不会因 session 失配被拒——
             * 但后续 otaSendAndPoll 走 otaFindFrame 仍会拒绝。
             * 在此同步 m_sessionId 到 GD32 实际值，避免失配。 */
            if (hdr.session_id != m_sessionId) {
                DEBUG_INFO("SPI", "appPing session 同步: 0x%08lX -> 0x%08lX",
                           (unsigned long)m_sessionId, (unsigned long)hdr.session_id);
                m_sessionId = hdr.session_id;
            }
            /* 成功收到 PING ACK 响应，递增发送序号 */
            m_txSeq = (uint8_t)(m_txSeq + 1U);

            /* payload 布局兼容两种 GD32 App 固件版本：
             *   16B: APP ver(4) + BOOT ver(4) + uptime_ms(4) + build_hash(4)
             *   12B: APP ver(4) + uptime_ms(4) + build_hash(4)  （旧固件，无 BOOT 版本） */
            if (version) {
                memset(version, 0, sizeof(gd32_version_t));
                version->app_major  = pload[0];
                version->app_minor  = pload[1];
                version->app_patch  = pload[2];
                version->app_build  = pload[3];
                if (hdr.payload_len >= 16U) {
                    /* 新固件：[4..7] = BOOT 版本 */
                    version->boot_major = pload[4];
                    version->boot_minor = pload[5];
                    version->boot_patch  = pload[6];
                    version->boot_build  = pload[7];
                }
                /* 旧固件 payload_len==12：boot 版本保持 0（已在 memset 中清零） */
            }
            if (uptime_sec) {
                if (hdr.payload_len >= 16U) {
                    /* 新固件：uptime 在 [8..11] */
                    const uint8_t *ut = &pload[8];
                    const uint32_t uptime_ms = (uint32_t)ut[0]
                                            | ((uint32_t)ut[1] << 8)
                                            | ((uint32_t)ut[2] << 16)
                                            | ((uint32_t)ut[3] << 24);
                    *uptime_sec = uptime_ms / 1000U;
                } else {
                    /* 旧固件 payload_len==12：uptime 在 [4..7] */
                    const uint8_t *ut = &pload[4];
                    const uint32_t uptime_ms = (uint32_t)ut[0]
                                            | ((uint32_t)ut[1] << 8)
                                            | ((uint32_t)ut[2] << 16)
                                            | ((uint32_t)ut[3] << 24);
                    *uptime_sec = uptime_ms / 1000U;
                }
            }
            DEBUG_DBG("SPI", "APP PING v2 成功: attempt=%lu", (unsigned long)attempt);
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    DEBUG_DBG("SPI", "APP PING 超时: timeout=%ums", (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 检查 GD32 是否有主动按铃请求，若有则发起读事务读取请求帧。
 *
 * GD32 下拉 SPIready 表示「我有请求帧要给你读」。此函数检测到该状态后
 * 发起一次 SPI 读事务（发 dummy 字节，读回 GD32 准备的请求帧）。
 *
 * @param timeout_ms 等待 GD32 请求的超时（毫秒），0 表示不等待仅检查。
 * @return ESP_OK 成功读到请求帧；ESP_ERR_NOT_FOUND 当前无请求；其余为错误码。
 */
esp_err_t SpiMaster::checkGd32Request(uint32_t timeout_ms)
{
    /* 登记 ISR 通知目标任务，确保下降沿中断能唤醒本任务 */
    m_spiTaskHandle = xTaskGetCurrentTaskHandle();

    if (!m_gd32RequestPending) {
        if (timeout_ms == 0U) {
            return ESP_ERR_NOT_FOUND;
        }
        /* 等待 GD32 按铃通知（bit 0x01） */
        xTaskNotifyWait(0x01U, 0x01U, NULL, 0U);
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
        while (!m_gd32RequestPending && xTaskGetTickCount() < deadline) {
            uint32_t val = 0U;
            xTaskNotifyWait(0x01U, 0x01U, &val, pdMS_TO_TICKS(10));
        }
        if (!m_gd32RequestPending) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    LockGuard lock(m_mutex);
    m_gd32RequestPending = false;
    m_linkState.enterBusy(HandshakeState::SERVING_GD32_RING);

    /* 占用 SPIready 线（保持低）：屏蔽 GD32 onCsRelease 的 setSpiReady(true)，
     * 使线在 ESP32 完成前始终保持忙碌。完成后 releaseReady 产生上升沿，
     * 作为 GD32 EXTI 的"ESP32 完成"信号。 */
    driveReadyLow();
    if (SPI_READY_DRIVE_DELAY_US > 0U) {
        esp_rom_delay_us(SPI_READY_DRIVE_DELAY_US);
    }

    constexpr size_t kRingReadBytes = 160U;
    uint8_t rx_raw[kRingReadBytes];
    uint8_t dummy[kRingReadBytes];
    memset(dummy, 0xFF, sizeof(dummy));
    memset(rx_raw, 0xFF, sizeof(rx_raw));

    /* GD32 已下拉 SPIready，我们是响应方直接读 */
    esp_err_t ret = transferClocked(dummy, sizeof(dummy), rx_raw, sizeof(rx_raw), 500U);

    /* 无论成功失败，先释放线 + 回 IDLE，
     * 让 GD32 EXTI 上升沿触发，解除 GD32 发送阻塞 */
    releaseReady();
    m_linkState.exitBusy();

    if (ret != ESP_OK) {
        DEBUG_WARN("SPI", "checkGd32Request 读事务失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 解析请求帧（m_linkState 已 IDLE，允许 GD32 下一帧按铃被 ISR 接受） */
    uint8_t found_type = 0;
    const uint8_t *found_payload = nullptr;
    size_t found_len = 0;
    size_t frame_offset = 0;
    if (otaFindFrame(rx_raw, sizeof(rx_raw), &found_type, &found_payload,
                     &found_len, &frame_offset)) {
        m_ringRxLen = found_len;
        m_ringRxType = found_type;
        if (found_payload && found_len > 0 && found_len <= sizeof(m_ringRxBuf)) {
            memcpy(m_ringRxBuf, found_payload, found_len);
        }
        /* 正常按铃静默，仅在异常时由上层告警 */
        return ESP_OK;
    }

    /* 降噪：无合法帧时限流，避免 ready 毛刺/残留 0xFF 刷屏 */
    static uint32_t s_ring_bad_logs = 0U;
    if ((++s_ring_bad_logs % 32U) == 1U) {
        DEBUG_WARN("SPI", "GD32 按铃读事务未找到合法帧 (log#%lu) head=%02X %02X %02X %02X",
                   (unsigned long)s_ring_bad_logs,
                   rx_raw[0], rx_raw[1], rx_raw[2], rx_raw[3]);
    }
    return ESP_ERR_INVALID_RESPONSE;
}

bool SpiMaster::takeRingRx(uint8_t *type, uint8_t *payload, size_t max_len, size_t *len)
{
    LockGuard lock(m_mutex);
    if (m_ringRxLen == 0U) {
        return false;
    }
    if (type) { *type = m_ringRxType; }
    size_t copy_len = (m_ringRxLen <= max_len) ? m_ringRxLen : max_len;
    if (len) { *len = copy_len; }
    if (payload && copy_len > 0U) {
        memcpy(payload, m_ringRxBuf, copy_len);
    }
    m_ringRxLen = 0U;
    m_ringRxType = 0U;
    return true;
}

// ============================================================
// SPI Link v2 链路状态
// ============================================================
SpiMaster::SpiLinkStats SpiMaster::linkStats()
{
    LockGuard lock(m_mutex);
    return m_stats;
}

esp_err_t SpiMaster::hello(uint32_t timeout_ms, uint8_t role_hint)
{
    LockGuard lock(m_mutex);

    /* 1. 生成随机非零 SESSION_ID（整次 hello 内固定，禁止每 CS 换 session） */
    m_sessionId = esp_random() | 0x00000001U;
    m_txSeq = 0;
    m_lastSentSeq = 0;
    m_rxAckedSeq = 0;
    m_expectedRxSeq = 0;
    m_gd32Role = 0;
    m_v2Active = false;

    /* 2. 构建 HELLO 帧 */
    uint8_t hello_frame[V2_MAX_FRAME];
    size_t hello_len = 0;
    if (!v2_build_hello_frame(m_sessionId, role_hint, hello_frame, &hello_len)) {
        DEBUG_ERR("SPI", "HELLO build failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* GD32 延迟一拍：采用与 appPing 相同的“同 session 连发”策略。
     * 第 N 次 CS 读到第 N-1 次 HELLO 的 ACK；dummy 紧贴重发会与 GD32 EXTI
     * 处理竞态，实测易得到全 0xFF。 */
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint8_t rx_raw[SPI_OTA_FRAME_MAX];
    uint32_t attempt = 0U;
    uint8_t last_type = 0xFFU;
    uint8_t last_flags = 0U;
    bool saw_any_frame = false;
    uint8_t last_rx_head[16];
    memset(last_rx_head, 0xFF, sizeof(last_rx_head));

    /* FreeRTOS tick 常为 10ms：pdMS_TO_TICKS(5)==0 会变成忙等上千次。
     * 至少 1 tick，并在两次 CS 间留出 GD32 EXTI 处理时间。 */
    const TickType_t gap_ticks = (pdMS_TO_TICKS(10) > 0) ? pdMS_TO_TICKS(10) : 1;

    while (xTaskGetTickCount() <= deadline) {
        ++attempt;

        m_linkState.enterBusy(HandshakeState::WAITING_RESPONSE);
        (void)waitSpiReady(SPI_READY_TIMEOUT_MS);
        m_linkState.enterBusy(HandshakeState::MASTER_SENDING);
        driveReadyLow();

        const size_t xfer_len = spiXferBytes(hello_len);
        uint8_t tx_padded[SPI_OTA_FRAME_MAX];
        memset(tx_padded, 0xFF, xfer_len);
        memcpy(tx_padded, hello_frame, hello_len);

        memset(rx_raw, 0xFF, xfer_len);
        esp_err_t ret = transferClocked(tx_padded, xfer_len, rx_raw, xfer_len, 0U);
        releaseReady();
        m_linkState.exitBusy();
        if (ret != ESP_OK) {
            DEBUG_WARN("SPI", "HELLO transfer failed[%lu]: %s",
                       (unsigned long)attempt, esp_err_to_name(ret));
            vTaskDelay(gap_ticks);
            continue;
        }

        memcpy(last_rx_head, rx_raw, sizeof(last_rx_head));

        v2_header_t hdr;
        const uint8_t *pload = nullptr;
        if (!v2_parse_frame(rx_raw, xfer_len, &hdr, &pload)) {
            vTaskDelay(gap_ticks);
            continue;
        }
        saw_any_frame = true;
        last_type = hdr.type;
        last_flags = hdr.flags;

        if (hdr.type != V2_TYPE_LINK_HELLO || !(hdr.flags & V2_FLAG_RESPONSE)) {
            vTaskDelay(gap_ticks);
            continue;
        }
        if (hdr.payload_len < 6U || !pload) {
            vTaskDelay(gap_ticks);
            continue;
        }

        const uint32_t session_echo = v2_get_u32le(&pload[0]);
        if (session_echo != m_sessionId) {
            /* 延迟一拍的旧 ACK：同 session 再发一次即可对齐 */
            DEBUG_DBG("SPI", "HELLO lag echo=0x%08lX expected=0x%08lX attempt=%lu",
                      (unsigned long)session_echo, (unsigned long)m_sessionId,
                      (unsigned long)attempt);
            vTaskDelay(gap_ticks);
            continue;
        }

        m_gd32Role = pload[4];
        m_localCredit = pload[5];
        m_v2Active = true;
        m_stats.resyncs++;
        /* 等 BOOT 时 role=APP 的成功极吵：仅 role 匹配 hint、或 hint=any、或已是 boot 时 INFO */
        if (role_hint == 0U || m_gd32Role == role_hint || m_gd32Role == V2_ROLE_BOOT) {
            DEBUG_INFO("SPI", "HELLO success: session=0x%08lX role=%u credit=%u attempt=%lu",
                       (unsigned long)m_sessionId, m_gd32Role, m_localCredit,
                       (unsigned long)attempt);
        } else {
            DEBUG_DBG("SPI", "HELLO ok role=%u (want hint=%u) attempt=%lu",
                      m_gd32Role, role_hint, (unsigned long)attempt);
        }
        return ESP_OK;
    }

    if (!saw_any_frame) {
        DEBUG_WARN("SPI",
                   "HELLO: no valid v2 frame in response (attempts=%lu) rx[0..15]="
                   "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                   (unsigned long)attempt,
                   last_rx_head[0], last_rx_head[1], last_rx_head[2], last_rx_head[3],
                   last_rx_head[4], last_rx_head[5], last_rx_head[6], last_rx_head[7],
                   last_rx_head[8], last_rx_head[9], last_rx_head[10], last_rx_head[11],
                   last_rx_head[12], last_rx_head[13], last_rx_head[14], last_rx_head[15]);
    } else {
        DEBUG_WARN("SPI", "HELLO: bad/lag response type=0x%02X flags=0x%02X attempts=%lu",
                   last_type, last_flags, (unsigned long)attempt);
    }
    return ESP_ERR_INVALID_RESPONSE;
}

// CANFD 接口已迁移至 transport/canfd_bridge.cpp（M4 解耦）

