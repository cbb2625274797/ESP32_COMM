/*
 * gd32_flash_store.h - GD32 OTA 固件 Flash 存储抽象层
 *
 * 封装 Flash 分区读写、NVS 元数据持久化和 SHA-256 校验。
 * 传输层（WiFi/BLE）和 OtaRelay 均通过本类操作 GD32 固件存储。
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
 * @brief gd32_ota 分区中存储镜像的身份分类（包级防呆）。
 *
 * 判定依据是镜像自身内容（Cortex-M 向量表前两个字），不依赖文件名：
 * SHA-256 只能证明"传对了"，不能证明"传的是对的包"。
 * 把 BOOT 包（或任意非 APP 包）中继进 GD32 APP 区会擦掉好固件，
 * 因此触发中继前必须确认镜像身份为 kApp。
 */
enum class Gd32ImageIdentity : uint8_t {
    kApp = 0,        ///< 复位向量落在 APP 区 → 合法 GD32 APP 固件
    kBootloader,     ///< 复位向量落在 BOOT 区（0x08000000~0x08011FFF）→ Bootloader 包，禁止刷入 APP
    kInvalid,        ///< 向量表非法 → 非 GD32 APP 固件（ESP32 镜像 / 文本 / 损坏数据）
    kReadFailed,     ///< 分区读取失败，无法判定
};

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
     * @brief 新一轮 GD32 固件写入会话开始时调用：复位擦除水位。
     * @param full_erase true=强制擦除整个分区（motor OTA 用，杜绝旧固件残留导致的位错误）；
     *                   false=只重置水位，由 flashWriteChunk 按需擦扇区（GD32 OTA 用，与收流交织）。
     */
    void beginWriteSession(bool full_erase = false);

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
     * @brief 读取 gd32_ota 分区镜像开头的 Cortex-M 向量表并判定包身份。
     *
     * 检查初始 MSP 是否落在 GD32E508 SRAM、复位向量是否落在 APP 区
     * （0x08048000~0x0807DFFB，与 BOOT 侧 validateApplication() 同一判据）。
     * 复位向量落在 BOOT 区（0x08000000~0x08011FFF）则判为 Bootloader 包。
     * 供中继触发前的包级防呆使用（WiFi /ota/relay 与 BLE 0xF7 共用）。
     * @return 镜像身份分类；调用方仅应在 kApp 时允许中继。
     */
    Gd32ImageIdentity validateStoredImageIdentity();

    /**
     * @brief 获取分区指针（懒初始化）。
     * @return 分区指针，找不到时返回 nullptr。
     */
    const esp_partition_t* getPartition();

    // ── OTA 期对齐缓冲生命周期 ──
    /** @brief GD32 OTA begin 时调用：预分配对齐缓冲（消除 flashWriteChunk 逐块 malloc）。 */
    static void acquireAlignBufForOta();
    /** @brief GD32 OTA end/abort 时调用：释放对齐缓冲（非 OTA 期零占用）。 */
    static void releaseAlignBufForOta();

private:
    Gd32FlashStore();
    ~Gd32FlashStore() = default;
    Gd32FlashStore(const Gd32FlashStore&) = delete;
    Gd32FlashStore& operator=(const Gd32FlashStore&) = delete;

    const esp_partition_t *m_partition = nullptr; ///< gd32_ota 分区指针（懒初始化）

    // ── 擦除水位：beginWriteSession 后从 0 起；
    //   flashWriteChunk → ensureErasedForWrite 按需推进（不再依赖 prepare 整区预擦）。
    static constexpr uint32_t kFlashSectorSize = 4096U;
    uint32_t m_erasedUpTo = 0;
    esp_err_t ensureErasedForWrite(uint32_t offset, size_t len);

    // NVS 互斥：lockNvs 返回 take 是否成功；unlockNvs(handle) 仅成功时 give
    SemaphoreHandle_t m_nvsMutex = nullptr;
    bool lockNvs(uint32_t timeout_ms);
    void unlockNvs(bool handle);
    static constexpr uint32_t kNvsLockTimeoutMs = 100; ///< NVS 加锁超时（ms）

    // 已持锁版本（调用方已持 m_nvsMutex）
    esp_err_t metaLoadLocked(ota_meta_t *meta);
    esp_err_t metaSaveLocked(const ota_meta_t *meta);
};

#endif // GD32_FLASH_STORE_H
