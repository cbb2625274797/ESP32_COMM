/*
 * can_feedback_task.cpp - 被动监听 GD32 主动推送的事件
 *
 * 架构（2026-08 改造后）：
 *   0xC1 CAN 反馈已改为请求驱动（前端请求时 ESP32 主动向 GD32 查询），
 *   不再被动缓存。本任务仅保留 0xC4/0xC5/0xC6/0xD2 事件分发：
 *     0xC4 ADC 调试反馈、0xC5 多电机状态、0xC6 多电机版本、0xD2 电机 OTA 进度
 *   这些仍由 GD32 主动推送，ESP32 被动接收。
 *
 *   OTA 期间通过 pauseCanFeedbackTask(true) 暂停本任务，避免与 OTA 抢占 SPI。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "transport/spi_master.h"
#include "monitor/debug_log.h"
#include "monitor/can_feedback_task.h"
#include "monitor/gd32_adc_feedback.h"
#include "monitor/motor_state.h"
#include "monitor/motor_version.h"
#include "monitor/motor_ota_relay.h"
#include "esp_system.h"

#include <string.h>

namespace {

constexpr uint32_t kWaitRingMs      = 500U;    ///< 单次等待 GD32 按铃超时
constexpr uint32_t kPauseDelayMs    = 100U;    ///< 暂停时的轮询间隔
constexpr uint8_t  kSpiAdcFeedbackType = 0xC4U;  ///< GD32 ADC 调试反馈
constexpr uint8_t  kSpiMotorStateType  = 0xC5U;  ///< GD32 多电机状态推送
constexpr uint8_t  kSpiMotorVersionType = 0xC6U; ///< GD32 多电机版本推送（事件驱动）
constexpr uint8_t  kSpiMotorOtaStatusType = 0xD2U; ///< GD32 电机 OTA relay 进度推送
constexpr size_t   kRingPayloadMax  = SPI_OTA_FRAME_MAX;  ///< 与变长读一致，避免截断
constexpr const char *kTag           = "CAN_FB";

static volatile bool s_paused = false;

/**
 * @brief can_fb 任务主循环：takeEvent 分发 0xC4/0xC5/0xC6/0xD2 推送事件。
 *   0xC1 CAN 反馈已改为请求驱动（canFdQueryFeedback），不再在此处理。
 */
static void taskLoop(void *)
{
    DEBUG_INFO(kTag, "事件监听任务启动 (0xC4 ADC / 0xC5 motor / 0xC6 ver / 0xD2 ota)");

    /* 冷启动延后开始监听，错开上电初始化窗口 */
    {
        esp_reset_reason_t rr = esp_reset_reason();
        if (rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT) {
            constexpr uint32_t kCanFbDeferMs = 1500U;
            DEBUG_INFO(kTag, "冷启动: %lu ms 后开始事件监听",
                       (unsigned long)kCanFbDeferMs);
            vTaskDelay(pdMS_TO_TICKS(kCanFbDeferMs));
        }
    }

    uint8_t  ring_type = 0U;
    uint8_t  ring_payload[kRingPayloadMax] = {};
    size_t   ring_len = 0U;

    for (;;) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(kPauseDelayMs));
            continue;
        }

        /* 阻塞等待 GD32 按铃事件（超时则重试） */
        if (!SpiMaster::instance().takeEvent(&ring_type, ring_payload,
                                             sizeof(ring_payload), &ring_len,
                                             kWaitRingMs)) {
            continue;
        }

        /* 0xC4 ADC 调试反馈：GD32 关开关则不推；ESP32 始终可解析（无本地开关） */
        if (ring_type == kSpiAdcFeedbackType) {
            (void)onGd32AdcFeedbackEvent(ring_payload, ring_len);
            continue;
        }

        /* 0xC5 多电机状态推送：GD32 250ms 分页推送，ESP32 解析入缓存 */
        if (ring_type == kSpiMotorStateType) {
            (void)onMotorStateEvent(ring_payload, ring_len);
            continue;
        }

        /* 0xC6 多电机版本推送：GD32 收到电机版本应答(0x00FF)后事件驱动推送 */
        if (ring_type == kSpiMotorVersionType) {
            (void)onMotorVersionEvent(ring_payload, ring_len);
            continue;
        }

        /* 0xD2 电机 OTA relay 进度推送 */
        if (ring_type == kSpiMotorOtaStatusType) {
            onMotorOtaStatusPush(ring_payload, ring_len);
            continue;
        }

        /* 0xC1 已改为请求驱动，不再在此处理；未知类型静默跳过 */
    }
}

} // namespace

/**
 * @brief 启动事件监听任务（幂等）。
 * @return true 表示任务已存在或创建成功。
 */
bool startCanFeedbackTask()
{
    static bool s_started = false;
    if (s_started) return true;

    BaseType_t xRet = xTaskCreatePinnedToCore(
        taskLoop,
        "can_fb",
        TASK_STACK_CAN_FB,
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

/**
 * @brief 暂停/恢复事件监听。
 * @param pause true=OTA 期间暂停；false=恢复
 */
void pauseCanFeedbackTask(bool pause)
{
    if (s_paused == pause) return;  // 幂等：状态未变时不重复日志
    s_paused = pause;
    if (pause) {
        DEBUG_DBG(kTag, "事件监听已暂停（OTA 进行中）");
    } else {
        DEBUG_DBG(kTag, "事件监听已恢复");
    }
}

/**
 * @brief 查询是否处于暂停（OTA / send_batch 期间为 true）。
 *   暂停期间 HTTP handler 应跳过 0xC1 查询，避免与 OTA 抢占 SPI。
 */
bool canFeedbackPaused()
{
    return s_paused;
}
