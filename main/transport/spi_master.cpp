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
#include "esp_intr_alloc.h"  // ESP_INTR_FLAG_IRAM

#include "spi_master.h"
#include "monitor/debug_log.h"


static constexpr size_t kSpiTransferMaxBytes = 256U;

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
 * @brief 下拉 SPIready（开漏驱动低）。
 *        线与架构下，ESP32 主动发起通信时调用，通知 GD32「我要来了」。
 */
void SpiMaster::driveReadyLow()
{
    gpio_set_level((gpio_num_t)SPI_READY_GPIO, 0);
}

/**
 * @brief 释放 SPIready（开漏高阻，靠上拉拉高）。
 *        线与架构下，ESP32 作为响应方完成读事务后调用。
 */
void SpiMaster::releaseReady()
{
    gpio_set_level((gpio_num_t)SPI_READY_GPIO, 1);
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
        if (self->m_hsState == HandshakeState::IDLE) {
            /* GD32 按铃 */
            self->m_gd32RequestPending = true;
            if (self->m_spiTaskHandle) {
                BaseType_t hp = pdFALSE;
                xTaskNotifyFromISR(self->m_spiTaskHandle, 0x01U,
                                   eSetBits, &hp);
                portYIELD_FROM_ISR(hp);
            }
        } else if (self->m_hsState == HandshakeState::WAITING_BOOT_READY) {
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
        if (self->m_hsState == HandshakeState::WAITING_RESPONSE) {
            /* GD32 释放 SPIready，可发起新 CS */
            if (self->m_spiTaskHandle) {
                BaseType_t hp = pdFALSE;
                xTaskNotifyFromISR(self->m_spiTaskHandle, 0x02U,
                                   eSetBits, &hp);
                portYIELD_FROM_ISR(hp);
            }
        } else if (self->m_hsState == HandshakeState::WAITING_BOOT_READY) {
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
 * @brief 等 GD32 释放 SPIready（上升沿），中断驱动非忙等。
 *
 * 调用前须将 m_hsState 置为 WAITING_RESPONSE。ISR 上升沿检测到该状态后
 * 发 0x02 通知唤醒本任务。GD32 写 Flash 期间保持 PB6 低，本函数阻塞，
 * 写完拉高 → 唤醒 → 继续，自然避免 overrun。
 */
esp_err_t SpiMaster::waitSpiReady(uint32_t timeout_ms)
{
    if (timeout_ms == 0U) { return ESP_OK; }

    /* 已高，立即返回 */
    if (readReadyLevelRaw() == 1) { return ESP_OK; }

    m_spiTaskHandle = xTaskGetCurrentTaskHandle();
    /* 清遗留 0x02 通知位 */
    xTaskNotifyWait(0x02U, 0x02U, NULL, 0U);

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) { return ESP_ERR_TIMEOUT; }
        TickType_t remain = deadline - now;
        uint32_t notify_val = 0U;
        BaseType_t ok = xTaskNotifyWait(0x02U, 0x02U, &notify_val,
                                        pdMS_TO_TICKS(remain));
        if (ok == pdTRUE && (notify_val & 0x02U)) { return ESP_OK; }
        /* 电平二次检查，防中断丢失 */
        if (readReadyLevelRaw() == 1) { return ESP_OK; }
    }
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
 */
uint32_t SpiMaster::otaCrc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint32_t)data[i] << 24;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x80000000U) {
                crc = (crc << 1) ^ 0x04C11DB7U;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* 同步帧头（6 字节）：
 *   SOF0 SOF1 CREDIT TYPE LEN_LO LEN_HI
 * 随后 payload，末尾 4 字节 CRC32（覆盖头 6 + payload，不含自身）。 */
static constexpr size_t kSyncHeaderLen = 6U;
static constexpr size_t kSyncCrcLen     = 4U;
static constexpr size_t kSyncOverhead   = kSyncHeaderLen + kSyncCrcLen;

static void putU16Le(uint8_t *pp, uint16_t v)
{
    pp[0] = (uint8_t)(v & 0xFFU);
    pp[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static uint16_t getU16Le(const uint8_t *pp)
{
    return (uint16_t)(pp[0] | ((uint16_t)pp[1] << 8));
}

static void putU32Le(uint8_t *pp, uint32_t v)
{
    pp[0] = (uint8_t)(v & 0xFFU);
    pp[1] = (uint8_t)((v >> 8) & 0xFFU);
    pp[2] = (uint8_t)((v >> 16) & 0xFFU);
    pp[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint32_t getU32Le(const uint8_t *pp)
{
    return (uint32_t)pp[0] | ((uint32_t)pp[1] << 8) |
           ((uint32_t)pp[2] << 16) | ((uint32_t)pp[3] << 24);
}

bool SpiMaster::otaBuildFrame(uint8_t type, const uint8_t *payload, size_t payload_len,
                              uint8_t *out, size_t *out_len)
{
    if (!out || !out_len) return false;
    if (payload_len > 0U && payload == NULL) return false;
    size_t total = kSyncOverhead + payload_len;
    if (total > SPI_OTA_FRAME_MAX) return false;

    SpiMaster &self = SpiMaster::instance();
    out[0] = SPI_SYNC_SOF0;
    out[1] = SPI_SYNC_SOF1;
    out[2] = self.m_localCredit;
    out[3] = type;
    putU16Le(&out[4], (uint16_t)payload_len);
    if (payload && payload_len > 0U) {
        memcpy(&out[kSyncHeaderLen], payload, payload_len);
    }
    uint32_t crc = otaCrc32(out, kSyncHeaderLen + payload_len);
    putU32Le(&out[kSyncHeaderLen + payload_len], crc);
    *out_len = total;
    return true;
}

bool SpiMaster::otaFindFrame(const uint8_t *buf, size_t buf_len,
                             uint8_t *type, const uint8_t **payload, size_t *payload_len,
                             size_t *frame_offset)
{
    if (!buf) return false;

    for (size_t i = 0; i + kSyncOverhead <= buf_len; ++i) {
        if (buf[i] != SPI_SYNC_SOF0 || buf[i + 1] != SPI_SYNC_SOF1) continue;
        size_t plen = getU16Le(&buf[i + 4]);
        size_t total = kSyncOverhead + plen;
        if (i + total > buf_len) continue;
        uint32_t expect = getU32Le(&buf[i + kSyncHeaderLen + plen]);
        uint32_t calc = otaCrc32(&buf[i], kSyncHeaderLen + plen);
        if (expect != calc) {
            DEBUG_WARN("SPI", "SYNC CRC mismatch @%u: plen=%u",
                       (unsigned)i, (unsigned)plen);
            continue;
        }
        /* 解析对端 CREDIT，让 ota_relay 的 CREDIT 感知逻辑生效 */
        SpiMaster::instance().m_remoteCredit = buf[i + 2];
        if (type) *type = buf[i + 3];
        if (payload) *payload = (plen > 0) ? &buf[i + kSyncHeaderLen] : nullptr;
        if (payload_len) *payload_len = plen;
        if (frame_offset) *frame_offset = i;
        return true;
    }
    return false;
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
    dev_cfg.cs_ena_posttrans = 20;
    dev_cfg.cs_ena_pretrans = 40;

    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &m_spiDevice);
    if (ret != ESP_OK) {
        DEBUG_ERR("SPI", "SPI 设备添加失败: %s", esp_err_to_name(ret));
        return ret;
    }

    DEBUG_INFO("SPI", "SPI2 总线初始化完成: MOSI=GPIO5, MISO=GPIO7, SCLK=GPIO6, CS=GPIO%d, CLK=%d Hz",
               SPI_MASTER_CS_GPIO, SPI_CLOCK_HZ);

    /* 配置 SPI Ready 握手引脚(GPIO10)，开漏线与（GPIO10<->GD32 PB6）。
     * 双方都是开漏输出+上拉，任一下拉 = 线低；双方释放 = 线高（空闲）。
     * 双边沿中断：下降沿=GD32 按铃，上升沿=GD32 响应就绪。 */
    gpio_config_t ready_cfg = {};
    ready_cfg.pin_bit_mask = (1ULL << SPI_READY_GPIO);
    ready_cfg.mode = GPIO_MODE_INPUT_OUTPUT_OD;  /* 开漏：可输出驱动低，也可读回电平 */
    ready_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    ready_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ready_cfg.intr_type = GPIO_INTR_ANYEDGE;     /* 双边沿中断 */
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
    m_hsState = HandshakeState::IDLE;
    m_gd32RequestPending = false;
    m_localCredit = 8;
    m_remoteCredit = 8;

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
    LockGuard lock(m_mutex);
    uint8_t tx_frame[SPI_OTA_FRAME_MAX];
    size_t tx_len = 0;
    if (!otaBuildFrame(type, payload, payload_len, tx_frame, &tx_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    DEBUG_DBG("SPI", "OTA TX: type=0x%02X len=%u", type, (unsigned)tx_len);

    /* OTA 帧内容 hex dump（前 18 字节），用于与 GD32 端 [RX-FRM] 日志对比诊断 CRC 问题。
     * 仅对 OTA 帧类型(0x01~0x0F)打印，避免 CAN 反馈(0xC1)每秒刷屏。 */
    if (type <= 0x0FU) {
        char hexbuf[64] = {0};
        size_t dump_len = (tx_len < 18U) ? tx_len : 18U;
        for (size_t i = 0U; i < dump_len; ++i) {
            snprintf(hexbuf + i * 3, sizeof(hexbuf) - i * 3, "%02X ", tx_frame[i]);
        }
        DEBUG_INFO("SPI", "OTA TX hex: %s", hexbuf);
    }

    if (tx_len > SPI_OTA_FRAME_MAX) {
        DEBUG_ERR("SPI", "OTA frame too large: %u", (unsigned)tx_len);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 同步单 CS 传输：拉低 SPIready 后，在单次 SPI 事务中同时发送请求并读回响应。
     * 注意：GD32 BOOT 采用"延迟一拍"协议——响应在 onCsAssert() 装载 DMA，
     * queueTxFrame() 只缓冲不启动 DMA。因此本笔 CS 内读到的响应属于上一笔请求，
     * 首笔请求必读 0xFF。握手请求幂等可通过重试匹配；唯一性请求需配合
     * otaPollResponse() 读取延迟响应。详见 SpiOta.cpp queueTxFrame 注释。 */
    m_hsState = HandshakeState::WAITING_RESPONSE;
    /* 等 GD32 释放 PB6（上一笔 onCsRelease 处理完）再发起新 CS。
     * 中断驱动：GD32 拉高 → ISR 0x02 → 唤醒。GD32 写 Flash 期间
     * PB6 保持低，本函数阻塞直到写完，自然避免 overrun。
     * 0xB0 不走 otaTransfer，不受此等待影响。 */
    (void)waitSpiReady(SPI_READY_TIMEOUT_MS);
    m_hsState = HandshakeState::MASTER_SENDING;

    driveReadyLow();
    if (SPI_READY_DRIVE_DELAY_US > 0U) {
        esp_rom_delay_us(SPI_READY_DRIVE_DELAY_US);
    }

    uint8_t tx_padded[SPI_OTA_FRAME_MAX];
    if (tx_len > sizeof(tx_padded)) {
        DEBUG_ERR("SPI", "OTA frame %u exceeds tx_padded %u", (unsigned)tx_len, (unsigned)sizeof(tx_padded));
        releaseReady();
        m_hsState = HandshakeState::IDLE;
        return ESP_ERR_INVALID_SIZE;
    }
    memset(tx_padded, 0xFF, sizeof(tx_padded));
    memcpy(tx_padded, tx_frame, tx_len);

    constexpr size_t kRxBytes = SPI_OTA_FRAME_MAX;
    uint8_t rx_raw[kRxBytes];
    memset(rx_raw, 0xFF, sizeof(rx_raw));

    esp_err_t ret = transferClocked(tx_padded, sizeof(tx_padded), rx_raw, sizeof(rx_raw), timeout_ms);
    releaseReady();
    m_hsState = HandshakeState::IDLE;
    if (ret != ESP_OK) {
        DEBUG_WARN("SPI", "OTA sync transfer failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t found_type = 0;
    const uint8_t *found_payload = nullptr;
    size_t found_len = 0;
    size_t frame_offset = 0;
    if (otaFindFrame(rx_raw, sizeof(rx_raw), &found_type, &found_payload, &found_len, &frame_offset)) {
        if (resp_type) *resp_type = found_type;
        if (resp_payload && resp_max > 0) {
            size_t copy_len = (found_len < resp_max) ? found_len : resp_max;
            if (found_payload && copy_len > 0) {
                memcpy(resp_payload, found_payload, copy_len);
            }
        }
        if (resp_len) *resp_len = found_len;
        return ESP_OK;
    }

    DEBUG_WARN("SPI", "OTA sync transfer response not found");
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
                                             size_t resp_max, size_t *resp_len, uint32_t timeout_ms)
{
    constexpr size_t kPollBytes = 160U;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint32_t attempt = 0;

    while (xTaskGetTickCount() <= deadline) {
        ++attempt;

        /* 等 GD32 释放 PB6（当前帧 onCsRelease 处理完）再发起 CS#2 poll。
         * 中断驱动：GD32 拉高 → ISR 0x02 → 唤醒。GD32 写 Flash 期间
         * PB6 保持低，本函数阻塞直到写完，自然避免 overrun。 */
        m_hsState = HandshakeState::WAITING_RESPONSE;
        if (waitSpiReady(SPI_READY_TIMEOUT_MS) != ESP_OK) {
            /* 超时则退化为继续 poll（容错） */
        }

        uint8_t rx_raw[kPollBytes];
        uint8_t dummy[kPollBytes];
        memset(dummy, 0xFF, sizeof(dummy));

        TickType_t now = xTaskGetTickCount();
        uint32_t remain_ms = (deadline > now) ? ((deadline - now) * portTICK_PERIOD_MS) : 0U;
        if (remain_ms == 0U) break;
        uint32_t spi_timeout = (remain_ms > 500U) ? 500U : remain_ms;
        esp_err_t ret = transferClocked(dummy, sizeof(dummy), rx_raw, sizeof(rx_raw), spi_timeout);
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
            /* Skip empty ACK/ERROR responses (stale from previous transaction) */
            if ((found_type == SPI_OTA_RSP_ACK || found_type == SPI_OTA_RSP_ERROR) && found_len == 0) {
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
            m_hsState = HandshakeState::IDLE;
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }

    m_hsState = HandshakeState::IDLE;
    DEBUG_WARN("SPI", "OTA response timeout: %lu ms", (unsigned long)timeout_ms);
    return ESP_ERR_TIMEOUT;
}


/**
 * @brief 通知 GD32 DCU_APP 设置 OTA 标志并软复位进入 Bootloader。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::appRequestBootloader()
{
    LockGuard lock(m_mutex);
    DEBUG_INFO("SPI", "请求 GD32 APP 进入 Bootloader...");

    /* 清空残留的 GD32 按铃标志，避免 can_feedback_task 的 checkGd32Request()
     * 在 OTA 期间因残留标志发起 SPI 读事务，与 0xB0 通信抢占总线。
     * can_feedback_task 此时已被 pauseCanFeedbackTask(true) 暂停，但可能
     * 有一次 checkGd32Request() 正在阻塞中，清空标志让它安全返回。 */
    m_gd32RequestPending = false;

    constexpr uint32_t kMaxRetries = 3U;
    constexpr uint32_t kPerRetryAckMs = 100U;
    constexpr uint32_t kRetryGapMs = 50U;

    for (uint32_t retry = 0U; retry < kMaxRetries; ++retry) {
        if (retry > 0U) {
            DEBUG_WARN("SPI", "Bootloader 请求重发 (%lu/%lu)", (unsigned long)retry, (unsigned long)kMaxRetries);
            vTaskDelay(pdMS_TO_TICKS(kRetryGapMs));
        }

        uint8_t cmd = SPI_APP_ENTER_BOOT_CMD;
        /* 0xB0 是单字节命令，GD32 APP 可能在主动按铃(PB6=LOW)。
         * GD32 onCsAssert() 会立即处理 0xB0，不等 SPIready 就绪信号。 */
        esp_err_t ret = transferClocked(&cmd, 1, NULL, 0, 0U);
        if (ret != ESP_OK) {
            DEBUG_WARN("SPI", "发送 Bootloader 请求失败: %s", esp_err_to_name(ret));
            continue;
        }

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kPerRetryAckMs);
        while (xTaskGetTickCount() <= deadline) {
            uint8_t rx = 0;
            uint8_t dummy = 0xFF;
            /* 读 ACK 时 GD32 APP/BOOT 可能仍持有 PB6=LOW(正在准备 0xB5 响应)，
             * 延迟一拍协议下下一次 CS 的 onCsAssert() 会装载 0xB5 到 DMA。 */
            ret = transferClocked(&dummy, 1, &rx, 1, 0U);
            if (ret == ESP_OK && rx == SPI_APP_ENTER_BOOT_ACK_OK) {
                DEBUG_INFO("SPI", "GD32 APP 已确认进入 Bootloader (retry=%lu)", (unsigned long)retry);
                return ESP_OK;
            }
            if (ret == ESP_OK && rx != 0xFFU) {
                DEBUG_DBG("SPI", "Bootloader ACK 字节=0x%02X (非0xB5，APP可能使用帧ACK)", rx);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    DEBUG_WARN("SPI", "GD32 APP 进入 Bootloader 确认超时 (retries exhausted)");
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 方案A: 通过 PB6 LOW→HIGH 序列等待 Bootloader 就绪（中断驱动）。
 *
 * 时序:
 *   APP 拉低 PB6 → 20ms → 复位(μs级HIGH) → Bootloader极早推挽LOW → init... → 释放HIGH
 * ESP32 过滤:
 *   Phase1: 置 WAITING_BOOT_READY，GPIO 下降沿中断唤醒（bit 0x04）
 *   Phase2: 同状态，GPIO 上升沿中断唤醒（bit 0x08）+ 10ms 稳定确认
 *
 * @param timeout_ms 总超时（毫秒）。
 * @return ESP_OK 表示 Bootloader 就绪；ESP_ERR_TIMEOUT 表示超时。
 */
esp_err_t SpiMaster::waitForBootloaderReady(uint32_t timeout_ms)
{
    m_spiTaskHandle = xTaskGetCurrentTaskHandle();

    /* 清除遗留通知位 (0x04=下降沿, 0x08=上升沿) */
    xTaskNotifyWait(0x0CU, 0x0CU, NULL, 0U);

    /* Phase1: 等待下降沿 (APP 复位前拉低 / Bootloader 极早拉低)
     * 先检查当前电平，若已为 LOW 则跳过等待 */
    m_hsState = HandshakeState::WAITING_BOOT_READY;

    if (gpio_get_level((gpio_num_t)SPI_READY_GPIO) == 1) {
        /* 仍为 HIGH，等下降沿中断 */
        constexpr uint32_t kPhase1TimeoutMs = 500U;
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kPhase1TimeoutMs);
        bool seen_low = false;
        for (;;) {
            TickType_t now = xTaskGetTickCount();
            if (now >= deadline) { break; }
            TickType_t remain = deadline - now;
            uint32_t notify_val = 0U;
            BaseType_t ok = xTaskNotifyWait(0x0CU, 0x0CU, &notify_val,
                                            pdMS_TO_TICKS(remain > portMAX_DELAY / 2 ? portMAX_DELAY / 2 : remain));
            if (ok == pdTRUE && (notify_val & 0x04U)) { seen_low = true; break; }
            /* 防中断丢失：再次检查电平 */
            if (gpio_get_level((gpio_num_t)SPI_READY_GPIO) == 0) { seen_low = true; break; }
        }
        if (!seen_low) {
            m_hsState = HandshakeState::IDLE;
            DEBUG_WARN("SPI", "waitForBootloaderReady: Phase1 未检测到 LOW");
            return ESP_ERR_TIMEOUT;
        }
    }

    DEBUG_INFO("SPI", "waitForBootloaderReady: Phase1 检测到 LOW，等待 Bootloader 就绪");

    /* Phase2: 等待上升沿 (Bootloader init 完成释放 HIGH)
     * 10ms 稳定确认过滤复位期间 μs 级虚假 HIGH */
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) { break; }
        TickType_t remain = deadline - now;
        uint32_t notify_val = 0U;
        BaseType_t ok = xTaskNotifyWait(0x0CU, 0x0CU, &notify_val,
                                        pdMS_TO_TICKS(remain > portMAX_DELAY / 2 ? portMAX_DELAY / 2 : remain));
        if (ok == pdTRUE && (notify_val & 0x08U)) {
            /* 收到上升沿，确认 HIGH 持续 10ms 过滤虚假脉冲 */
            vTaskDelay(pdMS_TO_TICKS(10));
            if (gpio_get_level((gpio_num_t)SPI_READY_GPIO) == 1) {
                m_hsState = HandshakeState::IDLE;
                DEBUG_INFO("SPI", "waitForBootloaderReady: Bootloader 就绪 (PB6 HIGH stable)");
                return ESP_OK;
            }
            /* 虚假脉冲，继续等待 */
            DEBUG_DBG("SPI", "waitForBootloaderReady: 虚假 HIGH，继续等待");
        }
        /* 防中断丢失：再次检查电平 */
        if (gpio_get_level((gpio_num_t)SPI_READY_GPIO) == 1) {
            vTaskDelay(pdMS_TO_TICKS(10));
            if (gpio_get_level((gpio_num_t)SPI_READY_GPIO) == 1) {
                m_hsState = HandshakeState::IDLE;
                DEBUG_INFO("SPI", "waitForBootloaderReady: Bootloader 就绪 (poll fallback)");
                return ESP_OK;
            }
        }
    }

    m_hsState = HandshakeState::IDLE;
    DEBUG_WARN("SPI", "waitForBootloaderReady: Phase2 等待 HIGH 超时 (%ums)", (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 向 GD32 APP 发送存活探测命令，等待 APP 回复 ALIVE ACK。
 * @param timeout_ms 超时时间（毫秒）。
 * @param uptime_sec 输出设备运行时间（秒）。
 * @param version 输出 GD32 版本信息（可选）。
 * @return ESP_OK 表示成功，否则返回错误码。
 */
esp_err_t SpiMaster::appPing(uint32_t timeout_ms, uint32_t *uptime_sec, gd32_version_t *version)
{
    LockGuard lock(m_mutex);
    constexpr uint8_t kAppPingType = 0xA1U;

    uint8_t tx_frame[16] = {0};
    size_t tx_len = 0U;
    if (!otaBuildFrame(kAppPingType, NULL, 0U, tx_frame, &tx_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint32_t attempt = 0U;

    while (xTaskGetTickCount() <= deadline) {
        ++attempt;

        /* 发送 PING 帧，填充至 16 字节：
         * GD32 SPI 从机可能在 CS 断言时丢失首字节（EXTI/SPI 中断竞态），
         * 填充确保即使首字节丢失，后续仍有完整 0xA5 帧头可供扫描定位。 */
        uint8_t tx_padded[16];
        memset(tx_padded, 0xFF, sizeof(tx_padded));
        memcpy(tx_padded, tx_frame, tx_len);  /* [0..4]=PING帧, [5..15]=0xFF */

        /* 同步单 CS PING：下拉 SPIready 后，在单次 SPI 事务中发送并读回响应。 */
        m_hsState = HandshakeState::MASTER_SENDING;
        driveReadyLow();
        if (SPI_READY_DRIVE_DELAY_US > 0U) {
            esp_rom_delay_us(SPI_READY_DRIVE_DELAY_US);
        }

        uint8_t rx_raw[48];  /* 增大缓冲区，吸收 SPI 字节偏移 */
        memset(rx_raw, 0xFF, sizeof(rx_raw));

        esp_err_t err = transferClocked(tx_padded, sizeof(tx_padded), rx_raw, sizeof(rx_raw), 0U);
        releaseReady();
        m_hsState = HandshakeState::IDLE;
        if (err != ESP_OK) {
            DEBUG_DBG("SPI", "APP PING sync transfer failed[%lu]: %s",
                      (unsigned long)attempt, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        bool found = false;

        /* GD32 SPI 从机字节偏移不可预测，直接在 raw buffer 中
         * 搜索完整的标签化 PING 响应模式：
         *   0xA1 [0x55...] app_vers [0xAA...] boot_vers
         * 找到 0xA1 后跳过重复字节，验证 0x55 和 0xAA 标签。
         */
        for (size_t i = 0; i + 14 < sizeof(rx_raw); ++i) {
            if (rx_raw[i] != kAppPingType) continue;  /* 找 0xA1 */
            size_t p = i + 1;
            /* 跳过最多 1 个重复的 A1 字节（SPI 偏移最多产生 1 个重复） */
            if (p < sizeof(rx_raw) && rx_raw[p] == kAppPingType) ++p;
            if (p >= sizeof(rx_raw) || rx_raw[p] != 0x55U) continue;
            /* 跳过最多 1 个重复的 0x55 */
            if (p + 1 < sizeof(rx_raw) && rx_raw[p + 1] == 0x55U) ++p;
            size_t app_start = p + 1;  /* APP 版本起始 */
            if (app_start + 4 > sizeof(rx_raw)) continue;
            size_t bp = app_start + 4;
            /* 跳过最多 1 个重复字节 */
            if (bp < sizeof(rx_raw) && bp > 0 && rx_raw[bp] == rx_raw[bp - 1]) ++bp;
            if (bp >= sizeof(rx_raw) || rx_raw[bp] != 0xAAU) continue;
            /* 跳过最多 1 个重复的 0xAA */
            if (bp + 1 < sizeof(rx_raw) && rx_raw[bp + 1] == 0xAAU) ++bp;
            size_t boot_start = bp + 1;
            if (boot_start + 4 > sizeof(rx_raw)) continue;

            /* 模式匹配成功！提取版本数据 */
            found = true;

            if (version) {
                memset(version, 0, sizeof(gd32_version_t));
                version->app_major = rx_raw[app_start];
                version->app_minor = rx_raw[app_start + 1];
                version->app_patch = rx_raw[app_start + 2];
                version->app_build = rx_raw[app_start + 3];
                version->boot_major = rx_raw[boot_start];
                version->boot_minor = rx_raw[boot_start + 1];
                version->boot_patch = rx_raw[boot_start + 2];
                version->boot_build = rx_raw[boot_start + 3];
            }
            if (uptime_sec) {
                size_t ut = boot_start + 4;
                if (ut + 3 < sizeof(rx_raw)) {
                    *uptime_sec = (uint32_t)rx_raw[ut]
                                | ((uint32_t)rx_raw[ut + 1] << 8)
                                | ((uint32_t)rx_raw[ut + 2] << 16)
                                | ((uint32_t)rx_raw[ut + 3] << 24);
                } else {
                    *uptime_sec = 0;
                }
            }
            DEBUG_WARN("SPI", "APP PING 标签匹配: A1@%u APP@%u BOOT@%u",
                       (unsigned)i, (unsigned)app_start, (unsigned)boot_start);
            break;
        }

        if (found) {
            DEBUG_INFO("SPI", "APP PING 成功: uptime=%lu sec, attempt=%lu",
                       uptime_sec ? (unsigned long)*uptime_sec : 0UL,
                       (unsigned long)attempt);
            return ESP_OK;
        }

        DEBUG_WARN("SPI", "APP PING 无匹配[%lu]: first8=%02X%02X%02X%02X%02X%02X%02X%02X",
                  (unsigned long)attempt,
                  rx_raw[0], rx_raw[1], rx_raw[2], rx_raw[3],
                  rx_raw[4], rx_raw[5], rx_raw[6], rx_raw[7]);

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
    m_hsState = HandshakeState::SERVING_GD32_RING;

    /* GD32 已下拉 SPIready 并准备好请求帧在 TX buffer。
     * 发起一次 SPI 读事务：发 dummy 字节，读回 GD32 的请求帧。
     * GD32 在 CS 释放后释放 SPIready。 */
    constexpr size_t kRingReadBytes = 160U;
    uint8_t rx_raw[kRingReadBytes];
    uint8_t dummy[kRingReadBytes];
    memset(dummy, 0xFF, sizeof(dummy));
    memset(rx_raw, 0xFF, sizeof(rx_raw));

    /* GD32 已下拉 SPIready，我们是响应方直接读 */
    esp_err_t ret = transferClocked(dummy, sizeof(dummy), rx_raw, sizeof(rx_raw), 500U);
    if (ret != ESP_OK) {
        DEBUG_WARN("SPI", "checkGd32Request 读事务失败: %s", esp_err_to_name(ret));
        m_hsState = HandshakeState::IDLE;
        return ret;
    }

    /* 解析请求帧 */
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
        DEBUG_INFO("SPI", "GD32 按铃请求帧: type=0x%02X len=%u",
                   found_type, (unsigned)found_len);
        m_hsState = HandshakeState::IDLE;
        /* 上层通过 takeRingRx() 取出 type/payload 进行处理 */
        return ESP_OK;
    }

    DEBUG_WARN("SPI", "GD32 按铃读事务未找到合法帧");
    m_hsState = HandshakeState::IDLE;
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

// CANFD 接口已迁移至 transport/canfd_bridge.cpp（M4 解耦）

