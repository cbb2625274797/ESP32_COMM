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
#include "server/http_util.h"
#include "monitor/debug_log.h"

namespace {

constexpr uint8_t  kProbeType      = 0xA1U;  /* APP PING：无副作用探针 */
constexpr uint32_t kFramesDefault  = 200U;
constexpr uint32_t kFramesMax      = 2000U;  /* ~3s 上限，避免占死 HTTP/worker */
constexpr size_t   kPayloadDefault = 236U;   /* 满帧：等效 0xD1 电机 OTA 数据节奏 */
constexpr size_t   kPayloadMax     = 236U;   /* APP kTxBufSize=256 - V2_OVERHEAD */
constexpr uint32_t kFrameTimeoutMs = 500U;

} // namespace

esp_err_t spi_speedtest_handler(httpd_req_t *req)
{
    uint32_t frames = http_query_u32(req, "frames", kFramesDefault);
    if (frames < 1U) { frames = 1U; }
    if (frames > kFramesMax) { frames = kFramesMax; }
    size_t payload_len = (size_t)http_query_u32(req, "payload", (uint32_t)kPayloadDefault);
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
    /* RTT 分布直方图（µs 计时，<2ms 区间细化为 0.5ms 粒度）：
     * <0.5 / 0.5-1 / 1-1.5 / 1.5-2 / 2-3 / 3-5 / >=5 ms，
     * 用于判断抖动形态（<2ms 内的尖峰/双峰 vs 离散 +1ms 碰撞） */
    uint32_t histo[7] = {0U, 0U, 0U, 0U, 0U, 0U, 0U};
    /* 事务耗时（worker 内执行段，SpiMaster::lastWorkerExecUs）与
     * 调度/唤醒开销（RTT − 事务）拆解：事务慢=GD32 应答/SPI 时序；
     * 调度慢=ESP32 任务抢占（WiFi/esp_timer 等）。 */
    uint64_t w_sum_us = 0U, w_min_us = UINT64_MAX, w_max_us = 0U;
    uint64_t s_sum_us = 0U, s_min_us = UINT64_MAX, s_max_us = 0U;
    /* 5 段细分（SpiMaster::SpiProfile）：提交/调度、发送帧、等应答、读响应、唤醒 */
    uint64_t seg_sum[5] = {0U, 0U, 0U, 0U, 0U};
    uint64_t seg_max[5] = {0U, 0U, 0U, 0U, 0U};

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
            const uint64_t wus = SpiMaster::instance().lastWorkerExecUs();
            const uint64_t sus = (rtt > wus) ? (rtt - wus) : 0U;
            w_sum_us += wus;
            if (wus < w_min_us) { w_min_us = wus; }
            if (wus > w_max_us) { w_max_us = wus; }
            s_sum_us += sus;
            if (sus < s_min_us) { s_min_us = sus; }
            if (sus > s_max_us) { s_max_us = sus; }
            /* 5 段细分（防回绕钳 0） */
            const SpiMaster::SpiProfile &pf = SpiMaster::instance().lastProfile();
            const uint64_t seg[5] = {
                (pf.t_w0 > pf.t_submit0) ? (pf.t_w0 - pf.t_submit0) : 0U,
                (pf.t_send1 > pf.t_send0) ? (pf.t_send1 - pf.t_send0) : 0U,
                (pf.t_wait1 > pf.t_wait0) ? (pf.t_wait1 - pf.t_wait0) : 0U,
                (pf.t_read1 > pf.t_wait1) ? (pf.t_read1 - pf.t_wait1) : 0U,
                (pf.t_submit1 > pf.t_w1) ? (pf.t_submit1 - pf.t_w1) : 0U,
            };
            for (int k = 0; k < 5; ++k) {
                seg_sum[k] += seg[k];
                if (seg[k] > seg_max[k]) { seg_max[k] = seg[k]; }
            }
            if (rtt < 500U)       { ++histo[0]; }
            else if (rtt < 1000U) { ++histo[1]; }
            else if (rtt < 1500U) { ++histo[2]; }
            else if (rtt < 2000U) { ++histo[3]; }
            else if (rtt < 3000U) { ++histo[4]; }
            else if (rtt < 5000U) { ++histo[5]; }
            else                  { ++histo[6]; }
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
    if (w_min_us == UINT64_MAX) { w_min_us = 0U; }
    if (s_min_us == UINT64_MAX) { s_min_us = 0U; }

    DEBUG_INFO("SPI", "[SPEEDTEST] done ok=%u fail=%u avg=%uus tput=%.1fKB/s",
               (unsigned)n_ok, (unsigned)n_fail, (unsigned)rtt_avg_us, tput_kbps);

    char text[640];
    const int len = snprintf(text, sizeof(text),
        "SPI 测速结果\n"
        "帧数: %u  成功: %u  失败: %u%s\n"
        "载荷: %u B/帧  总耗时: %u ms\n"
        "往返延迟: 平均 %llu µs  最快 %llu µs  最慢 %llu µs\n"
        "SPI 事务: 平均 %llu µs  最快 %llu µs  最慢 %llu µs\n"
        "调度开销: 平均 %llu µs  最快 %llu µs  最慢 %llu µs\n"
        "单帧分段 平均: 提交/调度 %llu | 发送帧 %llu | 等应答 %llu | 读响应 %llu | 唤醒 %llu µs\n"
        "单帧分段 最慢: 提交/调度 %llu | 发送帧 %llu | 等应答 %llu | 读响应 %llu | 唤醒 %llu µs\n"
        "延迟分布: <0.5ms:%u  0.5-1ms:%u  1-1.5ms:%u  1.5-2ms:%u  2-3ms:%u  3-5ms:%u  >=5ms:%u\n"
        "等效吞吐: %.1f KB/s\n",
        (unsigned)frames, (unsigned)n_ok, (unsigned)n_fail,
        (n_fail > 0U) ? "  <-- 存在失败帧, 链路异常!" : "",
        (unsigned)payload_len, (unsigned)elapsed_ms,
        (unsigned long long)rtt_avg_us, (unsigned long long)rtt_min_us,
        (unsigned long long)rtt_max_us,
        (unsigned long long)(w_sum_us / (n_ok > 0U ? n_ok : 1U)),
        (unsigned long long)w_min_us, (unsigned long long)w_max_us,
        (unsigned long long)(s_sum_us / (n_ok > 0U ? n_ok : 1U)),
        (unsigned long long)s_min_us, (unsigned long long)s_max_us,
        (unsigned long long)(seg_sum[0] / (n_ok > 0U ? n_ok : 1U)),
        (unsigned long long)(seg_sum[1] / (n_ok > 0U ? n_ok : 1U)),
        (unsigned long long)(seg_sum[2] / (n_ok > 0U ? n_ok : 1U)),
        (unsigned long long)(seg_sum[3] / (n_ok > 0U ? n_ok : 1U)),
        (unsigned long long)(seg_sum[4] / (n_ok > 0U ? n_ok : 1U)),
        (unsigned long long)seg_max[0], (unsigned long long)seg_max[1],
        (unsigned long long)seg_max[2], (unsigned long long)seg_max[3],
        (unsigned long long)seg_max[4],
        (unsigned)histo[0], (unsigned)histo[1], (unsigned)histo[2],
        (unsigned)histo[3], (unsigned)histo[4],
        (unsigned)histo[5], (unsigned)histo[6], tput_kbps);

    http_set_cors_headers(req);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, text, (len > 0) ? len : 0);
}
