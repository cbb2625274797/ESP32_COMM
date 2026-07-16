/*
 * esp32_ota_core.h - ESP32 本机 OTA 核心流程（WiFi / BLE 通道共用）
 *
 * 将 ESP32 自身固件升级的完整生命周期封装为单例：
 *   begin → write*(N) → finalize / abort
 *
 * 统一以 WiFi 流程为基准：
 *   - begin：esp_ota_begin(OTA_WITH_SEQUENTIAL_WRITES) 边写边擦（避免 HTTP 头卡预擦）
 *   - SHA-256 在 finalize 时从 OTA 分区读回计算（不在上传期间增量计算）
 *   - finalize 顺序: 读回 SHA-256 → 比对前端 SHA → esp_ota_end → esp_ota_set_boot_partition → 可选重启
 *   - m_ended 标志位防止 esp_ota_end 成功后再次 esp_ota_abort 同一 handle
 *
 * WiFi 与 BLE 仅传输方式不同（HTTP / GATT），OTA 写入与收尾逻辑完全复用本核心。
 */
#ifndef ESP32_OTA_CORE_H
#define ESP32_OTA_CORE_H

#include <stddef.h>
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class Esp32OtaCore {
public:
    /**
      * @brief 获取 Esp32OtaCore 单例实例。
     * @return Esp32OtaCore 引用。
     */
    static Esp32OtaCore& instance();

    // ── 阶段 1：开启 OTA 会话 ──
    //   total_size: 固件总大小（字节），用于分区大小校验和传给 esp_ota_begin
    //   返回 ESP_OK / ESP_ERR_INVALID_STATE（被其它通道占用）/ ESP_FAIL
    /**
      * @brief 开启 OTA 会话。
     * @param total_size 固件总大小（字节）。
     * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 被占用，ESP_FAIL 其他错误。
     */
    esp_err_t begin(size_t total_size);

    // ── 阶段 2：写入固件数据（可在 begin 后多次调用）──
    //   纯 esp_ota_write，不做增量 SHA（SHA 在 finalize 时统一从分区读回计算）
    /**
      * @brief 写入固件数据块。
     * @param data 数据缓冲区。
     * @param len 数据长度。
     * @return ESP_OK 成功，否则错误码。
     */
    esp_err_t write(const uint8_t *data, size_t len);

    // ── 阶段 3：收尾（校验 SHA → end → set_boot → 可选重启）──
    //   frontend_sha_hex: 前端提供的 64 字符 hex SHA-256，nullptr 表示跳过校验直接应用
    //   reboot:           true 则 set_boot 成功后延时 500ms 调 esp_restart()
    //   out_device_sha_hex: 输出设备端计算出的 SHA hex（缓冲区至少 65 字节），可为 nullptr
    //   返回:
    //     ESP_OK                   成功（若 reboot=true，本调用不会返回）
    //     ESP_ERR_INVALID_VERSION  SHA 不匹配（已 abort 并清理）
    //     其他                     end/set_boot 失败（已清理）
    /**
      * @brief 收尾 OTA 会话：校验 SHA、end、set_boot、可选重启。
     * @param frontend_sha_hex 前端提供的 64 字符 hex SHA-256。
     * @param reboot 成功后是否重启。
     * @param out_device_sha_hex 输出设备端 SHA hex 缓冲区。
     * @return ESP_OK 成功，ESP_ERR_INVALID_VERSION SHA 不匹配。
     */
    esp_err_t finalize(const char *frontend_sha_hex, bool reboot,
                       char *out_device_sha_hex);

    // ── 仅计算设备端 SHA（不 end、不 set_boot、不重启）──
    //   供前端只读校验场景（如 WiFi verify_only 模式）
    //   out_device_sha_hex: 输出缓冲区，至少 65 字节
    //   返回 ESP_OK / 失败
    /**
      * @brief 仅计算设备端 SHA（不 end、不 set_boot、不重启）。
     * @param out_device_sha_hex 输出缓冲区，至少 65 字节。
     * @return ESP_OK 成功，否则错误码。
     */
    esp_err_t computeDeviceSha(char *out_device_sha_hex);

    // ── 中止并清理（abort + 释放 mutex + 状态复位）──
    //   若 esp_ota_end 已成功（m_ended=true），则跳过 esp_ota_abort 仅复位状态
    /**
      * @brief 中止并清理 OTA 会话。
     */
    void abort();

    // ── 强制退出状态机并释放互斥锁 ──
    //   用于新通道发现锁被残留占用时，主动走完整的状态机退出流程：
    //   活跃会话 → abort()；仅锁残留 → 直接释放；均无 → 不操作
    //   返回 true = 执行了清理（调用方可重试 begin）
    /**
      * @brief 强制退出 OTA 状态机并释放互斥锁。
     * @return true 执行了清理，false 无需清理。
     */
    bool forceCleanup();

    // ── 状态查询 ──
    /** @brief 查询 OTA 是否激活。 */
    bool   isActive() const  { return m_active; }
    /** @brief 获取已接收的字节数。 */
    size_t getReceived() const { return m_received; }

    // ── 分区互斥锁管理 ──
    /**
     * @brief 初始化 ESP32 OTA 分区互斥锁（在 app_main 中调用）。
     */
    static void initMutex();
private:
    Esp32OtaCore() = default;

    // ── 分区互斥锁 ──
    static SemaphoreHandle_t s_mutex;      ///< ESP32 OTA 分区互斥锁
    static volatile bool     s_occupied;   ///< 分区占用标志
    static bool acquireMutex(uint32_t timeout_ms);
    static void releaseMutex();

    // 从 OTA 分区读回计算 SHA-256（WiFi 风格），结果写入 m_sha256，置 m_shaComputed=true
    /**
      * @brief 从 OTA 分区读回计算 SHA-256。
     * @return ESP_OK 成功，否则错误码。
     */
    esp_err_t computeShaFromPartition();

    // 状态全复位（不调 esp_ota_abort，由调用方根据 m_ended 决定）
    /**
      * @brief 复位所有状态（不调 esp_ota_abort）。
     */
    void resetState();

    // begin() 失败后的统一清理：释放 mutex + resetState() + 返回 code
    //   不调 esp_ota_abort（begin 失败时 m_handle 未成功创建）
    /**
      * @brief begin() 失败后的统一清理。
     * @param code 返回给调用方的错误码。
     * @return 透传 code。
     */
    esp_err_t beginCleanupAndReturn(esp_err_t code);

    // finalize() 失败后的统一清理：若 m_ended=false 调 esp_ota_abort 释放 handle；
    //   再复位 m_ended + 释放 mutex + resetState() + 返回 code
    /**
      * @brief finalize() 失败后的统一清理。
     * @param code 返回给调用方的错误码。
     * @return 透传 code。
     */
    esp_err_t finalizeCleanupAndReturn(esp_err_t code);

    bool                 m_active = false;
    esp_ota_handle_t     m_handle = 0;
    const esp_partition_t *m_partition = nullptr;
    size_t               m_received = 0;
    uint8_t              m_sha256[32] = {0};
    bool                 m_shaComputed = false;
    bool                 m_ended = false;          // ★ esp_ota_end 成功后置 true，防 abort 误用
};

#endif // ESP32_OTA_CORE_H
