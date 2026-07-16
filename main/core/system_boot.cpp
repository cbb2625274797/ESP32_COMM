/**
 * @file system_boot.cpp
 * @brief 启动模块位图实现。
 */
#include "system_boot.h"
#include "monitor/debug_log.h"

#include <stdio.h>
#include <string.h>

namespace {

struct BootEntry {
    uint32_t    bit;
    const char *name;
    bool        critical;
};

static const BootEntry kEntries[] = {
    { BOOT_NETIF,          "netif",           true  },
    { BOOT_EVENT_LOOP,     "event_loop",      true  },
    { BOOT_DEBUG_LOG,      "debug_log",       true  },
    { BOOT_WIFI_EARLY,     "wifi_early",      true  },
    { BOOT_HEALTH_MON,     "health_mon",      true  },
    { BOOT_HEALTH_TASK,    "health_task",     true  },
    { BOOT_CPU_STATS,      "cpu_stats",       false },
    { BOOT_PROTOCOL,       "protocol",        true  },
    { BOOT_OTA_COORD,      "ota_coord",       true  },
    { BOOT_SPI,            "spi",             true  },
    { BOOT_OTA_RELAY_TASK, "ota_relay_task",  true  },
    { BOOT_WIFI_TASK,      "wifi_task",       true  },
    { BOOT_BLE_TASK,       "ble_task",        true  },
    { BOOT_CAN_LINK,       "can_link",        false },
    { BOOT_CAN_FB_TASK,    "can_fb_task",     false },
    { BOOT_OTA_VALID,      "ota_mark_valid",  false },
};

static uint32_t s_mask = 0U;

static void evaluate(bool *critical_ok, bool *all_ok, char *fail_list, size_t fail_cap)
{
    *critical_ok = true;
    *all_ok = true;
    if (fail_list && fail_cap > 0U) {
        fail_list[0] = '\0';
    }
    size_t fail_len = 0U;

    for (size_t i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); ++i) {
        const BootEntry &e = kEntries[i];
        const bool ok = (s_mask & e.bit) != 0U;
        if (ok) {
            continue;
        }
        *all_ok = false;
        if (e.critical) {
            *critical_ok = false;
        }
        if (!fail_list || fail_cap == 0U) {
            continue;
        }
        if (fail_len + 2U < fail_cap) {
            if (fail_len > 0U) {
                fail_list[fail_len++] = ',';
            }
            const char *p = e.name;
            while (*p != '\0' && fail_len + 1U < fail_cap) {
                fail_list[fail_len++] = *p++;
            }
            fail_list[fail_len] = '\0';
        }
    }
}

} // namespace

void SystemBoot::reset()
{
    s_mask = 0U;
}

void SystemBoot::mark(SystemBootBit bit)
{
    s_mask |= static_cast<uint32_t>(bit);
}

bool SystemBoot::report()
{
    bool critical_ok = true;
    bool all_ok = true;
    char fail_list[192] = {0};
    evaluate(&critical_ok, &all_ok, fail_list, sizeof(fail_list));

    if (critical_ok && all_ok) {
        DEBUG_INFO("BOOT", "init: OK (all modules)");
    } else if (critical_ok) {
        DEBUG_WARN("BOOT", "init: OK (critical), skipped/failed: %s",
                   fail_list[0] != '\0' ? fail_list : "?");
    } else {
        DEBUG_ERR("BOOT", "init: FAIL, modules: %s",
                  fail_list[0] != '\0' ? fail_list : "?");
    }
    return critical_ok;
}

int SystemBoot::appendStatusJson(char *buf, size_t cap, int offset)
{
    if (!buf || cap == 0 || offset < 0 || (size_t)offset >= cap) {
        return offset;
    }

    bool critical_ok = true;
    bool all_ok = true;
    char fail_list[192] = {0};
    evaluate(&critical_ok, &all_ok, fail_list, sizeof(fail_list));

    const char *summary;
    if (critical_ok && all_ok) {
        summary = "OK (all modules)";
    } else if (critical_ok) {
        summary = "OK (critical)";
    } else {
        summary = "FAIL";
    }

    int n = snprintf(buf + offset, cap - (size_t)offset,
        ",\"boot\":{\"critical_ok\":%s,\"all_ok\":%s,\"mask\":%lu,"
        "\"summary\":\"%s\",\"failed\":\"%s\",\"modules\":[",
        critical_ok ? "true" : "false",
        all_ok ? "true" : "false",
        (unsigned long)s_mask,
        summary,
        fail_list);
    if (n < 0) {
        return offset;
    }
    if (offset + n >= (int)cap) {
        buf[cap - 1] = '\0';
        return (int)(cap - 1);
    }
    offset += n;

    for (size_t i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); ++i) {
        const BootEntry &e = kEntries[i];
        const bool ok = (s_mask & e.bit) != 0U;
        n = snprintf(buf + offset, cap - (size_t)offset,
            "%s{\"name\":\"%s\",\"ok\":%s,\"critical\":%s}",
            i == 0 ? "" : ",",
            e.name,
            ok ? "true" : "false",
            e.critical ? "true" : "false");
        if (n < 0) {
            return offset;
        }
        if (offset + n >= (int)cap) {
            buf[cap - 1] = '\0';
            return (int)(cap - 1);
        }
        offset += n;
    }

    n = snprintf(buf + offset, cap - (size_t)offset, "]}");
    if (n < 0) {
        return offset;
    }
    if (offset + n >= (int)cap) {
        buf[cap - 1] = '\0';
        return (int)(cap - 1);
    }
    return offset + n;
}
