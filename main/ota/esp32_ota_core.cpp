/*
 * esp32_ota_core.cpp - ESP32 本机 OTA 核心流程实现（WiFi / BLE 通道共用）
 *
 * 设计依据 WiFi 原流程（wifi_ota.cpp 旧实现），修复已知 bug:
 *   - 旧 wifi_ota.cpp:625 在 esp_ota_end 成功后又调 esp_ota_abort → 用 m_ended 标志避免
 *   - SHA 统一从分区读回计算，不在传输过程中做增量 SHA
 */
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/sha256.h"

#include "esp32_ota_core.h"
#include "app_config.h"
#include "monitor/debug_log.h"

#define TAG "OTA_CORE"

// ============================================================
// 分区互斥锁
// ============================================================
SemaphoreHandle_t Esp32OtaCore::s_mutex = NULL;
volatile bool Esp32OtaCore::s_occupied = false;

void Esp32OtaCore::initMutex()
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        s_occupied = false;
    }
}

bool Esp32OtaCore::acquireMutex(uint32_t timeout_ms)
{
    if (!s_mutex) return false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    if (s_occupied) {
        xSemaphoreGive(s_mutex);
        return false;
    }
    s_occupied = true;
    xSemaphoreGive(s_mutex);
    return true;
}

void Esp32OtaCore::releaseMutex()
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_occupied = false;
    xSemaphoreGive(s_mutex);
}

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
  * @brief begin() 失败后的统一清理。
  *        仅释放 mutex + 复位状态（不调 esp_ota_abort，
  *        begin 失败时 m_handle 未成功创建，无需也不能 abort）。
  * @param code 返回给调用方的错误码。
  * @return 透传 code。
  */
esp_err_t Esp32OtaCore::beginCleanupAndReturn(esp_err_t code)
{
    releaseMutex();
    resetState();
    return code;
}

/**
  * @brief finalize() 失败后的统一清理。
  *        若 esp_ota_end 未成功（m_ended=false），调 esp_ota_abort 释放 handle；
  *        然后复位 m_ended、释放 mutex、复位状态，返回 code。
  *        与现有 abort() 的 m_ended 守卫语义一致，避免 end 成功后误调 abort。
  * @param code 返回给调用方的错误码。
  * @return 透传 code。
  */
esp_err_t Esp32OtaCore::finalizeCleanupAndReturn(esp_err_t code)
{
    if (!m_ended) {
        esp_ota_abort(m_handle);
    }
    m_ended = false;
    releaseMutex();
    resetState();
    return code;
}

/**
  * @brief 开启 OTA 会话。
 * @param total_size 固件总大小（字节）。
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 被占用，ESP_FAIL 其他错误。
 */
esp_err_t Esp32OtaCore::begin(size_t total_size)
{
    // ★ 全局互斥：必须在清理旧会话之前检查，
    //   否则 WiFi 的 begin() 会先释放 BLE 持有的锁，再自己获取，导致互斥失效
    if (!acquireMutex(1000)) {
        DEBUG_ERR(TAG, "ESP32 OTA 被其他通道占用，拒绝 begin");
        return ESP_ERR_INVALID_STATE;
    }

    // 若本核心已有未完成会话（同一通道的重试），先中止并释放全局互斥
    if (m_active) {
        DEBUG_WARN(TAG, "检测到未完成的 ESP32 OTA 会话，先中止旧会话");
        if (!m_ended) {
            esp_ota_abort(m_handle);
        }
        releaseMutex();
        resetState();
        // 重新获取全局互斥（刚刚释放了）
        if (!acquireMutex(1000)) {
            DEBUG_ERR(TAG, "ESP32 OTA 重新获取互斥失败");
            return ESP_ERR_INVALID_STATE;
        }
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
        return beginCleanupAndReturn(ESP_FAIL);
    }

    DEBUG_INFO(TAG, "OTA 目标分区: %s, size=%u, address=0x%lx",
               m_partition->label, m_partition->size, m_partition->address);

    if (total_size > m_partition->size) {
        DEBUG_ERR(TAG, "固件太大: %u > 分区大小: %u",
                  (unsigned)total_size, (unsigned)m_partition->size);
        return beginCleanupAndReturn(ESP_FAIL);
    }

    /* ★ 整分区预擦（非 SEQUENTIAL 边写边擦）：
     * 1) 扇区循环 erase + 喂狗/yield，避免 TASK_WDT 与 WiFi 饿死
     * 2) esp_ota_resume(erase_size≠SEQUENTIAL) → need_erase=false，写路径纯 write
     * 3) 显式 invalidate inactive slot（resume 不像 begin 那样自动做）
     * 预擦会在 begin 阶段占用数秒～数十秒，之后收流写 Flash 更快更稳。 */
    const size_t part_size = m_partition->size;
    const size_t sector = m_partition->erase_size ? m_partition->erase_size : 4096U;
    DEBUG_INFO(TAG, "整分区预擦: %s size=%u sector=%u free=%lu...",
               m_partition->label, (unsigned)part_size, (unsigned)sector,
               (unsigned long)esp_get_free_heap_size());
    const int64_t t0 = esp_timer_get_time();
    for (size_t off = 0; off < part_size; ) {
        size_t chunk = sector;
        if (off + chunk > part_size) {
            chunk = part_size - off;
        }
        esp_err_t erase_err = esp_partition_erase_range(m_partition, off, chunk);
        if (erase_err != ESP_OK) {
            DEBUG_ERR(TAG, "整分区预擦失败 @%u: %s",
                      (unsigned)off, esp_err_to_name(erase_err));
            return beginCleanupAndReturn(erase_err);
        }
        off += chunk;
        (void)esp_task_wdt_reset();
        taskYIELD();
    }
    const int64_t t1 = esp_timer_get_time();
    DEBUG_INFO(TAG, "整分区预擦完成 耗时 %lld ms",
               (long long)((t1 - t0) / 1000));

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) || defined(CONFIG_APP_ROLLBACK_ENABLE)
    {
        esp_err_t inv = esp_ota_invalidate_inactive_ota_data_slot();
        if (inv != ESP_OK && inv != ESP_ERR_NOT_SUPPORTED) {
            DEBUG_WARN(TAG, "invalidate inactive OTA slot: %s（继续）",
                       esp_err_to_name(inv));
        }
    }
#endif

    /* erase_size 传 0（≠ OTA_WITH_SEQUENTIAL_WRITES）→ 写时不再擦扇区 */
    esp_err_t err = esp_ota_resume(m_partition, /*erase_size=*/0,
                                   /*image_offset=*/0, &m_handle);
    const int64_t t2 = esp_timer_get_time();
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "esp_ota_resume 失败: %s (耗时 %lld ms)",
                  esp_err_to_name(err), (long long)((t2 - t1) / 1000));
        return beginCleanupAndReturn(err);
    }
    DEBUG_INFO(TAG, "esp_ota_resume 完成 耗时 %lld ms free=%lu",
               (long long)((t2 - t1) / 1000),
               (unsigned long)esp_get_free_heap_size());

    m_received = 0;
    m_active = true;
    m_shaComputed = false;
    m_ended = false;
    memset(m_sha256, 0, sizeof(m_sha256));

    DEBUG_INFO(TAG, "ESP32 OTA 开始，目标分区: %s, 声明大小: %u 字节 (full-partition erase)",
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

    /* 整分区已预擦：此处为纯 write，不再扇区擦除。 */
    (void)esp_task_wdt_reset();
    esp_err_t err = esp_ota_write(m_handle, data, len);
    if (err == ESP_OK) {
        m_received += len;
    } else {
        DEBUG_ERR(TAG, "esp_ota_write 失败: %s (len=%u offset=%u free=%lu)",
                  esp_err_to_name(err), (unsigned)len, (unsigned)m_received,
                  (unsigned long)esp_get_free_heap_size());
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

    /* static context + 小读缓冲：勿占 httpd 栈，也勿再静态吃 4KB BSS */
    static mbedtls_sha256_context s_sha_ctx;
    static uint8_t s_sha_read_buf[1024];

    mbedtls_sha256_init(&s_sha_ctx);
    mbedtls_sha256_starts(&s_sha_ctx, 0);

    size_t offset = 0;
    uint32_t wdt_chunk = 0;
    while (offset < m_received) {
        size_t to_read = m_received - offset;
        if (to_read > sizeof(s_sha_read_buf)) to_read = sizeof(s_sha_read_buf);
        esp_err_t rd_err = esp_partition_read(m_partition, offset, s_sha_read_buf, to_read);
        if (rd_err != ESP_OK) {
            DEBUG_ERR(TAG, "SHA 计算: 分区读失败 @offset %u: %s",
                      (unsigned)offset, esp_err_to_name(rd_err));
            mbedtls_sha256_free(&s_sha_ctx);
            return rd_err;
        }
        mbedtls_sha256_update(&s_sha_ctx, s_sha_read_buf, to_read);
        offset += to_read;
        /* 长固件 SHA 可能 > TASK_WDT(5s)；在 httpd 上跑时必须喂狗 */
        if ((++wdt_chunk & 0x0FU) == 0U) {
            (void)esp_task_wdt_reset();
            taskYIELD();
        }
    }

    mbedtls_sha256_finish(&s_sha_ctx, m_sha256);
    mbedtls_sha256_free(&s_sha_ctx);
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
  * @brief 收尾 OTA 会话：校验 SHA、end、set_boot、可选重启。
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

    // ── 先读回分区计算 SHA-256，并与前端文件 SHA 比对 ──
    if (frontend_sha_hex != nullptr && strlen(frontend_sha_hex) == 64) {
        DEBUG_INFO(TAG, "ESP32 OTA 校验 SHA-256...");
        esp_err_t sha_err = computeShaFromPartition();
        if (sha_err != ESP_OK) {
            DEBUG_ERR(TAG, "SHA 计算失败: %s，中止", esp_err_to_name(sha_err));
            return finalizeCleanupAndReturn(sha_err);
        }

        char device_hex[65] = {0};
        for (int i = 0; i < 32; ++i) {
            snprintf(&device_hex[i * 2], sizeof(device_hex) - i * 2, "%02x", m_sha256[i]);
        }

        bool sha_match = true;
        for (int i = 0; i < 64; i++) {
            if (tolower((unsigned char)frontend_sha_hex[i]) !=
                tolower((unsigned char)device_hex[i])) {
                sha_match = false;
                break;
            }
        }

        if (out_device_sha_hex) {
            strncpy(out_device_sha_hex, device_hex, 64);
            out_device_sha_hex[64] = '\0';
        }

        if (sha_match) {
            DEBUG_INFO(TAG, "SHA-256 校验通过");
        } else {
            DEBUG_ERR(TAG, "SHA-256 不匹配: frontend=%s device=%s",
                      frontend_sha_hex, device_hex);
            return finalizeCleanupAndReturn(ESP_ERR_INVALID_VERSION);
        }
    } else if (out_device_sha_hex) {
        // 未提供前端 SHA，但仍计算设备端 SHA 供输出
        if (computeShaFromPartition() == ESP_OK) {
            for (int i = 0; i < 32; ++i) {
                snprintf(&out_device_sha_hex[i * 2], 65 - i * 2, "%02x", m_sha256[i]);
            }
            out_device_sha_hex[64] = '\0';
        }
    }

    // ── esp_ota_end ──
    esp_err_t err = esp_ota_end(m_handle);
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "esp_ota_end 失败: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        // end 失败时仍可 abort（未真正 end）
        return finalizeCleanupAndReturn(err);
    }
    m_ended = true;  // ★ end 成功，后续不得再 abort

    // ── 设置启动分区 ──
    err = esp_ota_set_boot_partition(m_partition);
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "esp_ota_set_boot_partition 失败: %s", esp_err_to_name(err));
        releaseMutex();
        resetState();
        return err;
    }

    releaseMutex();
    DEBUG_INFO(TAG, "ESP32 OTA 收尾完成，共 %u 字节", (unsigned)m_received);

    // 保持 m_active 直到重启或下次 begin；resetState 由 begin/abort 处理
    // （此处不 reset，以便 BLE finish task 在重启前可查询状态）

    if (reboot) {
        DEBUG_INFO(TAG, "500ms 后重启 ESP32");
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
    releaseMutex();
    resetState();
    DEBUG_INFO(TAG, "ESP32 OTA 已中止并清理");
}

/**
  * @brief 强制退出 OTA 状态机并释放互斥锁。
  *
  * 用于新通道（WiFi/BLE）发现锁被残留占用时，主动走完整的状态机退出流程：
  *   1. 若会话活跃（m_active）→ 走 abort() 完整清理（esp_ota_abort + 释放锁 + 复位状态）
  *   2. 若会话不活跃但锁被占（s_occupied）→ 仅释放残留锁
  *   3. 否则无需操作
  *
  * @return true 执行了清理（调用方可重试 begin），false 无需清理。
  */
bool Esp32OtaCore::forceCleanup()
{
    if (m_active) {
        // 会话仍活跃（前一个通道未正常结束），走完整 abort 流程
        DEBUG_WARN(TAG, "强制清理: 中止活跃的 OTA 会话 (received=%u)", (unsigned)m_received);
        abort();
        return true;
    }

    // 会话不活跃，但锁可能被残留占用
    if (!s_mutex) return false;

    // 在互斥保护下检查 s_occupied
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;

    bool was_occupied = s_occupied;
    if (was_occupied) {
        s_occupied = false;
    }
    xSemaphoreGive(s_mutex);

    if (was_occupied) {
        DEBUG_WARN(TAG, "强制清理: 释放残留 OTA 锁（无活跃会话）");
        return true;
    }

    return false;
}
