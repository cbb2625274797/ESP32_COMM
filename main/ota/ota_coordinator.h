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
#include "app_config.h"        // OTA_MAX_FIRMWARE_SIZE / GD32_OTA_PACKET_SIZE（validateGd32Size 用）
#include "storage/gd32_flash_store.h"
#include "relay/ota_relay.h"   // IRelayListener 接口定义 + RelaySnapshot（H4 解耦）

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

// ============================================================
// OTA 传输通道标识（用于同类型重启时区分同通道/跨通道）
// ============================================================
typedef enum {
    OTA_CHANNEL_WIFI = 0,
    OTA_CHANNEL_BLE = 1,
} ota_channel_t;

// ============================================================
// OTA 系统全量只读快照（PR1: 收口状态读取）
// ============================================================
/**
 * @brief OTA 系统全量只读快照。
 *        由 OtaCoordinator::getSnapshot() 一次性填充，
 *        供 /system/status 等只读接口消费，避免散读各模块状态。
 */
struct OtaSnapshot {
    ota_system_state_t sys_state = OTA_SYS_IDLE;  ///< 系统级 OTA 状态
    const char *sys_state_str = "IDLE";            ///< 状态字符串
    ota_channel_t channel = OTA_CHANNEL_WIFI;      ///< 当前/最近一次 OTA 通道
    bool storage_done = false;                     ///< GD32 固件存储完成标志
    ota_meta_t meta = {};                          ///< GD32 NVS 元数据
    RelaySnapshot relay;                           ///< 中继进度快照
};

// ============================================================
// IBleCoexListener - BLE 射频共存通知接口（解决 L1 横向耦合）
// ============================================================
// WifiOta 实现此接口并注册到 OtaCoordinator，
// BLE OTA 开始/结束时通过协调器通知 WiFi 层，
// 消除 ble_ota → wifi_ota 的直接依赖。
/**
 * @brief BLE 射频共存监听器接口。
 *        BLE OTA 开始/结束时，协调器通过此接口通知 WiFi 层暂停/恢复射频活动。
 */
class IBleCoexListener {
public:
    virtual ~IBleCoexListener() = default;
    /** @brief BLE OTA 开始，暂停 WiFi RSSI 轮询等射频活动。 */
    virtual void onBleOtaStart() = 0;
    /** @brief BLE OTA 结束，恢复 WiFi 射频活动。 */
    virtual void onBleOtaEnd() = 0;
};

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
class OtaCoordinator : public IRelayListener {
public:
    /**
     * @brief 获取 OtaCoordinator 单例实例。
     * @return OtaCoordinator 单例引用。
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
     * @param channel 发起通道（WiFi/BLE），用于同类型重启时区分同通道/跨通道。
     * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 系统忙。
     */
    esp_err_t beginGd32Storage(uint32_t total_size, ota_channel_t channel);

    /**
     * @brief 统一 GD32 OTA 写入前的准备工作：校验大小 + begin + 擦除分区 + 清空 meta + 写入初始 meta。
     *        供 BLE seq=0 首包 / HTTP GD32 upload（非 prepared 复用路径）调用，
     *        消除两路径对"擦除/meta 初始化"序列的隐式差异（原 BLE 路径未擦除分区）。
     *        任意步骤失败时内部调 abortCurrent 清理，调用方无需再清理。
     * @param total_size 固件总字节数。
     * @param channel 发起通道（WiFi/BLE）。
     * @return ESP_OK 成功；ESP_ERR_INVALID_SIZE 大小非法；ESP_ERR_INVALID_STATE 系统忙；其他擦除/写入错误。
     */
    esp_err_t prepareGd32Storage(uint32_t total_size, ota_channel_t channel);

    /**
     * @brief GD32 固件接收完毕时调用。
     *        更新系统状态为 OTA_SYS_GD32_STORED。
     */
    void completeGd32Storage();

    // ── GD32 存储操作（传输层通过协调器操作 Flash，解决 H3 绕过问题）──
    /**
     * @brief 向 GD32 Flash 分区写入固件数据块。
     *        传输层（WiFi/BLE）通过此接口写入，不再直接调用 Gd32FlashStore。
     * @param offset 写入偏移。
     * @param data 数据缓冲区。
     * @param len 数据长度。
     * @return ESP_OK 成功。
     */
    inline esp_err_t writeGd32Chunk(uint32_t offset, const uint8_t *data, size_t len)
    {
        return Gd32FlashStore::instance().flashWriteChunk(offset, data, len);
    }

    /**
     * @brief 保存 GD32 OTA 元数据。
     * @param meta 元数据结构体。
     * @return ESP_OK 成功。
     */
    inline esp_err_t saveGd32Meta(const ota_meta_t *meta)
    {
        return Gd32FlashStore::instance().metaSave(meta);
    }

    /**
     * @brief 加载 GD32 OTA 元数据。
     * @param meta 输出元数据结构体。
     * @return ESP_OK 成功。
     */
    inline esp_err_t loadGd32Meta(ota_meta_t *meta)
    {
        return Gd32FlashStore::instance().metaLoad(meta);
    }

    /**
     * @brief 更新 GD32 OTA 元数据的进度。
     * @param stored_size 已存储大小。
     * @param last_seq 最后包序号。
     * @return ESP_OK 成功。
     */
    inline esp_err_t updateGd32Meta(uint32_t stored_size, uint32_t last_seq)
    {
        return Gd32FlashStore::instance().metaUpdate(stored_size, last_seq);
    }

    /**
     * @brief 清除 GD32 OTA 元数据。
     * @return ESP_OK 成功。
     */
    inline esp_err_t clearGd32Meta()
    {
        return Gd32FlashStore::instance().metaClear();
    }

    /**
     * @brief 校验前端 SHA-256 与设备端固件是否一致。
     * @param frontend_sha32 前端 32 字节 SHA。
     * @param out_device_sha 输出设备端 32 字节 SHA（可为 nullptr）。
     * @return ESP_OK 匹配，ESP_ERR_INVALID_CRC 不匹配。
     */
    inline esp_err_t verifyGd32Firmware(const uint8_t frontend_sha32[32],
                                         uint8_t out_device_sha[32])
    {
        return Gd32FlashStore::instance().verifyFrontendSha(frontend_sha32, out_device_sha);
    }

    /**
     * @brief 计算 GD32 固件 SHA-256 并写入元数据。
     * @return ESP_OK 成功。
     */
    inline esp_err_t finalizeGd32Checksum()
    {
        return Gd32FlashStore::instance().metaFinalizeChecksum();
    }

    /**
     * @brief 擦除 GD32 OTA 分区。
     * @param size 擦除大小。
     * @return ESP_OK 成功。
     */
    inline esp_err_t eraseGd32Partition(uint32_t size)
    {
        return Gd32FlashStore::instance().erasePartition(size);
    }

    /**
     * @brief 校验 GD32 固件大小是否合法（BLE/HTTP 双通道共用）。
     *
     * 统一规则：raw==0 或 raw>MAX 拒绝；按 GD32_OTA_PACKET_SIZE 向上取整后仍>MAX 拒绝。
     * 调用方各自翻译为通道特定错误码（BLE: BLE_OTA_FINISH_ERR_GD32_SIZE；HTTP: HTTPD_500）。
     *
     * @param raw_size 固件原始字节数。
     * @return ESP_OK 合法；ESP_ERR_INVALID_SIZE 非法。
     */
    inline esp_err_t validateGd32Size(uint32_t raw_size) const
    {
        if (raw_size == 0U || raw_size > (uint32_t)OTA_MAX_FIRMWARE_SIZE) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint32_t packet_size = (uint32_t)GD32_OTA_PACKET_SIZE;
        const uint32_t rounded = ((raw_size + packet_size - 1U) / packet_size) * packet_size;
        if (rounded > (uint32_t)OTA_MAX_FIRMWARE_SIZE) {
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    }

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
     * @param channel 发起通道（WiFi/BLE），用于同类型重启时区分同通道/跨通道。
     * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 系统忙或通道被占用。
     */
    esp_err_t beginEsp32Ota(uint32_t total_size, ota_channel_t channel);

    /**
     * @brief 发起 ESP32 OTA，若被 ESP_ERR_INVALID_STATE 拒绝则 forceCleanup 后重试一次。
     *        封装 BLE/WiFi 两端完全相同的重试模式，调用方只需调一次。
     * @param total_size 固件总字节数。
     * @param channel 发起通道（WiFi/BLE）。
     * @return ESP_OK 成功；其他 = 协调器拒绝或 Esp32OtaCore::begin 失败。
     */
    esp_err_t beginEsp32OtaWithRetry(uint32_t total_size, ota_channel_t channel);

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

    /**
     * @brief 获取当前 OTA 会话的 epoch（代次）。
     *        每次 beginEsp32Ota/beginGd32Storage 成功时递增。
     *        传输层 handler 在 begin 成功后捕获 epoch，
     *        在失败清理时通过 abortCurrentIfEpoch() 检查是否仍为当前会话，
     *        避免旧 handler 的清理代码误中止新会话（同类型重启场景）。
     * @return 当前 epoch 值。
     */
    uint32_t getOtaEpoch() const;

    /**
     * @brief 仅当 epoch 匹配当前会话时才中止 OTA。
     *        用于旧 handler 的失败清理路径：若新请求已经接管（epoch 不匹配），
     *        旧 handler 不应调 abortCurrent，否则会误中止新会话。
     * @param epoch 调用方在 begin 成功时捕获的 epoch。
     * @return true 已中止（epoch 匹配），false 未中止（新会话已接管）。
     */
    bool abortCurrentIfEpoch(uint32_t epoch);

    // ── 状态查询 ──
    /**
     * @brief 一次性读取系统 OTA 全量状态快照（PR1: 收口状态读取）。
     *        coordinator mutex + relay mutex 各取一次，
     *        供 /system/status 等只读接口消费。
     * @return 包含 sys_state、meta、relay 进度的完整快照。
     */
    OtaSnapshot getSnapshot() const;

    /**
     * @brief 单次 SPI 探测 GD32 APP 是否存活（PR2: 从 status 移出）。
     *        成功时更新 OtaRelay 缓存的 alive/version，
     *        失败时清除 alive 标志。
     * @param out_version 输出版本信息，可为 nullptr。
     * @param out_uptime  输出 GD32 运行时间（秒），可为 nullptr。
     * @return ESP_OK 存活，其他失败。
     */
    esp_err_t probeGd32AppOnce(gd32_version_t *out_version, uint32_t *out_uptime);

    /**
     * @brief 获取当前系统级 OTA 状态。
     * @return 当前 ota_system_state_t 状态值。
     */
    ota_system_state_t getSystemState() const;

    /**
     * @brief 获取当前 OTA 会话的发起通道（WiFi/BLE）。
     *        仅在活跃状态（ESP32_RECEIVING / GD32_STORING）下有意义；
     *        IDLE 等非活跃状态返回值未定义（默认 OTA_CHANNEL_WIFI）。
     * @return 当前会话通道枚举值。
     */
    ota_channel_t getOtaChannel() const;

    /**
     * @brief 把状态枚举转成可读字符串（不加锁，纯函数）。
     *        供持锁代码路径和已取一次状态值的调用方复用，避免二次加锁。
     * @param s 状态枚举值。
     * @return 状态名称字符串（如 "IDLE"、"GD32_STORING" 等）。
     */
    static const char* stateToString(ota_system_state_t s);

    /**
     * @brief 检查给定状态值是否属于空闲态（不加锁，纯判断）。
     *        调用方先 getSystemState() 取一次再传入，避免连续加锁。
     * @param s 状态枚举值。
     * @return true 表示 IDLE/STORED/COMPLETE。
     */
    bool isIdleState(ota_system_state_t s) const;

    /**
     * @brief 检查给定状态值是否属于 GD32 忙态（不加锁，纯判断）。
     * @param s 状态枚举值。
     * @return true 表示 STORING 或 RELAYING。
     */
    bool isGd32BusyState(ota_system_state_t s) const;

    /**
     * @brief 检查给定状态值是否属于 ESP32 忙态（不加锁，纯判断）。
     * @param s 状态枚举值。
     * @return true 表示 RECEIVING/FINALIZING/REBOOTING。
     */
    bool isEsp32BusyState(ota_system_state_t s) const;

    /**
     * @brief 检查系统是否空闲（可接受新的 OTA 请求）。
     *        内部仍加锁取一次状态后调 isIdleState()。
     * @return true 系统空闲（IDLE/STORED/COMPLETE）。
     */
    bool isIdle() const;

    /**
     * @brief 检查是否有 GD32 相关操作正在进行。
     *        内部仍加锁取一次状态后调 isGd32BusyState()。
     * @return true 表示 STORING 或 RELAYING。
     */
    bool isGd32Busy() const;

    /**
     * @brief 检查是否有 ESP32 OTA 正在进行。
     *        内部仍加锁取一次状态后调 isEsp32BusyState()。
     * @return true 表示 RECEIVING/FINALIZING/REBOOTING。
     */
    bool isEsp32Busy() const;

    /**
     * @brief 获取系统状态的可读字符串描述。
     *        内部委托 stateToString(getSystemState())，保持向后兼容。
     * @return 状态名称字符串（如 "IDLE"、"GD32_STORING" 等）。
     */
    const char* getSystemStateString() const;

    // ── GD32 状态（供 status API 使用，委托给 OtaRelay 查询）──
    /**
     * @brief 获取 GD32 APP 存活状态。
     * @return true 表示 GD32 APP 存活。
     */
    bool getGd32AppAlive() const;

    /**
     * @brief GD32 固件存储完成标志（供状态查询）。
     * @return true 表示固件已完整存储。
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
     * @param err finalize 返回的 esp_err_t。
     * @return 0=成功；1=SHA 不匹配；2=其他失败。
     */
    static int esp32FinalizeErrCategory(esp_err_t err);

    // ── IRelayListener 接口实现（H4: OtaRelay 通过接口回调通知协调器）──
    /**
     * @brief IRelayListener 回调：中继已开始。
     *        由 OtaRelay task 在 forwardFirmware() 前通过 s_relayListener 调用。
     */
    void onRelayStarted() override;

    /**
     * @brief IRelayListener 回调：中继已完成。
     *        由 OtaRelay task 在 forwardFirmware() 返回后通过 s_relayListener 调用。
     * @param success true 成功，false 失败。
     */
    void onRelayComplete(bool success) override;

    // ── BLE 射频共存管理（L1: 替代 ble_ota → wifi_ota 直接调用）──
    /**
     * @brief 注册 BLE 共存监听器（由 main 在初始化时调用）。
     * @param listener WifiOta 实现的监听器指针，nullptr 取消注册。
     */
    void setBleCoexListener(IBleCoexListener *listener);

    /**
     * @brief BLE OTA 开始时调用，通知 WiFi 层暂停 RSSI 轮询。
     */
    void notifyBleCoexStart();

    /**
     * @brief BLE OTA 结束时调用，通知 WiFi 层恢复射频活动。
     */
    void notifyBleCoexEnd();

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
    bool lock() const;
    void unlock(bool handle) const;

    // ★ 卡死自愈检查：若忙状态停留超过 watchdog 超时，强制转为 ERROR。
    //   用于在中继 task 因任何原因（崩溃/SPI 死锁/看门狗复位残留）未能回调
    //   notifyRelayComplete/notifyEsp32*Complete 时，让协调器自动脱离卡死状态。
    //   必须在已持锁状态下调用。
    void watchdogCheckLocked();

    // ★ 已持锁版本的中止序列：根据当前 m_sysState 调用对应子模块的清理，
    //   并把状态机复位为 IDLE。abortCurrent / abortCurrentIfEpoch 共享此实现。
    void doAbortLocked();

    // 系统状态
    ota_system_state_t m_sysState = OTA_SYS_IDLE;

    // 进入当前忙状态的时刻（tick）。IDLE/STORED/COMPLETE/ERROR 时为 0。
    TickType_t m_busySinceTick = 0;

    // GD32 存储跟踪
    bool m_gd32StorageDone = false;

    // OTA 会话代次：每次 beginEsp32Ota/beginGd32Storage 成功时递增。
    // 传输层 handler 在 begin 后捕获 epoch，失败清理时通过 abortCurrentIfEpoch
    // 检查 epoch 是否仍匹配，避免旧 handler 误中止同类型重启后的新会话。
    uint32_t m_otaEpoch = 0;

    // 当前 OTA 会话的发起通道（WiFi/BLE），用于同类型重启时区分同通道/跨通道。
    // 仅在同类型活跃状态（ESP32_RECEIVING / GD32_STORING）下有意义。
    ota_channel_t m_otaChannel = OTA_CHANNEL_WIFI;

    // 互斥锁（M1/L3: mutable 允许在 const 方法中加锁，消除 const_cast）
    mutable SemaphoreHandle_t m_mutex = nullptr;

    // BLE 共存监听器（L1: 由 WifiOta 实现并注册）
    IBleCoexListener *m_bleCoexListener = nullptr;
    static constexpr TickType_t kLockTimeout = pdMS_TO_TICKS(100);

    // ★ 忙状态 watchdog 超时。GD32 中继理论最大耗时 ≈ 60s(全局超时) + 15s(APP存活轮询) + 1s(bootloader) ≈ 76s，
    //   取 90 s，正常流程远不会触及，仅在 task 卡死时触发自愈。
    static constexpr TickType_t kBusyWatchdogTicks = pdMS_TO_TICKS(90U * 1000U);
};

#endif // OTA_COORDINATOR_H
