/*
 * esp32_ota_core.h - ESP32 本机 OTA 核心流程（WiFi / BLE 通道共用）
 *
 * 将 ESP32 自身固件升级的完整生命周期封装为单例：
 *   begin → write*(N) → finalize / abort
 *
 * 统一以 WiFi 流程为基准：
 *   - esp_ota_begin 第二参数传 total_size
 *   - SHA-256 在 finalize 时从 OTA 分区读回计算（不在上传期间增量计算）
 *   - finalize 顺序: esp_ota_end → 比对前端 SHA → esp_ota_set_boot_partition → 可选重启
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

class Esp32OtaCore {
public:
    static Esp32OtaCore& instance();

    // ── 阶段 1：开启 OTA 会话 ──
    //   total_size: 固件总大小（字节），用于分区大小校验和传给 esp_ota_begin
    //   返回 ESP_OK / ESP_ERR_INVALID_STATE（被其它通道占用）/ ESP_FAIL
    esp_err_t begin(size_t total_size);

    // ── 阶段 2：写入固件数据（可在 begin 后多次调用）──
    //   纯 esp_ota_write，不做增量 SHA（SHA 在 finalize 时统一从分区读回计算）
    esp_err_t write(const uint8_t *data, size_t len);

    // ── 阶段 3：收尾（end → 校验 SHA → set_boot → 可选重启）──
    //   frontend_sha_hex: 前端提供的 64 字符 hex SHA-256，nullptr 表示跳过校验直接应用
    //   reboot:           true 则 set_boot 成功后延时 500ms 调 esp_restart()
    //   out_device_sha_hex: 输出设备端计算出的 SHA hex（缓冲区至少 65 字节），可为 nullptr
    //   返回:
    //     ESP_OK                   成功（若 reboot=true，本调用不会返回）
    //     ESP_ERR_INVALID_VERSION  SHA 不匹配（已 abort 并清理）
    //     其他                     end/set_boot 失败（已清理）
    esp_err_t finalize(const char *frontend_sha_hex, bool reboot,
                       char *out_device_sha_hex);

    // ── 仅计算设备端 SHA（不 end、不 set_boot、不重启）──
    //   供前端只读校验场景（如 WiFi verify_only 模式）
    //   out_device_sha_hex: 输出缓冲区，至少 65 字节
    //   返回 ESP_OK / 失败
    esp_err_t computeDeviceSha(char *out_device_sha_hex);

    // ── 中止并清理（abort + 释放 mutex + 状态复位）──
    //   若 esp_ota_end 已成功（m_ended=true），则跳过 esp_ota_abort 仅复位状态
    void abort();

    // ── 状态查询 ──
    bool   isActive() const  { return m_active; }
    size_t getReceived() const { return m_received; }
    bool   isRebootPending() const { return m_rebootPending; }
    void   setRebootPending(bool v) { m_rebootPending = v; }

private:
    Esp32OtaCore() = default;

    // 从 OTA 分区读回计算 SHA-256（WiFi 风格），结果写入 m_sha256，置 m_shaComputed=true
    esp_err_t computeShaFromPartition();

    // 状态全复位（不调 esp_ota_abort，由调用方根据 m_ended 决定）
    void resetState();

    bool                 m_active = false;
    esp_ota_handle_t     m_handle = 0;
    const esp_partition_t *m_partition = nullptr;
    size_t               m_received = 0;
    uint8_t              m_sha256[32] = {0};
    bool                 m_shaComputed = false;
    bool                 m_ended = false;          // ★ esp_ota_end 成功后置 true，防 abort 误用
    bool                 m_rebootPending = false;  // BLE 收尾任务去重用
};

#endif // ESP32_OTA_CORE_H
