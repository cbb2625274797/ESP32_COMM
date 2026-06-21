/*
 * ota_coordinator.cpp - 全局 OTA 协调器实现
 *
 * OtaCoordinator 是系统级 OTA 状态的唯一权威来源。
 * 传输层（WiFi/BLE）通过本协调器发起所有 OTA 操作。
 */
#include "ota_coordinator.h"
#include "esp32_ota_core.h"
#include "relay/ota_relay.h"
#include "storage/gd32_flash_store.h"
#include "monitor/debug_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
bool OtaCoordinator::lock() const
{
    // ★ 修复: 返回 take 结果，由调用方传回 unlock()。
    //   旧实现用成员 bool m_locked 记录 take 结果，多任务并发 lock() 时
    //   超时方会把 m_locked 置 false，覆盖持锁方的 true，导致 unlock() 跳过 give，
    //   mutex 永久死锁 → 后续 take 超时触发 vTaskPriorityDisinherit assert → PANIC。
    if (!m_mutex) return false;
    bool ok = (xSemaphoreTake(m_mutex, kLockTimeout) == pdTRUE);
    if (!ok) {
        DEBUG_WARN(TAG, "lock() 超时 (%u ms)，本次状态变更被跳过",
                   (unsigned)pdTICKS_TO_MS(kLockTimeout));
    }
    return ok;
}

void OtaCoordinator::unlock(bool handle) const
{
    // ★ 修复: 仅在对应的 lock() 成功 take 时才 give。
    if (handle && m_mutex) {
        xSemaphoreGive(m_mutex);
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

    // ★ H4 解耦：注册协调器为 OtaRelay 的中继事件监听器
    OtaRelay::setRelayListener(this);

    // ★ 重启恢复：如果 NVS 里有完整固件元数据，开机时立即进入 STORED 状态，
    //   否则用户重启后看到的状态是 IDLE 但 NVS 里其实有未中继的固件，前后不一致。
    ota_meta_t boot_meta;
    if (Gd32FlashStore::instance().metaLoad(&boot_meta) == ESP_OK &&
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
    bool h = lock();

    // ★ 先做卡死自愈检查：若之前忙状态卡死，自动转 ERROR 继续后续流程
    if (h) watchdogCheckLocked();

    // 系统忙检查：仅拒绝真正"在传输"的活跃状态
    //   STORED / COMPLETE 是"待中继/已中继"的非活跃状态 → 允许新存储覆盖
    if (h && (m_sysState == OTA_SYS_ESP32_RECEIVING ||
        m_sysState == OTA_SYS_ESP32_FINALIZING ||
        m_sysState == OTA_SYS_ESP32_REBOOTING ||
        m_sysState == OTA_SYS_GD32_STORING ||
        m_sysState == OTA_SYS_GD32_RELAYING)) {
        DEBUG_WARN(TAG, "系统忙 (state=%d)，拒绝 GD32 存储请求", (int)m_sysState);
        unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    // 如果之前已有存储但未中继（STORED）或刚中继完（COMPLETE），先清理再开始新存储
    if (h && (m_sysState == OTA_SYS_GD32_STORED || m_sysState == OTA_SYS_GD32_COMPLETE)) {
        DEBUG_INFO(TAG, "清理旧 GD32 存储状态，开始新存储 (size=%" PRIu32 ")", total_size);
        Gd32FlashStore::instance().metaClear();
    }

    if (h) {
        m_sysState = OTA_SYS_GD32_STORING;
        m_gd32StorageDone = false;
        m_busySinceTick = xTaskGetTickCount();  // ★ 记录忙状态起始时刻
        DEBUG_INFO(TAG, "GD32 固件存储开始: %" PRIu32 " 字节, 系统状态 → GD32_STORING", total_size);
    }
    unlock(h);
    return h ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void OtaCoordinator::completeGd32Storage()
{
    bool h = lock();
    if (h && m_sysState == OTA_SYS_GD32_STORING) {
        m_sysState = OTA_SYS_GD32_STORED;
        m_gd32StorageDone = true;
        m_busySinceTick = 0;  // ★ STORED 是非忙状态
        DEBUG_INFO(TAG, "GD32 固件存储完成, 系统状态 → GD32_STORED");
    } else if (h) {
        DEBUG_WARN(TAG, "completeGd32Storage 在非预期状态: %d", (int)m_sysState);
    }
    unlock(h);
}

// ============================================================
// GD32 存储操作（传输层统一入口，解决 H3 绕过协调器问题）
// ============================================================
esp_err_t OtaCoordinator::writeGd32Chunk(uint32_t offset, const uint8_t *data, size_t len)
{
    return Gd32FlashStore::instance().flashWriteChunk(offset, data, len);
}

esp_err_t OtaCoordinator::saveGd32Meta(const ota_meta_t *meta)
{
    return Gd32FlashStore::instance().metaSave(meta);
}

esp_err_t OtaCoordinator::loadGd32Meta(ota_meta_t *meta)
{
    return Gd32FlashStore::instance().metaLoad(meta);
}

esp_err_t OtaCoordinator::updateGd32Meta(uint32_t stored_size, uint32_t last_seq)
{
    return Gd32FlashStore::instance().metaUpdate(stored_size, last_seq);
}

esp_err_t OtaCoordinator::clearGd32Meta()
{
    return Gd32FlashStore::instance().metaClear();
}

esp_err_t OtaCoordinator::verifyGd32Firmware(const uint8_t frontend_sha32[32],
                                              uint8_t out_device_sha[32])
{
    return Gd32FlashStore::instance().verifyFrontendSha(frontend_sha32, out_device_sha);
}

esp_err_t OtaCoordinator::finalizeGd32Checksum()
{
    return Gd32FlashStore::instance().metaFinalizeChecksum();
}

esp_err_t OtaCoordinator::eraseGd32Partition(uint32_t size)
{
    return Gd32FlashStore::instance().erasePartition(size);
}

// ============================================================
// GD32 中继阶段
// ============================================================
esp_err_t OtaCoordinator::triggerGd32Relay()
{
    bool h = lock();

    // ★ 卡死自愈检查
    if (h) watchdogCheckLocked();

    // 校验：必须处于"已存储"或"已中继完成"状态
    if (h && m_sysState != OTA_SYS_GD32_STORED && m_sysState != OTA_SYS_IDLE) {
        if (m_sysState == OTA_SYS_GD32_STORING) {
            DEBUG_WARN(TAG, "GD32 正在存储中，拒绝触发中继");
            unlock(h);
            return ESP_ERR_INVALID_STATE;
        }
        if (m_sysState == OTA_SYS_GD32_RELAYING) {
            DEBUG_WARN(TAG, "GD32 中继已在进行中");
            unlock(h);
            return ESP_ERR_INVALID_STATE;
        }
        if (m_sysState == OTA_SYS_ESP32_RECEIVING ||
            m_sysState == OTA_SYS_ESP32_FINALIZING ||
            m_sysState == OTA_SYS_ESP32_REBOOTING) {
            DEBUG_WARN(TAG, "ESP32 OTA 进行中，拒绝中继触发");
            unlock(h);
            return ESP_ERR_INVALID_STATE;
        }
    }

    // 验证固件是否确实已存储（通过 NVS 元数据）
    ota_meta_t meta;
    if (h && (Gd32FlashStore::instance().metaLoad(&meta) != ESP_OK ||
        meta.total_size == 0 ||
        meta.stored_size < meta.total_size)) {
        DEBUG_WARN(TAG, "GD32 固件未完整存储，拒绝触发中继");
        unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    // 检查 OtaRelay 是否空闲
    if (h && OtaRelay::getState() != RELAY_IDLE &&
        OtaRelay::getState() != RELAY_DONE &&
        OtaRelay::getState() != RELAY_ERROR) {
        DEBUG_WARN(TAG, "OtaRelay 正忙 (state=%d)，拒绝触发", (int)OtaRelay::getState());
        unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    // 向 OtaRelay task 发送启动消息
    QueueHandle_t dq = OtaRelay::getDispatchQueue();
    if (h && !dq) {
        DEBUG_ERR(TAG, "OtaRelay dispatchQueue 未就绪");
        unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    if (!h) {
        unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    m_sysState = OTA_SYS_GD32_RELAYING;
    m_busySinceTick = xTaskGetTickCount();  // ★ 记录忙状态起始时刻
    DEBUG_INFO(TAG, "触发 GD32 中继, 系统状态 → GD32_RELAYING");

    uint32_t cmd = 1;
    BaseType_t ret = xQueueSend(dq, &cmd, pdMS_TO_TICKS(200));
    if (ret != pdTRUE) {
        DEBUG_ERR(TAG, "向 OtaRelay 发送启动消息失败");
        m_sysState = OTA_SYS_GD32_STORED;
        m_busySinceTick = 0;  // 回退到 STORED（非忙）
        unlock(h);
        return ESP_FAIL;
    }

    unlock(h);
    return ESP_OK;
}

/** @brief 通知协调器：GD32 中继已开始，更新系统状态为 GD32_RELAYING。 */
void OtaCoordinator::notifyRelayStarted()
{
    bool h = lock();
    if (h && m_sysState != OTA_SYS_GD32_RELAYING) {
        m_sysState = OTA_SYS_GD32_RELAYING;
        m_busySinceTick = xTaskGetTickCount();  // ★ 记录起始时刻
        DEBUG_INFO(TAG, "中继已开始, 系统状态 → GD32_RELAYING");
    }
    unlock(h);
}

/** @brief 通知协调器：GD32 中继已完成，根据结果转为 COMPLETE 或 ERROR。 */
void OtaCoordinator::notifyRelayComplete(bool success)
{
    bool h = lock();
    if (h) {
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
    }
    unlock(h);
}

// ============================================================
// IRelayListener 接口实现（H4: OtaRelay 通过接口回调通知协调器）
// ============================================================
/** @brief IRelayListener 回调：中继已开始，委托给 notifyRelayStarted()。 */
void OtaCoordinator::onRelayStarted()
{
    notifyRelayStarted();
}

/** @brief IRelayListener 回调：中继已完成，委托给 notifyRelayComplete()。 */
void OtaCoordinator::onRelayComplete(bool success)
{
    notifyRelayComplete(success);
}

// ============================================================
// BLE 射频共存管理（L1: 消除 ble_ota → wifi_ota 直接依赖）
// ============================================================
void OtaCoordinator::setBleCoexListener(IBleCoexListener *listener)
{
    m_bleCoexListener = listener;
}

void OtaCoordinator::notifyBleCoexStart()
{
    if (m_bleCoexListener) {
        m_bleCoexListener->onBleOtaStart();
    }
}

void OtaCoordinator::notifyBleCoexEnd()
{
    if (m_bleCoexListener) {
        m_bleCoexListener->onBleOtaEnd();
    }
}

// ============================================================
// ESP32 OTA 阶段
// ============================================================
esp_err_t OtaCoordinator::beginEsp32Ota(uint32_t total_size)
{
    bool h = lock();

    // ★ 卡死自愈检查
    if (h) watchdogCheckLocked();

    // 系统忙检查：GD32 传输/中继进行中，或另一通道的 ESP32 OTA 进行中，均拒绝。
    //   ★ 必须显式拒绝 ESP32_RECEIVING/FINALIZING/REBOOTING：
    //     若放行，本调用会落到 Esp32OtaCore::begin 去抢全局互斥失败而返回 INVALID_STATE，
    //     随后 beginEsp32OtaWithRetry 的 forceCleanup 会把另一通道的活跃会话
    //     （esp_ota_abort + 释放互斥）误中止 → 两通道并发写同一 OTA 分区。
    if (h && (m_sysState == OTA_SYS_GD32_STORING ||
              m_sysState == OTA_SYS_GD32_RELAYING ||
              m_sysState == OTA_SYS_ESP32_RECEIVING ||
              m_sysState == OTA_SYS_ESP32_FINALIZING ||
              m_sysState == OTA_SYS_ESP32_REBOOTING)) {
        DEBUG_WARN(TAG, "系统忙 (state=%d, %s)，拒绝 ESP32 OTA",
                   (int)m_sysState, getSystemStateString());
        unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (h) {
        // 委托 Esp32OtaCore 处理分区/互斥逻辑
        err = Esp32OtaCore::instance().begin(total_size);
        if (err == ESP_OK) {
            m_sysState = OTA_SYS_ESP32_RECEIVING;
            m_busySinceTick = xTaskGetTickCount();  // ★ 记录起始时刻
            DEBUG_INFO(TAG, "ESP32 OTA 开始: %" PRIu32 " 字节, 系统状态 → ESP32_RECEIVING", total_size);
        } else {
            DEBUG_WARN(TAG, "ESP32 OTA begin 失败: %s", esp_err_to_name(err));
        }
    }

    unlock(h);
    return err;
}

esp_err_t OtaCoordinator::beginEsp32OtaWithRetry(uint32_t total_size)
{
    esp_err_t err = beginEsp32Ota(total_size);
    if (err == ESP_ERR_INVALID_STATE) {
        // ★ 仅当系统确实空闲（无任何活跃 OTA）时才允许 forceCleanup：
        //   这种情况只可能是"锁残留"——前一次 OTA 异常退出没释放全局互斥，
        //   而协调器状态已回 IDLE。
        //   若系统正处于任何活跃 OTA（ESP32/GD32），绝不强制清理——那会
        //   把另一通道正在进行的 esp_ota_handle 中止、释放互斥，导致
        //   两通道并发写同一 OTA 分区（之前 BLE 与 WiFi 可同时 OTA 的根因）。
        if (!isIdle()) {
            DEBUG_WARN(TAG, "ESP32 OTA 被拒绝且系统非空闲 (state=%d)，不强制清理，直接失败",
                       (int)getSystemState());
            return err;
        }
        // 协调器空闲但 Esp32OtaCore.begin 仍被拒 → 锁残留，强制清理后重试
        DEBUG_WARN(TAG, "ESP32 OTA 被拒绝但系统空闲，判定为锁残留，强制清理状态机后重试");
        Esp32OtaCore::instance().forceCleanup();
        err = beginEsp32Ota(total_size);
    }
    return err;
}

/** @brief 通知协调器：ESP32 固件接收完毕，状态转为 FINALIZING。 */
void OtaCoordinator::notifyEsp32ReceiveComplete()
{
    bool h = lock();
    if (h && m_sysState == OTA_SYS_ESP32_RECEIVING) {
        m_sysState = OTA_SYS_ESP32_FINALIZING;
        DEBUG_INFO(TAG, "ESP32 固件接收完毕, 系统状态 → ESP32_FINALIZING");
    }
    unlock(h);
}

/** @brief 通知协调器：ESP32 OTA 收尾完成（SHA验证+标记启动），状态转为 REBOOTING。 */
void OtaCoordinator::notifyEsp32FinalizeComplete()
{
    bool h = lock();
    if (h) {
        m_sysState = OTA_SYS_ESP32_REBOOTING;
        DEBUG_INFO(TAG, "ESP32 OTA 收尾完成, 系统状态 → ESP32_REBOOTING");
    }
    unlock(h);
}

/** @brief 通知协调器：ESP32 即将重启。 */
void OtaCoordinator::notifyEsp32Rebooting()
{
    bool h = lock();
    if (h) {
        m_sysState = OTA_SYS_ESP32_REBOOTING;
        DEBUG_INFO(TAG, "ESP32 即将重启, 系统状态 → ESP32_REBOOTING");
    }
    unlock(h);
}

// ============================================================
// ESP32 OTA 收尾+重启序列（BLE/WiFi 共用封装）
// 关键：finalize() 内部已 end+set_boot+释放锁，reboot=false 返回后
//   m_active 仍为 true、m_ended 为 true，调用方可在重启前自由查询状态。
// ============================================================
Esp32FinalizeResult OtaCoordinator::finalizeForReboot(const char *frontend_sha_hex)
{
    Esp32FinalizeResult r = {};
    notifyEsp32ReceiveComplete();
    // reboot=false：让 core.finalize 真正返回，由调用方上报 device_sha 后再调 restartEsp32
    r.err = Esp32OtaCore::instance().finalize(frontend_sha_hex,
                                              /*reboot=*/false,
                                              r.device_sha_hex);
    if (r.err == ESP_OK) {
        notifyEsp32FinalizeComplete();
    }
    return r;
}

void OtaCoordinator::restartEsp32()
{
    notifyEsp32Rebooting();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    // 不会返回
}

/**
 * @brief 将 ESP32 OTA finalize 错误码分类。
 * @param err finalize 返回的 esp_err_t。
 * @return 0=成功，1=SHA不匹配，2=其他失败。
 */
int OtaCoordinator::esp32FinalizeErrCategory(esp_err_t err)
{
    if (err == ESP_OK)                  return 0;  // 成功
    if (err == ESP_ERR_INVALID_VERSION) return 1;  // SHA 不匹配
    return 2;                                       // 其他失败
}

// ============================================================
// 中止
// ============================================================
void OtaCoordinator::abortCurrent()
{
    bool h = lock();
    if (h) {
        DEBUG_INFO(TAG, "中止当前 OTA 操作 (state=%d)", (int)m_sysState);

        if (m_sysState == OTA_SYS_GD32_STORING) {
            Gd32FlashStore::instance().metaClear();
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
    }
    unlock(h);
}

// ============================================================
// 状态查询
// ============================================================
ota_system_state_t OtaCoordinator::getSystemState() const
{
    // 对于纯读取，短暂加锁保证一致性
    // ★ M1/L3: m_mutex 为 mutable + lock()/unlock() 为 const，无需 const_cast
    bool h = lock();
    ota_system_state_t s = m_sysState;
    unlock(h);
    return s;
}

/** @brief 判断系统是否处于空闲状态（IDLE/STORED/COMPLETE）。 */
bool OtaCoordinator::isIdle() const
{
    return (getSystemState() == OTA_SYS_IDLE ||
            getSystemState() == OTA_SYS_GD32_STORED ||
            getSystemState() == OTA_SYS_GD32_COMPLETE);
}

/** @brief 判断 GD32 通道是否正忙（STORING 或 RELAYING）。 */
bool OtaCoordinator::isGd32Busy() const
{
    ota_system_state_t s = getSystemState();
    return (s == OTA_SYS_GD32_STORING ||
            s == OTA_SYS_GD32_RELAYING);
}

/** @brief 判断 ESP32 通道是否正忙（RECEIVING/FINALIZING/REBOOTING）。 */
bool OtaCoordinator::isEsp32Busy() const
{
    ota_system_state_t s = getSystemState();
    return (s == OTA_SYS_ESP32_RECEIVING ||
            s == OTA_SYS_ESP32_FINALIZING ||
            s == OTA_SYS_ESP32_REBOOTING);
}

/** @brief 获取系统状态的字符串名称（用于日志和前端显示）。 */
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

/** @brief 获取 GD32 APP 存活状态（委托 OtaRelay）。 */
bool OtaCoordinator::getGd32AppAlive() const
{
    return OtaRelay::getGd32AppAlive();
}

/** @brief 获取 GD32 固件是否已存储完成（线程安全）。 */
bool OtaCoordinator::isGd32StorageDone() const
{
    // ★ M1/L3: m_mutex 为 mutable + lock()/unlock() 为 const，无需 const_cast
    bool h = lock();
    bool done = m_gd32StorageDone;
    unlock(h);
    return done;
}

// ============================================================
// 完全复位
// ============================================================
void OtaCoordinator::reset()
{
    bool h = lock();
    if (h) {
        m_sysState = OTA_SYS_IDLE;
        m_gd32StorageDone = false;
        m_busySinceTick = 0;  // ★ 复位
        DEBUG_INFO(TAG, "协调器已完全复位");
    }
    unlock(h);
}
