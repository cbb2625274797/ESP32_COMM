/*
 * gd32_flash_store.h - GD32 OTA 固件 Flash 存储抽象层
 *
 * 从 OtaRelay 中剥离的 Flash 分区读写、NVS 元数据持久化和 SHA-256 校验职责。
 * 传输层（WiFi/BLE）和 OtaRelay 均通过本类操作 GD32 固件存储，
 * 不再由 OtaRelay 承担存储职责（解决 H1 职责超载）。
 *
 * 线程模型：
 *   - 通过 instance() 单例访问，NVS 元数据读写由内部 mutex 保护
 *   - metaLoad/metaSave/metaUpdate/metaClear 串行化，读-改-写原子
 *   - 分区指针懒初始化，首次使用时查找并缓存
 */
#ifndef GD32_FLASH_STORE_H
#define GD32_FLASH_STORE_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define OTA_META_NVS_NAMESPACE "ota_meta" ///< OTA 元数据 NVS 命名空间

/** @brief OTA 传输元数据结构，持久化到 NVS 用于断点续传和完整性校验。 */
typedef struct {
    uint32_t total_size;   ///< 固件总大小（字节）
    uint32_t stored_size;  ///< 已存储到 Flash 的大小（字节）
    uint32_t last_seq;     ///< 最后一次成功写入的包序号
    uint8_t  checksum[32]; ///< 固件 SHA-256 校验值
} ota_meta_t;

/**
 * @brief GD32 OTA 固件 Flash 存储抽象层（纯静态工具类）。
 *
 * 封装 gd32_ota 分区的读写、NVS 元数据持久化、SHA-256 校验。
 * 传输层通过 OtaCoordinator 或直接调用本类完成存储操作，
 * OtaRelay 仅负责 SPI 中继状态机，不再承担存储职责。
 */
class Gd32FlashStore {
public:
    /**
     * @brief 获取单例实例。
     * @return Gd32FlashStore 单例引用。
     */
    static Gd32FlashStore& instance();

    // ── Flash 分区读写 ──

    /**
     * @brief 向 gd32_ota 分区写入数据块（支持非 4 字节对齐写入）。
     * @param offset 分区内写入偏移（字节）。
     * @param data 待写入数据缓冲区。
     * @param len 数据长度（字节）。
     * @return ESP_OK 成功，其他错误码表示失败。
     */
    esp_err_t flashWriteChunk(uint32_t offset, const uint8_t *data, size_t len);

    /**
     * @brief 从 gd32_ota 分区读取数据块。
     * @param offset 分区内读取偏移（字节）。
     * @param data 输出数据缓冲区。
     * @param len 读取长度（字节）。
     * @return ESP_OK 成功，其他错误码表示失败。
     */
    esp_err_t flashReadChunk(uint32_t offset, uint8_t *data, size_t len);

    /**
     * @brief 擦除 gd32_ota 分区指定范围。
     * @param size 擦除大小（字节），从偏移 0 开始。
     * @return ESP_OK 成功，其他错误码表示失败。
     */
    esp_err_t erasePartition(uint32_t size);

    /**
     * @brief 新一轮 GD32 固件写入会话开始时调用：复位按需擦除水位。
     *        prepareGd32Storage 在开始收流前调用，避免整段预擦阻塞 HTTP/BLE 收包。
     */
    void beginWriteSession();

    // ── NVS 元数据持久化 ──

    /**
     * @brief 从 NVS 加载 OTA 元数据。
     * @param meta 输出元数据结构体指针（任何错误路径下都会被清零）。
     * @return ESP_OK 成功（含命名空间不存在的兼容路径，返回全零结构）；
     *         ESP_ERR_TIMEOUT 加锁超时；
     *         其他错误码 = nvs_open 真实失败（如 NVS 未初始化/损坏），不再一律当 OK。
     */
    esp_err_t metaLoad(ota_meta_t *meta);

    /**
     * @brief 将 OTA 元数据写入 NVS。
     * @param meta 待保存的元数据结构体指针。
     * @return ESP_OK 成功，NVS 写入失败时返回错误码。
     */
    esp_err_t metaSave(const ota_meta_t *meta);

    /**
     * @brief 传输过程中更新元数据的 stored_size 和 last_seq。
     * @param stored_size 已存储大小（字节）。
     * @param last_seq 最后成功写入的包序号。
     * @return ESP_OK 成功，其他错误码表示失败。
     */
    esp_err_t metaUpdate(uint32_t stored_size, uint32_t last_seq);

    /**
     * @brief OTA 完成或中止时清除元数据（全零写入）。
     * @return ESP_OK 成功。
     */
    esp_err_t metaClear();

    // ── SHA-256 校验 ──

    /**
     * @brief 计算 gd32_ota 分区中已存储固件的 SHA-256。
     * @param out_sha256 输出 32 字节 SHA-256 哈希。
     * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 元数据无效。
     */
    esp_err_t computeFlashSha256(uint8_t out_sha256[32]);

    /**
     * @brief 固件接收完毕后计算 SHA-256 并写入元数据。
     * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 固件不完整。
     */
    esp_err_t metaFinalizeChecksum();

    /**
     * @brief 校验固件完整性（重新计算 SHA-256 并与 meta.checksum 比对）。
     * @return ESP_OK 校验通过，ESP_ERR_INVALID_CRC 校验失败。
     */
    esp_err_t metaVerifyChecksum();

    /**
     * @brief 校验前端提供的 SHA-256 是否与设备端计算的一致。
     * @param frontend_sha32 前端 32 字节原始 SHA（非 hex）。
     * @param out_device_sha 可为 nullptr；非空时输出设备端 32 字节 SHA。
     * @return ESP_OK 匹配；ESP_ERR_INVALID_CRC 不匹配；其他 = 元数据/计算失败。
     */
    esp_err_t verifyFrontendSha(const uint8_t frontend_sha32[32],
                                uint8_t out_device_sha[32]);

    /**
     * @brief 获取分区指针（懒初始化）。
     * @return 分区指针，找不到时返回 nullptr。
     */
    const esp_partition_t* getPartition();

private:
    Gd32FlashStore();
    ~Gd32FlashStore() = default;
    Gd32FlashStore(const Gd32FlashStore&) = delete;
    Gd32FlashStore& operator=(const Gd32FlashStore&) = delete;

    const esp_partition_t *m_partition = nullptr; ///< gd32_ota 分区指针（懒初始化）

    // ── 按需擦除水位：已保证 [0, m_erasedUpTo) 为擦除态（扇区对齐）──
    //   收流时 flashWriteChunk 按写入范围推进擦除，替代上传前整段预擦。
    //   这样 HTTP 主线程可边收边擦写，浏览器进度不会在“100% 发完”后空等整段擦除。
    static constexpr uint32_t kFlashSectorSize = 4096U;
    uint32_t m_erasedUpTo = 0;
    esp_err_t ensureErasedForWrite(uint32_t offset, size_t len);

    // ── NVS 互斥锁（保护元数据读-改-写原子性）──
    //   lockNvs 返回是否成功 take；unlockNvs(handle) 据此决定是否 give，
    //   参考 OtaCoordinator::lock/unlock 模式（避免共享 bool 被并发覆盖导致死锁）。
    SemaphoreHandle_t m_nvsMutex = nullptr;
    bool lockNvs(uint32_t timeout_ms);
    void unlockNvs(bool handle);
    static constexpr uint32_t kNvsLockTimeoutMs = 100; ///< NVS 加锁超时（ms）

    // 已持锁版本：由持锁的公开方法或 metaUpdate 调用，不再自行加锁，避免递归 take 死锁
    esp_err_t metaLoadLocked(ota_meta_t *meta);
    esp_err_t metaSaveLocked(const ota_meta_t *meta);
};

#endif // GD32_FLASH_STORE_H
