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
#include "freertos/queue.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/spi_struct.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"   // esp_random() for HELLO session_id
#include "esp_intr_alloc.h"  // ESP_INTR_FLAG_IRAM

#include "spi_master.h"
#include "spi_link_v2.h"
#include "app_config.h"
#include "monitor/debug_log.h"


static constexpr size_t kSpiTransferMaxBytes = 256U;

/**
 * @brief 发送路径 CS 时钟字节数。
 * Boot TX DMA 按满缓冲预装（kTxBufSize=128），主机过短时钟会：
 *  - 截断延迟一拍的响应；
 *  - 两段变长读若中途释放 CS，Boot 会把 s_txPending 当已交付清掉 → 后续全 0xFF。
 * 下限 64：覆盖 HELLO_ACK(~26)/短 ACK；上限 SPI_OTA_FRAME_MAX。
 * App 大帧仍可由 poll 路径按 128/256 读。
 */
static size_t spiXferBytes(size_t frame_len)
{
    size_t n = frame_len;
    if (n < 64U) {
        n = 64U;
    }
    if (n > SPI_OTA_FRAME_MAX) {
        n = SPI_OTA_FRAME_MAX;
    }
    return n;
}

/** @brief 读响应时单 CS 时钟预算（Boot 满缓冲 128；App 可达 256）。 */
static size_t spiRespReadBudget(void)
{
    return 128U;
}

/** @brief ISR-safe GPIO 电平读取（不调用 Flash 中的 gpio_get_level）。
 *  用于 SPIready 边沿中断，避免在 cache 被禁用期间访问 Flash 代码。
 */
static int IRAM_ATTR readReadyLevelRaw(void)
{
    return (REG_READ(GPIO_IN_REG) >> SPI_READY_GPIO) & 0x1;
}

/* 前向声明：waitSpiDataReady 定义在文件下方（waitSpiReady 之后），
 * 但 serviceRingOnce / otaPollResponseUnlocked 等更早出现的方法需要它。 */
static esp_err_t waitSpiDataReady(uint32_t timeout_ms);

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

// ============================================================
// SPI Worker：唯一持 SPI 的任务；ISR 只通知本任务
// ============================================================

bool SpiMaster::onWorker() const
{
    return m_workerTask != nullptr && xTaskGetCurrentTaskHandle() == m_workerTask;
}

bool SpiMaster::pushEvent(uint8_t type, const uint8_t *payload, size_t len)
{
    if (m_eventCount >= kEventSlots) {
        /* 队列满：丢最旧一帧，保证新事件可进 */
        m_eventTail = (uint8_t)((m_eventTail + 1U) % kEventSlots);
        m_eventCount--;
        if (m_eventSem) {
            (void)xSemaphoreTake(m_eventSem, 0);
        }
    }
    EventSlot &slot = m_eventQ[m_eventHead];
    slot.type = type;
    slot.len = (uint16_t)((len > sizeof(slot.data)) ? sizeof(slot.data) : len);
    if (payload && slot.len > 0U) {
        memcpy(slot.data, payload, slot.len);
    }
    m_eventHead = (uint8_t)((m_eventHead + 1U) % kEventSlots);
    m_eventCount++;
    if (m_eventSem) {
        (void)xSemaphoreGive(m_eventSem);
    }
    return true;
}

bool SpiMaster::popEvent(uint8_t *type, uint8_t *payload, size_t max_len, size_t *len)
{
    if (m_eventCount == 0U) {
        return false;
    }
    EventSlot &slot = m_eventQ[m_eventTail];
    if (type) {
        *type = slot.type;
    }
    size_t copy_len = slot.len;
    if (copy_len > max_len) {
        copy_len = max_len;
    }
    if (len) {
        *len = copy_len;
    }
    if (payload && copy_len > 0U) {
        memcpy(payload, slot.data, copy_len);
    }
    m_eventTail = (uint8_t)((m_eventTail + 1U) % kEventSlots);
    m_eventCount--;
    return true;
}

void SpiMaster::setSuppressRingService(bool suppress)
{
    m_suppressRingService = suppress;
    if (suppress) {
        /* 丢弃已锁存的按铃标志，避免解除后立刻误读残留边沿 */
        m_gd32RequestPending = false;
    }
}

void SpiMaster::confirmTxSeqAccepted()
{
    /* 业务 ACK 已确认：保证下一包使用 lastSentSeq+1，即使头 ack_seq 曾失配 */
    m_txSeq = (uint8_t)(m_lastSentSeq + 1U);
    m_rxAckedSeq = m_lastSentSeq;
    m_lastRxAckMatched = true;
}

esp_err_t SpiMaster::serviceRingOnce()
{
    /* OTA 中继期间禁止按铃读：OTA ACK/WAIT 也拉低 PB6，偷读会导致 poll 空等。 */
    if (m_suppressRingService) {
        m_gd32RequestPending = false;
        return ESP_ERR_NOT_FOUND;
    }

    /* 仅 worker 调用。有 pending 或 DATA_READY 低时读一帧 EVENT。 */
    if (!m_gd32RequestPending) {
        /* 无 ISR 标志时，若线已低仍尝试读（丢边沿兜底） */
        if (readReadyLevelRaw() != 0) {
            return ESP_ERR_NOT_FOUND;
        }
    }
    m_gd32RequestPending = false;
    m_linkState.enterBusy(HandshakeState::SERVING_GD32_RING);

    if (waitSpiDataReady(3U) != ESP_OK) {
        m_linkState.exitBusy();
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t rx_raw[SPI_OTA_FRAME_MAX];
    size_t rx_len = 0U;
    esp_err_t ret = readV2FrameVarlen(rx_raw, sizeof(rx_raw), &rx_len, 500U);
    m_linkState.exitBusy();

    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t found_type = 0;
    const uint8_t *found_payload = nullptr;
    size_t found_len = 0;
    size_t frame_offset = 0;
    if (!otaFindFrame(rx_raw, rx_len, &found_type, &found_payload,
                      &found_len, &frame_offset)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (found_type == V2_TYPE_CREDIT_UPDATE) {
        DEBUG_DBG("SPI", "worker CREDIT_UPDATE credit=%u", (unsigned)m_remoteCredit);
        return ESP_ERR_NOT_FOUND;
    }
    /* RESPONSE（OTA ACK/WAIT/ERROR）绝不能当 EVENT 入队；否则 OTA poll 丢响应。
     * 此处帧已被时钟读走，只能记统计；真正防护靠 m_suppressRingService。 */
    if (found_type == SPI_OTA_RSP_ACK || found_type == SPI_OTA_RSP_WAIT ||
        found_type == SPI_OTA_RSP_ERROR || found_type == V2_TYPE_RSP_ACK ||
        found_type == V2_TYPE_RSP_WAIT || found_type == V2_TYPE_RSP_ERROR) {
        static uint32_t s_steal_logs = 0U;
        if ((++s_steal_logs % 16U) == 1U) {
            DEBUG_WARN("SPI", "serviceRing 读到 RESPONSE type=0x%02X（应禁止，log#%lu）",
                       found_type, (unsigned long)s_steal_logs);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }
    {
        LockGuard lock(m_mutex);
        (void)pushEvent(found_type, found_payload, found_len);
    }
    return ESP_OK;
}

void SpiMaster::executeJob(Job &job)
{
    /* 已在 worker 上下文；下列 public API 见 onWorker() 后走实现体，不再二次入队。 */
    switch (job.kind) {
    case JobKind::Transact:
        job.result = otaSendAndPoll(job.type,
                                    job.payload_len ? job.payload : nullptr,
                                    job.payload_len,
                                    job.out_type, job.out_payload, job.out_max,
                                    job.out_len, job.timeout_ms);
        break;
    case JobKind::Poll:
        job.result = otaPollResponse(job.out_type, job.out_payload, job.out_max,
                                     job.out_len, job.timeout_ms);
        break;
    case JobKind::Hello:
        job.result = hello(job.timeout_ms, job.role_hint);
        break;
    case JobKind::EnterBoot:
        job.result = appRequestBootloader();
        break;
    case JobKind::Ping:
        job.result = appPing(job.timeout_ms, job.out_uptime, job.out_version);
        break;
    case JobKind::WaitBoot:
        job.result = waitForBootloaderReady(job.timeout_ms);
        break;
    case JobKind::ServiceRing:
        job.result = serviceRingOnce();
        break;
    default:
        job.result = ESP_ERR_INVALID_ARG;
        break;
    }
}

esp_err_t SpiMaster::submitJob(Job &job)
{
    if (m_jobQueue == nullptr || m_workerTask == nullptr || onWorker()) {
        executeJob(job);
        return job.result;
    }

    StaticSemaphore_t done_buf;
    job.done = xSemaphoreCreateBinaryStatic(&done_buf);
    if (job.done == nullptr) {
        executeJob(job);
        return job.result;
    }
    (void)xSemaphoreTake(job.done, 0);

    Job *ptr = &job;
    const TickType_t qwait = pdMS_TO_TICKS(
        (job.timeout_ms > 0U ? job.timeout_ms : 1000U) + 2000U);
    if (xQueueSend(m_jobQueue, &ptr, qwait) != pdTRUE) {
        DEBUG_WARN("SPI", "worker job queue full/timeout kind=%u", (unsigned)job.kind);
        return ESP_ERR_TIMEOUT;
    }
    /* 唤醒可能阻塞在 notify wait 的 worker */
    xTaskNotify(m_workerTask, 0x10U, eSetBits);

    const TickType_t dwait = pdMS_TO_TICKS(
        (job.timeout_ms > 0U ? job.timeout_ms : 1000U) + 10000U);
    if (xSemaphoreTake(job.done, dwait) != pdTRUE) {
        DEBUG_ERR("SPI", "worker job done timeout kind=%u", (unsigned)job.kind);
        return ESP_ERR_TIMEOUT;
    }
    return job.result;
}

void SpiMaster::workerEntry(void *arg)
{
    static_cast<SpiMaster *>(arg)->workerLoop();
}

void SpiMaster::workerLoop()
{
    m_workerTask = xTaskGetCurrentTaskHandle();
    m_spiTaskHandle = m_workerTask;
    DEBUG_INFO("SPI", "SPI worker started, handle=%p", (void *)m_workerTask);

    for (;;) {
        /* ★ 只在 ISR 置位 m_gd32RequestPending 时读 EVENT。
         * 禁止见 DATA_READY 低就 serviceRing：命令 HELLO_ACK/OTA 响应也会拉低
         * PB6，会被偷走当事件 → 业务 poll 全 0xFF，OTA/ENTER_BOOT 全挂。
         * OTA 中继期间 m_suppressRingService 屏蔽按铃读。 */
        if (m_gd32RequestPending && !m_suppressRingService) {
            (void)serviceRingOnce();
        } else if (m_suppressRingService) {
            m_gd32RequestPending = false;
        }

        Job *job = nullptr;
        if (xQueueReceive(m_jobQueue, &job, pdMS_TO_TICKS(1)) == pdTRUE && job != nullptr) {
            executeJob(*job);
            if (job->done) {
                xSemaphoreGive(job->done);
            }
            continue;
        }

        /* 无 job：等 GPIO / 提交唤醒。
         * 超时只是无事件时的兜底回扫周期；ISR 的 xTaskNotifyFromISR 会立即唤醒
         * 本任务，响应延迟不由这里决定。过短会令 spi_worker(优先级6) 忙转压住
         * httpd(优先级5)，导致 /system/status、/logs 响应不来 → 网页收不到消息。 */
        uint32_t bits = 0U;
        (void)xTaskNotifyWait(0U, 0xFFFFFFFFU, &bits, pdMS_TO_TICKS(25));
        if (m_suppressRingService) {
            m_gd32RequestPending = false;
        } else if ((bits & 0x01U) || m_gd32RequestPending) {
            (void)serviceRingOnce();
        }
    }
}

/**
 * @brief GPIO 边沿中断回调（SPIready 线跳变）。
 *
 * - 下降沿：GD32 主动按铃（请求 ESP32 来读）。若当前空闲则置
 *   m_gd32RequestPending 并唤醒 SPI 任务。
 * - 上升沿：GD32 释放 SPIready（WAITING_RESPONSE 态），唤醒等待任务。
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
        if (self->m_suppressRingService) {
            /* OTA 中继：PB6 低是 WAIT/ACK 就绪，留给业务 poll，不按 EVENT 处理 */
        } else if (self->m_linkState.acceptRing()) {
            /* GD32 按铃：仅 IDLE 态接受 */
            self->m_gd32RequestPending = true;
            if (self->m_spiTaskHandle) {
                BaseType_t hp = pdFALSE;
                xTaskNotifyFromISR(self->m_spiTaskHandle, 0x01U,
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
     * 超时返回 ESP_ERR_TIMEOUT，禁止调用方假装“已就绪”后盲发。 */
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        if (readReadyLevelRaw() == 1) { return ESP_OK; }
        vTaskDelay(pdMS_TO_TICKS(1) > 0 ? pdMS_TO_TICKS(1) : 1);
    }
    m_stats.ready_timeouts++;
    return ESP_ERR_TIMEOUT;
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
    return transferClockedFlags(tx_buf, tx_len, rx_buf, rx_len, timeout_ms, 0U);
}

esp_err_t SpiMaster::transferClockedFlags(const uint8_t *tx_buf, size_t tx_len,
                                          uint8_t *rx_buf, size_t rx_len,
                                          uint32_t timeout_ms, uint32_t trans_flags)
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
    t.flags = trans_flags;
    t.length = total_len * 8U;
    t.rxlength = (rx_len > 0U) ? (total_len * 8U) : 0U;
    t.tx_buffer = tx_ptr;
    t.rx_buffer = (rx_len > 0U) ? rx_ptr : NULL;

    /* 禁止 portMAX_DELAY：SPI 硬件/从机异常时 worker 会永久卡死，
     * 表现即 OTA「转发进度 1/1117 后完全沉默」。所有等待必须有上界。 */
    const uint32_t effective_ms = (timeout_ms == 0U) ? 500U
        : (timeout_ms < 50U ? 50U : timeout_ms);
    const TickType_t queue_wait = pdMS_TO_TICKS(effective_ms);
    esp_err_t ret = spi_device_queue_trans(m_spiDevice, &t, queue_wait);
    if (ret != ESP_OK) {
        DEBUG_WARN("SPI", "spi_device_queue_trans fail: %s len=%u",
                   esp_err_to_name(ret), (unsigned)(total_len * 8U));
        return ret;
    }

    spi_transaction_t *ret_trans = NULL;
    /* 结果等待：调用方超时 + 余量，硬上限 2s，绝不用无限等 */
    uint32_t result_ms = effective_ms + 200U;
    if (result_ms < 100U) {
        result_ms = 100U;
    }
    if (result_ms > 2000U) {
        result_ms = 2000U;
    }
    const TickType_t result_wait = pdMS_TO_TICKS(result_ms);
    ret = spi_device_get_trans_result(m_spiDevice, &ret_trans, result_wait);
    if (ret != ESP_OK) {
        /* 再给一次有界抢救（最多 1s），清掉可能残留的 in-flight 事务。
         * 旧代码用 portMAX_DELAY —— GD32 不响应时 SPI worker 永久死锁。 */
        DEBUG_WARN("SPI", "spi_device_get_trans_result timeout: %s len=%u, bounded retry 1s",
                   esp_err_to_name(ret), (unsigned)(total_len * 8U));
        ret = spi_device_get_trans_result(m_spiDevice, &ret_trans, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            DEBUG_ERR("SPI", "spi_device_get_trans_result 放弃: %s（可能需复位 SPI）",
                      esp_err_to_name(ret));
            /* 尽力排空 ret_queue，避免后续 queue_trans 永久堵死 */
            spi_transaction_t *junk = nullptr;
            while (spi_device_get_trans_result(m_spiDevice, &junk, 0) == ESP_OK) {
                (void)junk;
            }
            return ESP_ERR_TIMEOUT;
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

/**
 * @brief 读一帧响应（单 CS 固定预算，再 v2_parse 截真实长度）。
 *
 * 不用两段 KEEP_ACTIVE：Boot 在 CS 释放时无论是否读完都清 s_txPending，
 * 中途断 CS 会丢 HELLO_ACK，后续 MISO 全 0xFF（线上已复现）。
 * 预算 128B 对齐 Boot TX DMA 满缓冲；App 大 EVENT 仍可到 256（out_max）。
 */
esp_err_t SpiMaster::readV2FrameVarlen(uint8_t *out_frame, size_t out_max, size_t *out_len,
                                       uint32_t timeout_ms)
{
    if (out_frame == nullptr || out_len == nullptr || out_max < V2_HEADER_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (m_spiDevice == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_len = 0U;

    size_t budget = spiRespReadBudget();
    if (budget > out_max) {
        budget = out_max;
    }
    if (budget > kSpiTransferMaxBytes) {
        budget = kSpiTransferMaxBytes;
    }

    uint8_t dummy[kSpiTransferMaxBytes];
    memset(dummy, 0xFF, budget);
    memset(out_frame, 0xFF, budget);

    esp_err_t ret = transferClocked(dummy, budget, out_frame, budget, timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    v2_header_t hdr;
    const uint8_t *pload = nullptr;
    if (!v2_parse_frame(out_frame, budget, &hdr, &pload)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t total = V2_HEADER_LEN + (size_t)hdr.payload_len + V2_CRC_LEN;
    if (total > out_max) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* parse 可能从 offset>0 找到帧：搬到缓冲区头，方便调用方 */
    if (hdr.frame_offset > 0U && hdr.frame_offset + total <= budget) {
        memmove(out_frame, out_frame + hdr.frame_offset, total);
    }
    *out_len = total;
    return ESP_OK;
}

// ============================================================
// OTA 帧工具
// ============================================================

/*
 * otaBuildFrame：使用当前 session/seq 成员填充 v2 帧头，FLAGS=REQUEST。
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
 * otaFindFrame：委托 v2_parse_frame + SESSION/SEQ 校验。
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

        /* 响应归属：ack_seq 应等于 m_lastSentSeq。
         *
         * ★ 关键：RESPONSE（ACK/WAIT/ERROR）在 CS 上已被读走，GD32 会清 s_txPending。
         * 若因 ack_seq 失配直接 return false，业务层永远看不到该帧 → 空等/重发/卡死。
         * 策略：CRC+session 合法的 RESPONSE 一律交给调用方；用 m_lastRxAckMatched
         * 标记是否归属当前请求。业务层以结构化 payload（BA C0+seq）做最终判定。 */
        if (hdr.flags & V2_FLAG_RESPONSE) {
            self.m_lastRxAckMatched = (hdr.ack_seq == self.m_lastSentSeq);
            if (!self.m_lastRxAckMatched) {
                static uint32_t s_stale_logs = 0U;
                if ((++s_stale_logs % 32U) == 1U) {
                    DEBUG_DBG("SPI", "v2 stale RESPONSE delivered: type=0x%02X ack_seq=%u expected=%u (log#%lu)",
                              hdr.type, hdr.ack_seq, self.m_lastSentSeq,
                              (unsigned long)s_stale_logs);
                }
            } else {
                self.m_rxAckedSeq = self.m_lastSentSeq;
            }
        } else {
            self.m_lastRxAckMatched = false;
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
    m_eventHead = m_eventTail = m_eventCount = 0;

    /* job 队列 + 事件信号量 + SPI worker。
     * worker 启动后可能立刻抢 m_mutex；init 持锁时 worker 会阻塞在 LockGuard，
     * init 返回释放锁后 worker 继续 —— 不会死锁。 */
    if (m_jobQueue == nullptr) {
        m_jobQueue = xQueueCreate(kJobQueueLen, sizeof(Job *));
        if (m_jobQueue == nullptr) {
            DEBUG_ERR("SPI", "SPI job queue create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (m_eventSem == nullptr) {
        m_eventSem = xSemaphoreCreateCounting(kEventSlots, 0);
        if (m_eventSem == nullptr) {
            DEBUG_ERR("SPI", "SPI event sem create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (m_workerTask == nullptr) {
        BaseType_t ok = xTaskCreate(workerEntry, "spi_worker", TASK_STACK_SPI_WORKER, this, 6, &m_workerTask);
        if (ok != pdPASS || m_workerTask == nullptr) {
            DEBUG_ERR("SPI", "SPI worker task create failed");
            m_workerTask = nullptr;
            return ESP_ERR_NO_MEM;
        }
        DEBUG_INFO("SPI", "SPI worker task created");
    }
    m_spiTaskHandle = m_workerTask;

    return ESP_OK;
}

// ============================================================
// SPI OTA 协议
// ============================================================
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

    /* CREDIT 背压（包间节奏）：
     * remoteCredit==0 禁止发新 REQUEST；等 CREDIT_UPDATE / 帧头 credit。
     * HELLO / 链路控制不受 credit 限制（session 建立前 credit 可能为 0）。 */
    if (type != V2_TYPE_LINK_HELLO && m_remoteCredit == 0U) {
        const TickType_t cred_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(
            timeout_ms > 0U ? timeout_ms : 500U);
        while (m_remoteCredit == 0U && xTaskGetTickCount() < cred_deadline) {
            /* 有 DATA_READY 时变长读一帧（可能是 CREDIT_UPDATE 或陈旧响应） */
            if (waitSpiDataReady(5U) == ESP_OK) {
                uint8_t drain[SPI_OTA_FRAME_MAX];
                size_t drain_len = 0U;
                if (readV2FrameVarlen(drain, sizeof(drain), &drain_len, 100U) == ESP_OK) {
                    (void)otaFindFrame(drain, drain_len, nullptr, nullptr, nullptr, nullptr);
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        if (m_remoteCredit == 0U) {
            DEBUG_WARN("SPI", "remoteCredit=0, refuse TX type=0x%02X", type);
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* 同步单 CS 传输：按帧长发送；延迟一拍下本笔 RX 为陈旧响应。 */
    m_linkState.enterBusy(HandshakeState::WAITING_RESPONSE);
    /* v2 延迟一拍：PB6 低表示 GD32 有上一笔响应待读，step1 的 transferClocked
     * 会自然把它当陈旧响应读走，不需要额外 drain（drain 会多消耗一个 CS 周期，
     * 可能误吃掉主循环刚排好的 ACK 导致后续包永久丢 ACK）。
     *
     * ★ 不调 waitSpiReady（等 PB6 变高）：v2 DATA_READY 下 PB6 低 = GD32 有待发
     * 响应，OTA 中继期间几乎持续为低。旧代码每包忙等 200ms 超时，21 包累计
     * ~4.2s 纯浪费，最终拖垮转发预算导致卡死。CS 本身即互斥，无需额外就绪等待。 */
    m_linkState.enterBusy(HandshakeState::MASTER_SENDING);

    const size_t xfer_len = spiXferBytes(tx_len);
    uint8_t tx_padded[SPI_OTA_FRAME_MAX];
    if (tx_len > sizeof(tx_padded) || xfer_len > sizeof(tx_padded)) {
        DEBUG_ERR("SPI", "OTA frame %u exceeds tx_padded %u", (unsigned)tx_len, (unsigned)sizeof(tx_padded));
        m_linkState.exitBusy();
        return ESP_ERR_INVALID_SIZE;
    }
    memset(tx_padded, 0xFF, xfer_len);
    memcpy(tx_padded, tx_frame, tx_len);

    uint8_t rx_raw[SPI_OTA_FRAME_MAX];
    memset(rx_raw, 0xFF, xfer_len);

    esp_err_t ret = transferClocked(tx_padded, xfer_len, rx_raw, xfer_len, timeout_ms);
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
        /* expect_stale（两步法 step1）读到的是延迟一拍陈旧帧，序号由 poll 成功时推进。
         * 此处若自增会导致 job 间隙/超时后 resend 使用错误 TX_SEQ。 */
        if (!expect_stale) {
            m_txSeq = (uint8_t)(m_txSeq + 1U);
        }
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
    if (!onWorker() && m_workerTask != nullptr) {
        Job job{};
        job.kind = JobKind::Poll;
        job.timeout_ms = timeout_ms;
        job.out_type = resp_type;
        job.out_payload = resp_payload;
        job.out_max = resp_max;
        job.out_len = resp_len;
        return submitJob(job);
    }
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
    if (timeout_ms == 0U) {
        timeout_ms = 1U;
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint32_t attempt = 0;
    bool did_spi_read = false;

    while (xTaskGetTickCount() <= deadline) {
        ++attempt;

        /* 优先等 DATA_READY；超时仍做有限次盲 poll。
         * 短 timeout（OTA 紧循环 50~150ms）必须保证至少 1 次 SPI 读，
         * 否则 PB6 偶发未拉低时整段切片空转 → 只见 “timeout: 30 ms” 然后失败。 */
        m_linkState.enterBusy(HandshakeState::WAITING_RESPONSE);
        TickType_t now = xTaskGetTickCount();
        uint32_t remain_ms = (deadline > now) ? ((deadline - now) * portTICK_PERIOD_MS) : 0U;
        if (remain_ms == 0U && did_spi_read) {
            break;
        }
        if (remain_ms == 0U) {
            remain_ms = 1U; /* 强制末次读 */
        }

        const uint32_t ready_slice = (attempt == 1U)
            ? ((timeout_ms >= 80U) ? 40U : timeout_ms)
            : 15U;
        const uint32_t wait_ms = (ready_slice < remain_ms) ? ready_slice : remain_ms;
        const bool ready = (waitSpiDataReady(wait_ms) == ESP_OK);

        now = xTaskGetTickCount();
        remain_ms = (deadline > now) ? ((deadline - now) * portTICK_PERIOD_MS) : 0U;
        const bool last_chance = (remain_ms == 0U) || (attempt >= 6U && !did_spi_read);
        /* 无 ready：前 2 轮盲读；之后每 8 轮盲读；deadline 末强制盲读一次 */
        if (!ready && attempt > 2U && (attempt % 8U) != 0U && !last_chance) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_DCU_OTA_POLL_BACKOFF_MS));
            continue;
        }
        if (remain_ms == 0U) {
            remain_ms = 20U; /* 末次 SPI 事务给足一点时钟时间 */
        }
        const uint32_t spi_timeout = (remain_ms > 200U) ? 200U : remain_ms;

        uint8_t rx_raw[SPI_OTA_FRAME_MAX];
        size_t rx_len = 0U;
        esp_err_t ret = readV2FrameVarlen(rx_raw, sizeof(rx_raw), &rx_len, spi_timeout);
        did_spi_read = true;
        if (ret != ESP_OK) {
            if (ret == ESP_ERR_INVALID_RESPONSE) {
                vTaskDelay(pdMS_TO_TICKS(CONFIG_DCU_OTA_POLL_BACKOFF_MS));
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            if (xTaskGetTickCount() > deadline) {
                break;
            }
            continue;
        }

        /* DEBUG: 限频，避免 OTA 紧循环刷屏 */
        { static uint32_t s_pdbg = 0;
          if ((++s_pdbg % 40) == 1 || (rx_raw[0] != 0xFF && rx_raw[0] != 0x00)) {
            v2_header_t dh; const uint8_t *dp = nullptr;
            bool parsed = v2_parse_frame(rx_raw, rx_len, &dh, &dp);
            if (parsed) {
              DEBUG_DBG("SPI", "poll att=%u type=0x%02X ack_seq=%u lastSent=%u",
                        (unsigned)attempt, (unsigned)dh.type, (unsigned)dh.ack_seq,
                        (unsigned)m_lastSentSeq);
            }
          }
        }

        uint8_t found_type = 0;
        const uint8_t *found_payload = nullptr;
        size_t found_len = 0;
        size_t frame_offset = 0;

        if (otaFindFrame(rx_raw, rx_len, &found_type, &found_payload, &found_len, &frame_offset)) {
            if (found_type == V2_TYPE_CREDIT_UPDATE) {
                DEBUG_DBG("SPI", "poll got CREDIT_UPDATE credit=%u", (unsigned)m_remoteCredit);
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            if (expected_type != 0xFFU && found_type != expected_type &&
                found_type != SPI_OTA_RSP_WAIT) {
                DEBUG_DBG("SPI", "poll type mismatch: got=0x%02X want=0x%02X",
                          found_type, expected_type);
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            if (resp_type) *resp_type = found_type;
            if (resp_payload && resp_max > 0) {
                size_t copy_len = (found_len < resp_max) ? found_len : resp_max;
                if (found_payload && copy_len > 0) {
                    memcpy(resp_payload, found_payload, copy_len);
                } else if (copy_len > 0) {
                    memset(resp_payload, 0, copy_len);
                }
            }
            if (resp_len) *resp_len = found_len;

            /* WAIT 立刻返回，由 OTA 紧循环再 poll（勿在此空转） */
            if (found_type == SPI_OTA_RSP_WAIT) {
                m_linkState.exitBusy();
                return ESP_OK;
            }
            if (m_lastRxAckMatched) {
                m_txSeq = (uint8_t)(m_lastSentSeq + 1U);
            }
            m_linkState.exitBusy();
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    m_linkState.exitBusy();
    m_stats.ready_timeouts++;
    /* 短切片超时是 OTA 正常紧循环，勿 WARN 刷爆 ring/网页 */
    if (timeout_ms >= 200U) {
        DEBUG_WARN("SPI", "OTA response timeout: %lu ms", (unsigned long)timeout_ms);
    } else {
        DEBUG_DBG("SPI", "OTA poll slice timeout: %lu ms", (unsigned long)timeout_ms);
    }
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief ESP32→GD32 发送命令 + 轮询读取真实响应（两步法，原子持锁）。
 *
 * 解决 GD32 "延迟一拍"协议：transact 首笔 CS 读到过期响应，
 * otaPollResponse 读取本笔命令的真实响应。命令只发送一次，不重发。
 * 适用于所有 ESP32→GD32 方向的命令（CAN 发送/模式切换/ID 快照等）。
 */
esp_err_t SpiMaster::transact(uint8_t type, const uint8_t *payload, size_t payload_len,
                              uint8_t *resp_type, uint8_t *resp_payload,
                              size_t resp_max, size_t *resp_len, uint32_t timeout_ms)
{
    if (payload_len > SPI_OTA_FRAME_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!onWorker() && m_workerTask != nullptr) {
        Job job{};
        job.kind = JobKind::Transact;
        job.timeout_ms = timeout_ms;
        job.type = type;
        job.payload_len = payload_len;
        if (payload && payload_len > 0U) {
            memcpy(job.payload, payload, payload_len);
        }
        job.out_type = resp_type;
        job.out_payload = resp_payload;
        job.out_max = resp_max;
        job.out_len = resp_len;
        return submitJob(job);
    }
    return otaSendAndPoll(type, payload, payload_len,
                          resp_type, resp_payload, resp_max, resp_len, timeout_ms);
}

bool SpiMaster::takeEvent(uint8_t *type, uint8_t *payload, size_t max_len, size_t *len,
                          uint32_t timeout_ms)
{
    /* 快速路径：先非阻塞尝试一次 */
    {
        LockGuard lock(m_mutex);
        if (popEvent(type, payload, max_len, len)) {
            if (m_eventSem) {
                (void)xSemaphoreTake(m_eventSem, 0);
            }
            return true;
        }
    }

    /* 队列空：若 timeout_ms==0 直接返回，否则在信号量上等待 */
    if (timeout_ms == 0U || m_eventSem == nullptr) {
        return false;
    }

    /* 循环等待：信号量到达后重新持锁取，取空则继续等（防虚假唤醒） */
    const TickType_t deadline_ticks = pdMS_TO_TICKS(timeout_ms);
    while (true) {
        if (xSemaphoreTake(m_eventSem, deadline_ticks) != pdTRUE) {
            return false; /* 超时 */
        }
        LockGuard lock(m_mutex);
        if (popEvent(type, payload, max_len, len)) {
            return true;
        }
        /* 信号量被消费但队列空（可能被并发 takeEvent 抢走），继续等 */
    }
}

esp_err_t SpiMaster::otaSendAndPoll(uint8_t type, const uint8_t *payload, size_t payload_len,
                                    uint8_t *resp_type, uint8_t *resp_payload,
                                    size_t resp_max, size_t *resp_len, uint32_t timeout_ms)
{
    /* 非 worker 调用：转 transact 入队，避免多任务直接碰 SPI */
    if (!onWorker() && m_workerTask != nullptr) {
        return transact(type, payload, payload_len,
                        resp_type, resp_payload, resp_max, resp_len, timeout_ms);
    }

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

    /* 等 GD32 完成 onCsRelease；poll 内仅在 DATA_READY 低时变长读（不盲 CS）。 */
    {
        const TickType_t gap = (pdMS_TO_TICKS(1) > 0) ? pdMS_TO_TICKS(1) : 1;
        vTaskDelay(gap);
    }

    /* 步骤 2: 轮询读取真实响应（变长读，不重发命令） */
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
                /* 延迟一拍连发 2 次，避免 DATA_READY/短读导致全 0xFF */
                const size_t xfer_len = spiXferBytes(hello_len);
                for (uint32_t hi = 0U; hi < 2U; ++hi) {
                    m_linkState.enterBusy(HandshakeState::WAITING_RESPONSE);
                    (void)waitSpiReady(SPI_READY_TIMEOUT_MS);
                    m_linkState.enterBusy(HandshakeState::MASTER_SENDING);
                    uint8_t tx_p[SPI_OTA_FRAME_MAX];
                    uint8_t rx_r[SPI_OTA_FRAME_MAX];
                    memset(tx_p, 0xFF, xfer_len);
                    memcpy(tx_p, hello_frame, hello_len);
                    memset(rx_r, 0xFF, xfer_len);
                    esp_err_t hr = transferClocked(tx_p, xfer_len, rx_r, xfer_len, 2000U);
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
                        m_remoteCredit = hdr.credit;
                        m_v2Active = true;
                        m_stats.resyncs++;
                        DEBUG_INFO("SPI", "transactWithRetry: HELLO 重同步成功 role=%u",
                                   m_gd32Role);
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
    if (!onWorker() && m_workerTask != nullptr) {
        Job job{};
        job.kind = JobKind::EnterBoot;
        job.timeout_ms = 5000U;
        return submitJob(job);
    }

    LockGuard lock(m_mutex);
    DEBUG_INFO("SPI", "请求 GD32 APP 进入 Bootloader (v2)...");

    m_gd32RequestPending = false;

    /* 先变长读最多 2 次，吞掉残留 0xC1（仅 DATA_READY 低时读，禁止固定长度盲读）。 */
    for (uint32_t d = 0U; d < 2U; ++d) {
        if (waitSpiDataReady(5U) != ESP_OK) {
            break;
        }
        uint8_t rx_raw[SPI_OTA_FRAME_MAX];
        size_t rx_len = 0U;
        m_linkState.enterBusy(HandshakeState::WAITING_RESPONSE);
        (void)readV2FrameVarlen(rx_raw, sizeof(rx_raw), &rx_len, 20U);
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
            vTaskDelay(pdMS_TO_TICKS(2));
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
                    /* 延迟一拍：连发 2 次 HELLO（≥64B 时钟），第 2 次 RX 为 ACK */
                    const size_t xlen = spiXferBytes(hl);
                    for (uint32_t hi = 0U; hi < 2U; ++hi) {
                        uint8_t tx_p[SPI_OTA_FRAME_MAX];
                        uint8_t rx_r[SPI_OTA_FRAME_MAX];
                        memset(tx_p, 0xFF, xlen);
                        memcpy(tx_p, hf, hl);
                        memset(rx_r, 0xFF, xlen);
                        m_linkState.enterBusy(HandshakeState::MASTER_SENDING);
                        (void)transferClocked(tx_p, xlen, rx_r, xlen, 200U);
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

        /* poll 最多 ~200ms：只认 0xB0+0xB5。优先 DATA_READY，超时仍单 CS 盲读。 */
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200U);
        uint8_t last_type = 0xFFU;
        uint32_t poll_i = 0U;
        while (xTaskGetTickCount() <= deadline) {
            ++poll_i;
            m_linkState.enterBusy(HandshakeState::WAITING_RESPONSE);
            (void)waitSpiDataReady(poll_i <= 2U ? 20U : 5U);

            uint8_t rx_raw[SPI_OTA_FRAME_MAX];
            size_t rx_len = 0U;
            ret = readV2FrameVarlen(rx_raw, sizeof(rx_raw), &rx_len, 40U);
            m_linkState.exitBusy();
            if (ret != ESP_OK) {
                vTaskDelay(gap);
                continue;
            }

            v2_header_t hdr;
            const uint8_t *pload = nullptr;
            if (!v2_parse_frame(rx_raw, rx_len, &hdr, &pload) ||
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
        const uint32_t kQuietMs = got_ack ? 100U : 30U;
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
    if (!onWorker() && m_workerTask != nullptr) {
        Job job{};
        job.kind = JobKind::WaitBoot;
        job.timeout_ms = timeout_ms;
        return submitJob(job);
    }

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
 * @brief 向 GD32 APP 发送存活探测（帧长发送 + DATA_READY 变长读）。
 *
 * 两步法（持锁）：otaTransferUnlocked(expect_stale) + otaPollResponseUnlocked。
 * GD32 App PING 响应 type=kRspAck(0x80)，
 * payload 16B = APP ver(4) + BOOT ver(4) + uptime_ms(4) + build_hash(4)。
 */
esp_err_t SpiMaster::appPing(uint32_t timeout_ms, uint32_t *uptime_sec, gd32_version_t *version)
{
    if (!onWorker() && m_workerTask != nullptr) {
        Job job{};
        job.kind = JobKind::Ping;
        job.timeout_ms = timeout_ms;
        job.out_uptime = uptime_sec;
        job.out_version = version;
        return submitJob(job);
    }

    LockGuard lock(m_mutex);
    constexpr uint8_t kAppPingType = 0xA1U;

    uint8_t resp_type = 0;
    uint8_t resp_payload[32];
    size_t resp_len = 0U;

    /* PING 无 payload，send 只发 1 帧 SPI（<50ms），不需要 500ms 预留。
     * 省下的时间给 poll，让 APP 命令层有更长窗口响应。 */
    const uint32_t send_timeout = (timeout_ms > 100U) ? 100U : timeout_ms;
    esp_err_t send_ret = otaTransferUnlocked(kAppPingType, nullptr, 0U,
                                             nullptr, nullptr, 0U, nullptr, send_timeout,
                                             /*expect_stale=*/true);
    if (send_ret != ESP_OK && send_ret != ESP_ERR_INVALID_RESPONSE) {
        DEBUG_DBG("SPI", "APP PING send failed: %s", esp_err_to_name(send_ret));
        return send_ret;
    }

    {
        const TickType_t gap = (pdMS_TO_TICKS(1) > 0) ? pdMS_TO_TICKS(1) : 1;
        vTaskDelay(gap);
    }

    const uint32_t poll_timeout = (timeout_ms > send_timeout)
                                  ? (timeout_ms - send_timeout) : 500U;
    esp_err_t err = otaPollResponseUnlocked(&resp_type, resp_payload, sizeof(resp_payload),
                                            &resp_len, poll_timeout, SPI_OTA_RSP_ACK);
    if (err != ESP_OK) {
        DEBUG_DBG("SPI", "APP PING poll failed: %s", esp_err_to_name(err));
        return err;
    }

    if (resp_type != SPI_OTA_RSP_ACK || resp_len < 12U) {
        DEBUG_DBG("SPI", "APP PING bad resp type=0x%02X len=%u", resp_type, (unsigned)resp_len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t *pload = resp_payload;
    if (version) {
        memset(version, 0, sizeof(gd32_version_t));
        version->app_major = pload[0];
        version->app_minor = pload[1];
        version->app_patch = pload[2];
        version->app_build = pload[3];
        if (resp_len >= 16U) {
            version->boot_major = pload[4];
            version->boot_minor = pload[5];
            version->boot_patch = pload[6];
            version->boot_build = pload[7];
        }
    }
    if (uptime_sec) {
        const uint8_t *ut = (resp_len >= 16U) ? &pload[8] : &pload[4];
        const uint32_t uptime_ms = (uint32_t)ut[0]
                                 | ((uint32_t)ut[1] << 8)
                                 | ((uint32_t)ut[2] << 16)
                                 | ((uint32_t)ut[3] << 24);
        *uptime_sec = uptime_ms / 1000U;
    }
    DEBUG_DBG("SPI", "APP PING v2 成功");
    return ESP_OK;
}

// ============================================================
// SPI Link v2 链路状态
// ============================================================
SpiMaster::SpiLinkStats SpiMaster::linkStats()
{
    /* OTA 中继期间 SPI worker 在 otaSendAndPoll 全程持有 m_mutex（发送+轮询 ACK，
     * 每包最长 6s）。若用 portMAX_DELAY 等锁，/system/status 会被阻塞整个传输阶段，
     * 前端进度完全不同步直到传输结束。
     * 统计仅为展示用途，拿不到锁时返回零值，绝不阻塞 status handler。 */
    SpiLinkStats ret{};
    if (m_mutex && xSemaphoreTake(m_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        ret = m_stats;
        xSemaphoreGive(m_mutex);
    }
    return ret;
}

SpiMaster::SpiLinkStatus SpiMaster::linkStatus()
{
    /* 状态字段为 volatile 单字节/双字读，原子性足够；统计复用 linkStats() 的有界取锁。 */
    SpiLinkStatus st{};
    st.version = 2U;
    st.gd32_role = m_gd32Role;
    st.session_id = m_sessionId;
    st.local_credit = m_localCredit;
    st.remote_credit = m_remoteCredit;
    st.stats = linkStats();
    return st;
}

esp_err_t SpiMaster::hello(uint32_t timeout_ms, uint8_t role_hint)
{
    if (!onWorker() && m_workerTask != nullptr) {
        Job job{};
        job.kind = JobKind::Hello;
        job.timeout_ms = timeout_ms;
        job.role_hint = role_hint;
        return submitJob(job);
    }

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

    /* HELLO 必须走延迟一拍全双工连发（与 Boot 实测一致）：
     * 第 N 次 CS 读到第 N-1 次 HELLO 的 ACK。不可只等 DATA_READY 再短读：
     * Boot TX 按 128 预装，过短/两段 CS 会清 pending → MISO 全 0xFF。 */
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint8_t rx_raw[SPI_OTA_FRAME_MAX];
    uint32_t attempt = 0U;
    uint8_t last_type = 0xFFU;
    uint8_t last_flags = 0U;
    bool saw_any_frame = false;
    uint8_t last_rx_head[16];
    memset(last_rx_head, 0xFF, sizeof(last_rx_head));

    const TickType_t gap_ticks = (pdMS_TO_TICKS(10) > 0) ? pdMS_TO_TICKS(10) : 1;
    const size_t xfer_len = spiXferBytes(hello_len); /* ≥64，盖住 HELLO_ACK */

    while (xTaskGetTickCount() <= deadline) {
        ++attempt;

        m_linkState.enterBusy(HandshakeState::WAITING_RESPONSE);
        (void)waitSpiReady(SPI_READY_TIMEOUT_MS);
        m_linkState.enterBusy(HandshakeState::MASTER_SENDING);

        uint8_t tx_padded[SPI_OTA_FRAME_MAX];
        memset(tx_padded, 0xFF, xfer_len);
        memcpy(tx_padded, hello_frame, hello_len);

        memset(rx_raw, 0xFF, xfer_len);
        esp_err_t ret = transferClocked(tx_padded, xfer_len, rx_raw, xfer_len, 200U);
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
            /* 延迟一拍旧 ACK：同 session 再发一次即可对齐 */
            DEBUG_DBG("SPI", "HELLO lag echo=0x%08lX expected=0x%08lX attempt=%lu",
                      (unsigned long)session_echo, (unsigned long)m_sessionId,
                      (unsigned long)attempt);
            vTaskDelay(gap_ticks);
            continue;
        }

        m_gd32Role = pload[4];
        m_localCredit = pload[5];
        m_remoteCredit = hdr.credit;
        m_v2Active = true;
        m_stats.resyncs++;
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

