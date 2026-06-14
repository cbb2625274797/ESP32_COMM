/*
 * spi_master.h - SPI Master 驱动头文件
 * CANFD 3通道发送接口 + 底层 SPI 总线
 *
 * SpiMaster 类封装 SPI 总线初始化、CANFD 通信和 SPI OTA 协议。
 */
#ifndef SPI_MASTER_H
#define SPI_MASTER_H

#include <stdint.h>
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// SPI 配置常量
#define SPI_MASTER_CS_GPIO    (4)           // CS 片选引脚
#define SPI_CLOCK_HZ          (4 * 1000 * 1000)  // 4MHz
#define SPI_MODE              (0)           // CPOL=0, CPHA=0
#define SPI_QUEUE_SIZE        (1)
#define SPI_OTA_FRAME_MAX     (96)

// GD32 SPI OTA 帧类型
#define SPI_OTA_SOF           (0xA5)
#define SPI_OTA_CMD_REQ       (0x01)
#define SPI_OTA_CMD_DATA      (0x02)
#define SPI_OTA_CMD_CANCEL    (0x03)
#define SPI_APP_ENTER_BOOT_CMD (0xB0)
#define SPI_APP_ENTER_BOOT_ACK_OK (0xB5)  // GD32 APP 固件须同步修改 ACK 字节为 0xB5
#define SPI_OTA_RSP_ACK       (0x80)
#define SPI_OTA_RSP_WAIT      (0x81)
#define SPI_OTA_RSP_ERROR     (0x82)

// ============================================================
// CANFD 帧格式（3通道）
// [通道编号: 1B] [扩展帧ID: 4B BE] [数据: 1~64B]
// 长度自动识别，无需发送 DLC
// ============================================================
#define CAN_EXTENDED_ID_BITS   (29)          // CAN 扩展帧 ID 位数
#define CAN_MAX_DLC            (64)            // 最大数据长度
#define CAN_CHANNEL_MASK        (0x03)         // 通道号掩码 (0~2)

// 通道定义
typedef enum {
    CAN_CHANNEL_0 = 0,
    CAN_CHANNEL_1 = 1,
    CAN_CHANNEL_2 = 2,
    CAN_CHANNEL_MAX = 3,
} can_channel_t;

// CANFD 帧结构
typedef struct {
    can_channel_t channel;      // CAN 通道 (0~2)
    uint32_t      ext_id;      // 扩展帧 ID (29bit, Big-Endian 存储)
    uint8_t       data[CAN_MAX_DLC];  // CAN 数据负载 (最大 64 字节)
    uint8_t       dlc;         // 数据长度 (1~64)
} can_frame_t;

// GD32 版本信息（从 APP PING 响应解析）
typedef struct {
    uint16_t app_major;
    uint16_t app_minor;
    uint16_t app_patch;
    uint16_t app_build;
    uint16_t boot_major;
    uint16_t boot_minor;
    uint16_t boot_patch;
    uint16_t boot_build;
} gd32_version_t;

/**
 * @brief SPI Master 驱动类（单例）
 *
 * 封装 SPI 总线初始化、同步传输、CANFD 通信和 SPI OTA 协议。
 */
class SpiMaster {
public:
    /**
     * @brief 获取 SpiMaster 单例实例。
     * @return SpiMaster 单例引用。
     */
    static SpiMaster& instance();

    /** @brief 初始化 SPI2 总线与目标从设备。 */
    esp_err_t init();

    // ============================================================
    // 底层 SPI 传输
    // ============================================================

    /** @brief 执行一次同步 SPI 事务，先发送请求，再读取响应字节。 */
    esp_err_t syncTransfer(uint8_t cmd, const uint8_t *payload,
                           size_t payload_len, uint8_t *recv_buf,
                           size_t recv_len, uint32_t timeout_ms);

    /** @brief 仅发送 SPI 请求数据，不关心返回内容。 */
    esp_err_t send(uint8_t cmd, const uint8_t *payload, size_t len);

    /** @brief 仅接收 SPI 数据，必要时先发送命令字节。 */
    esp_err_t recv(uint8_t cmd, uint8_t *recv_buf, size_t len, uint32_t timeout_ms);

    // ============================================================
    // SPI OTA 协议
    // ============================================================

    /** @brief 按 GD32 SPI OTA 最小协议执行一次请求 + 响应读取。 */
    esp_err_t otaTransfer(uint8_t type, const uint8_t *payload, size_t payload_len,
                          uint8_t *resp_type, uint8_t *resp_payload,
                          size_t resp_max, size_t *resp_len, uint32_t timeout_ms);

    /** @brief 仅执行 SPI OTA 响应读取事务，不重复发送请求帧。 */
    esp_err_t otaPollResponse(uint8_t *resp_type, uint8_t *resp_payload,
                              size_t resp_max, size_t *resp_len, uint32_t timeout_ms);

    /** @brief 通知 GD32 DCU_APP 设置 OTA 标志并软复位进入 Bootloader。 */
    esp_err_t appRequestBootloader();

    /** @brief 向 GD32 APP 发送存活探测命令，等待 APP 回复 ALIVE ACK。 */
    esp_err_t appPing(uint32_t timeout_ms, uint32_t *uptime_sec, gd32_version_t *version = nullptr);

    /** @brief 执行带重试的 SPI 同步传输。 */
    esp_err_t transferWithRetry(uint8_t cmd, const uint8_t *payload,
                                size_t payload_len, uint8_t *recv_buf,
                                size_t recv_len, int max_retries);

    // ============================================================
    // CANFD 接口
    // ============================================================

    /** @brief 发送一帧 CANFD 数据，可选等待并读取响应内容。 */
    int canSend(can_channel_t channel, uint32_t ext_id,
                const uint8_t *data, uint8_t dlc,
                uint8_t *recv_buf, uint8_t recv_max,
                uint32_t timeout_ms);

    /** @brief 发送一帧无需响应的 CANFD 数据。 */
    int canSendNoReply(can_channel_t channel, uint32_t ext_id,
                       const uint8_t *data, uint8_t dlc);

    /** @brief 获取 SPI 设备句柄（供外部状态查询）。 */
    spi_device_handle_t deviceHandle() const { return m_spiDevice; }

private:
    /**
     * @brief 私有默认构造函数。
     */
    SpiMaster() = default;
    /**
     * @brief 私有析构函数。
     */
    ~SpiMaster() = default;
    /**
     * @brief 禁止复制构造。
     */
    SpiMaster(const SpiMaster&) = delete;
    /**
     * @brief 禁止复制赋值。
     */
    SpiMaster& operator=(const SpiMaster&) = delete;

    spi_device_handle_t m_spiDevice = NULL;
    SemaphoreHandle_t m_mutex = NULL;

    // RAII 锁守卫
    class LockGuard {
    public:
        /**
         * @brief 构造加锁守卫，获取互斥锁。
         * @param mutex 互斥锁句柄。
         */
        explicit LockGuard(SemaphoreHandle_t mutex) : m_lock(mutex) {
            if (m_lock) { xSemaphoreTake(m_lock, portMAX_DELAY); }
        }
        /**
         * @brief 析构解锁守卫，释放互斥锁。
         */
        ~LockGuard() {
            if (m_lock) { xSemaphoreGive(m_lock); }
        }
        /**
         * @brief 禁止复制构造。
         */
        LockGuard(const LockGuard&) = delete;
        /**
         * @brief 禁止复制赋值。
         */
        LockGuard& operator=(const LockGuard&) = delete;
    private:
        SemaphoreHandle_t m_lock;
    };

    // 内部 SPI 传输
    /**
     * @brief 执行底层 SPI 时钟同步传输。
     * @param tx_buf 发送数据缓冲区。
     * @param tx_len 发送数据长度。
     * @param rx_buf 接收数据缓冲区。
     * @param rx_len 接收数据长度。
     * @param timeout_ms 超时时间（毫秒），0 表示无限等待。
     * @return ESP_OK 表示成功，否则返回错误码。
     */
    esp_err_t transferClocked(const uint8_t *tx_buf, size_t tx_len,
                              uint8_t *rx_buf, size_t rx_len,
                              uint32_t timeout_ms = 0U);
    /**
     * @brief 执行 SPI 请求-响应组合传输。
     * @param cmd 命令字节（0 表示无命令）。
     * @param payload 负载数据缓冲区。
     * @param payload_len 负载数据长度。
     * @param rx_buf 接收数据缓冲区。
     * @param rx_len 期望接收的数据长度。
     * @param timeout_ms 超时时间（毫秒），0 表示无限等待。
     * @return ESP_OK 表示成功，否则返回错误码。
     */
    esp_err_t transferRequestResponse(uint8_t cmd, const uint8_t *payload, size_t payload_len,
                                      uint8_t *rx_buf, size_t rx_len,
                                      uint32_t timeout_ms = 0U);


    // 无锁内部版本（供已持锁的公共方法调用，避免递归死锁）
    /**
     * @brief 无锁版本的同步 SPI 传输（调用者须已持有锁）。
     * @param cmd 命令字节。
     * @param payload 负载数据缓冲区。
     * @param payload_len 负载数据长度。
     * @param recv_buf 接收数据缓冲区。
     * @param recv_len 接收数据长度。
     * @param timeout_ms 超时时间（毫秒）。
     * @return ESP_OK 表示成功，否则返回错误码。
     */
    esp_err_t syncTransferUnlocked(uint8_t cmd, const uint8_t *payload,
                                   size_t payload_len, uint8_t *recv_buf,
                                   size_t recv_len, uint32_t timeout_ms);
    /**
     * @brief 无锁版本的 SPI 发送（调用者须已持有锁）。
     * @param cmd 命令字节。
     * @param payload 负载数据缓冲区。
     * @param len 负载数据长度。
     * @return ESP_OK 表示成功，否则返回错误码。
     */
    esp_err_t sendUnlocked(uint8_t cmd, const uint8_t *payload, size_t len);
    /**
     * @brief 无锁版本的 OTA 响应轮询（调用者须已持有锁）。
     * @param resp_type 输出响应类型。
     * @param resp_payload 输出响应负载缓冲区。
     * @param resp_max 响应负载缓冲区最大长度。
     * @param resp_len 输出实际响应长度。
     * @param timeout_ms 超时时间（毫秒）。
     * @return ESP_OK 表示成功，否则返回错误码。
     */
    esp_err_t otaPollResponseUnlocked(uint8_t *resp_type, uint8_t *resp_payload,
                                      size_t resp_max, size_t *resp_len, uint32_t timeout_ms);
    // OTA 帧工具（静态，无状态）
    /**
     * @brief 计算 OTA 帧的 8 位 CRC 校验和。
     * @param data 数据缓冲区。
     * @param len 数据长度。
     * @return 计算得到的 CRC 值。
     */
    static uint8_t otaCrc8(const uint8_t *data, size_t len);
    /**
     * @brief 构建 OTA 协议帧。
     * @param type 帧类型。
     * @param payload 负载数据缓冲区。
     * @param payload_len 负载数据长度。
     * @param out 输出帧缓冲区。
     * @param out_len 输出实际帧长度。
     * @return true 表示构建成功，false 表示失败。
     */
    static bool otaBuildFrame(uint8_t type, const uint8_t *payload, size_t payload_len,
                              uint8_t *out, size_t *out_len);
    /**
     * @brief 解析 OTA 协议帧。
     * @param frame 帧数据缓冲区。
     * @param frame_len 帧数据长度。
     * @param type 输出解析得到的帧类型。
     * @param payload 输出指向负载数据的指针。
     * @param payload_len 输出负载数据长度。
     * @return true 表示解析成功，false 表示失败。
     */
    static bool otaParseFrame(const uint8_t *frame, size_t frame_len,
                              uint8_t *type, const uint8_t **payload, size_t *payload_len);
    /**
     * @brief 在数据缓冲区中搜索并解析 OTA 帧。
     * @param buf 数据缓冲区。
     * @param buf_len 数据缓冲区长度。
     * @param type 输出帧类型。
     * @param payload 输出指向负载数据的指针。
     * @param payload_len 输出负载数据长度。
     * @param frame_offset 输出帧在缓冲区中的偏移位置。
     * @return true 表示找到并成功解析，false 表示未找到。
     */
    static bool otaFindFrame(const uint8_t *buf, size_t buf_len,
                             uint8_t *type, const uint8_t **payload, size_t *payload_len,
                             size_t *frame_offset);
};

#endif // SPI_MASTER_H


