/*
 * can_feedback_task.cpp - 被动监听 GD32 主动推送的 CAN 反馈快照
 *
 * 架构（GD32 主动推送，ESP32 被动接收）：
 *   GD32 APP 在主循环中调用 canFeedbackPushStep()，循环遍历 3 通道 × 2 页，
 *   通过 requestEsp32Read(0xC1) 下拉 SPIready 主动按铃，将每页反馈 payload
 *   推送给 ESP32。ESP32 本任务调用 checkGd32Request() 阻塞等待按铃，读到
 *   一帧后用 canFdParseFeedbackPage() 解析并累积到 s_work[channel]，再
 *   publish() 到 s_latest[channel] 供 Web/WebSocket 读取。
 *
 *   替代了原先 ESP32 1Hz 主动发 0xC1 轮询的逻辑，避免占用主线 SPI 带宽。
 *
 *   OTA 期间通过 pauseCanFeedbackTask(true) 暂停本任务，避免与 OTA 抢占 SPI。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "transport/canfd_bridge.h"
#include "transport/spi_master.h"
#include "monitor/debug_log.h"
#include "monitor/can_feedback_task.h"

#include <string.h>

namespace {

constexpr uint32_t kWaitRingMs      = 500U;    ///< 单次等待 GD32 按铃超时
constexpr uint32_t kPauseDelayMs    = 100U;    ///< 暂停时的轮询间隔
constexpr uint8_t  kSpiCanFeedbackType = 0xC1U;
constexpr size_t   kRingPayloadMax  = 128U;    ///< 反馈 payload 最大长度
constexpr const char *kTag           = "CAN_FB";

static can_feedback_result_t s_latest[CAN_CHANNEL_MAX];
static bool                   s_valid [CAN_CHANNEL_MAX];
static SemaphoreHandle_t      s_mutex = nullptr;
static volatile bool          s_paused = false;

/* 累积工作缓冲：跨 page 0/1 累积每通道的完整反馈 */
static can_feedback_result_t s_work  [CAN_CHANNEL_MAX];
static uint8_t               s_workIdx[CAN_CHANNEL_MAX];
static uint32_t              s_lastLoggedError[CAN_CHANNEL_MAX];

static void publish(uint8_t ch, const can_feedback_result_t &fb)
{
    if (s_mutex == nullptr) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_latest[ch] = fb;
        s_valid[ch]  = true;
        xSemaphoreGive(s_mutex);
    }
}

static void taskLoop(void *)
{
    DEBUG_INFO(kTag, "CAN 反馈被动监听任务启动 (GD32 主动推送, 0xC1)");

    uint8_t  ring_type = 0U;
    uint8_t  ring_payload[kRingPayloadMax] = {};
    size_t   ring_len = 0U;

    for (;;) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(kPauseDelayMs));
            continue;
        }

        /* 阻塞等待 GD32 按铃并读取一帧（超时则重试） */
        esp_err_t ret = SpiMaster::instance().checkGd32Request(kWaitRingMs);
        if (ret != ESP_OK) {
            continue;
        }

        /* 取出 checkGd32Request 存入的 type/payload */
        if (!SpiMaster::instance().takeRingRx(&ring_type, ring_payload,
                                              sizeof(ring_payload), &ring_len)) {
            continue;
        }

        if (ring_type != kSpiCanFeedbackType || ring_len < 28U) {
            continue;
        }

        const uint8_t ch = ring_payload[0];
        if (ch >= CAN_CHANNEL_MAX) {
            continue;
        }

        /* 解析本页并累积到工作缓冲，page=0 会自动重置 */
        if (canFdParseFeedbackPage(ring_payload, ring_len,
                                   &s_work[ch], &s_workIdx[ch]) == 0) {
            publish(ch, s_work[ch]);

            /* 累计错误发生变化时才告警一次；P0/P1 携带相同统计，不能重复打印。 */
            if (s_work[ch].stats.error_count != 0U
                && s_work[ch].stats.error_count != s_lastLoggedError[ch]) {
                DEBUG_WARN(kTag,
                    "CH%u seq=%lu err=%lu TEC=%u REC=%u state=0x%02X",
                    static_cast<unsigned>(ch),
                    static_cast<unsigned long>(s_work[ch].stats.snapshot_seq),
                    static_cast<unsigned long>(s_work[ch].stats.error_count),
                    static_cast<unsigned>(s_work[ch].stats.tec),
                    static_cast<unsigned>(s_work[ch].stats.rec),
                    static_cast<unsigned>(s_work[ch].stats.bus_state));
                s_lastLoggedError[ch] = s_work[ch].stats.error_count;
            }
        }
    }
}

} // namespace

/**
 * @brief 启动 CAN 反馈被动监听任务（幂等）。
 * @return true 表示任务已存在或创建成功。
 */
bool startCanFeedbackTask()
{
    static bool s_started = false;
    if (s_started) return true;

    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == nullptr) {
            DEBUG_ERR(kTag, "互斥量创建失败");
            return false;
        }
    }
    memset(s_latest, 0, sizeof(s_latest));
    memset(s_lastLoggedError, 0, sizeof(s_lastLoggedError));
    memset(s_valid,  0, sizeof(s_valid));
    memset(s_work,   0, sizeof(s_work));
    memset(s_workIdx, 0, sizeof(s_workIdx));

    BaseType_t xRet = xTaskCreatePinnedToCore(
        taskLoop,
        "can_fb",
        4096,
        nullptr,
        4,
        nullptr,
        0);
    if (xRet != pdPASS) {
        DEBUG_ERR(kTag, "任务创建失败");
        return false;
    }
    s_started = true;
    return true;
}

void pauseCanFeedbackTask(bool pause)
{
    if (s_paused == pause) return;  // 幂等：状态未变时不重复日志
    s_paused = pause;
    if (pause) {
        DEBUG_INFO(kTag, "CAN 监听已暂停（OTA 进行中）");
    } else {
        DEBUG_INFO(kTag, "CAN 监听已恢复");
    }
}

/**
 * @brief 拷贝某通道的最新快照（线程安全）。
 * @return true 表示拿到一份有效数据。
 */
bool getLatestCanFeedback(uint8_t channel, can_feedback_result_t *out)
{
    if (channel >= CAN_CHANNEL_MAX || out == nullptr || s_mutex == nullptr) {
        return false;
    }
    bool ok = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_valid[channel]) {
            *out = s_latest[channel];
            ok = true;
        }
        xSemaphoreGive(s_mutex);
    }
    return ok;
}
