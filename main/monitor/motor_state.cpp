/*
 * motor_state.cpp - GD32 多电机状态推送解析与缓存 (SPI TYPE 0xC5)
 */
#include "monitor/motor_state.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

namespace {

static SemaphoreHandle_t s_mux = nullptr;
static motor_state_table_t s_table = {};

static uint32_t rdU32Le(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

static void ensureMux()
{
    if (s_mux == nullptr) {
        s_mux = xSemaphoreCreateMutex();
    }
}

} // namespace

bool onMotorStateEvent(const uint8_t *payload, size_t len)
{
    if (payload == nullptr || len < MOTOR_PAGE_HEADER) {
        return false;
    }

    const uint8_t page        = payload[0];
    const uint8_t total_pages = payload[1];
    const uint8_t entry_count = payload[2];
    const uint8_t active_cnt  = payload[3];

    if (page >= MOTOR_TOTAL_PAGES || total_pages == 0 || total_pages > MOTOR_TOTAL_PAGES) {
        return false;
    }

    /* 验证长度足够容纳声明的 entry 数 */
    const size_t expected = MOTOR_PAGE_HEADER + static_cast<size_t>(entry_count) * MOTOR_ENTRY_SIZE;
    if (len < expected) {
        return false;
    }

    ensureMux();
    if (s_mux == nullptr) return false;
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(20)) != pdTRUE) return false;

    /* page 0 时刷新全局字段。
     * active 字节 bit7 = boot_active（总线上有电机在 BOOT，0x103 心跳），
     * 低 7 位才是真实在线电机数。 */
    if (page == 0) {
        s_table.active_count = static_cast<uint8_t>(active_cnt & 0x7FU);
        s_table.boot_active  = (active_cnt & 0x80U) ? 1U : 0U;
    }

    /* 逐条解析 entry：node×channel 双键槽位，同节点号多通道互不覆盖 */
    size_t offset = MOTOR_PAGE_HEADER;
    for (uint8_t i = 0; i < entry_count; ++i) {
        const uint8_t *e = &payload[offset];
        const uint8_t node = e[0];
        const uint8_t ch   = e[1];
        if (node < 1 || node > MOTOR_MAX_NODES || ch >= MOTOR_CHANNELS) {
            offset += MOTOR_ENTRY_SIZE;
            continue;
        }

        motor_slot_t &slot = s_table.slots[(node - 1) * MOTOR_CHANNELS + ch];
        slot.node_id  = node;
        slot.channel  = ch;
        slot.online   = e[2];
        slot.dlc      = e[3];
        slot.rx_count = rdU32Le(&e[4]);
        slot.seq      = rdU32Le(&e[8]);
        memcpy(slot.data, &e[12], MOTOR_STATUS_DLC);

        offset += MOTOR_ENTRY_SIZE;
    }

    /* 统计有效槽位 */
    uint8_t valid = 0;
    for (uint8_t i = 0; i < MOTOR_MAX_SLOTS; ++i) {
        if (s_table.slots[i].node_id != 0) valid++;
    }
    s_table.valid_slots = valid;
    s_table.recv_ms = esp_timer_get_time() / 1000;
    s_table.have_data = true;

    xSemaphoreGive(s_mux);
    return true;
}

bool getLatestMotorState(motor_state_table_t *out)
{
    if (out == nullptr) return false;
    ensureMux();
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

uint32_t motorStateAgeMs(void)
{
    ensureMux();
    if (s_mux == nullptr) return UINT32_MAX;

    int64_t recv = 0;
    bool have = false;
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(20)) == pdTRUE) {
        have = s_table.have_data;
        recv = s_table.recv_ms;
        xSemaphoreGive(s_mux);
    }
    if (!have || recv <= 0) return UINT32_MAX;

    const int64_t now = esp_timer_get_time() / 1000;
    if (now < recv) return 0U;
    const int64_t age = now - recv;
    return (age > static_cast<int64_t>(UINT32_MAX)) ? UINT32_MAX
                                                     : static_cast<uint32_t>(age);
}
