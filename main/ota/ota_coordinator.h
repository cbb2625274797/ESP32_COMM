/*
 * ota_coordinator.h - 全局 OTA 协调器
 *
 * OtaCoordinator 是系统级 OTA 状态的唯一权威来源，负责：
 *   1. 串行化 GD32 中继与 ESP32 OTA 操作（同一时刻只能进行一种）
 *   2. 为传输层（WiFi/BLE）提供统一的 OTA 操作入口
 *   3. 通过 FreeRTOS 队列向 OtaRelay task 投递中继命令
 *   4. 向上层（web UI / status API）暴露系统级状态
 *
 * 线程模型：
 *   - 状态读写由内部 mutex 保护
 *   - begin/complete 接口可在任意任务中调用
 *   - triggerGd32Relay() 向 OtaRelay task 的 dispatchQueue 发送消息
 */
#ifndef OTA_COORDINATOR_H
#define OTA_COORDINATOR_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"

// ============================================================
// 系统级 OTA 状态枚举
// ============================================================
typedef enum {
    OTA_SYS_IDLE = 0,
    OTA_SYS_GD32_STORING,       // 正在从传输层接收 GD32 固件
    OTA_SYS_GD32_STORED,        // GD32 固件已完整存储，等待中继触发
    OTA_SYS_GD32_RELAYING,      // 正在通过 SPI 向 GD32 中继固件
    OTA_SYS_GD32_COMPLETE,      // GD32 OTA 完成（APP 已存活确认）
    OTA_SYS_ESP32_RECEIVING,    // 正在接收 ESP32 自身固件
    OTA_SYS_ESP32_FINALIZING,   // 正在收尾 ESP32 OTA（SHA校验 + end + set_boot）
    OTA_SYS_ESP32_REBOOTING,    // 即将重启 ESP32
    OTA_SYS_ERROR               // 错误状态
} ota_system_state_t;

/**
 * @brief ESP32 OTA 收尾结果（由 finalizeForReboot 返回）。
 *        err 为 ESP_OK 时 device_sha_hex 有效；SHA 不匹配时也填充 device_sha_hex 供前端核对。
 */
typedef struct {
    esp_err_t err;                ///< ESP_OK / ESP_ERR_INVALID_VERSION(SHA不匹配) / 其他
    char      device_sha_hex[65]; ///< 设备端 SHA hex（成功或 SHA 不匹配时填充）
} Esp32FinalizeResult;

/**
 * @brief 全局 OTA 协调器（单例）
 *
 * 提供系统级 OTA 状态管理与操作串行化。
 * 传输层（WiFi/BLE）通过本协调器发起所有 OTA 操作，
 * 不再直接调用 OtaRelay / Esp32OtaCore 的底层接口。
 */
class OtaCoordinator {
public:
    /**
     * @brief 获取 OtaCoordinator 单例实例。
     */
    static OtaCoordinator& instance();

    // ── 初始化 ──
    /**
     * @brief 初始化协调器（创建 mutex 和队列）。
     *        在 OtaRelay task 创建前调用。
     */
    void init();

    // ── GD32 存储阶段（传输层 → Flash）──
    /**
     * @brief 传输层开始接收 GD32 固件前调用。
     *        若系统正忙（ESP32 OTA 进行中或 GD32 中继中），返回失败。
     * @param total_size 固件总字节数。
     * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 系统忙。
     */
    esp_err_t beginGd32Storage(uint32_t total_size);

    /**
     * @brief GD32 固件接收完毕时调用。
     *        更新系统状态为 OTA_SYS_GD32_STORED。
     */
    void completeGd32Storage();

    // ── GD32 中继阶段 ──
    /**
     * @brief 触发 GD32 中继转发。
     *        向 OtaRelay task 的 dispatchQueue 发送启动消息。
     * @return ESP_OK 消息已投递，ESP_ERR_INVALID_STATE 不允许触发。
     */
    esp_err_t triggerGd32Relay();

    /**
     * @brief OtaRelay task 回调：中继已开始。
     *        由 OtaRelay 在开始 forwardFirmware() 时调用。
     */
    void notifyRelayStarted();

    /**
     * @brief OtaRelay task 回调：中继已完成。
     *        由 OtaRelay 在 forwardFirmware() 返回后调用。
     * @param success true 成功，false 失败。
     */
    void notifyRelayComplete(bool success);

    // ── ESP32 OTA 阶段 ──
    /**
     * @brief ESP32 OTA 开始接收固件。
     *        内部委托 Esp32OtaCore::begin()。
     * @param total_size 固件总字节数。
     * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 系统忙或通道被占用。
     */
    esp_err_t beginEsp32Ota(uint32_t total_size);

    /**
     * @brief 发起 ESP32 OTA，若被 ESP_ERR_INVALID_STATE 拒绝则 forceCleanup 后重试一次。
     *        封装 BLE/WiFi 两端完全相同的重试模式，调用方只需调一次。
     * @param total_size 固件总字节数。
     * @return ESP_OK 成功；其他 = 协调器拒绝或 Esp32OtaCore::begin 失败。
     */
    esp_err_t beginEsp32OtaWithRetry(uint32_t total_size);

    /**
     * @brief ESP32 OTA 固件接收完毕，准备收尾。
     *        更新系统状态为 OTA_SYS_ESP32_FINALIZING。
     */
    void notifyEsp32ReceiveComplete();

    /**
     * @brief ESP32 OTA 收尾完成（finalize 成功）。
     */
    void notifyEsp32FinalizeComplete();

    /**
     * @brief ESP32 即将重启。
     */
    void notifyEsp32Rebooting();

    /**
     * @brief ESP32 OTA 收尾（统一封装 BLE/WiFi 两端重复的收尾序列）。
     *        步骤：notifyEsp32ReceiveComplete → Esp32OtaCore::finalize(reboot=false)
     *        → 成功则 notifyEsp32FinalizeComplete。
     *        不重启；调用方负责把 device_sha_hex 上报前端后再调 restartEsp32()。
     * @param frontend_sha_hex 前端 64 字符 hex SHA-256，nullptr 表示跳过比对。
     * @return Esp32FinalizeResult，err 为 ESP_OK/ESP_ERR_INVALID_VERSION/其他。
     */
    Esp32FinalizeResult finalizeForReboot(const char *frontend_sha_hex);

    /**
     * @brief 重启 ESP32（统一封装 BLE/WiFi 两端重复的重启序列）。
     *        步骤：notifyEsp32Rebooting → vTaskDelay(500ms) → esp_restart()。
     *        应在 finalizeForReboot 成功并完成前端上报后调用。不会返回。
     */
    void restartEsp32();

    // ── 中止 ──
    /**
     * @brief 中止当前 OTA 操作（GD32 或 ESP32）。
     *        内部调用对应子系统的 abort/reset。
     */
    void abortCurrent();

    // ── 状态查询 ──
    /**
     * @brief 获取当前系统级 OTA 状态。
     */
    ota_system_state_t getSystemState() const;

    /**
     * @brief 检查系统是否空闲（可接受新的 OTA 请求）。
     */
    bool isIdle() const;

    /**
     * @brief 检查是否有 GD32 相关操作正在进行。
     */
    bool isGd32Busy() const;

    /**
     * @brief 检查是否有 ESP32 OTA 正在进行。
     */
    bool isEsp32Busy() const;

    /**
     * @brief 获取系统状态的可读字符串描述。
     */
    const char* getSystemStateString() const;

    // ── GD32 状态（供 status API 使用，委托给 OtaRelay 查询）──
    /**
     * @brief 获取 GD32 APP 存活状态。
     */
    bool getGd32AppAlive() const;

    /**
     * @brief GD32 固件存储完成标志（供状态查询）。
     */
    bool isGd32StorageDone() const;

    // ── 完全复位 ──
    /**
     * @brief 完全复位协调器和所有子系统状态。
     */
    void reset();

    // ── ESP32 OTA finalize 错误分类（BLE/WiFi 共用）──
    /**
     * @brief 把 finalize 返回的 esp_err_t 映射为前端可读的事件码。
     *        统一 BLE/WiFi 两端语义，便于日志/状态一致。
     * @return 0=成功；1=SHA 不匹配；2=其他失败。
     */
    static int esp32FinalizeErrCategory(esp_err_t err);

private:
    OtaCoordinator() = default;
    ~OtaCoordinator() = default;
    OtaCoordinator(const OtaCoordinator&) = delete;
    OtaCoordinator& operator=(const OtaCoordinator&) = delete;

    // 线程安全辅助
    // ★ lock() 返回是否成功 take；unlock(handle) 据此决定是否 give。
    //   旧实现用共享 bool m_locked 记录 take 结果，多任务并发 lock() 时
    //   超时方会把 m_locked 置 false，覆盖持锁方的 true，导致 unlock() 跳过 give，
    //   mutex 永久死锁 → 后续 take 超时触发 vTaskPriorityDisinherit assert → PANIC。
    bool lock();
    void unlock(bool handle);

    // ★ 卡死自愈检查：若忙状态停留超过 watchdog 超时，强制转为 ERROR。
    //   用于在中继 task 因任何原因（崩溃/SPI 死锁/看门狗复位残留）未能回调
    //   notifyRelayComplete/notifyEsp32*Complete 时，让协调器自动脱离卡死状态。
    //   必须在已持锁状态下调用。
    void watchdogCheckLocked();

    // 系统状态
    ota_system_state_t m_sysState = OTA_SYS_IDLE;

    // 进入当前忙状态的时刻（tick）。IDLE/STORED/COMPLETE/ERROR 时为 0。
    TickType_t m_busySinceTick = 0;

    // GD32 存储跟踪
    bool m_gd32StorageDone = false;

    // 互斥锁
    SemaphoreHandle_t m_mutex = nullptr;
    static constexpr TickType_t kLockTimeout = pdMS_TO_TICKS(100);

    // ★ 忙状态 watchdog 超时。GD32 中继理论最大耗时 ≈ 60s(全局超时) + 15s(APP存活轮询) + 1s(bootloader) ≈ 76s，
    //   取 90 s，正常流程远不会触及，仅在 task 卡死时触发自愈。
    static constexpr TickType_t kBusyWatchdogTicks = pdMS_TO_TICKS(90U * 1000U);
};

#endif // OTA_COORDINATOR_H
