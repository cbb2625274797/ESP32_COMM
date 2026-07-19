/*
 * gd32_adc_feedback.cpp - GD32 ADC 调试反馈解析与缓存
 */
#include "monitor/gd32_adc_feedback.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

namespace {

constexpr uint8_t  kPayloadVer = 1U;
constexpr size_t   kPayloadLen = 20U;

static SemaphoreHandle_t s_mux = nullptr;
static gd32_adc_feedback_t s_latest = {};
static bool s_have = false;

/** @brief 读小端 u16。 */
static uint16_t rdU16Le(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

/** @brief 读小端 u32。 */
static uint32_t rdU32Le(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

/** @brief 懒创建快照互斥量。 */
static void ensureMux()
{
    if (s_mux == nullptr) {
        s_mux = xSemaphoreCreateMutex();
    }
}

} // namespace

/**
 * @brief 处理一帧 0xC4 ADC 调试反馈并更新最新快照。
 * @param payload  固定 20B 载荷
 * @param len      实际长度
 * @return true 解析并写入成功
 */
bool onGd32AdcFeedbackEvent(const uint8_t *payload, size_t len)
{
    if (payload == nullptr || len < kPayloadLen) {
        return false;
    }
    if (payload[0] != kPayloadVer) {
        return false;
    }

    const uint8_t flags = payload[1];
    const int16_t temp_x100 = static_cast<int16_t>(rdU16Le(&payload[2]));
    const uint16_t vdda_mV = rdU16Le(&payload[4]);
    const uint16_t vin_mV = rdU16Le(&payload[6]);
    const uint16_t temp_raw = rdU16Le(&payload[8]);
    const uint16_t vref_raw = rdU16Le(&payload[10]);
    const uint32_t seq = rdU32Le(&payload[12]);
    const uint32_t uptime = rdU32Le(&payload[16]);

    gd32_adc_feedback_t fb = {};
    fb.valid = (flags & 0x01U) != 0U;
    fb.temperature_c = static_cast<float>(temp_x100) / 100.0f;
    fb.vdda_v = static_cast<float>(vdda_mV) / 1000.0f;
    fb.vin_adc_v = static_cast<float>(vin_mV) / 1000.0f;
    /* 原理图：VCC — R18 47k — VIN_ADC — R13 4.7k — GND
     * VCC = V_adc * (R18+R13)/R13 = V_adc * 11 */
    fb.vcc_v = fb.vin_adc_v * ((47.0f + 4.7f) / 4.7f);
    fb.temp_raw = temp_raw;
    fb.vref_raw = vref_raw;
    fb.seq = seq;
    fb.uptime_sec = uptime;
    fb.recv_ms = esp_timer_get_time() / 1000;

    ensureMux();
    if (s_mux == nullptr) {
        return false;
    }
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    s_latest = fb;
    s_have = true;
    xSemaphoreGive(s_mux);
    return true;
}

/**
 * @brief 线程安全拷贝最近一次 ADC 反馈。
 * @param[out] out  输出缓冲
 * @return true 已有至少一帧有效数据
 */
bool getLatestGd32AdcFeedback(gd32_adc_feedback_t *out)
{
    if (out == nullptr) {
        return false;
    }
    ensureMux();
    if (s_mux == nullptr) {
        return false;
    }
    bool ok = false;
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_have) {
            *out = s_latest;
            ok = true;
        }
        xSemaphoreGive(s_mux);
    }
    return ok;
}

/**
 * @brief 距上次收到 0xC4 的毫秒数。
 * @return 年龄 ms；尚无数据返回 UINT32_MAX
 */
uint32_t gd32AdcFeedbackAgeMs(void)
{
    ensureMux();
    if (s_mux == nullptr) {
        return UINT32_MAX;
    }
    int64_t recv = 0;
    bool have = false;
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(20)) == pdTRUE) {
        have = s_have;
        recv = s_latest.recv_ms;
        xSemaphoreGive(s_mux);
    }
    if (!have || recv <= 0) {
        return UINT32_MAX;
    }
    const int64_t now = esp_timer_get_time() / 1000;
    if (now < recv) {
        return 0U;
    }
    const int64_t age = now - recv;
    if (age > static_cast<int64_t>(UINT32_MAX)) {
        return UINT32_MAX;
    }
    return static_cast<uint32_t>(age);
}
