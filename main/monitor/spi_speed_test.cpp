/**
 * @file spi_speed_test.cpp
 * @brief 【临时功能 SPI-SPEEDTEST】SPI 链路测速端点 GET /spi/speedtest
 *
 * 用 0xA1 APP PING（无副作用、GD32 在 onCsRelease ISR 内 µs 级应答）作探针，
 * 经 SpiMaster::transact() 走与业务完全相同的 worker 串行路径，
 * 测量事件驱动握手（ready 下降沿唤醒）下的真实 RTT 与等效吞吐。
 *
 * 用法:
 *   GET /spi/speedtest                     # 默认 200 帧 × 236B payload
 *   GET /spi/speedtest?frames=500&payload=0   # 500 帧空载（纯 RTT）
 * 返回中文纯文本: 帧数/成功/失败、总耗时、往返延迟 平均/最快/最慢、等效吞吐
 *
 * ── 删除本临时功能的步骤（共 3 处挂接点，全部以 [SPI-SPEEDTEST] 标记）──
 *   1. 删除本文件与 monitor/spi_speed_test.h
 *   2. main/CMakeLists.txt      删除 "monitor/spi_speed_test.cpp" 一行
 *   3. server/wifi_ota.cpp      删除 include 与 kHttpRoutes 中各一行
 *   （spi_master.cpp 中 0xA1 零盲等白名单为正式优化，保留不删）
 */
#include "spi_speed_test.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_timer.h"
#include "spi_master.h"
#include "http_handlers.h"   /* http_set_cors_headers */
#include "monitor/debug_log.h"

namespace {

constexpr uint8_t  kProbeType      = 0xA1U;  /* APP PING：无副作用探针 */
constexpr uint32_t kFramesDefault  = 200U;
constexpr uint32_t kFramesMax      = 2000U;  /* ~3s 上限，避免占死 HTTP/worker */
constexpr size_t   kPayloadDefault = 236U;   /* 满帧：等效 0xD1 电机 OTA 数据节奏 */
constexpr size_t   kPayloadMax     = 236U;   /* APP kTxBufSize=256 - V2_OVERHEAD */
constexpr uint32_t kFrameTimeoutMs = 500U;

/** @brief 从 query 里取无符号整型参数，缺失/非法时返回默认值。 */
uint32_t queryU32(httpd_req_t *req, const char *key, uint32_t defval)
{
    char qbuf[64];
    char vbuf[16];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) {
        return defval;
    }
    if (httpd_query_key_value(qbuf, key, vbuf, sizeof(vbuf)) != ESP_OK) {
        return defval;
    }
    char *end = nullptr;
    const unsigned long v = strtoul(vbuf, &end, 10);
    return (end && *end == '\0') ? (uint32_t)v : defval;
}

} // namespace

esp_err_t spi_speedtest_handler(httpd_req_t *req)
{
    uint32_t frames = queryU32(req, "frames", kFramesDefault);
    if (frames < 1U) { frames = 1U; }
    if (frames > kFramesMax) { frames = kFramesMax; }
    size_t payload_len = (size_t)queryU32(req, "payload", (uint32_t)kPayloadDefault);
    if (payload_len > kPayloadMax) { payload_len = kPayloadMax; }

    /* 探针 payload：递增填充（CRC32 覆盖，顺带校验数据路径完整性） */
    uint8_t probe[kPayloadMax];
    for (size_t i = 0; i < payload_len; ++i) {
        probe[i] = (uint8_t)i;
    }

    uint32_t n_ok = 0U;
    uint32_t n_fail = 0U;
    uint64_t rtt_sum_us = 0U;
    uint64_t rtt_min_us = UINT64_MAX;
    uint64_t rtt_max_us = 0U;
    /* RTT 分布直方图：<2 / 2-3 / 3-4 / 4-5 / >=5 ms，用于判断抖动形态
     * （双峰=离散 +1ms 碰撞；均匀=tick 相位量化） */
    uint32_t histo[5] = {0U, 0U, 0U, 0U, 0U};

    DEBUG_INFO("SPI", "[SPEEDTEST] start frames=%u payload=%u",
               (unsigned)frames, (unsigned)payload_len);
    const int64_t t_start = esp_timer_get_time();

    for (uint32_t i = 0U; i < frames; ++i) {
        uint8_t resp_type = 0U;
        uint8_t resp_payload[64];
        size_t resp_len = 0U;

        const int64_t t0 = esp_timer_get_time();
        const esp_err_t ret = SpiMaster::instance().transact(
            kProbeType, probe, payload_len,
            &resp_type, resp_payload, sizeof(resp_payload), &resp_len,
            kFrameTimeoutMs);
        const int64_t t1 = esp_timer_get_time();

        if (ret == ESP_OK && resp_type == 0x80U) {
            const uint64_t rtt = (uint64_t)(t1 - t0);
            ++n_ok;
            rtt_sum_us += rtt;
            if (rtt < rtt_min_us) { rtt_min_us = rtt; }
            if (rtt > rtt_max_us) { rtt_max_us = rtt; }
            const uint32_t ms_bucket = (uint32_t)(rtt / 1000U);
            if (ms_bucket < 2U)      { ++histo[0]; }
            else if (ms_bucket < 3U) { ++histo[1]; }
            else if (ms_bucket < 4U) { ++histo[2]; }
            else if (ms_bucket < 5U) { ++histo[3]; }
            else                     { ++histo[4]; }
        } else {
            ++n_fail;
        }
    }

    const int64_t elapsed_us = esp_timer_get_time() - t_start;
    const uint32_t elapsed_ms = (uint32_t)(elapsed_us / 1000);
    const uint64_t rtt_avg_us = (n_ok > 0U) ? (rtt_sum_us / n_ok) : 0U;
    /* 吞吐语义：payload 字节 / 完整"发送+应答"周期，等效 0xD1 单缓冲喂速上限 */
    double tput_kbps = 0.0;
    if (elapsed_us > 0 && n_ok > 0U) {
        tput_kbps = ((double)n_ok * (double)payload_len) / ((double)elapsed_us / 1000000.0) / 1024.0;
    }
    if (rtt_min_us == UINT64_MAX) { rtt_min_us = 0U; }

    DEBUG_INFO("SPI", "[SPEEDTEST] done ok=%u fail=%u avg=%uus tput=%.1fKB/s",
               (unsigned)n_ok, (unsigned)n_fail, (unsigned)rtt_avg_us, tput_kbps);

    char text[480];
    const int len = snprintf(text, sizeof(text),
        "SPI 测速结果\n"
        "帧数: %u  成功: %u  失败: %u%s\n"
        "载荷: %u B/帧  总耗时: %u ms\n"
        "往返延迟: 平均 %.1f ms  最快 %.1f ms  最慢 %.1f ms\n"
        "延迟分布: <2ms:%u  2-3ms:%u  3-4ms:%u  4-5ms:%u  >=5ms:%u\n"
        "等效吞吐: %.1f KB/s\n",
        (unsigned)frames, (unsigned)n_ok, (unsigned)n_fail,
        (n_fail > 0U) ? "  <-- 存在失败帧, 链路异常!" : "",
        (unsigned)payload_len, (unsigned)elapsed_ms,
        (double)rtt_avg_us / 1000.0, (double)rtt_min_us / 1000.0,
        (double)rtt_max_us / 1000.0,
        (unsigned)histo[0], (unsigned)histo[1], (unsigned)histo[2],
        (unsigned)histo[3], (unsigned)histo[4], tput_kbps);

    http_set_cors_headers(req);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, text, (len > 0) ? len : 0);
}
