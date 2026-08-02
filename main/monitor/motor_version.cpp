/*
 * motor_version.cpp - GD32 多电机版本推送解析与缓存 (SPI TYPE 0xC6)
 *
 * 镜像 motor_state.cpp 的互斥保护缓存模型。
 */
#include "monitor/motor_version.h"
#include "monitor/spi_ring_cache.h"

#include <string.h>

namespace {

static SemaphoreHandle_t s_mux = nullptr;
static motor_version_table_t s_table = {};

} // namespace

bool onMotorVersionEvent(const uint8_t *payload, size_t len)
{
    if (payload == nullptr || len < MOTORVER_HEADER_LEN) {
        return false;
    }

    const uint8_t count = payload[0];

    ringCacheEnsureMux(&s_mux);
    if (s_mux == nullptr) return false;
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(20)) != pdTRUE) return false;

    size_t offset = MOTORVER_HEADER_LEN;
    for (uint8_t i = 0; i < count; ++i) {
        if (offset + MOTORVER_ENTRY_SIZE > len) {
            break;
        }
        const uint8_t node = payload[offset];
        if (node >= 1 && node <= MOTORVER_MAX_NODES) {
            motor_version_slot_t &slot = s_table.slots[node - 1];
            slot.node_id = node;
            slot.seq     = rdU32Le(&payload[offset + 1]);
            memcpy(slot.version, &payload[offset + 5], MOTORVER_DLC);
            slot.recv_ms = esp_timer_get_time() / 1000;
            slot.valid   = true;
            s_table.have_data = true;
        }
        offset += MOTORVER_ENTRY_SIZE;
    }

    xSemaphoreGive(s_mux);
    return true;
}

bool getLatestMotorVersion(motor_version_table_t *out)
{
    if (out == nullptr) return false;
    ringCacheEnsureMux(&s_mux);
    if (s_mux == nullptr) return false;

    bool ok = false;
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_table.have_data) {
            *out = s_table;
            ok = true;
        }
        xSemaphoreGive(s_mux);
    }
    return ok;
}