/*
 * gd32_adc_feedback.h - 被动接收 GD32 ADC 调试反馈（SPI TYPE 0xC4）
 *
 * 与 CAN 0xC1 同模型：GD32 1Hz 主动按铃推送，ESP32 takeEvent 侧解析入库。
 * 是否推送由 GD32 CONFIG_SPI_ADC_FEEDBACK_ENABLE 决定，经 HELLO/PING 身份块
 * （V2_FEAT_ADC_FEEDBACK）上报；ESP32 无本地开关，有帧即解析。
 */
#ifndef GD32_ADC_FEEDBACK_H
#define GD32_ADC_FEEDBACK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 最近一次 GD32 ADC 调试快照（工程量已换算）。 */
typedef struct {
    bool     valid;          ///< GD32 sample.valid
    float    temperature_c;  ///< ℃
    float    vdda_v;         ///< V（MCU 模拟供电，约 3.3）
    float    vin_adc_v;      ///< V（分压后 ADC 引脚 PA6）
    float    vcc_v;          ///< V（输入母线 VCC，由 vin 分压反推）
    uint16_t temp_raw;
    uint16_t vref_raw;
    uint32_t seq;
    uint32_t uptime_sec;     ///< GD32 侧 uptime
    int64_t  recv_ms;        ///< ESP32 收到时 esp_timer ms
} gd32_adc_feedback_t;

/**
 * @brief 处理一帧 0xC4 EVENT payload（由 can_fb 任务 takeEvent 分发）。
 * @return true 表示已接受并更新快照。
 */
bool onGd32AdcFeedbackEvent(const uint8_t *payload, size_t len);

/**
 * @brief 拷贝最新快照。
 * @param[out] out  输出缓冲
 * @return true 表示至少收到过一帧
 */
bool getLatestGd32AdcFeedback(gd32_adc_feedback_t *out);

/** @brief 距离上次收到的毫秒数；无数据返回 UINT32_MAX。 */
uint32_t gd32AdcFeedbackAgeMs(void);

#ifdef __cplusplus
}
#endif

#endif /* GD32_ADC_FEEDBACK_H */
