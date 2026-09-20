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
#include "freertos/portmacro.h"
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
#include "esp_timer.h"
#include "esp_rom_sys.h"    // esp_timer_get_time()：worker job 执行耗时采样
#include "spi_master.h"
#include "spi_link_v2.h"
#include "app_config.h"
#include "monitor/debug_log.h"


static constexpr size_t kSpiTransferMaxBytes = 256U;

/** @brief 上一笔 CS 事务结束时刻（µs，worker 单线程读写）。
 *  transferClockedFlags 用它强制 CS 事务间最小间隔（0.4.4 RX 撕裂根因修复）。 */
static int64_t s_lastCsEndUs = 0;

/** @brief submitJob 池 Job 与 worker 之间的 finished/abandoned/in_use 交接。 */
static portMUX_TYPE s_spiJobMux = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief 发送路径 CS 时钟字节数。
 * Boot TX DMA 按满缓冲预装（kTxBufSize=128），主机过短时钟会：
 *  - 截断延迟一拍的响应；
 *  - 两段变长读若中途释放 CS，Boot 会把 s_txPending 当已交付清掉 → 后续全 0xFF。
 * 下限 64：覆盖 HELLO_ACK(~26)/短 ACK；上限 SPI_OTA_FRAME_MAX(256)。
 * APP 侧 TX 满缓冲 256：poll/ring 读预算见 spiRespReadBudget()。
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

/**
 * @brief 读响应时单 CS 时钟预算（按最近 HELLO 的 GD32 role 分支）。
 *  - BOOT：TX DMA 满缓冲 128 → 读 128（与 Git 稳定路径一致，避免 BOOT underrun）
 *  - APP / unknown：TX DMA 满缓冲 256 → 读 256，避免 0xC1 等长 EVENT 被截断
 * role 粘滞 boot 时靠 isAppOnlySpiType 纠正为 APP 后再走 256。
 */
static size_t spiRespReadBudget(void)
{
    const uint8_t role = SpiMaster::instance().gd32Role();
    if (role == V2_ROLE_BOOT) {
        return SPI_BOOT_TX_BUF_SIZE;
    }
    return SPI_APP_TX_BUF_SIZE;
}

/**
 * @brief 已知"短响应"的主动请求类型：应答帧 ≤ ~70B（实测 0xA1 PING ~60-70B、
 * DATA/REQ/FINISH ACK ~30-40B、0xB5/0xC0/0xC2/0xD1 ACK 均短、0xC4 ADC 20B）。
 * 对这些类型的主动 poll 可把读预算从 256 收紧到 96B，省 ~160B 0xFF 填充时钟
 * （@13.3MHz ≈ 96µs/帧）。排除 0xC3（ID 快照响应可能很长）、0xC1/0xC5
 * （分页响应 ~236B）与被动 0xC1 事件读。
 */
static bool isShortRespType(uint8_t type)
{
    switch (type) {
    case 0xA1U: case 0xA3U: case 0x02U: case 0x01U: case 0xF7U:
    case 0xB0U: case 0xC0U: case 0xC2U: case 0xD1U:
    case 0xC4U: case 0xC7U:
        return true;
    default:
        return false;
    }
}
/** 短响应读预算（覆盖最长 ~70B 应答 + 余量）。 */
static constexpr size_t kShortRespReadBudget = 96U;

/**
 * @brief 已知 GD32 APP 在 onCsRelease ISR 内 µs 级装载响应的主动请求类型。
 * 这些类型 CS 释放后无需 5ms 静默：waitSpiDataReady 由下降沿 ISR 精确唤醒
 * （与 0xD1/0xA1 同机制）。仅 BOOT 主循环处理的 OTA 命令（0x01/0x02/0x03）
 * 保留 5ms——过早盲读会打断 BOOT 的 EXTI/DMA 时序（实测 CHALLENGE 超时）。
 */
static bool isAppIsrFastRespType(uint8_t type)
{
    switch (type) {
    case 0xA1U: case 0xA3U: case 0xC0U: case 0xC1U: case 0xC2U:
    case 0xC3U: case 0xC4U: case 0xC5U: case 0xC7U: case 0xD1U:
    case 0xF7U:
        return true;
    default:
        return false;
    }
}

/** APP 独有业务 TYPE：见到即可纠正 m_gd32Role（BOOT 不会发这些）。 */
bool SpiMaster::isAppOnlySpiType(uint8_t type)
{
    switch (type) {
    case 0xC1U: /* CAN feedback */
    case 0xC3U: /* ID snapshot */
    case 0xC4U: /* ADC */
    case 0xC5U: /* motor state */
    case 0xC6U: /* motor version */
    case 0xD0U: /* motor OTA cmd（APP relay） */
    case 0xD1U:
    case 0xD2U:
        return true;
    default:
        return false;
    }
}

/* ── 去重辅助实现 ── */

void SpiMaster::copyFoundFrame(uint8_t *resp_type, uint8_t found_type,
                               uint8_t *resp_payload, size_t resp_max,
                               const uint8_t *found_payload, size_t found_len,
                               size_t *resp_len)
{
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
}

void SpiMaster::correctRoleFromType(uint8_t found_type, const char *src)
{
    if (isAppOnlySpiType(found_type) && m_gd32Role != V2_ROLE_APP) {
        DEBUG_INFO("SPI", "role %u→APP (saw type=0x%02X, %s)",
                   (unsigned)m_gd32Role, found_type, src);
        m_gd32Role = V2_ROLE_APP;
    }
}

/** @brief ISR-safe GPIO 电平读取（不调用 Flash 中的 gpio_get_level）。
 *  用于 SPIready 边沿中断，避免在 cache 被禁用期间访问 Flash 代码。
 */
static int IRAM_ATTR readReadyLevelRaw(void)
{
    return (REG_READ(GPIO_IN_REG) >> SPI_READY_GPIO) & 0x1;
}

/* 前向声明：waitSpiDataReady 定义在文件下方，
 * 但 serviceRingOnce / otaPollResponseUnlocked 等更早出现的方法需要它。 */
static esp_err_t waitSpiDataReady(uint32_t timeout_ms);

/** @brief DATA_READY 下降沿事件信号量（ISR give / waitSpiDataReady take）。
 *  二值信号量：多余边沿饱和为 1，消费方醒来后必须重查电平，
 *  陈旧 give 只多一次电平检查，无副作用。 */
static SemaphoreHandle_t s_readyEdgeSem = nullptr;

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

/** @brief 当前任务是否为 SPI worker（仅 worker 可直接走无锁路径）。 */
bool SpiMaster::onWorker() const
{
    return m_workerTask != nullptr && xTaskGetCurrentTaskHandle() == m_workerTask;
}

/**
 * @brief 将 GD32 EVENT 帧入队（worker 上下文）。
 * @param type     业务 TYPE（如 0xC1/0xC4）
 * @param payload  载荷指针，可为 nullptr
 * @param len      载荷长度
 * @return 始终 true；队列满时丢最旧一帧再写入
 */
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

/**
 * @brief 从事件队列弹出一帧（非阻塞）。
 * @param[out] type     业务 TYPE
 * @param[out] payload  拷贝载荷；可为 nullptr
 * @param max_len       payload 缓冲容量
 * @param[out] len      实际拷贝长度
 * @return true 取到一帧；false 队列空
 */
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

/**
 * @brief 暂停/恢复对 GD32 主动按铃的 serviceRing 处理（引用计数）。
 * @param suppress true=+1（OTA 等关键路径禁止偷读 PB6）；false=-1（钳位到 0）。
 *
 * GD32 中继（forwardFirmware）与电机 OTA feedTask 都可能持有抑制；任一
 * 持有者退出时若直接清位，会把另一方的保护误清（电机 OTA 收尾曾把进行中
 * 的 GD32 中继抑制清掉 → worker 偷读 OTA ACK/WAIT → poll 空等 6s）。
 */
void SpiMaster::setSuppressRingService(bool suppress)
{
    taskENTER_CRITICAL(&s_spiJobMux);
    uint32_t refs = m_ringSuppressRefs;
    if (suppress) {
        ++refs;
    } else if (refs > 0U) {
        --refs;
    }
    m_ringSuppressRefs = refs;
    const bool now_suppressed = (refs > 0U);
    taskEXIT_CRITICAL(&s_spiJobMux);

    m_suppressRingService = now_suppressed;
    if (now_suppressed) {
        /* 丢弃已锁存的按铃标志，避免解除后立刻误读残留边沿 */
        m_gd32RequestPending = false;
    }
}

/**
 * @brief ESP32 自我 OTA 期间暂停/恢复全部 SPI 活动。
 *
 * 暂停：worker 空转不读 SPI（GD32 按铃丢弃），submitJob 快速拒绝
 * （业务调用立即返回 ESP_ERR_INVALID_STATE，不排队不阻塞）。
 * 恢复：唤醒 worker 恢复正常。幂等，可多次调用。
 */
void SpiMaster::setSpiPaused(bool paused)
{
    m_spiPaused = paused;
    if (paused) {
        /* 丢弃已锁存的按铃标志：暂停期间不读 GD32，解除后勿误读残留边沿 */
        m_gd32RequestPending = false;
    }
    if (m_workerTask != nullptr) {
        xTaskNotify(m_workerTask, 0x10U, eSetBits);  /* 唤醒 worker 处理状态变化 */
    }
}

/**
 * @brief 业务侧确认本端 TX 序号已被对端接受，推进 m_txSeq。
 * @note  在收到明确业务 ACK 后调用，避免重发与序号失配。
 */
void SpiMaster::confirmTxSeqAccepted()
{
    /* 业务 ACK 已确认：保证下一包使用 lastSentSeq+1，即使头 ack_seq 曾失配 */
    m_txSeq = (uint8_t)(m_lastSentSeq + 1U);
    m_rxAckedSeq = m_lastSentSeq;
    m_lastRxAckMatched = true;
}

/**
 * @brief worker 单次服务 GD32 按铃：读一帧并入 EVENT 队列。
 * @return ESP_OK 入队成功；ESP_ERR_NOT_FOUND 无线/被抑制；其它为读失败
 */
esp_err_t SpiMaster::serviceRingOnce()
{
    /* OTA 中继期间禁止按铃读：OTA ACK/WAIT 也拉低 PB6，偷读会导致 poll 空等。 */
    if (m_suppressRingService) {
        m_gd32RequestPending = false;
        return ESP_ERR_NOT_FOUND;
    }

    /* 仅 worker 调用。有 pending 或 DATA_READY 低时读一帧 EVENT。
     * ★ pending 也必须验电平：业务响应（PING/OTA ACK）拉低 PB6 时链路忙，
     * ISR 误记 pending；响应被 poll 读走后 PB6 已回高，若不验电平会白等
     * waitSpiDataReady(3ms) 超时，把每笔业务周期拖慢 ~3ms（测速直方图
     * 79% 帧堆在 3-4ms 的根因）。真按铃时 PB6 在被读走前保持低，不误伤。 */
    if (readReadyLevelRaw() != 0) {
        m_gd32RequestPending = false;
        return ESP_ERR_NOT_FOUND;
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
    uint8_t found_flags = 0U;
    if (!otaFindFrame(rx_raw, rx_len, &found_type, &found_payload,
                      &found_len, &frame_offset, &found_flags)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (found_type == V2_TYPE_CREDIT_UPDATE) {
        DEBUG_DBG("SPI", "worker CREDIT_UPDATE credit=%u", (unsigned)m_remoteCredit);
        return ESP_ERR_NOT_FOUND;
    }
    /* RESPONSE（ACK/WAIT/ERROR/LINK_ERROR，含 0xC4 等回自身类型的响应）绝不能当
     * EVENT 入队；此处帧已被时钟读走，真正防护靠 m_suppressRingService。本分支
     * 只在 poll 已超时放弃、GD32 忙导致响应迟到时可达。
     * ★ 必须与 poll 成功路径一样推进 m_txSeq：ack_seq 匹配说明 GD32 已消费本端
     * TX_SEQ（缓存响应待重放）。若不推进，下一条命令（含重试）复用旧序号 →
     * GD32 判为重复请求直接回放缓存旧帧、新命令被吞 → 业务反复收到同一个
     * 旧 0x82/旧响应，命令永远不再执行（"一直读到 0x82"的根因）。 */
    if ((found_flags & V2_FLAG_RESPONSE) != 0U) {
        const bool ack_matched = m_lastRxAckMatched;
        if (ack_matched) {
            m_txSeq = (uint8_t)(m_lastSentSeq + 1U);
        }
        m_stats.ring_steals++;  /* 0.4.3：量化归因（/system/status 直读，不依赖 /logs） */
        m_stats.ring_steal_last_type = found_type;      /* 0.4.5：偷的什么帧 */
        m_stats.ring_steal_last_ack = ack_matched ? 1U : 0U;
        m_stats.ring_steal_last_p0 = (found_len > 0U) ? found_payload[0] : 0U;  /* 0.4.6 */
        m_stats.ring_steal_last_req = m_lastReqType;
        m_stats.ring_steal_last_p1 = (found_len > 1U) ? found_payload[1] : 0U;   /* 0.4.8 */
        m_stats.ring_steal_last_p2 = (found_len > 2U) ? found_payload[2] : 0U;
        m_stats.ring_steal_last_p3 = (found_len > 3U) ? found_payload[3] : 0U;
        static uint32_t s_steal_logs = 0U;
        if ((++s_steal_logs % 16U) == 1U) {
            DEBUG_WARN("SPI", "serviceRing 偷读迟到 RESPONSE t=0x%02X p0=0x%02X "
                       "ack_match=%u lastReq=0x%02X（poll 已超时，帧丢弃%s，log#%lu）",
                       found_type,
                       (found_len > 0U) ? found_payload[0] : 0U,
                       (unsigned)ack_matched, (unsigned)m_lastReqType,
                       ack_matched ? "、seq 已推进" : "",
                       (unsigned long)s_steal_logs);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }
    {
        LockGuard lock(m_mutex);
        /* APP 独有事件 → 纠正粘滞的 boot role（OTA 后 HELLO 未刷新时常见） */
        correctRoleFromType(found_type, "ring");
        (void)pushEvent(found_type, found_payload, found_len);
    }
    return ESP_OK;
}

/**
 * @brief worker 执行一条排队 Job（Transact/Hello/Ping/EnterBoot 等）。
 * @param job  入出参：写入 job.result
 */
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
    case JobKind::InitStatus:
        job.result = appInitStatus(job.timeout_ms, job.out_init_status);
        break;
    case JobKind::WaitBoot:
        job.result = waitForBootloaderReady(job.timeout_ms);
        break;
    default:
        job.result = ESP_ERR_INVALID_ARG;
        break;
    }
}

/**
 * @brief 从固定 Job 池取空闲槽；池耗尽返回 nullptr。
 * @note  in_use 在 critical 内置 true 并保持，禁止整体 Job{} 赋值清掉占用标志。
 */
SpiMaster::Job *SpiMaster::acquireJobSlot()
{
    if (!m_jobPoolReady) {
        return nullptr;
    }
    Job *slot = nullptr;
    taskENTER_CRITICAL(&s_spiJobMux);
    for (UBaseType_t i = 0; i < kJobPoolSize; ++i) {
        if (!m_jobPool[i].in_use) {
            slot = &m_jobPool[i];
            /* 清业务字段但 in_use 全程 true，避免与其它 acquire 竞态 */
            SemaphoreHandle_t done = slot->done;
            slot->kind = JobKind::Transact;
            slot->timeout_ms = 0U;
            slot->type = 0U;
            slot->expected_type = 0xFFU;
            slot->role_hint = 0U;
            slot->payload_len = 0U;
            slot->out_type = nullptr;
            slot->out_payload = nullptr;
            slot->out_max = 0U;
            slot->out_len = nullptr;
            slot->out_uptime = nullptr;
            slot->out_version = nullptr;
            slot->out_init_status = nullptr;
            slot->result = ESP_FAIL;
            slot->done = done;
            slot->finished = false;
            slot->abandoned = false;
            slot->executing = false;
            slot->in_use = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_spiJobMux);
    if (slot == nullptr) {
        return nullptr;
    }
    /* 仅本槽所有者 drain；不可在 critical 内 take */
    if (slot->done != nullptr) {
        while (xSemaphoreTake(slot->done, 0) == pdTRUE) {
        }
    }
    return slot;
}

/**
 * @brief 归还池槽（调用方已独占该 Job，不再与 worker 共享）。
 */
void SpiMaster::releaseJobSlot(Job *job)
{
    if (job == nullptr) {
        return;
    }
    if (job->done != nullptr) {
        while (xSemaphoreTake(job->done, 0) == pdTRUE) {
        }
    }
    job->out_type = nullptr;
    job->out_payload = nullptr;
    job->out_len = nullptr;
    job->out_uptime = nullptr;
    job->out_version = nullptr;
    job->out_init_status = nullptr;
    job->payload_len = 0U;
    job->result = ESP_FAIL;
    taskENTER_CRITICAL(&s_spiJobMux);
    job->finished = false;
    job->abandoned = false;
    job->executing = false;
    job->in_use = false;
    taskEXIT_CRITICAL(&s_spiJobMux);
}

/**
 * @brief worker 完成池 Job 后的收尾：give done；abandoned 时由 worker release 槽。
 * @param job  池内 Job 指针
 */
void SpiMaster::finishPoolJob(Job *job)
{
    if (job == nullptr) {
        return;
    }
    /* 先 give，再标 finished；提交方 take 成功后会 spin 等 finished 再 release。 */
    if (job->done) {
        (void)xSemaphoreGive(job->done);
    }
    taskENTER_CRITICAL(&s_spiJobMux);
    job->finished = true;
    const bool release_it = job->abandoned;
    taskEXIT_CRITICAL(&s_spiJobMux);
    if (release_it) {
        releaseJobSlot(job);
    }
}

/**
 * @brief 将 Job 提交给 SPI worker 串行执行（非 worker 线程走队列）。
 * @param job  任务描述（栈上字段拷入固定池槽再排队）
 * @return 业务结果码；池耗尽 ESP_ERR_NO_MEM；队列满/超时 ESP_ERR_TIMEOUT
 */
esp_err_t SpiMaster::submitJob(Job &job)
{
    /* ESP32 自我 OTA 暂停：快速拒绝，不排队不等待（worker 空转中）。 */
    if (m_spiPaused) {
        return ESP_ERR_INVALID_STATE;
    }
    if (m_jobQueue == nullptr || m_workerTask == nullptr || onWorker()) {
        executeJob(job);
        return job.result;
    }

    /* 跨任务：固定池 Job + 静态 binary 完成信号量（无 calloc/xSemaphoreCreate） */
    Job *pool_job = acquireJobSlot();
    if (pool_job == nullptr) {
        DEBUG_ERR("SPI", "submitJob: job pool exhausted kind=%u", (unsigned)job.kind);
        return ESP_ERR_NO_MEM;
    }

    /* 逐字段拷贝业务参数；禁止整体赋值覆盖 done/in_use */
    pool_job->kind = job.kind;
    pool_job->timeout_ms = job.timeout_ms;
    pool_job->type = job.type;
    pool_job->expected_type = job.expected_type;
    pool_job->role_hint = job.role_hint;
    pool_job->payload_len = job.payload_len;
    if (job.payload_len > 0U) {
        const size_t n = (job.payload_len > sizeof(pool_job->payload))
                             ? sizeof(pool_job->payload)
                             : job.payload_len;
        memcpy(pool_job->payload, job.payload, n);
        pool_job->payload_len = n;
    } else {
        pool_job->payload_len = 0U;
    }
    pool_job->out_type = job.out_type;
    pool_job->out_payload = job.out_payload;
    pool_job->out_max = job.out_max;
    pool_job->out_len = job.out_len;
    pool_job->out_uptime = job.out_uptime;
    pool_job->out_version = job.out_version;
    pool_job->result = ESP_FAIL;
    pool_job->finished = false;
    pool_job->abandoned = false;
    pool_job->executing = false;
    /* done / in_use 保持 acquire 时状态 */

    const TickType_t qwait = pdMS_TO_TICKS(
        (job.timeout_ms > 0U ? job.timeout_ms : 1000U) + 2000U);
    if (xQueueSend(m_jobQueue, &pool_job, qwait) != pdTRUE) {
        DEBUG_WARN("SPI", "worker job queue full/timeout kind=%u", (unsigned)job.kind);
        releaseJobSlot(pool_job);
        return ESP_ERR_TIMEOUT;
    }
    xTaskNotify(m_workerTask, 0x10U, eSetBits);

    /* dwait 覆盖 worker 单笔执行上界：otaSendAndPoll 最多 3 次尝试、每次 ≈
     * timeout_ms，故 3×timeout+10s 保证"已开始执行的 job"必然在提交方放弃前
     * 完成（执行中 worker 在写本函数调用方的栈缓冲，栈帧必须存活到 finished）。 */
    const TickType_t dwait = pdMS_TO_TICKS(
        3U * (job.timeout_ms > 0U ? job.timeout_ms : 1000U) + 10000U);
    if (xSemaphoreTake(pool_job->done, dwait) == pdTRUE) {
        const esp_err_t result = pool_job->result;
        /* worker 在 give 后才写 finished；等其退出临界区后再由本侧独占 release。 */
        for (;;) {
            taskENTER_CRITICAL(&s_spiJobMux);
            const bool fin = pool_job->finished;
            taskEXIT_CRITICAL(&s_spiJobMux);
            if (fin) {
                break;
            }
            vTaskDelay(1);
        }
        releaseJobSlot(pool_job);
        return result;
    }

    /* 超时：按 worker 是否已开始执行分型（executing 由 workerLoop dequeue 时置位）。 */
    taskENTER_CRITICAL(&s_spiJobMux);
    const bool executing = pool_job->executing;
    const bool already_done = pool_job->finished;
    if (!executing && !already_done) {
        /* 尚未执行：标 abandoned，worker dequeue 时跳过 executeJob——
         * 本函数即将返回，out_* 将指向已消亡栈帧，再执行即 use-after-return。 */
        pool_job->abandoned = true;
    }
    taskEXIT_CRITICAL(&s_spiJobMux);

    if (already_done) {
        (void)xSemaphoreTake(pool_job->done, 0);
        const esp_err_t result = pool_job->result;
        releaseJobSlot(pool_job);
        return result;
    }
    if (executing) {
        /* 执行中：worker 此刻正写调用方栈缓冲，不可返回，须等 finished。
         * 正常时执行上界已被 dwait 覆盖，这里只兜 dequeue/dwait 边界的尾差；
         * 再等一整个 dwait（执行中意味着 dequeue 早于 dwait，S+dwait 执行上界
         * ≤ 2×dwait 必然完成）。 */
        if (xSemaphoreTake(pool_job->done, dwait) == pdTRUE) {
            for (;;) {
                taskENTER_CRITICAL(&s_spiJobMux);
                const bool fin = pool_job->finished;
                taskEXIT_CRITICAL(&s_spiJobMux);
                if (fin) {
                    break;
                }
                vTaskDelay(1);
            }
            const esp_err_t result = pool_job->result;
            releaseJobSlot(pool_job);
            return result;
        }
        /* 仅 worker 本身卡死才会走到这里（执行上界被打破）：标 abandoned
         * 让 worker（若仍能收尾）释放槽，本侧放弃等待。 */
        DEBUG_ERR("SPI", "worker 执行中 job 仍未完成 kind=%u（标放弃）",
                  (unsigned)job.kind);
        taskENTER_CRITICAL(&s_spiJobMux);
        if (!pool_job->finished) {
            pool_job->abandoned = true;
        }
        taskEXIT_CRITICAL(&s_spiJobMux);
        return ESP_ERR_TIMEOUT;
    }

    DEBUG_ERR("SPI", "worker job done timeout kind=%u（未执行已放弃，池槽交 worker 释放）",
              (unsigned)job.kind);
    return ESP_ERR_TIMEOUT;
}

/** @brief FreeRTOS 任务入口，转到 workerLoop。 */
void SpiMaster::workerEntry(void *arg)
{
    static_cast<SpiMaster *>(arg)->workerLoop();
}

/**
 * @brief SPI worker 主循环：处理按铃 EVENT + 执行 job 队列。
 * @note  全栈唯一允许直接访问 SPI 总线的任务上下文。
 */
void SpiMaster::workerLoop()
{
    m_workerTask = xTaskGetCurrentTaskHandle();
    m_spiTaskHandle = m_workerTask;
    DEBUG_INFO("SPI", "SPI worker started, handle=%p", (void *)m_workerTask);

    for (;;) {
        /* ESP32 自我 OTA 暂停：空转不读 SPI（flash 写期间 cache 失效会拖慢
         * flash 驻留代码，且此时 GD32 数据无意义）。恢复由 setSpiPaused(false)
         * 通知唤醒。 */
        if (m_spiPaused) {
            m_gd32RequestPending = false;
            uint32_t bits = 0U;
            (void)xTaskNotifyWait(0U, 0xFFFFFFFFU, &bits, pdMS_TO_TICKS(50));
            continue;
        }
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
            /* ★ 执行前原子检查 abandoned：提交方超时返回后 out_* 指向已消亡的
             * 栈帧，此时执行会写悬垂指针（use-after-return → 调用方栈破坏）。
             * executing 与 abandoned 判定同临界区：提交方超时路径据此区分
             * "未执行（标 abandoned 跳过）"与"执行中（必须等写完才能返回）"。 */
            taskENTER_CRITICAL(&s_spiJobMux);
            const bool job_abandoned = job->abandoned;
            job->executing = true;
            taskEXIT_CRITICAL(&s_spiJobMux);
            if (!job_abandoned) {
                /* 计时 worker 实际执行段（取帧→发→读响应→完成）：
                 * 测速页 RTT − 本值 = 提交/调度/唤醒开销，用于区分
                 * "ESP32 侧任务抢占" vs "GD32 应答慢" 的抖动归属。 */
                const int64_t w0 = esp_timer_get_time();
                m_lastProfile.t_w0 = (uint64_t)w0;
                executeJob(*job);
                const int64_t w1 = esp_timer_get_time();
                m_lastProfile.t_w1 = (uint64_t)w1;
                const int64_t wus = w1 - w0;
                m_lastWorkerExecUs = (wus > 0) ? (uint32_t)wus : 0U;
            } else {
                DEBUG_WARN("SPI", "跳过已放弃 job kind=%u（提交方已超时返回）",
                           (unsigned)job->kind);
            }
            finishPoolJob(job);
            continue;
        }

        /* 无 job：先处理 GD32 重启重同步（session=0 响应置位；5s 冷却防空转）。
         * OTA 中继/暂停期间不 HELLO，避免干扰在途 OTA 事务。 */
        if (m_resyncPending && !m_suppressRingService && !m_spiPaused) {
            const TickType_t now_tick = xTaskGetTickCount();
            if ((uint32_t)(now_tick - m_lastResyncTick) >= pdMS_TO_TICKS(5000)) {
                m_lastResyncTick = now_tick;
                m_resyncPending = false;
                DEBUG_INFO("SPI", "GD32 session=0，HELLO 重同步...");
                (void)hello(1500U, 0U);
                continue;
            }
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
 * @brief GPIO 边沿中断回调（DATA_READY / PB6）。
 *
 * 仅处理下降沿：GD32 有待读数据（按铃）。上升沿不再使用（旧的"等上升沿
 * ready"逻辑已删除）。
 * ISR 内仅做标志翻转 + 信号量/任务通知，不做 SPI 传输。
 *
 * 所有下降沿先 give s_readyEdgeSem：唤醒阻塞在 waitSpiDataReady 的任务
 * （OTA 响应等待），把原来 1ms tick 轮询变成 µs 级事件驱动。
 *
 * 本端忙时（acceptRing()==false）：记下 pending 但不 notify，避免抢断
 * 当前 SPI 事务；事务结束 worker 下一轮会立即消费此标志，省去最多 25ms
 * 的回扫延迟。PB6 在被读走前保持低、不会产生新下降沿，不记 pending 即丢唤醒。
 */
void IRAM_ATTR SpiMaster::onReadyEdgeISR(void *arg)
{
    SpiMaster *self = static_cast<SpiMaster *>(arg);
    if (!self) { return; }
    /* 只关心低电平（下降沿后采样）：有数据待读 */
    if (readReadyLevelRaw() != 0) {
        return;
    }
    /* 无条件先 give 边沿事件：精确唤醒 waitSpiDataReady（响应已装载）。
     * 无人等待时二值信号量饱和为 1，由消费方电平重查兜底。 */
    if (s_readyEdgeSem != nullptr) {
        BaseType_t sem_hp = pdFALSE;
        xSemaphoreGiveFromISR(s_readyEdgeSem, &sem_hp);
        portYIELD_FROM_ISR(sem_hp);
    }
    if (self->m_suppressRingService) {
        /* OTA 中继：PB6 低是 WAIT/ACK 就绪，留给业务 poll，不按 EVENT 处理 */
        return;
    }
    if (!self->m_linkState.acceptRing()) {
        /* 本端忙：记下 pending，不 notify 抢断当前事务；
         * 事务结束 worker 下一轮会消费此标志（PB6 保持低、无新下降沿，
         * 不记则只能等 25ms 回扫兜底）。 */
        self->m_gd32RequestPending = true;
        return;
    }
    self->m_gd32RequestPending = true;
    if (self->m_spiTaskHandle) {
        BaseType_t hp = pdFALSE;
        xTaskNotifyFromISR(self->m_spiTaskHandle, 0x01U, eSetBits, &hp);
        portYIELD_FROM_ISR(hp);
    }
}

/**
 * @brief 等 GD32 拉低 PB6（有待读响应）。
 * v2 DATA_READY：低 = 有数据；高 = 队列空。TX 前不等 HIGH（CS 即互斥）。
 * 事件驱动：阻塞在 s_readyEdgeSem 上，由下降沿 ISR 精确唤醒（µs 级），
 * 每次醒来重查电平以过滤陈旧 give；信号量不可用时退回 1ms tick 轮询。
 * @return ESP_OK 已低；ESP_ERR_TIMEOUT 超时仍为高（调用方可再决定是否盲 poll）。
 */
static esp_err_t waitSpiDataReady(uint32_t timeout_ms)
{
    if (readReadyLevelRaw() == 0) { return ESP_OK; }  /* 已低 = 有数据 */
    if (timeout_ms == 0U) { return ESP_ERR_TIMEOUT; }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        if (readReadyLevelRaw() == 0) { return ESP_OK; }
        const TickType_t now = xTaskGetTickCount();
        if (now >= deadline) { return ESP_ERR_TIMEOUT; }
        if (s_readyEdgeSem != nullptr) {
            (void)xSemaphoreTake(s_readyEdgeSem, deadline - now);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1) > 0 ? pdMS_TO_TICKS(1) : 1);
        }
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
        DEBUG_WARN("SPI", "xfer INVALID_ARG tx=%u rx=%u dev=%p",
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

    /* 0.4.4：CS 事务间最小间隔（根因修复）。
     * GD32 侧每个 CS 事务要跑一次 EXTI ISR（stop DMA → processCommand → re-arm，
     * ~几十 µs）；该 ISR 抢占级 1，会被 ECAT SYNC0/CAN-RX（级 0）及同级 PDI ISR
     * 周期性延迟。ESP32 背靠背 CS 脉冲（send→read µs 级间隙）一旦撞进其
     * "pin 已回高但 re-arm 未完成"的窗口，GD32 的 RX DMA 就 mid-pulse 重装——
     * 下一笔查询帧头部丢失、尾部被 256B 缓冲截断（GD32 诊断实测：rxLen=255、
     * 缓冲全 FF + 帧残片）→ v2 解析失败回 0x82 0xE8 → 迟到被 serviceRing 偷读
     * （"偷读迟到 RESPONSE"持续风暴的根因）。1ms 间隔给 GD32 留出确定性窗口，
     * 换取查询循环 ~5% 吞吐，彻底消除 RX 撕裂。 */
    {
        const int64_t now_us = esp_timer_get_time();
        const int64_t since_us = now_us - s_lastCsEndUs;
        if (since_us >= 0 && since_us < 1000) {
            esp_rom_delay_us((uint32_t)(1000 - since_us));
        }
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

    /* 中断模式（queue+result 信号量）：2026-08-02 实测 polling 模式在 C3 单核上
     * 整体更慢——传输段虽略快（-14~-17µs），但 worker 忙等占死 CPU，提交/调度
     * 13→140µs、唤醒 18→76µs、2-5ms 尾巴 23→129 帧，RTT 658→822µs。回退。 */
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
         * 使用有界超时，避免对端无响应时 worker 永久阻塞。 */
        DEBUG_WARN("SPI", "spi_device_get_trans_result timeout: %s len=%u, bounded retry 1s",
                   esp_err_to_name(ret), (unsigned)(total_len * 8U));
        ret = spi_device_get_trans_result(m_spiDevice, &ret_trans, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            DEBUG_ERR("SPI", "trans_result放弃:%s 或复位SPI",
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
    /* CS 上升沿（事务结束）时间戳：供下一笔事务前的最小间隔判断 */
    s_lastCsEndUs = esp_timer_get_time();
    return ESP_OK;
}

/**
 * @brief 读一帧响应（单 CS 固定预算，再 v2_parse 截真实长度）。
 *
 * 不用两段 KEEP_ACTIVE：Boot 在 CS 释放时无论是否读完都清 s_txPending，
 * 中途断 CS 会丢 HELLO_ACK，后续 MISO 全 0xFF（线上已复现）。
 * 时钟预算见 spiRespReadBudget()：BOOT=128，APP=256。
 */
esp_err_t SpiMaster::readV2FrameVarlen(uint8_t *out_frame, size_t out_max, size_t *out_len,
                                       uint32_t timeout_ms, size_t budget_hint)
{
    if (out_frame == nullptr || out_len == nullptr || out_max < V2_HEADER_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (m_spiDevice == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_len = 0U;

    size_t budget = spiRespReadBudget();
    if (budget_hint > 0U && budget_hint < budget) {
        budget = budget_hint;   /* 主动 poll 按请求类型收紧，省 0xFF 填充时钟 */
    }
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
                             size_t *frame_offset, uint8_t *out_flags)
{
    if (!buf) return false;

    v2_header_t hdr;
    const uint8_t *pload = nullptr;
    if (!v2_parse_frame(buf, buf_len, &hdr, &pload)) {
        return false;
    }

    SpiMaster &self = SpiMaster::instance();

    /* ★ credit 更新必须晚于 session 校验：被拒收的帧（session 不匹配且非 0/非 EVENT）
     * 其 credit 字段不可信（可能是 GD32 旧 session 残留帧的背压值）。
     * 若在拒收前更新 m_remoteCredit，一次 credit=0 的失配帧就会把 m_remoteCredit
     * 锁存为 0，后续所有 transact 进入 otaTransferUnlocked 的 CREDIT 背压死循环
     * （drain 读到的帧 session 仍不匹配 → 持续 return false → m_remoteCredit 永远 0）。
     * 现象：一次通信后所有 SPI 出错，只有复位 GD32（发 session=0 触发 resync）才恢复。 */
    if (self.m_v2Active) {
        /* SESSION_ID 校验 */
        if (hdr.session_id != self.m_sessionId) {
            self.m_stats.seq_errors++;
            const bool is_event = (hdr.flags & V2_FLAG_EVENT) != 0U;
            /* session_id==0：GD32 重启后 g_v2Active=false，响应帧 session 填 0
             * （见 GD32 spi_build_frame）。典型场景：刷完 GD32 固件、ESP32 未重启。
             * 若拒收：GD32 明明在回 ACK，ESP32 全丢 → OTA/命令全超时死循环
             * （日志特征：v2 SES mismatch got=00000000 刷屏 + fed 卡首 chunk）。
             * 放行并按 stale 语义交给上层（ack_seq 归属照常判定）。 */
            const bool is_sess0 = (hdr.session_id == 0U);
            if (!is_event && !is_sess0) {
                static uint32_t s_sess_mm_logs = 0U;
                if ((++s_sess_mm_logs % 32U) == 1U) {
                    DEBUG_WARN("SPI", "v2 SES mismatch got=%08lX exp=%08lX t=%02X #%lu (credit 不更新)",
                               (unsigned long)hdr.session_id, (unsigned long)self.m_sessionId,
                               hdr.type, (unsigned long)s_sess_mm_logs);
                }
                /* 拒收：不更新 m_remoteCredit，避免失配帧的 credit 污染背压状态机 */
                return false;
            }
            if (is_sess0 && !is_event) {
                static uint32_t s_sess0_logs = 0U;
                if ((++s_sess0_logs % 64U) == 1U) {
                    DEBUG_WARN("SPI", "GD32 session=0 (对端已重启?) 放行 RESPONSE t=0x%02X #%lu",
                               hdr.type, (unsigned long)s_sess0_logs);
                }
                /* GD32 重启后 v2 未激活：置重同步标志，worker 空闲时 HELLO 重建 session。
                 * 功能由 session=0 放行兜底，重同步只是把链路拉回校验态。 */
                self.m_resyncPending = true;
            }
            static uint32_t s_evt_sess0_logs = 0U;
            if (is_event && (++s_evt_sess0_logs % 64U) == 1U) {
                DEBUG_DBG("SPI", "EVENT session mismatch accepted: got=0x%08lX exp=0x%08lX type=0x%02X",
                          (unsigned long)hdr.session_id, (unsigned long)self.m_sessionId, hdr.type);
            }
        }
    }

    /* session 校验通过（m_v2Active==false / 匹配 / sess0 放行 / EVENT 放行）：
     * 此帧 credit 可信，更新对端 credit。 */
    self.m_remoteCredit = hdr.credit;

    if (self.m_v2Active) {

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
                    DEBUG_DBG("SPI", "v2 stale rsp t=0x%02X ack=%u exp=%u #%lu",
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
    if (out_flags)    *out_flags = hdr.flags;
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
    /* 先创建互斥锁，再进入临界区 */
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

    DEBUG_INFO("SPI", "SPI2 ok CS=GPIO%d CLK=%dHz",
               SPI_MASTER_CS_GPIO, SPI_CLOCK_HZ);

    /* 配置 SPI Ready 握手引脚(GPIO10)，v2 DATA_READY 模式：仅输入。
     * PB6 仅由 GD32 下拉（有数据待发时拉低，队列空时释放高）。
     * ESP32 不再下拉 PB6，只检测边沿：下降沿=GD32 有数据按铃。 */
    gpio_config_t ready_cfg = {};
    ready_cfg.pin_bit_mask = (1ULL << SPI_READY_GPIO);
    ready_cfg.mode = GPIO_MODE_INPUT;             /* v2: 仅输入，不再开漏输出 */
    ready_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    ready_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ready_cfg.intr_type = GPIO_INTR_NEGEDGE;       /* 仅下降沿：有数据待读 */
    esp_err_t gpio_ret = gpio_config(&ready_cfg);
    if (gpio_ret != ESP_OK) {
        DEBUG_WARN("SPI", "SPI Ready 握手引脚 GPIO%d 配置失败: %s", SPI_READY_GPIO, esp_err_to_name(gpio_ret));
    } else {
        DEBUG_INFO("SPI", "DATA_READY 输入已配置: GPIO%d (← GD32 PB6，低=有数据)", SPI_READY_GPIO);
    }

    /* 边沿事件信号量必须先于 ISR 注册创建，避免 ISR 触发时句柄为空 */
    if (s_readyEdgeSem == nullptr) {
        s_readyEdgeSem = xSemaphoreCreateBinary();
        if (s_readyEdgeSem == nullptr) {
            DEBUG_WARN("SPI", "ready edge sem create failed, 退回轮询等待");
        }
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

    /* 创建 job 队列、固定 Job pool（静态 done 信号量）、事件信号量与 SPI worker */
    if (m_jobQueue == nullptr) {
        m_jobQueue = xQueueCreate(kJobQueueLen, sizeof(Job *));
        if (m_jobQueue == nullptr) {
            DEBUG_ERR("SPI", "SPI job queue create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!m_jobPoolReady) {
        for (UBaseType_t i = 0; i < kJobPoolSize; ++i) {
            m_jobPool[i] = Job{};
            m_jobPool[i].done = xSemaphoreCreateBinary();
            if (m_jobPool[i].done == nullptr) {
                DEBUG_ERR("SPI", "SPI job pool done sem create failed idx=%u",
                          (unsigned)i);
                for (UBaseType_t j = 0; j < i; ++j) {
                    vSemaphoreDelete(m_jobPool[j].done);
                    m_jobPool[j].done = nullptr;
                }
                return ESP_ERR_NO_MEM;
            }
            m_jobPool[i].in_use = false;
        }
        m_jobPoolReady = true;
        DEBUG_INFO("SPI", "SPI job pool ready size=%u queue=%u",
                   (unsigned)kJobPoolSize, (unsigned)kJobQueueLen);
    }
    if (m_eventSem == nullptr) {
        m_eventSem = xSemaphoreCreateCounting(kEventSlots, 0);
        if (m_eventSem == nullptr) {
            DEBUG_ERR("SPI", "SPI event sem create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (m_workerTask == nullptr) {
        /* 优先级 8（2026-08-02 测速实验）：原 6 会被 ota_feed(7) 抢占，
         * SPI 完成 ISR 唤醒 worker 后还要等 feed 让出，拖长 RTT 唤醒链。
         * 8 仍低于 health_mon(21)/ota_relay(23)；worker 阻塞循环为主，
         * 仅 job 执行时运行，不会饿死 ota_feed/httpd。 */
        BaseType_t ok = xTaskCreate(workerEntry, "spi_worker", TASK_STACK_SPI_WORKER, this, 8, &m_workerTask);
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

/**
 * @brief L0 本机自检：SPI 设备、互斥、worker、job 队列是否就绪。
 * @return true 本机 SPI 栈可用
 */
bool SpiMaster::selfTestL0() const
{
    return (m_spiDevice != nullptr) && (m_mutex != nullptr) &&
           (m_workerTask != nullptr) && (m_jobQueue != nullptr);
}

/**
 * @brief L0 对端轻检：短超时 HELLO，确认链路可达 GD32。
 * @return true 至少一次 HELLO 成功
 */
bool SpiMaster::selfTestPeerL0()
{
    /* 短超时、最多 2 次；不拉长整体启动 */
    for (uint32_t attempt = 0U; attempt < 2U; ++attempt) {
        if (hello(600U, 0U) == ESP_OK) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
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
    /* 记录最近一次主动请求类型：poll 读预算按它收紧（isShortRespType）。 */
    m_lastReqType = type;

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
                /* PB6 高 = GD32 无待发数据 = 响应缓冲已释放、可接收新命令。
                 * 此时 remoteCredit 仍为 0 是陈旧值：credit 只随帧头传递，GD32 转空闲后
                 * 没有帧能把 credit=1 送回主机（典型为电机 OTA 取消后：一次瞬时 credit=0
                 * 被锁存，GD32 转空闲再不发帧）→ 死锁，所有发送被拒。
                 * PB6 高即证明对端可收，直接放行打破死锁；真忙时 PB6 会低、走上面 drain。 */
                m_remoteCredit = 1U;
                break;
            }
        }
        if (m_remoteCredit == 0U) {
            DEBUG_WARN("SPI", "remoteCredit=0 且 PB6 持续低, refuse TX type=0x%02X", type);
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* 同步单 CS 传输：按帧长发送；延迟一拍下本笔 RX 为陈旧响应。
     * v2：PB6 低 = 有待读；陈旧响应由本笔 transferClocked 自然读走（勿额外 drain，
     * 以免误吃主循环刚排好的 ACK）。CS 即互斥，TX 前不等 PB6 HIGH。 */
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
    /* 与 Git 稳定路径一致：step1 陈旧帧原样解析即可（otaSendAndPoll 传 nullptr 丢弃）。
     * 勿在此处过滤 LINK/EVENT，否则改变 delayed-by-one 语义。 */
    if (otaFindFrame(rx_raw, xfer_len, &found_type, &found_payload, &found_len, &frame_offset)) {
        copyFoundFrame(resp_type, found_type, resp_payload, resp_max, found_payload, found_len, resp_len);
        correctRoleFromType(found_type, "send");
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
        /* 分段剖析：等 READY 段 = GD32 收到帧→拉低 PB6 的处理时间 */
        m_lastProfile.t_wait0 = (uint64_t)esp_timer_get_time();
        const bool ready = (waitSpiDataReady(wait_ms) == ESP_OK);
        if (ready) {
            m_lastProfile.t_wait1 = (uint64_t)esp_timer_get_time();
        }

        now = xTaskGetTickCount();
        remain_ms = (deadline > now) ? ((deadline - now) * portTICK_PERIOD_MS) : 0U;
        const bool last_chance = (remain_ms == 0U) || (attempt >= 6U && !did_spi_read);
        /* 0.4.4：APP ISR 快速响应类型禁用盲读。
         * 盲读在 PB6 未低时强行打 256 时钟 0xFF——若 GD32 的 CS EXTI 还压着
         * ECAT/CAN 中断没处理完上一笔（re-arm 未跑），盲读会撕碎其 RX 缓冲
         * （实测 GD32 侧 rxLen=255 全 FF + 查询帧残片）→ 解析失败回 0x82 0xE8
         * → 迟到被 serviceRing 偷读（"偷读迟到 RESPONSE 0xEE/E8"风暴的根因）。
         * 快速类型的 PB6 是事件驱动（GD32 ISR µs 级拉低），纯等电平即可；
         * 响应真丢了就走 timeout→重发（GD32 DUP 缓存回放兜底），无 corrupt。 */
        const bool no_blind = isAppIsrFastRespType(m_lastReqType);
        /* 无 ready：前 2 轮盲读；之后每 8 轮盲读；deadline 末强制盲读一次 */
        if (!ready && (no_blind ||
                       (attempt > 2U && (attempt % 8U) != 0U && !last_chance))) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_DCU_OTA_POLL_BACKOFF_MS));
            continue;
        }
        if (remain_ms == 0U) {
            remain_ms = 20U; /* 末次 SPI 事务给足一点时钟时间 */
        }
        const uint32_t spi_timeout = (remain_ms > 200U) ? 200U : remain_ms;

        uint8_t rx_raw[SPI_OTA_FRAME_MAX];
        size_t rx_len = 0U;
        /* 读预算按最近请求类型收紧（短响应类型 96B，其余按角色默认） */
        const size_t budget_hint = isShortRespType(m_lastReqType) ? kShortRespReadBudget : 0U;
        esp_err_t ret = readV2FrameVarlen(rx_raw, sizeof(rx_raw), &rx_len, spi_timeout, budget_hint);
        if (ret == ESP_OK) {
            m_lastProfile.t_read1 = (uint64_t)esp_timer_get_time();
        }
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
        uint8_t found_flags = 0U;

        /* 恢复 Git 稳定 poll 语义（GD32 BOOT OTA 以此为准）。
         * 仅额外：EVENT 帧跳过（电机 OTA 喂数时勿把 0xC1/0xC5 当 ACK）；
         * APP 独有 TYPE 纠正 role 粘滞。不做 LINK/RESPONSE 额外过滤。 */
        if (otaFindFrame(rx_raw, rx_len, &found_type, &found_payload, &found_len, &frame_offset, &found_flags)) {
            if (found_type == V2_TYPE_CREDIT_UPDATE) {
                DEBUG_DBG("SPI", "poll got CREDIT_UPDATE credit=%u", (unsigned)m_remoteCredit);
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            if ((found_flags & V2_FLAG_EVENT) != 0U) {
                correctRoleFromType(found_type, "poll");
                DEBUG_DBG("SPI", "poll skip EVENT type=0x%02X", found_type);
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

            copyFoundFrame(resp_type, found_type, resp_payload, resp_max, found_payload, found_len, resp_len);

/* WAIT/ACK/ERROR：只要 ack_seq 匹配，GD32 已消费本 TX_SEQ，必须推进。
	             * ★ 电机 OTA：feedData 缓冲满回 WAIT 时若不清 seq，ESP32 同 seq 重发
	             *   → GD32 命中“重复请求”只回缓存 WAIT，feedData 永不再调用
	             *   → fed 卡在首 chunk（日志 fed=232）、CAN 抽空后 0x01 超时。
	             * BOOT OTA 见 WAIT 后只 poll 不重发 DATA，推进 seq 同样正确。 */
	            if (m_lastRxAckMatched) {
		                m_txSeq = (uint8_t)(m_lastSentSeq + 1U);
		            }
	            /* 0.4.3 诊断：GD32 明确回 0x82 的量化归因（/system/status 直读） */
	            if (found_type == V2_TYPE_RSP_ERROR) {
	                m_stats.rsp_err_0x82++;
	                m_stats.rsp_err_last_req = m_lastReqType;
	                m_stats.rsp_err_last_p0 = (*resp_len > 0U) ? resp_payload[0] : 0U;
	                m_stats.rsp_err_last_p1 = (*resp_len > 1U) ? resp_payload[1] : 0U;
	                m_stats.rsp_err_last_p2 = (*resp_len > 2U) ? resp_payload[2] : 0U;
	                m_stats.rsp_err_last_p3 = (*resp_len > 3U) ? resp_payload[3] : 0U;
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
        /* 分段剖析：记录调用方侧两个时间戳（worker 侧在 workerLoop/otaSendAndPoll 内写） */
        m_lastProfile.t_submit0 = (uint64_t)esp_timer_get_time();
        const esp_err_t p_ret = submitJob(job);
        m_lastProfile.t_submit1 = (uint64_t)esp_timer_get_time();
        return p_ret;
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

    /* 用绝对截止时间：虚假唤醒/抢事件后按剩余时间等，避免每次重新等满 timeout。 */
    const TickType_t start = xTaskGetTickCount();
    const TickType_t total = pdMS_TO_TICKS(timeout_ms);
    while (true) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= total) {
            return false;
        }
        const TickType_t remain = total - elapsed;
        if (xSemaphoreTake(m_eventSem, remain) != pdTRUE) {
            return false;
        }
        LockGuard lock(m_mutex);
        if (popEvent(type, payload, max_len, len)) {
            return true;
        }
        /* 信号量被消费但队列空（可能被并发 takeEvent 抢走），继续等剩余时间 */
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

    /* 0.4.9：量化"透明重试"下的真实失败率（DBG 级日志被编译期裁掉，
     * 旧实现里 send/poll 超时重试完全静默）。/system/status 直读：
     *   tx_total=进入事务次数  tx_retry=走到重发的次数  tx_fail=耗尽重试仍失败 */
    m_stats.tx_total++;
    LockGuard lock(m_mutex);

    /* 步骤 1: 发送命令（忽略延迟一拍的过期响应）。
     * otaTransferUnlocked 可能返回:
     *   ESP_OK                    - 读到过期帧（正常，忽略）
     *   ESP_ERR_INVALID_RESPONSE  - 读到 0xFF（正常，延迟一拍）
     *   其他                      - SPI 真实错误 */
    /* 总超时在 send 与 poll 之间分配，二者之和不超过 timeout_ms */
    uint32_t send_timeout = (timeout_ms > 500U) ? 500U : timeout_ms;
    if (send_timeout == 0U) {
        send_timeout = 1U;
    }
    uint32_t poll_timeout;
    if (timeout_ms > send_timeout) {
        poll_timeout = timeout_ms - send_timeout;
    } else if (timeout_ms >= 40U) {
        send_timeout = timeout_ms / 2U;
        if (send_timeout == 0U) {
            send_timeout = 1U;
        }
        poll_timeout = timeout_ms - send_timeout;
    } else {
        poll_timeout = timeout_ms;
    }
    if (poll_timeout == 0U) {
        poll_timeout = 1U;
    }

    /* 统一重发机制（嵌入 worker，所有业务共用，调用方无需各自重试）：
     * 仅当"没读到任何有效响应"时同 TX_SEQ 重发（失败路径 seq 不推进）：
     *   - 发送级失败（transferClocked 错误/credit 拒绝）：帧大概率未送达 GD32；
     *   - poll 超时（无响应）：帧可能未送达或响应丢失，重发靠 GD32 DUP-SEQ 去重兜底。
     * 读到任何帧（WAIT/ACK/ERROR/陈旧）即成功返回，不重发：
     *   - 保证 GD32 明确拒绝（ERROR 0x82）天然不重试（如 CAN 负载高/BUS_OFF）；
     *   - OTA relay 的 WAIT 语义、旧帧排空语义均不受影响。 */
    constexpr uint32_t kTxRetryMax = 3U;   /* 总尝试次数（1 次首发 + 2 次重发） */
    for (uint32_t attempt = 0U; attempt < kTxRetryMax; ++attempt) {
        m_lastProfile.t_send0 = (uint64_t)esp_timer_get_time();
        esp_err_t send_ret = otaTransferUnlocked(type, payload, payload_len,
                                                 nullptr, nullptr, 0U, nullptr, send_timeout,
                                                 /*expect_stale=*/true);
        m_lastProfile.t_send1 = (uint64_t)esp_timer_get_time();
        if (send_ret != ESP_OK && send_ret != ESP_ERR_INVALID_RESPONSE) {
            if (attempt + 1U < kTxRetryMax) {
                m_stats.tx_retry++;   /* 0.4.9：发送级失败→重发（原静默） */
                DEBUG_DBG("SPI", "TX retry(%u/%u) send fail type=0x%02X %s",
                          (unsigned)(attempt + 1U), (unsigned)kTxRetryMax,
                          type, esp_err_to_name(send_ret));
                vTaskDelay(pdMS_TO_TICKS(3));
                continue;
            }
            m_stats.tx_fail++;        /* 0.4.9：耗尽重试仍失败 */
            m_stats.tx_fail_last_type = type;
            return send_ret;
        }

        /* CS 释放后的静默期分型处理：
         * - APP ISR 响应类型（0xC1/0xC4/0xC5 查询、0xD1 电机 OTA、0xA1 PING 等）：
         *   GD32 APP 在 onCsRelease ISR 内 µs 级装好响应并拉低 PB6 → 零盲等，
         *   waitSpiDataReady 由下降沿 ISR 精确唤醒（见 isAppIsrFastRespType）。
         * - 其他类型（BOOT OTA 命令 0x01/0x02/0x03 等）：保留 5ms——BOOT 在主循环
         *   处理 CHALLENGE/flash，poll 前 2 轮盲读 CS 过早打进去会打断其
         *   EXTI/DMA 时序（实测去掉后 CHALLENGE 超时）。 */
        if (!isAppIsrFastRespType(type)) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        /* 步骤 2: 轮询读取真实响应（变长读，不重发命令） */
        esp_err_t poll_ret = otaPollResponseUnlocked(resp_type, resp_payload, resp_max, resp_len,
                                                     poll_timeout, /*expected_type=*/0xFFU);
        if (poll_ret == ESP_OK) {
            return ESP_OK;   /* 已读到有效响应帧（ACK/ERROR/陈旧） */
        }
        if (attempt + 1U < kTxRetryMax) {
            m_stats.tx_retry++;   /* 0.4.9：poll 无响应→重发（原静默） */
            DEBUG_DBG("SPI", "TX retry(%u/%u) no resp type=0x%02X %s",
                      (unsigned)(attempt + 1U), (unsigned)kTxRetryMax,
                      type, esp_err_to_name(poll_ret));
            vTaskDelay(pdMS_TO_TICKS(3));
            continue;
        }
        m_stats.tx_fail++;        /* 0.4.9：耗尽重试仍失败 */
        m_stats.tx_fail_last_type = type;
        return poll_ret;
    }
    return ESP_ERR_TIMEOUT;  /* 不可达：循环必有返回 */
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
                /* credit 复位：避免旧 session 锁存的 credit=0 跨重同步污染 */
                m_localCredit = 8;
                m_remoteCredit = 8;
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
                DEBUG_WARN("SPI", "ENTER_BOOT拒 code=0x%02X exp=%u last=%u",
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
        DEBUG_INFO("SPI", "ENTER_BOOT ok ack=%s waitAPP %lums",
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

    DEBUG_WARN("SPI", "BOOT就绪超时 %lums r=%lu app=%lu",
               (unsigned long)timeout_ms, (unsigned long)rounds, (unsigned long)app_hits);
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 解析 GD32 APP 身份 16B 并写入本端缓存。
 *
 * 布局与 HELLO_ACK 附加块 / APP PING body 相同（见 spi_link_v2.h V2_APP_IDENTITY_LEN）：
 *   [0..3] APP ver · [4..7] BOOT ver · [8..11] uptime_ms LE
 *   [12] feat_map_ver · [13..14] feature_flags LE · [15] init_state
 *
 * @param id             身份块指针（至少 16 字节）
 * @param out_version    可选，输出解析后的版本结构
 * @param out_uptime_sec 可选，输出 uptime（秒）
 */
void SpiMaster::applyAppIdentity16(const uint8_t *id, gd32_version_t *out_version,
                                   uint32_t *out_uptime_sec)
{
    if (!id) {
        return;
    }
    gd32_version_t ver{};
    ver.app_major = id[0];
    ver.app_minor = id[1];
    ver.app_patch = id[2];
    ver.app_build = id[3];
    ver.boot_major = id[4];
    ver.boot_minor = id[5];
    ver.boot_patch = id[6];
    ver.boot_build = id[7];

    const uint32_t uptime_ms = (uint32_t)id[8]
                             | ((uint32_t)id[9] << 8)
                             | ((uint32_t)id[10] << 16)
                             | ((uint32_t)id[11] << 24);
    if (out_uptime_sec) {
        *out_uptime_sec = uptime_ms / 1000U;
    }

    uint16_t feat_flags = 0U;
    bool feat_known = false;
    if (id[12] == V2_FEAT_MAP_VER) {
        feat_flags = v2_get_u16le(&id[13]);
        feat_known = true;
    }

    /* [15] init_state（GD32 1.5.34+；旧固件恒 0=OK，与未知区分靠 feat_known 同帧到达） */
    const uint8_t init_state = id[15];
    if (init_state != m_gd32InitState) {
        if (init_state != GD32_INIT_STATE_OK) {
            DEBUG_WARN("SPI", "GD32 init_state=%u (0=OK 1=degraded 2=critical_fail)",
                       (unsigned)init_state);
        }
        m_gd32InitState = init_state;
    }

    /* HELLO 与 PING 都会带同一份 16B 身份：仅版本/feat 变化时打 INFO，避免连打两遍 */
    const bool ver_changed =
        !m_gd32IdKnown ||
        m_gd32Version.app_major != ver.app_major ||
        m_gd32Version.app_minor != ver.app_minor ||
        m_gd32Version.app_patch != ver.app_patch ||
        m_gd32Version.app_build != ver.app_build ||
        m_gd32Version.boot_major != ver.boot_major ||
        m_gd32Version.boot_minor != ver.boot_minor ||
        m_gd32Version.boot_patch != ver.boot_patch ||
        m_gd32Version.boot_build != ver.boot_build ||
        m_gd32FeatKnown != feat_known ||
        (feat_known && m_gd32FeatFlags != feat_flags);

    m_gd32Version = ver;
    m_gd32IdKnown = true;
    if (out_version) {
        *out_version = ver;
    }
    m_gd32FeatKnown = feat_known;
    m_gd32FeatFlags = feat_known ? feat_flags : 0U;

    if (ver_changed && feat_known) {
        DEBUG_INFO("SPI", "ID APP=%u.%u.%u.%u feat=%04lX",
                   (unsigned)ver.app_major, (unsigned)ver.app_minor,
                   (unsigned)ver.app_patch, (unsigned)ver.app_build,
                   (unsigned long)m_gd32FeatFlags);
        DEBUG_INFO("SPI", "ID BOOT=%u.%u.%u.%u ch=%u",
                   (unsigned)ver.boot_major, (unsigned)ver.boot_minor,
                   (unsigned)ver.boot_patch, (unsigned)ver.boot_build,
                   (unsigned)((m_gd32FeatFlags & V2_FEAT_CAN_CH_MASK) >> V2_FEAT_CAN_CH_SHIFT));
    }
}

/**
 * @brief 向 GD32 APP 发送存活探测（帧长发送 + DATA_READY 变长读）。
 *
 * 两步法（持锁）：otaTransferUnlocked(expect_stale) + otaPollResponseUnlocked。
 * 成功时 body 为身份 16B，与 HELLO_ACK 附加块同布局，经 applyAppIdentity16 入库。
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
    if (resp_len >= V2_APP_IDENTITY_LEN) {
        applyAppIdentity16(pload, version, uptime_sec);
    } else if (version) {
        memset(version, 0, sizeof(gd32_version_t));
        version->app_major = pload[0];
        version->app_minor = pload[1];
        version->app_patch = pload[2];
        version->app_build = pload[3];
    }
    DEBUG_DBG("SPI", "APP PING 成功 len=%u", (unsigned)resp_len);
    return ESP_OK;
}

/**
 * @brief 查询 GD32 APP 初始化状态（SPI 0xA3 INIT_STATUS）。
 *
 * 两步法（持锁），机制与 appPing 相同。应答 ACK：
 *   [0..3] module_mask LE · [4..7] critical_fail_mask LE
 *   [8] reset_cause · [9] init_state · [10..11] reserved
 *   [12..24] fault_codes[13]（GD32 1.5.36+，每模块 1B 故障码）
 * 成功时填充 out（valid=true）并更新本端缓存 m_gd32InitStatus。
 * 仅 APP 角色支持：BOOT 不认识 0xA3，角色不符时快速拒绝不打 SPI。
 */
esp_err_t SpiMaster::appInitStatus(uint32_t timeout_ms, gd32_init_status_t *out)
{
    if (!onWorker() && m_workerTask != nullptr) {
        Job job{};
        job.kind = JobKind::InitStatus;
        job.timeout_ms = timeout_ms;
        job.out_init_status = out;
        return submitJob(job);
    }

    if (m_gd32Role != V2_ROLE_APP) {
        return ESP_ERR_INVALID_STATE;
    }

    LockGuard lock(m_mutex);
    constexpr uint8_t kInitStatusType = 0xA3U;

    uint8_t resp_type = 0;
    uint8_t resp_payload[32];
    size_t resp_len = 0U;

    const uint32_t send_timeout = (timeout_ms > 100U) ? 100U : timeout_ms;
    esp_err_t send_ret = otaTransferUnlocked(kInitStatusType, nullptr, 0U,
                                             nullptr, nullptr, 0U, nullptr, send_timeout,
                                             /*expect_stale=*/true);
    if (send_ret != ESP_OK && send_ret != ESP_ERR_INVALID_RESPONSE) {
        DEBUG_DBG("SPI", "INIT_STATUS send failed: %s", esp_err_to_name(send_ret));
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
        DEBUG_DBG("SPI", "INIT_STATUS poll failed: %s", esp_err_to_name(err));
        return err;
    }

    if (resp_type != SPI_OTA_RSP_ACK || resp_len < 10U) {
        DEBUG_DBG("SPI", "INIT_STATUS bad resp type=0x%02X len=%u", resp_type, (unsigned)resp_len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    gd32_init_status_t st = {};
    st.module_mask = v2_get_u32le(&resp_payload[0]);
    st.critical_fail_mask = v2_get_u32le(&resp_payload[4]);
    st.reset_cause = resp_payload[8];
    st.init_state = resp_payload[9];
    /* GD32 1.5.36+ 响应 25B：附带每模块 1B 故障码；1.5.34 仅 12B 无此数组 */
    if (resp_len >= 25U) {
        memcpy(st.fault_codes, &resp_payload[12], GD32_INIT_MODULE_COUNT);
        st.has_fault_codes = true;
    }
    st.valid = true;

    m_gd32InitStatus = st;
    m_gd32InitState = st.init_state;
    if (out) {
        *out = st;
    }
    DEBUG_INFO("SPI", "INIT_STATUS state=%u mod=0x%08lX crit_fail=0x%08lX rst=0x%02X",
               (unsigned)st.init_state, (unsigned long)st.module_mask,
               (unsigned long)st.critical_fail_mask, (unsigned)st.reset_cause);
    /* 故障码 hex 一行（编码见 GD32 sys/initial.h；canfd 高 4 位=通道号） */
    if (st.has_fault_codes) {
        char fc_hex[GD32_INIT_MODULE_COUNT * 2 + 1];
        for (uint8_t fi = 0; fi < GD32_INIT_MODULE_COUNT; ++fi) {
            fc_hex[fi * 2]     = "0123456789ABCDEF"[(st.fault_codes[fi] >> 4) & 0xFU];
            fc_hex[fi * 2 + 1] = "0123456789ABCDEF"[st.fault_codes[fi] & 0xFU];
        }
        fc_hex[GD32_INIT_MODULE_COUNT * 2] = '\0';
        DEBUG_INFO("SPI", "INIT_STATUS fault_codes=%s", fc_hex);
    }
    return ESP_OK;
}

/**
 * @brief 读取缓存的 0xA3 查询结果（不打 SPI）。
 * @param out  输出结构
 * @return true=缓存有效（已至少成功查询一次）；false=无有效缓存
 */
bool SpiMaster::gd32InitStatusCached(gd32_init_status_t *out) const
{
    if (!m_gd32InitStatus.valid) {
        return false;
    }
    if (out) {
        *out = m_gd32InitStatus;
    }
    return true;
}

// ============================================================
// SPI Link v2 链路状态
// ============================================================
/**
 * @brief 拷贝链路统计快照（有界取锁，失败返回零值）。
 * @return 统计结构副本
 */
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

/**
 * @brief 完整链路状态（session/role/credit + 统计），供 /system/status。
 * @return 状态快照
 */
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

/**
 * @brief 发起 HELLO 建 session；APP 侧 ACK 可附带身份 16B（版本+开关）。
 * @param timeout_ms  总超时
 * @param role_hint   0=any，1=app，2=boot
 * @return ESP_OK 成功；超时/坏响应见错误码
 */
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
    /* credit 与 init() 对齐：HELLO 成功时 m_remoteCredit 会被 ACK 覆盖，
     * 失败时保持 8（而非残留旧值，避免上次通信锁存的 credit=0 跨 session 污染）。 */
    m_localCredit = 8;
    m_remoteCredit = 8;

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
        /* payload[5]/hdr.credit 均为对端 credit_max → 写入 m_remoteCredit。
         * 勿写入 m_localCredit（本端 RX 窗口，init 为 8）。 */
        m_remoteCredit = (pload[5] != 0U) ? pload[5] : hdr.credit;
        /* 新 session = GD32 可能重启过：0xA3 缓存与 init_state 失效，
         * 由下方身份块 / 后续 PING 刷新。 */
        m_gd32InitStatus = gd32_init_status_t{};
        m_gd32InitState = GD32_INIT_STATE_UNKNOWN;
        /* APP：HELLO_ACK 带 16B 身份 → 建连即完成开机身份（与 PING 同布局） */
        if (m_gd32Role == V2_ROLE_APP &&
            hdr.payload_len >= V2_HELLO_ACK_APP_LEN) {
            applyAppIdentity16(&pload[V2_HELLO_ACK_BASE_LEN], nullptr, nullptr);
        }
        /* HELLO 成功时打印 role，便于对照 /system/status 粘滞 boot 问题 */
        DEBUG_INFO("SPI", "HELLO ok role=%u credit=%u sess=0x%08lX",
                   (unsigned)m_gd32Role, (unsigned)m_remoteCredit,
                   (unsigned long)m_sessionId);
        m_v2Active = true;
        m_stats.resyncs++;
        return ESP_OK;
    }

    if (!saw_any_frame) {
        /* 不打全包 hex：只记 attempts + 前 4 字节便于判 SOF */
        DEBUG_WARN("SPI",
                   "HELLO no v2 frame att=%lu rx0-3=%02X %02X %02X %02X",
                   (unsigned long)attempt,
                   last_rx_head[0], last_rx_head[1], last_rx_head[2], last_rx_head[3]);
    } else {
        DEBUG_WARN("SPI", "HELLO: bad/lag response type=0x%02X flags=0x%02X attempts=%lu",
                   last_type, last_flags, (unsigned long)attempt);
    }
    return ESP_ERR_INVALID_RESPONSE;
}

