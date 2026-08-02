/*
 * spi_ring_cache.h - SPI ring push 缓存共享辅助
 *
 * gd32_adc_feedback / motor_state / motor_version 三个模块共用相同的
 * 互斥懒创建 + 小端读 + ageMs 计算样板，提取到此处消除三份拷贝。
 */
#ifndef SPI_RING_CACHE_H
#define SPI_RING_CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 小端读 ── */

static inline uint16_t rdU16Le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rdU32Le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ── 互斥懒创建 ── */

static inline void ringCacheEnsureMux(SemaphoreHandle_t *mux)
{
    if (*mux == nullptr) {
        *mux = xSemaphoreCreateMutex();
    }
}

/* ── 通用 ageMs ──
 * 传入缓存的 have 标志 + recv_ms 时间戳，返回距 now 的毫秒数。
 * 无数据或 recv_ms<=0 返回 UINT32_MAX。
 */
static inline uint32_t ringCacheAgeMs(bool have, int64_t recv_ms)
{
    if (!have || recv_ms <= 0) return UINT32_MAX;
    const int64_t now = esp_timer_get_time() / 1000;
    if (now < recv_ms) return 0U;
    const int64_t age = now - recv_ms;
    return (age > (int64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)age;
}

#ifdef __cplusplus
}
#endif

#endif // SPI_RING_CACHE_H