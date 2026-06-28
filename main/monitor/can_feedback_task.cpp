/*
 * can_feedback_task.cpp - 周期性轮询 GD32 三路 CAN 1Hz 反馈快照
 *
 * 通过 SPI 协议帧 0xC1 拉取 GD32 端的通道统计与最近一帧信息，
 * 用调试日志输出；其它消费者（Web/WebSocket）可直接读取
 * CanFeedbackTask::snapshot(channel) 拷贝最新一份数据。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "transport/canfd_bridge.h"
#include "monitor/debug_log.h"
#include "monitor/can_feedback_task.h"

#include <string.h>

namespace {

constexpr uint32_t kPollIntervalMs   = 1000U;    ///< 1Hz 拉取节拍
constexpr uint32_t kPollTimeoutMs    = 200U;     ///< 单次 SPI 事务超时
constexpr const char *kTag           = "CAN_FB";

static can_feedback_result_t s_latest[CAN_CHANNEL_MAX];
static bool                   s_valid [CAN_CHANNEL_MAX];
static SemaphoreHandle_t      s_mutex = nullptr;
static volatile bool          s_paused = false;
static can_channel_feedback_t s_prev_stats[CAN_CHANNEL_MAX];
static bool                   s_prev_valid[CAN_CHANNEL_MAX];
static bool                   s_prev_poll_ok[CAN_CHANNEL_MAX];

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
    DEBUG_INFO(kTag, "CAN 反馈轮询任务启动 (1Hz, 0xC1)");

    can_feedback_result_t fb{};
    for (;;) {
        /* OTA 进行中时暂停 CAN 轮询，避免抢占 SPI 导致 OTA 握手失败 */
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
            continue;
        }

        for (uint8_t ch = 0U; ch < CAN_CHANNEL_MAX; ++ch) {
            const int rc = canFdPollFeedback(static_cast<can_channel_t>(ch),
                                             &fb, kPollTimeoutMs);
            if (rc == 0) {
                publish(ch, fb);

                const bool prevOk  = s_prev_poll_ok[ch];
                const bool changed = (!s_prev_valid[ch]) ||
                                     (fb.stats.rx_count      != s_prev_stats[ch].rx_count) ||
                                     (fb.stats.tx_count      != s_prev_stats[ch].tx_count) ||
                                     (fb.stats.rx_drop_count != s_prev_stats[ch].rx_drop_count) ||
                                     (fb.stats.error_count   != s_prev_stats[ch].error_count) ||
                                     (fb.stats.mode          != s_prev_stats[ch].mode) ||
                                     (fb.stats.snapshot_seq  != s_prev_stats[ch].snapshot_seq);
                const bool abnormal = (fb.stats.rx_drop_count != 0U) ||
                                      (fb.stats.error_count   != 0U) ||
                                      (fb.stats.mode          != 0U);

                if (!prevOk || changed || abnormal) {
                    DEBUG_INFO(kTag,
                        "CH%u seq=%lu mode=%u rx=%lu tx=%lu drop=%lu err=%lu ids=%u",
                        static_cast<unsigned>(ch),
                        static_cast<unsigned long>(fb.stats.snapshot_seq),
                        static_cast<unsigned>(fb.stats.mode),
                        static_cast<unsigned long>(fb.stats.rx_count),
                        static_cast<unsigned long>(fb.stats.tx_count),
                        static_cast<unsigned long>(fb.stats.rx_drop_count),
                        static_cast<unsigned long>(fb.stats.error_count),
                        static_cast<unsigned>(fb.valid));
                }

                s_prev_stats[ch]   = fb.stats;
                s_prev_valid[ch]   = true;
                s_prev_poll_ok[ch] = true;
            } else {
                if (s_prev_poll_ok[ch]) {
                    DEBUG_WARN(kTag, "CH%u 轮询失败", static_cast<unsigned>(ch));
                }
                s_prev_stats[ch]   = {};
                s_prev_valid[ch]   = false;
                s_prev_poll_ok[ch] = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

} // namespace

/**
 * @brief 启动 1Hz CAN 反馈轮询任务（幂等）。
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
    memset(s_valid,  0, sizeof(s_valid));

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
    s_paused = pause;
    if (pause) {
        DEBUG_INFO(kTag, "CAN 轮询已暂停（OTA 进行中）");
    } else {
        DEBUG_INFO(kTag, "CAN 轮询已恢复");
    }
}

bool getLatestIdSnapshot(uint8_t channel, id_snapshot_cache_t *out)
{
    if (channel >= CAN_CHANNEL_MAX || out == nullptr || s_mutex == nullptr) return false;
    bool ok = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_latest[channel].valid) {
            out->count = s_latest[channel].valid;
            memcpy(out->entries, s_latest[channel].entries, sizeof(out->entries));
            ok = true;
        }
        xSemaphoreGive(s_mutex);
    }
    return ok;
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
