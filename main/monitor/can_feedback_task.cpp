/*
 * can_feedback_task.cpp - GD32 周期查询 + 事件消费任务
 *
 * 架构（2026-08 改造后）：
 *   0xC1 CAN 反馈：请求驱动（HTTP 侧 canFdQueryFeedback），本任务不处理。
 *   0xC4 ADC / 0xC5 电机状态：由 GD32 主动按铃推送改为本任务周期查询——
 *     0xC5 每 250ms 查 MOTOR_TOTAL_PAGES 页，0xC4 每 1s 查 1 帧，查询响应
 *     payload 格式与推送一致，复用 onMotorStateEvent()/onGd32AdcFeedbackEvent()
 *     解析入库。消除 OTA 期间 GD32 悬空按铃（ESP32 抑制 ring 不读）导致
 *     “按铃超时取消”刷屏。
 *   0xC6 电机版本 / 0xD2 电机 OTA 进度：仍由 GD32 事件驱动推送，本任务 takeEvent 消费。
 *
 *   OTA 期间通过 pauseCanFeedbackTask(true) 暂停本任务（不查询不消费）；
 *   电机 OTA 期间（motorOtaIsActive()）也暂停，避免与 0xD1 喂数抢占 SPI。
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

constexpr uint32_t kQueryMotorStateMs = 250U;   ///< 0xC5 电机状态查询周期
constexpr uint32_t kQueryAdcMs        = 1000U;  ///< 0xC4 ADC 查询周期
constexpr uint32_t kEventWaitMs       = 20U;    ///< 单次等 0xC6/0xD2 事件超时
constexpr uint32_t kPauseDelayMs      = 100U;   ///< 暂停时的轮询间隔
constexpr uint8_t  kSpiAdcFeedbackType    = 0xC4U;  ///< GD32 ADC 调试反馈
constexpr uint8_t  kSpiMotorStateType     = 0xC5U;  ///< GD32 多电机状态查询
constexpr uint8_t  kSpiMotorVersionType   = 0xC6U;  ///< GD32 多电机版本推送（事件驱动）
constexpr uint8_t  kSpiMotorOtaStatusType = 0xD2U;  ///< GD32 电机 OTA relay 进度推送
constexpr size_t   kPayloadMax = SPI_OTA_FRAME_MAX; ///< 与变长读一致，避免截断
constexpr const char *kTag     = "CAN_FB";

static volatile bool s_paused = false;
static TaskHandle_t s_taskHandle = nullptr;

/* 任务独占静态响应缓冲（消除每次在任务栈上分配 256B 大数组，防栈溢出） */
static uint8_t s_motor_resp_buf[kPayloadMax] = {};
static uint8_t s_adc_resp_buf[kPayloadMax]   = {};
static uint8_t s_push_ring_buf[kPayloadMax]  = {};

/**
 * @brief 按页查询 GD32 电机状态（0xC5 请求驱动，替代 GD32 250ms 推送）。
 *   任一页失败立即中断本轮：GD32 未就绪/瞬时错误时不空耗 6×200ms，下轮再试；
 *   page0 会刷新全局字段（active_count/boot_active），部分页成功即部分更新。
 */
static bool queryMotorStatePages(void)
{
    uint8_t req[1] = {0U};
    for (uint8_t page = 0U; page < MOTOR_TOTAL_PAGES; ++page) {
        req[0] = page;
        uint8_t resp_type = 0U;
        size_t resp_len = 0U;
        esp_err_t err = SpiMaster::instance().transact(
            kSpiMotorStateType, req, sizeof(req),
            &resp_type, s_motor_resp_buf, sizeof(s_motor_resp_buf), &resp_len, 200U);
        if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK ||
            resp_len < MOTOR_PAGE_HEADER) {
            return false;
        }
        (void)onMotorStateEvent(s_motor_resp_buf, resp_len);
    }
    return true;
}

/**
 * @brief 查询 GD32 ADC 调试快照（0xC4 请求驱动，替代 GD32 1Hz 推送）。
 *   GD32 0xC4 handler 回自身类型 0xC4（异于 0xC1/0xC5 的 0x80），两者皆接受。
 */
static bool queryAdcFeedback(void)
{
    uint8_t req[1] = {0U};
    uint8_t resp_type = 0U;
    size_t resp_len = 0U;
    esp_err_t err = SpiMaster::instance().transact(
        kSpiAdcFeedbackType, req, sizeof(req),
        &resp_type, s_adc_resp_buf, sizeof(s_adc_resp_buf), &resp_len, 200U);
    if (err != ESP_OK || resp_len < 1U) {
        return false;
    }
    if (resp_type != kSpiAdcFeedbackType && resp_type != SPI_OTA_RSP_ACK) {
        return false;
    }
    (void)onGd32AdcFeedbackEvent(s_adc_resp_buf, resp_len);
    return true;
}

/**
 * @brief 消费剩余事件驱动推送（0xC6 电机版本 / 0xD2 电机 OTA 进度）。
 *   0xC4/0xC5 已改请求驱动，不再有对应推送；未知类型静默跳过。
 */
static void drainPushEvents(uint32_t wait_ms)
{
    uint8_t ring_type = 0U;
    size_t ring_len = 0U;
    if (!SpiMaster::instance().takeEvent(&ring_type, s_push_ring_buf,
                                         sizeof(s_push_ring_buf), &ring_len,
                                         wait_ms)) {
        return;
    }
    if (ring_type == kSpiMotorVersionType) {
        (void)onMotorVersionEvent(s_push_ring_buf, ring_len);
    } else if (ring_type == kSpiMotorOtaStatusType) {
        onMotorOtaStatusPush(s_push_ring_buf, ring_len);
    }
}

/**
 * @brief can_fb 任务主循环：周期查询 0xC5/0xC4 + 消费 0xC6/0xD2 推送。
 *   OTA / 电机 OTA 期间暂停，SPI 交由 OTA 独占。
 */
static void taskLoop(void *)
{
    DEBUG_INFO(kTag, "请求驱动查询任务启动 (0xC5 250ms / 0xC4 1s / 0xC6,0xD2 推送)");

    /* 冷启动延后开始查询，错开上电初始化窗口 */
    {
        esp_reset_reason_t rr = esp_reset_reason();
        if (rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT) {
            constexpr uint32_t kCanFbDeferMs = 1500U;
            DEBUG_INFO(kTag, "冷启动: %lu ms 后开始查询",
                       (unsigned long)kCanFbDeferMs);
            vTaskDelay(pdMS_TO_TICKS(kCanFbDeferMs));
        }
    }

    TickType_t last_motor_q = xTaskGetTickCount();
    TickType_t last_adc_q   = xTaskGetTickCount();

    for (;;) {
        if (s_paused || motorOtaIsActive()) {
            vTaskDelay(pdMS_TO_TICKS(kPauseDelayMs));
            continue;
        }

        /* 链路在线自适应退避：若 SPI 处于离线状态，进入休眠退避，避免死循环重试和日志风暴 */
        if (!SpiMaster::instance().isLinkOnline()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            /* 尝试轻量查询一次作为探活 */
            (void)queryMotorStatePages();
            last_motor_q = xTaskGetTickCount();
            last_adc_q = xTaskGetTickCount();
            continue;
        }

        /* 周期查询：到期才执行（时间差对比，暂停恢复后立即补一次） */
        const TickType_t now = xTaskGetTickCount();
        if ((now - last_motor_q) >= pdMS_TO_TICKS(kQueryMotorStateMs)) {
            queryMotorStatePages();
            last_motor_q = xTaskGetTickCount();
        }
        if ((now - last_adc_q) >= pdMS_TO_TICKS(kQueryAdcMs)) {
            queryAdcFeedback();
            last_adc_q = xTaskGetTickCount();
        }

        /* 消费 0xC6/0xD2：短阻塞等 worker 入队（事件驱动唤醒） */
        drainPushEvents(kEventWaitMs);
    }
}

} // namespace

TaskHandle_t getCanFeedbackTaskHandle()
{
    return s_taskHandle;
}

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
        &s_taskHandle,
        0);
    if (xRet != pdPASS) {
        DEBUG_ERR(kTag, "任务创建失败");
        return false;
    }
    s_started = true;
    return true;
}

/**
 * @brief 暂停/恢复周期查询与事件消费。
 * @param pause true=OTA 期间暂停；false=恢复
 */
void pauseCanFeedbackTask(bool pause)
{
    if (s_paused == pause) return;  // 幂等：状态未变时不重复日志
    s_paused = pause;
    if (pause) {
        DEBUG_DBG(kTag, "周期查询已暂停（OTA 进行中）");
    } else {
        DEBUG_DBG(kTag, "周期查询已恢复");
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