/*
 * ota_coordinator.cpp - 全局 OTA 协调器实现
 *
 * OtaCoordinator 是系统级 OTA 状态的唯一权威来源。
 * 传输层（WiFi/BLE）通过本协调器发起所有 OTA 操作。
 */
#include "ota_coordinator.h"
#include "esp32_ota_core.h"
#include "relay/ota_relay.h"
#include "monitor/debug_log.h"

#define TAG "OTA_COORD"

// ============================================================
// 单例
// ============================================================
OtaCoordinator& OtaCoordinator::instance()
{
    static OtaCoordinator inst;
    return inst;
}

// ============================================================
// 线程安全辅助
// ============================================================
void OtaCoordinator::lock()
{
    // ★ 修复:FreeRTOS mutex (xSemaphoreCreateMutex) 只能由 take 的 task give。
    //   超时后必须在 unlock() 中跳过 give,避免 xTaskPriorityDisinherit assert。
    //   此处假设 OtaCoordinator 内部不嵌套 lock(全局已确认:无嵌套调用)。
    m_locked = false;
    if (m_mutex) {
        m_locked = (xSemaphoreTake(m_mutex, kLockTimeout) == pdTRUE);
        if (!m_locked) {
            DEBUG_WARN(TAG, "lock() 超时 (%u ms)，本次状态变更被跳过",
                       (unsigned)pdTICKS_TO_MS(kLockTimeout));
        }
    }
}

void OtaCoordinator::unlock()
{
    // ★ 修复:仅在 lock() 成功 take 时才 give。
    //   错误的 task 给同一 mutex give 会触发 xTaskPriorityDisinherit assert → PANIC。
    if (m_locked && m_mutex) {
        xSemaphoreGive(m_mutex);
        m_locked = false;
    }
}

// ============================================================
// 卡死自愈检查（必须在持锁状态下调用）
// ============================================================
void OtaCoordinator::watchdogCheckLocked()
{
    // 仅对"忙"状态（可能卡死的）做检查
    switch (m_sysState) {
        case OTA_SYS_GD32_STORING:
        case OTA_SYS_GD32_RELAYING:
        case OTA_SYS_ESP32_RECEIVING:
        case OTA_SYS_ESP32_FINALIZING:
        case OTA_SYS_ESP32_REBOOTING:
            break;
        default:
            return;  // IDLE/STORED/COMPLETE/ERROR 无需检查
    }

    if (m_busySinceTick == 0) {
        // 未记录起始时刻（理论上不应发生），保守不触发
        return;
    }

    TickType_t elapsed = xTaskGetTickCount() - m_busySinceTick;
    if (elapsed < kBusyWatchdogTicks) {
        return;  // 未超时
    }

    // ★ 超时自愈：强制转 ERROR。
    //   ERROR 不在任何 beginXxx() 的拒绝列表中，下次请求会自动从 ERROR 恢复。
    //   清理由各子系统自行负责（abortCurrent 中会按 ERROR 不做实际清理，仅复位状态）。
    DEBUG_ERR(TAG, "忙状态 %d 卡死超过 %" PRIu32 " ms，强制自愈 → ERROR",
              (int)m_sysState, (uint32_t)pdTICKS_TO_MS(kBusyWatchdogTicks));
    m_sysState = OTA_SYS_ERROR;
    m_busySinceTick = 0;
}

// ============================================================
// 初始化
// ============================================================
void OtaCoordinator::init()
{
    if (!m_mutex) {
        m_mutex = xSemaphoreCreateMutex();
        if (m_mutex) {
            DEBUG_INFO(TAG, "OTA 全局协调器已初始化");
        } else {
            DEBUG_ERR(TAG, "协调器 mutex 创建失败");
        }
    }
    m_sysState = OTA_SYS_IDLE;
    m_gd32StorageDone = false;
    m_busySinceTick = 0;

    // ★ 重启恢复：如果 NVS 里有完整固件元数据，开机时立即进入 STORED 状态，
    //   否则用户重启后看到的状态是 IDLE 但 NVS 里其实有未中继的固件，前后不一致。
    ota_meta_t boot_meta;
    if (OtaRelay::metaLoad(&boot_meta) == ESP_OK &&
        boot_meta.total_size > 0 &&
        boot_meta.stored_size >= boot_meta.total_size) {
        m_sysState = OTA_SYS_GD32_STORED;
        m_gd32StorageDone = true;
        DEBUG_INFO(TAG, "启动恢复: NVS 检测到完整 GD32 固件 (%" PRIu32 " 字节), → GD32_STORED",
                   boot_meta.total_size);
    }
}

// ============================================================
// GD32 存储阶段
// ============================================================
esp_err_t OtaCoordinator::beginGd32Storage(uint32_t total_size)
{
    lock();

    // ★ 先做卡死自愈检查：若之前忙状态卡死，自动转 ERROR 继续后续流程
    watchdogCheckLocked();

    // 系统忙检查：仅拒绝真正"在传输"的活跃状态
    //   STORED / COMPLETE 是"待中继/已中继"的非活跃状态 → 允许新存储覆盖
    //   ★ 修复：之前把所有非 IDLE 都当忙，导致"Flash 缓存里有固件但没在传输"时无法上传新固件
    if (m_sysState == OTA_SYS_ESP32_RECEIVING ||
        m_sysState == OTA_SYS_ESP32_FINALIZING ||
        m_sysState == OTA_SYS_ESP32_REBOOTING ||
        m_sysState == OTA_SYS_GD32_STORING ||
        m_sysState == OTA_SYS_GD32_RELAYING) {
        DEBUG_WARN(TAG, "系统忙 (state=%d)，拒绝 GD32 存储请求", (int)m_sysState);
        unlock();
        return ESP_ERR_INVALID_STATE;
    }

    // 如果之前已有存储但未中继（STORED）或刚中继完（COMPLETE），先清理再开始新存储
    if (m_sysState == OTA_SYS_GD32_STORED || m_sysState == OTA_SYS_GD32_COMPLETE) {
        DEBUG_INFO(TAG, "清理旧 GD32 存储状态，开始新存储 (size=%" PRIu32 ")", total_size);
        OtaRelay::metaClear();
    }

    m_sysState = OTA_SYS_GD32_STORING;
    m_gd32StorageDone = false;
    m_busySinceTick = xTaskGetTickCount();  // ★ 记录忙状态起始时刻

    DEBUG_INFO(TAG, "GD32 固件存储开始: %" PRIu32 " 字节, 系统状态 → GD32_STORING", total_size);
    unlock();
    return ESP_OK;
}

void OtaCoordinator::completeGd32Storage()
{
    lock();
    if (m_sysState == OTA_SYS_GD32_STORING) {
        m_sysState = OTA_SYS_GD32_STORED;
        m_gd32StorageDone = true;
        m_busySinceTick = 0;  // ★ STORED 是非忙状态
        DEBUG_INFO(TAG, "GD32 固件存储完成, 系统状态 → GD32_STORED");
    } else {
        DEBUG_WARN(TAG, "completeGd32Storage 在非预期状态: %d", (int)m_sysState);
    }
    unlock();
}

// ============================================================
// GD32 中继阶段
// ============================================================
esp_err_t OtaCoordinator::triggerGd32Relay()
{
    lock();

    // ★ 卡死自愈检查
    watchdogCheckLocked();

    // 校验：必须处于"已存储"或"已中继完成"状态
    //   ★ 修复：COMPLETE（旧固件已成功推给 GD32，NVS 元数据已被 doFinishAndVerify 清掉，
    //     OtaRelay 状态为 DONE）也能触发——但需要先回滚到 STORED 以保证 m_busySinceTick 一致。
    //     通常用户应先上传新固件再触发中继，COMPLETE 时元数据已被清，metaLoad 会返回 stored=0
    //     并在 188 行被拒绝，自然不会走到这里。
    if (m_sysState != OTA_SYS_GD32_STORED && m_sysState != OTA_SYS_IDLE) {
        if (m_sysState == OTA_SYS_GD32_STORING) {
            DEBUG_WARN(TAG, "GD32 正在存储中，拒绝触发中继");
            unlock();
            return ESP_ERR_INVALID_STATE;
        }
        if (m_sysState == OTA_SYS_GD32_RELAYING) {
            DEBUG_WARN(TAG, "GD32 中继已在进行中");
            unlock();
            return ESP_ERR_INVALID_STATE;
        }
        if (m_sysState == OTA_SYS_ESP32_RECEIVING ||
            m_sysState == OTA_SYS_ESP32_FINALIZING ||
            m_sysState == OTA_SYS_ESP32_REBOOTING) {
            DEBUG_WARN(TAG, "ESP32 OTA 进行中，拒绝中继触发");
            unlock();
            return ESP_ERR_INVALID_STATE;
        }
    }

    // 验证固件是否确实已存储（通过 NVS 元数据）
    ota_meta_t meta;
    if (OtaRelay::metaLoad(&meta) != ESP_OK ||
        meta.total_size == 0 ||
        meta.stored_size < meta.total_size) {
        DEBUG_WARN(TAG, "GD32 固件未完整存储，拒绝触发中继");
        unlock();
        return ESP_ERR_INVALID_STATE;
    }

    // 检查 OtaRelay 是否空闲
    if (OtaRelay::getState() != RELAY_IDLE &&
        OtaRelay::getState() != RELAY_DONE &&
        OtaRelay::getState() != RELAY_ERROR) {
        DEBUG_WARN(TAG, "OtaRelay 正忙 (state=%d)，拒绝触发", (int)OtaRelay::getState());
        unlock();
        return ESP_ERR_INVALID_STATE;
    }

    // 向 OtaRelay task 发送启动消息
    if (!OtaRelay::dispatchQueue) {
        DEBUG_ERR(TAG, "OtaRelay dispatchQueue 未就绪");
        unlock();
        return ESP_ERR_INVALID_STATE;
    }

    m_sysState = OTA_SYS_GD32_RELAYING;
    m_busySinceTick = xTaskGetTickCount();  // ★ 记录忙状态起始时刻
    DEBUG_INFO(TAG, "触发 GD32 中继, 系统状态 → GD32_RELAYING");

    uint32_t cmd = 1;
    BaseType_t ret = xQueueSend(OtaRelay::dispatchQueue, &cmd, pdMS_TO_TICKS(200));
    if (ret != pdTRUE) {
        DEBUG_ERR(TAG, "向 OtaRelay 发送启动消息失败");
        m_sysState = OTA_SYS_GD32_STORED;
        m_busySinceTick = 0;  // 回退到 STORED（非忙）
        unlock();
        return ESP_FAIL;
    }

    unlock();
    return ESP_OK;
}

void OtaCoordinator::notifyRelayStarted()
{
    lock();
    if (m_sysState != OTA_SYS_GD32_RELAYING) {
        m_sysState = OTA_SYS_GD32_RELAYING;
        m_busySinceTick = xTaskGetTickCount();  // ★ 记录起始时刻
        DEBUG_INFO(TAG, "中继已开始, 系统状态 → GD32_RELAYING");
    }
    unlock();
}

void OtaCoordinator::notifyRelayComplete(bool success)
{
    lock();
    if (success) {
        m_sysState = OTA_SYS_GD32_COMPLETE;
        m_gd32StorageDone = false;
        m_busySinceTick = 0;  // ★ COMPLETE 非忙
        DEBUG_INFO(TAG, "GD32 中继完成, 系统状态 → GD32_COMPLETE");
    } else {
        m_sysState = OTA_SYS_ERROR;
        m_busySinceTick = 0;  // ★ ERROR 非忙（可被后续请求恢复）
        DEBUG_ERR(TAG, "GD32 中继失败, 系统状态 → ERROR");
    }
    unlock();
}

// ============================================================
// ESP32 OTA 阶段
// ============================================================
esp_err_t OtaCoordinator::beginEsp32Ota(uint32_t total_size)
{
    lock();

    // ★ 卡死自愈检查
    watchdogCheckLocked();

    // 系统忙检查
    //   ★ 修复：STORING/RELAYING 都拒绝（防止中继中改 gd32_ota 分区被 ESP32 OTA 误触发）。
    //     STORED/COMPLETE 允许——它们是非活跃状态，ESP32 OTA 与 gd32_ota 分区无冲突。
    if (m_sysState == OTA_SYS_GD32_STORING ||
        m_sysState == OTA_SYS_GD32_RELAYING) {
        DEBUG_WARN(TAG, "GD32 正在 %s，拒绝 ESP32 OTA",
                   m_sysState == OTA_SYS_GD32_STORING ? "存储" : "中继");
        unlock();
        return ESP_ERR_INVALID_STATE;
    }

    // 委托 Esp32OtaCore 处理分区/互斥逻辑
    esp_err_t err = Esp32OtaCore::instance().begin(total_size);
    if (err == ESP_OK) {
        m_sysState = OTA_SYS_ESP32_RECEIVING;
        m_busySinceTick = xTaskGetTickCount();  // ★ 记录起始时刻
        DEBUG_INFO(TAG, "ESP32 OTA 开始: %" PRIu32 " 字节, 系统状态 → ESP32_RECEIVING", total_size);
    } else {
        DEBUG_WARN(TAG, "ESP32 OTA begin 失败: %s", esp_err_to_name(err));
    }

    unlock();
    return err;
}

void OtaCoordinator::notifyEsp32ReceiveComplete()
{
    lock();
    if (m_sysState == OTA_SYS_ESP32_RECEIVING) {
        m_sysState = OTA_SYS_ESP32_FINALIZING;
        DEBUG_INFO(TAG, "ESP32 固件接收完毕, 系统状态 → ESP32_FINALIZING");
    }
    unlock();
}

void OtaCoordinator::notifyEsp32FinalizeComplete()
{
    lock();
    m_sysState = OTA_SYS_ESP32_REBOOTING;
    DEBUG_INFO(TAG, "ESP32 OTA 收尾完成, 系统状态 → ESP32_REBOOTING");
    unlock();
}

void OtaCoordinator::notifyEsp32Rebooting()
{
    lock();
    m_sysState = OTA_SYS_ESP32_REBOOTING;
    DEBUG_INFO(TAG, "ESP32 即将重启, 系统状态 → ESP32_REBOOTING");
    unlock();
}

// ============================================================
// 中止
// ============================================================
void OtaCoordinator::abortCurrent()
{
    lock();
    DEBUG_INFO(TAG, "中止当前 OTA 操作 (state=%d)", (int)m_sysState);

    if (m_sysState == OTA_SYS_GD32_STORING) {
        OtaRelay::metaClear();
    } else if (m_sysState == OTA_SYS_GD32_RELAYING) {
        OtaRelay::instance().reset();
    } else if (m_sysState == OTA_SYS_ESP32_RECEIVING ||
               m_sysState == OTA_SYS_ESP32_FINALIZING) {
        Esp32OtaCore::instance().abort();
    }

    m_sysState = OTA_SYS_IDLE;
    m_gd32StorageDone = false;
    m_busySinceTick = 0;  // ★ 离开忙状态
    DEBUG_INFO(TAG, "已中止, 系统状态 → IDLE");
    unlock();
}

// ============================================================
// 状态查询
// ============================================================
ota_system_state_t OtaCoordinator::getSystemState() const
{
    // 对于纯读取，短暂加锁保证一致性
    OtaCoordinator* self = const_cast<OtaCoordinator*>(this);
    self->lock();
    ota_system_state_t s = m_sysState;
    self->unlock();
    return s;
}

bool OtaCoordinator::isIdle() const
{
    return (getSystemState() == OTA_SYS_IDLE ||
            getSystemState() == OTA_SYS_GD32_STORED ||
            getSystemState() == OTA_SYS_GD32_COMPLETE);
}

bool OtaCoordinator::isGd32Busy() const
{
    ota_system_state_t s = getSystemState();
    return (s == OTA_SYS_GD32_STORING ||
            s == OTA_SYS_GD32_RELAYING);
}

bool OtaCoordinator::isEsp32Busy() const
{
    ota_system_state_t s = getSystemState();
    return (s == OTA_SYS_ESP32_RECEIVING ||
            s == OTA_SYS_ESP32_FINALIZING ||
            s == OTA_SYS_ESP32_REBOOTING);
}

const char* OtaCoordinator::getSystemStateString() const
{
    switch (getSystemState()) {
        case OTA_SYS_IDLE:              return "IDLE";
        case OTA_SYS_GD32_STORING:      return "GD32_STORING";
        case OTA_SYS_GD32_STORED:       return "GD32_STORED";
        case OTA_SYS_GD32_RELAYING:     return "GD32_RELAYING";
        case OTA_SYS_GD32_COMPLETE:     return "GD32_COMPLETE";
        case OTA_SYS_ESP32_RECEIVING:   return "ESP32_RECEIVING";
        case OTA_SYS_ESP32_FINALIZING:  return "ESP32_FINALIZING";
        case OTA_SYS_ESP32_REBOOTING:   return "ESP32_REBOOTING";
        case OTA_SYS_ERROR:             return "ERROR";
        default:                        return "UNKNOWN";
    }
}

bool OtaCoordinator::getGd32AppAlive() const
{
    return OtaRelay::getGd32AppAlive();
}

bool OtaCoordinator::isGd32StorageDone() const
{
    OtaCoordinator* self = const_cast<OtaCoordinator*>(this);
    self->lock();
    bool done = m_gd32StorageDone;
    self->unlock();
    return done;
}

// ============================================================
// 完全复位
// ============================================================
void OtaCoordinator::reset()
{
    lock();
    m_sysState = OTA_SYS_IDLE;
    m_gd32StorageDone = false;
    m_busySinceTick = 0;  // ★ 复位
    DEBUG_INFO(TAG, "协调器已完全复位");
    unlock();
}
