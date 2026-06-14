/*
 * esp32_ota_core.cpp - ESP32 本机 OTA 核心流程实现（WiFi / BLE 通道共用）
 *
 * 设计依据 WiFi 原流程（wifi_ota.cpp 旧实现），修复已知 bug:
 *   - 旧 wifi_ota.cpp:625 在 esp_ota_end 成功后又调 esp_ota_abort → 用 m_ended 标志避免
 *   - SHA 统一从分区读回计算，与 BLE 增量计算方式统一为本风格
 */
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/sha256.h"

#include "esp32_ota_core.h"
#include "ota_mutex.h"
#include "app_config.h"
#include "monitor/debug_log.h"

#define TAG "OTA_CORE"

/**
  * @brief 获取 Esp32OtaCore 单例实例。
 * @return Esp32OtaCore 引用。
 */
Esp32OtaCore& Esp32OtaCore::instance()
{
    static Esp32OtaCore inst;
    return inst;
}

/**
  * @brief 复位所有状态变量。
 */
void Esp32OtaCore::resetState()
{
    m_active = false;
    m_handle = 0;
    m_partition = nullptr;
    m_received = 0;
    m_shaComputed = false;
    memset(m_sha256, 0, sizeof(m_sha256));
    m_ended = false;
}

/**
  * @brief 开启 OTA 会话。
 * @param total_size 固件总大小（字节）。
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 被占用，ESP_FAIL 其他错误。
 */
esp_err_t Esp32OtaCore::begin(size_t total_size)
{
    // 若本核心已有未完成会话，先中止并释放全局互斥
    if (m_active) {
        DEBUG_WARN(TAG, "检测到未完成的 ESP32 OTA 会话，先中止旧会话");
        if (!m_ended) {
            esp_ota_abort(m_handle);
        }
        esp32_ota_mutex_release();
        resetState();
    }

    // ★ 全局互斥：检查 ESP32 OTA 是否已被其他通道占用
    if (!esp32_ota_mutex_acquire(1000)) {
        DEBUG_ERR(TAG, "ESP32 OTA 被其他通道占用，拒绝 begin");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *boot_partition = esp_ota_get_running_partition();
    DEBUG_INFO(TAG, "当前启动分区: %s, size=%u, address=0x%lx",
               boot_partition->label, boot_partition->size, boot_partition->address);

    // 选择非当前启动的 OTA 分区（OTA_0 → OTA_1 fallback）
    m_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (m_partition == NULL ||
        m_partition->address == boot_partition->address) {
        m_partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    }

    if (m_partition == NULL) {
        DEBUG_ERR(TAG, "找不到可用的 OTA 分区");
        esp32_ota_mutex_release();
        return ESP_FAIL;
    }

    DEBUG_INFO(TAG, "OTA 目标分区: %s, size=%u, address=0x%lx",
               m_partition->label, m_partition->size, m_partition->address);

    if (total_size > m_partition->size) {
        DEBUG_ERR(TAG, "固件太大: %u > 分区大小: %u",
                  (unsigned)total_size, (unsigned)m_partition->size);
        m_partition = nullptr;
        esp32_ota_mutex_release();
        return ESP_FAIL;
    }

    // 统一传 total_size 给 esp_ota_begin（WiFi 风格）
    esp_err_t err = esp_ota_begin(m_partition, total_size, &m_handle);
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "esp_ota_begin 失败: %s", esp_err_to_name(err));
        m_partition = nullptr;
        m_handle = 0;
        esp32_ota_mutex_release();
        return err;
    }

    m_received = 0;
    m_active = true;
    m_shaComputed = false;
    m_ended = false;
    memset(m_sha256, 0, sizeof(m_sha256));

    DEBUG_INFO(TAG, "ESP32 OTA 开始，目标分区: %s, 声明大小: %u 字节",
               m_partition->label, (unsigned)total_size);
    return ESP_OK;
}

/**
  * @brief 写入固件数据块。
 * @param data 数据缓冲区。
 * @param len 数据长度。
 * @return ESP_OK 成功，否则错误码。
 */
esp_err_t Esp32OtaCore::write(const uint8_t *data, size_t len)
{
    if (!m_active || m_partition == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_ota_write(m_handle, data, len);
    if (err == ESP_OK) {
        m_received += len;
    } else {
        DEBUG_ERR(TAG, "esp_ota_write 失败: %s (len=%u)", esp_err_to_name(err), (unsigned)len);
    }
    return err;
}

/**
  * @brief 从 OTA 分区读回数据并计算 SHA-256。
 * @return ESP_OK 成功，否则错误码。
 */
esp_err_t Esp32OtaCore::computeShaFromPartition()
{
    if (m_partition == NULL || m_received == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    DEBUG_INFO(TAG, "从 OTA 分区读回计算 SHA-256 (%u 字节)...", (unsigned)m_received);

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    uint8_t *read_buf = (uint8_t *)malloc(OTA_BLOCK_SIZE);
    if (!read_buf) {
        DEBUG_ERR(TAG, "SHA 计算: malloc 失败");
        mbedtls_sha256_free(&ctx);
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0;
    while (offset < m_received) {
        size_t to_read = m_received - offset;
        if (to_read > OTA_BLOCK_SIZE) to_read = OTA_BLOCK_SIZE;
        esp_err_t rd_err = esp_partition_read(m_partition, offset, read_buf, to_read);
        if (rd_err != ESP_OK) {
            DEBUG_ERR(TAG, "SHA 计算: 分区读失败 @offset %u: %s",
                      (unsigned)offset, esp_err_to_name(rd_err));
            free(read_buf);
            mbedtls_sha256_free(&ctx);
            return rd_err;
        }
        mbedtls_sha256_update(&ctx, read_buf, to_read);
        offset += to_read;
    }

    free(read_buf);
    mbedtls_sha256_finish(&ctx, m_sha256);
    mbedtls_sha256_free(&ctx);
    m_shaComputed = true;

    char hex[65] = {0};
    for (int i = 0; i < 32; ++i) {
        snprintf(&hex[i * 2], sizeof(hex) - i * 2, "%02x", m_sha256[i]);
    }
    DEBUG_INFO(TAG, "设备端 SHA-256: %s", hex);
    return ESP_OK;
}

/**
  * @brief 计算设备端 SHA-256 并输出 hex 字符串。
 * @param out_device_sha_hex 输出缓冲区，至少 65 字节。
 * @return ESP_OK 成功，否则错误码。
 */
esp_err_t Esp32OtaCore::computeDeviceSha(char *out_device_sha_hex)
{
    if (!m_active || m_partition == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // 若已计算过，直接复用
    if (!m_shaComputed) {
        esp_err_t err = computeShaFromPartition();
        if (err != ESP_OK) return err;
    }
    if (out_device_sha_hex) {
        for (int i = 0; i < 32; ++i) {
            snprintf(&out_device_sha_hex[i * 2], 65 - i * 2, "%02x", m_sha256[i]);
        }
        out_device_sha_hex[64] = '\0';
    }
    return ESP_OK;
}

/**
  * @brief 收尾 OTA 会话：end、校验 SHA、set_boot、可选重启。
 * @param frontend_sha_hex 前端提供的 64 字符 hex SHA-256。
 * @param reboot 成功后是否重启。
 * @param out_device_sha_hex 输出设备端 SHA hex 缓冲区。
 * @return ESP_OK 成功，ESP_ERR_INVALID_VERSION SHA 不匹配，否则错误码。
 */
esp_err_t Esp32OtaCore::finalize(const char *frontend_sha_hex, bool reboot,
                                 char *out_device_sha_hex)
{
    if (!m_active) {
        DEBUG_ERR(TAG, "finalize: OTA 未激活");
        return ESP_ERR_INVALID_STATE;
    }

    // ── esp_ota_end ──
    DEBUG_INFO(TAG, "[FINALIZE] esp_ota_end...");
    esp_err_t err = esp_ota_end(m_handle);
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "esp_ota_end 失败: %s", esp_err_to_name(err));
        // end 失败时仍可 abort（未真正 end）
        esp_ota_abort(m_handle);
        m_ended = false;
        esp32_ota_mutex_release();
        resetState();
        return err;
    }
    m_ended = true;  // ★ end 成功，后续不得再 abort
    DEBUG_INFO(TAG, "[FINALIZE] esp_ota_end 完成");

    // ── SHA-256 校验（若前端提供）──
    if (frontend_sha_hex != nullptr && strlen(frontend_sha_hex) == 64) {
        esp_err_t sha_err = computeShaFromPartition();
        if (sha_err != ESP_OK) {
            DEBUG_ERR(TAG, "SHA 计算失败: %s，中止", esp_err_to_name(sha_err));
            esp32_ota_mutex_release();
            resetState();
            return sha_err;
        }

        char device_hex[65] = {0};
        for (int i = 0; i < 32; ++i) {
            snprintf(&device_hex[i * 2], sizeof(device_hex) - i * 2, "%02x", m_sha256[i]);
        }

        bool match = true;
        for (int i = 0; i < 64; i++) {
            if (tolower((unsigned char)frontend_sha_hex[i]) !=
                tolower((unsigned char)device_hex[i])) {
                match = false;
                break;
            }
        }

        if (out_device_sha_hex) {
            strncpy(out_device_sha_hex, device_hex, 64);
            out_device_sha_hex[64] = '\0';
        }

        if (!match) {
            DEBUG_ERR(TAG, "SHA-256 不匹配! frontend=%s device=%s，中止",
                      frontend_sha_hex, device_hex);
            esp32_ota_mutex_release();
            resetState();
            return ESP_ERR_INVALID_VERSION;  // 调用方据此返回错误给前端
        }
        DEBUG_INFO(TAG, "SHA-256 校验通过");
    } else if (out_device_sha_hex) {
        // 未提供前端 SHA，但调用方仍想拿到设备端 SHA
        if (computeShaFromPartition() == ESP_OK) {
            for (int i = 0; i < 32; ++i) {
                snprintf(&out_device_sha_hex[i * 2], 65 - i * 2, "%02x", m_sha256[i]);
            }
            out_device_sha_hex[64] = '\0';
        }
    }

    // ── 设置启动分区 ──
    DEBUG_INFO(TAG, "[FINALIZE] esp_ota_set_boot_partition...");
    err = esp_ota_set_boot_partition(m_partition);
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "esp_ota_set_boot_partition 失败: %s", esp_err_to_name(err));
        esp32_ota_mutex_release();
        resetState();
        return err;
    }
    DEBUG_INFO(TAG, "[FINALIZE] esp_ota_set_boot_partition 完成");

    esp32_ota_mutex_release();
    DEBUG_INFO(TAG, "ESP32 OTA 收尾完成，共 %u 字节", (unsigned)m_received);

    // 保持 m_active 直到重启或下次 begin；resetState 由 begin/abort 处理
    // （此处不 reset，以便 BLE finish task 在重启前可查询状态）

    if (reboot) {
        DEBUG_INFO(TAG, "[FINALIZE] 500ms 后重启...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        // 不会返回
    }
    return ESP_OK;
}

/**
  * @brief 中止 OTA 会话并清理资源。
 */
void Esp32OtaCore::abort()
{
    if (!m_active) {
        return;
    }
    if (!m_ended) {
        esp_ota_abort(m_handle);
    }
    esp32_ota_mutex_release();
    resetState();
    DEBUG_INFO(TAG, "ESP32 OTA 已中止并清理");
}
