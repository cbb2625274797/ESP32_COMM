/*
 * gd32_flash_store.cpp - GD32 OTA 固件 Flash 存储抽象层实现
 *
 * 从 OtaRelay 剥离的 Flash 分区读写、NVS 元数据持久化和 SHA-256 校验。
 */
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/sha256.h"

#include "gd32_flash_store.h"
#include "monitor/debug_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ============================================================
// 单例
// ============================================================
Gd32FlashStore& Gd32FlashStore::instance()
{
    static Gd32FlashStore inst;
    return inst;
}

// 构造时创建 NVS 互斥锁。
//   单例静态局部变量在首次 instance() 调用时构造，此时 FreeRTOS 已就绪。
Gd32FlashStore::Gd32FlashStore()
{
    m_nvsMutex = xSemaphoreCreateMutex();
    if (!m_nvsMutex) {
        DEBUG_ERR("OTA", "Gd32FlashStore NVS mutex 创建失败");
    }
}

// ============================================================
// NVS 互斥锁辅助
// ============================================================
bool Gd32FlashStore::lockNvs(uint32_t timeout_ms)
{
    // 参考 OtaCoordinator::lock：返回 take 结果，由调用方传回 unlockNvs 决定是否 give。
    if (!m_nvsMutex) return false;
    return xSemaphoreTake(m_nvsMutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void Gd32FlashStore::unlockNvs(bool handle)
{
    // 仅在对应 lockNvs 成功 take 时才 give，避免误释放。
    if (handle && m_nvsMutex) {
        xSemaphoreGive(m_nvsMutex);
    }
}

// ============================================================
// 分区懒初始化
// ============================================================
const esp_partition_t* Gd32FlashStore::getPartition()
{
    if (!m_partition) {
        m_partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "gd32_ota");
        if (!m_partition) {
            DEBUG_ERR("OTA", "找不到 gd32_ota 分区");
        }
    }
    return m_partition;
}

// ============================================================
// Flash 分区读写
// ============================================================

/** @brief 向 gd32_ota 分区写入数据块，自动处理非 4 字节对齐的读写。 */
esp_err_t Gd32FlashStore::flashWriteChunk(uint32_t offset, const uint8_t *data, size_t len)
{
    const esp_partition_t *part = getPartition();
    if (!part) return ESP_ERR_NOT_FOUND;

    // 计算 head/tail padding，使写入地址和长度都对齐到 4 字节
    size_t head_pad = offset % 4;
    uint32_t aligned_offset = offset - head_pad;
    size_t total = head_pad + len;
    size_t aligned_len = (total + 3) & ~(size_t)3;

    if (aligned_offset + aligned_len > part->size) {
        DEBUG_ERR("OTA", "flashWriteChunk越界: offset=%" PRIu32 " len=%zu aligned_off=%" PRIu32 " aligned_len=%zu part=%" PRIu32,
                  offset, len, aligned_offset, aligned_len, part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    // 快速路径：offset 和 len 都对齐
    if (head_pad == 0 && len % 4 == 0) {
        return esp_partition_write(part, offset, data, len);
    }

    if (head_pad > 0 || len % 4 != 0) {
        DEBUG_WARN("OTA", "flashWriteChunk 非对齐: offset=%" PRIu32 " len=%zu head=%zu aligned_len=%zu",
                   offset, len, head_pad, aligned_len);
    }

    uint8_t *aligned_buf = (uint8_t *)malloc(aligned_len);
    if (!aligned_buf) {
        DEBUG_ERR("OTA", "flashWriteChunk malloc失败: %zu字节", aligned_len);
        return ESP_ERR_NO_MEM;
    }

    // 读回 head padding
    if (head_pad > 0) {
        esp_err_t err = esp_partition_read(part, aligned_offset, aligned_buf, head_pad);
        if (err != ESP_OK) {
            DEBUG_ERR("OTA", "flashWriteChunk读回head失败: offset=%" PRIu32 " head=%zu err=%s",
                      aligned_offset, head_pad, esp_err_to_name(err));
            free(aligned_buf);
            return err;
        }
    }

    // 拷贝实际数据
    memcpy(aligned_buf + head_pad, data, len);

    // 读回 tail padding
    size_t tail_pad = aligned_len - total;
    if (tail_pad > 0) {
        esp_err_t err = esp_partition_read(part, aligned_offset + total, aligned_buf + total, tail_pad);
        if (err != ESP_OK) {
            DEBUG_ERR("OTA", "flashWriteChunk读回tail失败: offset=%" PRIu32 " tail=%zu err=%s",
                      aligned_offset + total, tail_pad, esp_err_to_name(err));
            free(aligned_buf);
            return err;
        }
    }

    esp_err_t err = esp_partition_write(part, aligned_offset, aligned_buf, aligned_len);
    free(aligned_buf);
    return err;
}

/** @brief 从 gd32_ota 分区读取数据块。 */
esp_err_t Gd32FlashStore::flashReadChunk(uint32_t offset, uint8_t *data, size_t len)
{
    const esp_partition_t *part = getPartition();
    if (!part) return ESP_ERR_NOT_FOUND;
    return esp_partition_read(part, offset, data, len);
}

/** @brief 擦除 gd32_ota 分区指定范围。 */
esp_err_t Gd32FlashStore::erasePartition(uint32_t size)
{
    const esp_partition_t *part = getPartition();
    if (!part) return ESP_ERR_NOT_FOUND;
    if (size > part->size) {
        DEBUG_ERR("OTA", "erasePartition: size=%" PRIu32 " > partition=%" PRIu32, size, part->size);
        return ESP_ERR_INVALID_SIZE;
    }
    return esp_partition_erase_range(part, 0, size);
}

// ============================================================
// NVS 元数据持久化
// ============================================================

/** @brief 已持锁版本：从 NVS 加载 OTA 元数据，命名空间不存在时返回全零结构。 */
esp_err_t Gd32FlashStore::metaLoadLocked(ota_meta_t *meta)
{
    if (!meta) return ESP_ERR_INVALID_ARG;
    memset(meta, 0, sizeof(ota_meta_t));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_META_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // 区分 nvs_open 失败原因，不再一律当 OK。
        //   ESP_ERR_NVS_NOT_FOUND = 命名空间不存在（首次使用/被擦除），视为空元数据，保持兼容。
        //   其他错误（NVS 未初始化/分区丢失/损坏等）返回真实错误码，
        //   避免掩盖底层故障导致调用方误判为"空元数据"继续执行。
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return ESP_OK;
        }
        DEBUG_WARN("OTA", "metaLoad nvs_open 失败（非 NOT_FOUND）: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t val = 0;
    if (nvs_get_u32(handle, "total_size", &val) == ESP_OK) meta->total_size = val;
    if (nvs_get_u32(handle, "stored_size", &val) == ESP_OK) meta->stored_size = val;
    if (nvs_get_u32(handle, "last_seq", &val) == ESP_OK) meta->last_seq = val;

    size_t sha_len = sizeof(meta->checksum);
    if (nvs_get_blob(handle, "checksum", meta->checksum, &sha_len) == ESP_OK && sha_len == sizeof(meta->checksum)) {
    } else {
        memset(meta->checksum, 0, sizeof(meta->checksum));
    }

    nvs_close(handle);
    return ESP_OK;
}

/** @brief 从 NVS 加载 OTA 元数据（加锁包装）。 */
esp_err_t Gd32FlashStore::metaLoad(ota_meta_t *meta)
{
    if (!meta) return ESP_ERR_INVALID_ARG;
    bool locked = lockNvs(kNvsLockTimeoutMs);
    if (!locked) {
        // 加锁失败不能跳过锁直接访问 NVS，清零输出并返回超时
        memset(meta, 0, sizeof(ota_meta_t));
        DEBUG_WARN("OTA", "metaLoad 加锁超时");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = metaLoadLocked(meta);
    unlockNvs(true);
    return err;
}

/** @brief 已持锁版本：将 OTA 元数据写入 NVS。 */
esp_err_t Gd32FlashStore::metaSaveLocked(const ota_meta_t *meta)
{
    if (!meta) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_META_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "无法打开 NVS 写入: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u32(handle, "total_size", meta->total_size);
    nvs_set_u32(handle, "stored_size", meta->stored_size);
    nvs_set_u32(handle, "last_seq", meta->last_seq);
    nvs_set_blob(handle, "checksum", meta->checksum, sizeof(meta->checksum));
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "NVS commit 失败: %s", esp_err_to_name(err));
    }
    return err;
}

/** @brief 将 OTA 元数据写入 NVS（加锁包装）。 */
esp_err_t Gd32FlashStore::metaSave(const ota_meta_t *meta)
{
    if (!meta) return ESP_ERR_INVALID_ARG;
    bool locked = lockNvs(kNvsLockTimeoutMs);
    if (!locked) {
        DEBUG_WARN("OTA", "metaSave 加锁超时");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = metaSaveLocked(meta);
    unlockNvs(true);
    return err;
}

/**
 * @brief 传输过程中更新元数据的 stored_size 和 last_seq 字段。
 *        整个读-改-写过程持锁，避免并发 metaUpdate 丢失更新。
 *        内部调用已持锁的 metaLoadLocked/metaSaveLocked，不再经公开方法重复加锁。
 */
esp_err_t Gd32FlashStore::metaUpdate(uint32_t stored_size, uint32_t last_seq)
{
    bool locked = lockNvs(kNvsLockTimeoutMs);
    if (!locked) {
        DEBUG_WARN("OTA", "metaUpdate 加锁超时");
        return ESP_ERR_TIMEOUT;
    }

    ota_meta_t meta;
    esp_err_t err = metaLoadLocked(&meta);
    if (err == ESP_OK) {
        meta.stored_size = stored_size;
        meta.last_seq = last_seq;
        err = metaSaveLocked(&meta);
    }
    unlockNvs(true);
    return err;
}

/** @brief OTA 完成或中止时清除元数据（全零写入）。 */
esp_err_t Gd32FlashStore::metaClear()
{
    DEBUG_INFO("OTA", "清除 OTA 元数据");
    bool locked = lockNvs(kNvsLockTimeoutMs);
    if (!locked) {
        DEBUG_WARN("OTA", "metaClear 加锁超时");
        return ESP_ERR_TIMEOUT;
    }
    ota_meta_t meta = {};
    esp_err_t err = metaSaveLocked(&meta);
    unlockNvs(true);
    return err;
}

// ============================================================
// SHA-256 校验
// ============================================================

/** @brief 从 gd32_ota 分区读取固件并计算 SHA-256，输出 32 字节哈希。 */
esp_err_t Gd32FlashStore::computeFlashSha256(uint8_t out_sha256[32])
{
    if (!out_sha256) return ESP_ERR_INVALID_ARG;

    ota_meta_t meta;
    esp_err_t err = metaLoad(&meta);
    if (err != ESP_OK || meta.total_size == 0) {
        DEBUG_ERR("OTA", "computeSha256: 元数据无效");
        return ESP_ERR_INVALID_STATE;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }

    static constexpr size_t kChunkSize = 1024;
    uint8_t *buf = (uint8_t *)malloc(kChunkSize);
    if (!buf) {
        mbedtls_sha256_free(&ctx);
        return ESP_ERR_NO_MEM;
    }

    uint32_t remaining = meta.total_size;
    uint32_t offset = 0;
    while (remaining > 0) {
        size_t to_read = (remaining > kChunkSize) ? kChunkSize : remaining;
        err = flashReadChunk(offset, buf, to_read);
        if (err != ESP_OK) {
            DEBUG_ERR("OTA", "computeSha256: Flash 读取失败 offset=%" PRIu32, offset);
            free(buf);
            mbedtls_sha256_free(&ctx);
            return err;
        }
        if (mbedtls_sha256_update(&ctx, buf, to_read) != 0) {
            free(buf);
            mbedtls_sha256_free(&ctx);
            return ESP_FAIL;
        }
        offset += to_read;
        remaining -= to_read;
    }

    free(buf);
    int ret = mbedtls_sha256_finish(&ctx, out_sha256);
    mbedtls_sha256_free(&ctx);
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

/** @brief 固件接收完毕后计算 SHA-256 并写入 meta.checksum。 */
esp_err_t Gd32FlashStore::metaFinalizeChecksum()
{
    ota_meta_t meta;
    esp_err_t err = metaLoad(&meta);
    if (err != ESP_OK) return err;

    if (meta.total_size == 0 || meta.stored_size < meta.total_size) {
        DEBUG_ERR("OTA", "metaFinalize: 固件不完整 stored=%" PRIu32 " total=%" PRIu32,
                  meta.stored_size, meta.total_size);
        return ESP_ERR_INVALID_STATE;
    }

    err = computeFlashSha256(meta.checksum);
    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "metaFinalize: SHA-256 计算失败");
        return err;
    }

    char hex[65] = {0};
    for (int i = 0; i < 32; ++i)
        snprintf(&hex[i * 2], sizeof(hex) - i * 2, "%02X", meta.checksum[i]);
    DEBUG_INFO("OTA", "固件 SHA-256: %s", hex);

    return metaSave(&meta);
}

/** @brief 校验固件完整性：重新计算 Flash SHA-256 并与 meta.checksum 比对。 */
esp_err_t Gd32FlashStore::metaVerifyChecksum()
{
    ota_meta_t meta;
    esp_err_t err = metaLoad(&meta);
    if (err != ESP_OK) return err;

    // 检查 checksum 是否全零（未计算过）
    // 注意：返回 INVALID_STATE 而非 ESP_OK，由调用方决定是否容忍。
    // 历史上这里直接返回 OK 跳过校验，但 meta 被意外清零时会掩盖固件损坏。
    static const uint8_t kZeroSha[32] = {0};
    if (memcmp(meta.checksum, kZeroSha, sizeof(meta.checksum)) == 0) {
        DEBUG_WARN("OTA", "metaVerify: checksum 未计算，返回 INVALID_STATE（调用方决定容忍或失败）");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t computed[32] = {0};
    err = computeFlashSha256(computed);
    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "metaVerify: SHA-256 计算失败");
        return err;
    }

    if (memcmp(computed, meta.checksum, 32) != 0) {
        char hex_stored[65] = {0}, hex_computed[65] = {0};
        for (int i = 0; i < 32; ++i) {
            snprintf(&hex_stored[i * 2], sizeof(hex_stored) - i * 2, "%02X", meta.checksum[i]);
            snprintf(&hex_computed[i * 2], sizeof(hex_computed) - i * 2, "%02X", computed[i]);
        }
        DEBUG_ERR("OTA", "固件完整性校验失败!");
        DEBUG_ERR("OTA", "  存储 SHA-256: %s", hex_stored);
        DEBUG_ERR("OTA", "  计算 SHA-256: %s", hex_computed);
        return ESP_ERR_INVALID_CRC;
    }

    DEBUG_INFO("OTA", "固件完整性校验通过 (SHA-256 匹配)");
    return ESP_OK;
}

esp_err_t Gd32FlashStore::verifyFrontendSha(const uint8_t frontend_sha32[32],
                                            uint8_t out_device_sha[32])
{
    if (!frontend_sha32) return ESP_ERR_INVALID_ARG;

    // 1) 计算设备端 SHA 并写入 meta.checksum
    esp_err_t err = metaFinalizeChecksum();
    if (err != ESP_OK) return err;

    // 2) 读回 meta 拿到 checksum
    ota_meta_t meta;
    err = metaLoad(&meta);
    if (err != ESP_OK) return err;
    if (out_device_sha) memcpy(out_device_sha, meta.checksum, 32);

    // 3) 比对前端 SHA 与设备端 SHA
    if (memcmp(meta.checksum, frontend_sha32, 32) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}
