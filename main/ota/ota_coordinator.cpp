/*
 * ota_coordinator.cpp - 全局 OTA 协调器实现
 *
 * OtaCoordinator 是系统级 OTA 状态的唯一权威来源。
 * 传输层（WiFi/BLE）通过本协调器发起所有 OTA 操作。
 */
#include "ota_coordinator.h"
#include "monitor/can_feedback_task.h"
#include "monitor/motor_ota_relay.h"
#include "esp32_ota_core.h"
#include "relay/ota_relay.h"
#include "storage/gd32_flash_store.h"
#include "server/ble_ota.h"
#include "monitor/debug_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>

#define TAG "OTA_COORD"

/** WiFi OTA 时挂起 BLE；BLE 通道自身 OTA 不挂。调用勿持 m_mutex。 */
static void wifiOtaSuspendBleIfNeeded(ota_channel_t channel)
{
    if (channel != OTA_CHANNEL_WIFI) {
        return;
    }
    (void)BleOta::instance().suspendStack();
}

/** 回到可空闲态时恢复 BLE。调用勿持 m_mutex。 */
static void wifiOtaResumeBleIfNeeded(void)
{
    if (BleOta::instance().isSuspendedForWifiOta()) {
        (void)BleOta::instance().resumeStack();
    }
}

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
    /* 返回 take 是否成功；调用方须把该值传给 unlock() */
    if (!m_mutex) return false;
    bool ok = (xSemaphoreTake(m_mutex, kLockTimeout) == pdTRUE);
    if (!ok) {
        DEBUG_ERR(TAG, "lock() 超时 (%u ms)，本次状态变更被跳过",
                  (unsigned)pdTICKS_TO_MS(kLockTimeout));
    }
    return ok;
}

void OtaCoordinator::unlock(bool handle) const
{
    /* 仅当对应 lock() 成功时 give */
    if (handle && m_mutex) {
        xSemaphoreGive(m_mutex);
    }
}

// ============================================================
// 卡死自愈检查（必须在持锁状态下调用）
// ============================================================
void OtaCoordinator::watchdogCheckLocked()
{
    // 仅对"忙"状态（可能卡死的）做检查。
    // REBOOTING：已 set_boot，即将 esp_restart，不入狗，避免 abort 成 IDLE 与重启竞态。
    switch (m_sysState) {
        case OTA_SYS_GD32_STORING:
        case OTA_SYS_GD32_RELAYING:
        case OTA_SYS_ESP32_RECEIVING:
        case OTA_SYS_ESP32_FINALIZING:
            break;
        default:
            return;  // IDLE/STORED/COMPLETE/ERROR/REBOOTING 无需检查
    }

    if (m_busySinceTick == 0) {
        // 未记录起始时刻（理论上不应发生），保守不触发
        return;
    }

    TickType_t elapsed = xTaskGetTickCount() - m_busySinceTick;
    if (elapsed < kBusyWatchdogTicks) {
        return;  // 未超时
    }

    DEBUG_ERR(TAG, "忙状态 %d 超过 %" PRIu32 " ms，强制中止 → IDLE",
              (int)m_sysState, (uint32_t)pdTICKS_TO_MS(kBusyWatchdogTicks));
    doAbortLocked();
}

esp_err_t OtaCoordinator::beginLockedWithChecks(const char *tag, bool &out_lock)
{
    out_lock = lock();
    if (!out_lock) return ESP_ERR_INVALID_STATE;

    // ★ 卡死自愈检查
    watchdogCheckLocked();

    // ★ 电机 OTA 互斥：电机喂数期间禁止新的 GD32/ESP32 OTA
    if (motorOtaIsActive()) {
        DEBUG_WARN(TAG, "电机 OTA 进行中，拒绝 %s", tag);
        unlock(true);
        out_lock = false;
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
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

    // 注册协调器为 OtaRelay 的中继事件监听器
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
esp_err_t OtaCoordinator::beginGd32Storage(uint32_t total_size, ota_channel_t channel)
{
    bool h;
    esp_err_t pre = beginLockedWithChecks("GD32 存储", h);
    if (pre != ESP_OK) return pre;

    // ★ 同类型重启：若上一次 GD32 存储仍在进行中（STORING）且来自同一通道，
    //   自动中止旧存储并重新开始。覆盖"上传过程中刷新网页再上传"场景。
    //   跨通道（BLE→WiFi 或 WiFi→BLE）仍拒绝，保持互斥。
    //   RELAYING 是中继阶段，不可中断，也拒绝。
    if (m_sysState == OTA_SYS_GD32_STORING) {
        if (m_otaChannel != channel) {
            DEBUG_WARN(TAG, "GD32存中 ch=%d 拒跨通道 ch=%d",
                       (int)m_otaChannel, (int)channel);
            unlock(h);
            return ESP_ERR_INVALID_STATE;
        }
        DEBUG_WARN(TAG, "GD32同通道重传:中止旧存储");
        doAbortLocked();  // 清 meta + IDLE，与 abort 路径一致
        // epoch 将在下方设置 GD32_STORING 后递增
    }

    // 系统忙检查：仅拒绝真正"在传输"的活跃状态
    //   STORED / COMPLETE 是"待中继/已中继"的非活跃状态 → 允许新存储覆盖
    if (m_sysState == OTA_SYS_ESP32_RECEIVING ||
        m_sysState == OTA_SYS_ESP32_FINALIZING ||
        m_sysState == OTA_SYS_ESP32_REBOOTING ||
        m_sysState == OTA_SYS_GD32_RELAYING) {
        DEBUG_WARN(TAG, "系统忙 (state=%d)，拒绝 GD32 存储请求", (int)m_sysState);
        unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    // 如果之前已有存储但未中继（STORED）或刚中继完（COMPLETE），允许新存储覆盖。
    if (m_sysState == OTA_SYS_GD32_STORED || m_sysState == OTA_SYS_GD32_COMPLETE) {
        DEBUG_INFO(TAG, "覆盖旧 GD32 存储状态，开始新存储 (size=%" PRIu32 ")", total_size);
    }

    m_otaEpoch++;  // ★ 新会话开始，递增 epoch
    m_otaChannel = channel;  // ★ 记录发起通道
    m_sysState = OTA_SYS_GD32_STORING;
    m_gd32StorageDone = false;
    m_busySinceTick = xTaskGetTickCount();  // ★ 记录忙状态起始时刻
    DEBUG_DBG(TAG, "GD32存开始 %" PRIu32 "B →STORING ep=%u ch=%d",
               total_size, m_otaEpoch, (int)channel);
    // ★ 预分配 flashWriteChunk 对齐缓冲（OTA 期消除逐块 malloc；非 OTA 期零占用）
    Gd32FlashStore::acquireAlignBufForOta();
    unlock(h);
    return ESP_OK;
}

esp_err_t OtaCoordinator::prepareGd32Storage(uint32_t total_size, ota_channel_t channel)
{
    // 1. 校验固件大小（统一 BLE/HTTP 两路径的大小检查）
    esp_err_t err = validateGd32Size(total_size);
    if (err != ESP_OK) {
        DEBUG_WARN(TAG, "prepareGd32Storage: 固件大小非法 raw=%" PRIu32, total_size);
        return err;
    }

    /* WiFi GD32 上传前先挂 BLE，腾堆给 HTTP 收流 */
    wifiOtaSuspendBleIfNeeded(channel);

    // 2. 进入 GD32_STORING 状态（系统忙则拒绝）
    err = beginGd32Storage(total_size, channel);
    if (err != ESP_OK) {
        DEBUG_WARN(TAG, "prepareGd32Storage: 协调器拒绝存储（系统忙）");
        if (channel == OTA_CHANNEL_WIFI) {
            wifiOtaResumeBleIfNeeded();
        }
        return err;
    }

    // 3. 复位擦除水位；不在 prepare 整区预擦。
    //    旧方案在 HTTP JSON 头后同步擦 gd32_ota，TCP 堵在开头。
    //    现由 flashWriteChunk → ensureErasedForWrite 按需擦扇区，与收流交织。
    Gd32FlashStore::instance().beginWriteSession();
    if (Gd32FlashStore::instance().getPartition() == nullptr) {
        DEBUG_ERR(TAG, "prepareGd32Storage: 找不到 gd32_ota 分区");
        abortCurrent();
        return ESP_ERR_NOT_FOUND;
    }
    DEBUG_INFO(TAG, "prepareGd32Storage: 会话就绪 fw=%" PRIu32 "（按需擦除，无整区预擦）",
               total_size);

    // 4. 清空旧 meta + 写入初始 meta（total_size + stored_size=0 + last_seq=0）
    clearGd32Meta();
    ota_meta_t meta = {};
    meta.total_size  = total_size;
    meta.stored_size = 0;
    meta.last_seq    = 0;
    err = saveGd32Meta(&meta);
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "prepareGd32Storage: 元数据保存失败: %s", esp_err_to_name(err));
        abortCurrent();
        return err;
    }

    return ESP_OK;
}

void OtaCoordinator::completeGd32Storage()
{
    bool h = lock();
    if (h && m_sysState == OTA_SYS_GD32_STORING) {
        m_sysState = OTA_SYS_GD32_STORED;
        m_gd32StorageDone = true;
        m_busySinceTick = 0;  // ★ STORED 是非忙状态
        DEBUG_DBG(TAG, "GD32 固件存储完成, 系统状态 → GD32_STORED");
        /* WiFi 路径不在此处恢复 BLE：
         * 旧逻辑 HTTP 一收完就 resume，55KB 极快时日志紧贴 raw upload start，
         * 像「一刷新就共存恢复」。挂起到中继结束/中止（notifyRelayComplete、abort）。 */
    } else if (h) {
        DEBUG_WARN(TAG, "completeGd32Storage 在非预期状态: %d", (int)m_sysState);
    }
    unlock(h);
}

esp_err_t OtaCoordinator::finishGd32StorageAfterReceive()
{
    completeGd32Storage();
    return finalizeGd32Checksum();
}

// ============================================================
// GD32 中继阶段
// ============================================================
esp_err_t OtaCoordinator::triggerGd32Relay()
{
    bool h = lock();
    if (!h) {
        DEBUG_ERR(TAG, "triggerGd32Relay: lock 超时");
        return ESP_ERR_INVALID_STATE;
    }

    watchdogCheckLocked();

    /* 白名单：仅 STORED / IDLE（NVS 有完整固件）/ COMPLETE（允许再中继）可触发 */
    if (m_sysState != OTA_SYS_GD32_STORED &&
        m_sysState != OTA_SYS_IDLE &&
        m_sysState != OTA_SYS_GD32_COMPLETE) {
        DEBUG_WARN(TAG, "拒中继 state=%s(%d)",
                   stateToString(m_sysState), (int)m_sysState);
        unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    ota_meta_t meta;
    if (Gd32FlashStore::instance().metaLoad(&meta) != ESP_OK ||
        meta.total_size == 0 ||
        meta.stored_size < meta.total_size) {
        DEBUG_WARN(TAG, "GD32 固件未完整存储，拒绝触发中继");
        unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    /* ★ 包级防呆：SHA 匹配只证明"传对了"，不证明"传的是对的包"。
     * 把 BOOT 包中继进 APP 区会擦掉好固件，触发前必须确认镜像身份为 APP。
     * 返回 ESP_ERR_INVALID_SIZE 供调用方（WiFi/BLE）映射成具体错误提示。 */
    const Gd32ImageIdentity identity = Gd32FlashStore::instance().validateStoredImageIdentity();
    if (identity != Gd32ImageIdentity::kApp) {
        DEBUG_ERR(TAG, "拒绝中继：GD32 固件身份校验失败 (identity=%d)", (int)identity);
        unlock(h);
        return ESP_ERR_INVALID_SIZE;
    }

    if (OtaRelay::getState() != RELAY_IDLE &&
        OtaRelay::getState() != RELAY_DONE &&
        OtaRelay::getState() != RELAY_ERROR) {
        DEBUG_WARN(TAG, "OtaRelay 正忙 (state=%d)，拒绝触发", (int)OtaRelay::getState());
        unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    QueueHandle_t dq = OtaRelay::getDispatchQueue();
    if (!dq) {
        DEBUG_ERR(TAG, "OtaRelay dispatchQueue 未就绪");
        unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    /* 先入队再标 RELAYING：失败时状态与 CAN 反馈都不变 */
    pauseCanFeedbackTask(true);
    uint32_t cmd = 1;
    BaseType_t ret = xQueueSend(dq, &cmd, pdMS_TO_TICKS(100));
    if (ret != pdTRUE) {
        DEBUG_ERR(TAG, "向 OtaRelay 发送启动消息失败");
        pauseCanFeedbackTask(false);
        unlock(h);
        return ESP_FAIL;
    }

    m_sysState = OTA_SYS_GD32_RELAYING;
    m_busySinceTick = xTaskGetTickCount();
    DEBUG_DBG(TAG, "触发 GD32 中继, 系统状态 → GD32_RELAYING");
    unlock(h);
    return ESP_OK;
}

/** @brief 中继 task 已真正开始：若 trigger 已标 RELAYING 则只补 pause；否则兜底设状态。 */
void OtaCoordinator::notifyRelayStarted()
{
    bool h = lock();
    if (h) {
        if (m_sysState != OTA_SYS_GD32_RELAYING) {
            m_sysState = OTA_SYS_GD32_RELAYING;
            m_busySinceTick = xTaskGetTickCount();
            DEBUG_DBG(TAG, "中继已开始(兜底), 系统状态 → GD32_RELAYING");
        }
        pauseCanFeedbackTask(true);
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
            pauseCanFeedbackTask(false);
            DEBUG_INFO(TAG, "GD32 中继完成, 系统状态 → GD32_COMPLETE");
        } else {
            m_sysState = OTA_SYS_ERROR;
            m_busySinceTick = 0;  // ★ ERROR 非忙（可被后续请求恢复）
            pauseCanFeedbackTask(false);
            DEBUG_ERR(TAG, "GD32 中继失败, 系统状态 → ERROR");
        }
    }
    unlock(h);
    /* 中继结束：若 BLE 仍因 WiFi OTA 挂起则恢复 */
    wifiOtaResumeBleIfNeeded();
}

// ============================================================
// IRelayListener 接口实现
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
// BLE 射频共存管理
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
esp_err_t OtaCoordinator::beginEsp32Ota(uint32_t total_size, ota_channel_t channel)
{
    bool h;
    esp_err_t pre = beginLockedWithChecks("ESP32 OTA", h);
    if (pre != ESP_OK) return pre;

    // ★ 同类型重启：若上一次 ESP32 OTA 仍在接收中（RECEIVING）且来自同一通道，
    //   自动中止旧会话并重新开始。覆盖"上传完成后刷新网页再上传"场景。
    //   跨通道（BLE→WiFi 或 WiFi→BLE）仍拒绝，保持互斥。
    if (m_sysState == OTA_SYS_ESP32_RECEIVING) {
        if (m_otaChannel != channel) {
            DEBUG_WARN(TAG, "ESP32收中 ch=%d 拒跨通道 ch=%d",
                       (int)m_otaChannel, (int)channel);
            unlock(h);
            return ESP_ERR_INVALID_STATE;
        }
        DEBUG_WARN(TAG, "ESP32同通道重传:中止旧会话");
        doAbortLocked();  // abort core + IDLE + 恢复 CAN 反馈
        // epoch 将在下方 begin 成功后递增，自动使旧 handler 的清理代码失效
    }

    // ★ FINALIZING 残留自愈：SHA 不匹配时 core 已 abort，但旧固件可能未把协调器
    //   复位到 IDLE。core 已空闲 ⇒ 协调器状态是假占用，允许清掉后开新会话。
    //   core 仍 active ⇒ 真正在收尾，拒绝（不可 forceCleanup，防并发写分区）。
    if (m_sysState == OTA_SYS_ESP32_FINALIZING) {
        if (!Esp32OtaCore::instance().isActive()) {
            DEBUG_WARN(TAG, "FINALIZING残留(core空闲),强制复位");
            doAbortLocked();
        } else {
            DEBUG_WARN(TAG, "系统忙 (ESP32_FINALIZING, core 活跃)，拒绝 ESP32 OTA");
            unlock(h);
            return ESP_ERR_INVALID_STATE;
        }
    }

    // 系统忙检查：GD32 传输/中继进行中，或 ESP32 重启中，均拒绝。
    if (m_sysState == OTA_SYS_GD32_STORING ||
        m_sysState == OTA_SYS_GD32_RELAYING ||
        m_sysState == OTA_SYS_ESP32_REBOOTING) {
        DEBUG_WARN(TAG, "忙 state=%d(%s) 拒ESP32 OTA",
                   (int)m_sysState, stateToString(m_sysState));
        unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    // 委托 Esp32OtaCore 处理分区/互斥逻辑
    esp_err_t err = Esp32OtaCore::instance().begin(total_size);
    if (err == ESP_OK) {
        m_otaEpoch++;  // ★ 新会话开始，递增 epoch（使旧 handler 的 abortCurrentIfEpoch 失效）
        m_otaChannel = channel;  // ★ 记录发起通道
        m_sysState = OTA_SYS_ESP32_RECEIVING;
        m_busySinceTick = xTaskGetTickCount();  // ★ 记录起始时刻
        pauseCanFeedbackTask(true);
    } else {
        DEBUG_WARN(TAG, "ESP32 OTA begin 失败: %s", esp_err_to_name(err));
    }

    unlock(h);
    return err;
}

esp_err_t OtaCoordinator::beginEsp32OtaWithRetry(uint32_t total_size, ota_channel_t channel)
{
    /* WiFi 路径：在 begin 前挂 BLE，把 BT 堆腾给 OTA 收流（BLE 通道 OTA 不挂）。 */
    wifiOtaSuspendBleIfNeeded(channel);

    esp_err_t err = beginEsp32Ota(total_size, channel);
    if (err == ESP_ERR_INVALID_STATE) {
        // ★ 仅当系统确实空闲（无任何活跃 OTA）时才允许 forceCleanup：
        //   这种情况只可能是"锁残留"——前一次 OTA 异常退出没释放全局互斥，
        //   而协调器状态已回 IDLE。
        //   若系统正处于任何活跃 OTA（ESP32/GD32），绝不强制清理——那会
        //   把另一通道正在进行的 esp_ota_handle 中止、释放互斥，导致
        //   两通道并发写同一 OTA 分区（之前 BLE 与 WiFi 可同时 OTA 的根因）。
        //   避免连续 isIdle() + getSystemState() 各加锁一次（共 2 次锁）。
        ota_system_state_t cur = getSystemState();
        if (!isIdleState(cur) && cur != OTA_SYS_ERROR) {
            DEBUG_WARN(TAG, "ESP32拒且非空闲 state=%d,不强制清",
                       (int)cur);
            if (channel == OTA_CHANNEL_WIFI) {
                wifiOtaResumeBleIfNeeded();
            }
            return err;
        }

        if (cur == OTA_SYS_ERROR) {
            DEBUG_WARN(TAG, "ESP32拒但ERROR,清残留后重试");
        } else {
            DEBUG_WARN(TAG, "ESP32拒但空闲,判锁残留,强制清后重试");
        }
        /* ★ 若 motor OTA 的 feedTask 残留活跃（s_feed_running 仍 true，
         *   常见于 cancel 请求未送达或 feedTask 卡在 SPI transact），
         *   必须先 motorOtaCancel() 让 feedTask 退出，否则 forceCleanup
         *   只清 ESP32 锁、motorOtaIsActive() 仍 true，重试时又被拒，
         *   形成日志里"强制清后重试→电机 OTA 进行中→再拒"的死循环。 */
        if (motorOtaIsActive()) {
            DEBUG_WARN(TAG, "强制清前先取消残留的电机 OTA feedTask");
            motorOtaCancel();
        }
        Esp32OtaCore::instance().forceCleanup();
        err = beginEsp32Ota(total_size, channel);
    }
    if (err != ESP_OK && channel == OTA_CHANNEL_WIFI) {
        wifiOtaResumeBleIfNeeded();
    }
    return err;
}

// ★ 已持锁版本的 ESP32 状态切换 helper：设置 m_sysState 并打印一条 INFO 日志。
//   供 notifyEsp32*Complete / notifyEsp32Rebooting 等锁内路径复用，避免重复样板代码。
void OtaCoordinator::setEsp32StateLocked(ota_system_state_t s, const char *dbg_msg)
{
    m_sysState = s;
    /* REBOOTING 不参与 busy watchdog，清零起点避免误中止 */
    if (s == OTA_SYS_ESP32_REBOOTING) {
        m_busySinceTick = 0;
    } else if (s == OTA_SYS_ESP32_FINALIZING && m_busySinceTick == 0) {
        m_busySinceTick = xTaskGetTickCount();
    }
    DEBUG_DBG(TAG, "%s", dbg_msg);
}

/** @brief 通知协调器：ESP32 固件接收完毕，状态转为 FINALIZING。 */
void OtaCoordinator::notifyEsp32ReceiveComplete()
{
    bool h = lock();
    if (h && m_sysState == OTA_SYS_ESP32_RECEIVING) {
        setEsp32StateLocked(OTA_SYS_ESP32_FINALIZING,
                            "ESP32 固件接收完毕, 系统状态 → ESP32_FINALIZING");
    }
    unlock(h);
}

/** @brief 通知协调器：ESP32 OTA 收尾完成（SHA验证+标记启动），状态转为 REBOOTING。 */
void OtaCoordinator::notifyEsp32FinalizeComplete()
{
    bool h = lock();
    if (h) {
        setEsp32StateLocked(OTA_SYS_ESP32_REBOOTING,
                            "ESP32 OTA 收尾完成, 系统状态 → ESP32_REBOOTING");
    }
    unlock(h);
}

/** @brief 通知协调器：ESP32 即将重启。 */
void OtaCoordinator::notifyEsp32Rebooting()
{
    bool h = lock();
    if (h) {
        setEsp32StateLocked(OTA_SYS_ESP32_REBOOTING,
                            "ESP32 即将重启, 系统状态 → ESP32_REBOOTING");
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
    } else {
        /* finalize 失败：复位协调器到 IDLE */
        DEBUG_ERR(TAG, "ESP32 finalize 失败: %s，复位协调器 → IDLE",
                  esp_err_to_name(r.err));
        abortCurrent();
    }
    return r;
}

void OtaCoordinator::restartEsp32()
{
    notifyEsp32Rebooting();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    // 不会返回
}

// ============================================================
// 中止
// ============================================================
void OtaCoordinator::doAbortLocked()
{
    DEBUG_INFO(TAG, "中止当前 OTA 操作 (state=%d, epoch=%u)", (int)m_sysState, m_otaEpoch);

    if (m_sysState == OTA_SYS_GD32_STORING) {
        Gd32FlashStore::instance().metaClear();
        Gd32FlashStore::releaseAlignBufForOta();   // 释放 OTA 期对齐缓冲
    } else if (m_sysState == OTA_SYS_GD32_RELAYING) {
        /* 中继中止必须 CANCEL 远端，否则 GD32 停在 WAITING_* 半会话 */
        OtaRelay::instance().abortRemoteAndReset(true);
        Gd32FlashStore::releaseAlignBufForOta();   // 中继阶段也可能写过 flash
    } else if (m_sysState == OTA_SYS_ESP32_RECEIVING ||
               m_sysState == OTA_SYS_ESP32_FINALIZING) {
        Esp32OtaCore::instance().abort();
    }

    pauseCanFeedbackTask(false);
    m_sysState = OTA_SYS_IDLE;
    m_gd32StorageDone = false;
    m_busySinceTick = 0;
    DEBUG_INFO(TAG, "已中止, 系统状态 → IDLE");
    /* 释放协调器锁后由调用方 resume BLE；doAbortLocked 可能持锁，标记后外层 resume */
}

void OtaCoordinator::abortCurrent()
{
    bool h = lock();
    if (h) {
        doAbortLocked();
    }
    unlock(h);
    wifiOtaResumeBleIfNeeded();
}

void OtaCoordinator::healOrphanStates()
{
    bool need_resume = false;
    bool h = lock();
    if (h) {
        ota_system_state_t before = m_sysState;
        watchdogCheckLocked();  // 超时则 doAbortLocked → IDLE
        // finalize 失败后 core 已空、协调器仍 FINALIZING 的假占用：status 轮询即可自愈
        if (m_sysState == OTA_SYS_ESP32_FINALIZING &&
            !Esp32OtaCore::instance().isActive()) {
            DEBUG_WARN(TAG, "status 路径发现 ESP32_FINALIZING 残留，自愈 → IDLE");
            doAbortLocked();
        }
        /* ERROR 非忙：status 轮询软清到 IDLE，避免 UI 长期卡 ERROR（新 begin 本也可覆盖） */
        if (m_sysState == OTA_SYS_ERROR) {
            DEBUG_INFO(TAG, "status 路径 ERROR → IDLE（软清）");
            m_sysState = OTA_SYS_IDLE;
            m_busySinceTick = 0;
            m_gd32StorageDone = false;
            pauseCanFeedbackTask(false);
        }
        // 任意自愈到 IDLE：恢复 WiFi OTA 期间挂起的 BLE（watchdog 旧路径漏 resume）
        if (before != OTA_SYS_IDLE && m_sysState == OTA_SYS_IDLE) {
            need_resume = true;
        }
        unlock(h);
    }
    if (need_resume) {
        wifiOtaResumeBleIfNeeded();
    }
}

uint32_t OtaCoordinator::getOtaEpoch() const
{
    bool h = lock();
    uint32_t epoch = m_otaEpoch;
    unlock(h);
    return epoch;
}

bool OtaCoordinator::abortCurrentIfEpoch(uint32_t epoch)
{
    bool h = lock();
    if (!h) return false;

    if (m_otaEpoch != epoch) {
        // ★ epoch 不匹配：新会话已接管，旧 handler 不应中止
        unlock(h);
        return false;
    }

    // epoch 匹配：执行中止（共享 doAbortLocked，已持锁）
    doAbortLocked();

    unlock(h);
    wifiOtaResumeBleIfNeeded();
    return true;
}

// ============================================================
// 状态查询
// ============================================================
ota_system_state_t OtaCoordinator::getSystemState() const
{
    // 对于纯读取，短暂加锁保证一致性
    // m_mutex 为 mutable + lock()/unlock() 为 const，无需 const_cast
    bool h = lock();
    ota_system_state_t s = m_sysState;
    unlock(h);
    return s;
}

ota_channel_t OtaCoordinator::getOtaChannel() const
{
    bool h = lock();
    ota_channel_t ch = m_otaChannel;
    unlock(h);
    return ch;
}

/** @brief 判断给定状态值是否属于空闲态（不加锁，纯判断）。
 *         供持锁路径或已取一次状态的调用方复用，避免连续加锁。 */
bool OtaCoordinator::isIdleState(ota_system_state_t s) const
{
    return (s == OTA_SYS_IDLE ||
            s == OTA_SYS_GD32_STORED ||
            s == OTA_SYS_GD32_COMPLETE);
}

/** @brief 判断系统是否处于空闲状态（IDLE/STORED/COMPLETE）。 */
bool OtaCoordinator::isIdle() const
{
    return isIdleState(getSystemState());
}

/** @brief 把状态枚举转成可读字符串（不加锁，纯函数）。
 *         供持锁代码路径和已取一次状态值的调用方复用，避免二次加锁。 */
const char* OtaCoordinator::stateToString(ota_system_state_t s)
{
    switch (s) {
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

// ============================================================
// OTA 状态快照
// ============================================================
OtaSnapshot OtaCoordinator::getSnapshot() const
{
    OtaSnapshot snap{};

    /* 持锁一次读取状态并生成字符串，保证快照字段一致 */
    bool h = lock();
    if (h) {
        snap.sys_state = m_sysState;
        snap.channel = m_otaChannel;
        snap.storage_done = m_gd32StorageDone;
        snap.sys_state_str = stateToString(snap.sys_state);
        unlock(h);
    } else {
        // 加锁失败时也填一个与默认 sys_state(=IDLE) 一致的字符串
        snap.sys_state_str = stateToString(snap.sys_state);
    }

    // ── GD32 NVS 元数据 ──
    Gd32FlashStore::instance().metaLoad(&snap.meta);

    // ── relay 进度（一次 relay mutex）──
    snap.relay = OtaRelay::getSnapshot();

    return snap;
}

// ============================================================
// GD32 APP 探测
// ============================================================
esp_err_t OtaCoordinator::probeGd32AppOnce(gd32_version_t *out_version, uint32_t *out_uptime)
{
    /* 统一走 OtaRelay 公开探测，避免与 probeAndCacheAppAlive 双份逻辑 */
    uint32_t uptime = 0;
    if (OtaRelay::probeAndCacheAppAlive(500, &uptime)) {
        if (out_version) {
            *out_version = OtaRelay::getSnapshot().version;
        }
        if (out_uptime) {
            *out_uptime = uptime;
        }
        DEBUG_INFO(TAG, "GD32 APP 探测成功: alive, uptime=%" PRIu32 "s", uptime);
        return ESP_OK;
    }
    if (out_version) {
        *out_version = gd32_version_t{};
    }
    if (out_uptime) {
        *out_uptime = 0;
    }
    return ESP_FAIL;
}

// ============================================================
// 完全复位
// ============================================================
void OtaCoordinator::reset()
{
    // ★ 与 abortCurrent 共用 doAbortLocked：清 Esp32OtaCore / relay / meta，勿只改枚举
    bool h = lock();
    if (h) {
        doAbortLocked();
        DEBUG_INFO(TAG, "协调器已完全复位（含子系统 abort）");
    }
    unlock(h);
    wifiOtaResumeBleIfNeeded();
}
