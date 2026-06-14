/*
 * ota_relay.h - OTA 中继核心状态机
 * T2.3, T3.1 - T3.7
 *
 * OtaRelay 类封装 OTA 中继状态机、Flash 存储和元数据管理。
 */
#ifndef OTA_RELAY_H
#define OTA_RELAY_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "transport/protocol.h"
#include "transport/spi_master.h"
#include "esp_partition.h"

#define OTA_META_NVS_NAMESPACE "ota_meta"

typedef struct {
    uint32_t total_size;
    uint32_t stored_size;
    uint32_t last_seq;
    uint8_t  checksum[32];
} ota_meta_t;

typedef enum {
    RELAY_IDLE = 0,
    RELAY_HANDSHAKE_SENT,
    RELAY_CHALLENGE_SENT,
    RELAY_CHALLENGE_RESPOND,
    RELAY_SESSION_KEY_SENT,
    RELAY_AUTH_COMPLETE,
    RELAY_SIZE_SENT,
    RELAY_SIZE_ACK,
    RELAY_RELAY_ACTIVE,
    RELAY_PACKET_SENT,
    RELAY_PACKET_ACK_OK,
    RELAY_PACKET_ACK_RETRY,
    RELAY_RECEIVE_DONE,
    RELAY_FINISH_SENT,
    RELAY_DONE,
    RELAY_APP_VERIFY,
    RELAY_ERROR = 0xFF
} relay_state_t;

typedef enum {
    OTA_RESULT_OK = 0,
    OTA_RESULT_ERR_TIMEOUT = -1,
    OTA_RESULT_ERR_CRC = -2,
    OTA_RESULT_ERR_AUTH = -3,
    OTA_RESULT_ERR_STORAGE = -4,
    OTA_RESULT_ERR_GD32 = -5,
    OTA_RESULT_ERR_HEALTH = -6,
    OTA_RESULT_ERR_USER_ABORT = -7
} ota_result_t;

class OtaRelay {
public:
    /**
      * @brief 获取OtaRelay单例引用.
     * @return OtaRelay单例引用.
     */
    static OtaRelay& instance();

    /**
      * @brief OTA中继FreeRTOS任务入口.
     * @param pvParameters FreeRTOS任务参数.
     */
    static void taskEntry(void *pvParameters);
    /**
      * @brief 从Flash向GD32转发固件（主流程）.
     * @return 操作结果码.
     */
    ota_result_t forwardFirmware();

    /* ---- 线程安全的状态访问接口 ----
     * OTA 任务写、HTTP 任务读，通过 mutex 保证一致性。
     * 状态机内部（OTA 任务）可直接写 static 成员，
     * 跨任务读/写必须走这些 API。
     */
    /**
      * @brief 获取当前中继状态.
     * @return 当前中继状态.
     */
    static relay_state_t getState();
    /**
      * @brief 获取中继当前序号.
     * @return 当前序号值.
     */
    static uint32_t getRelaySeq();
    /**
      * @brief 获取中继总包数.
     * @return 总包数.
     */
    static uint32_t getRelayTotalPackets();
    /**
      * @brief 获取GD32 APP存活标志位.
     * @return true 存活，false 未存活.
     */
    static bool getGd32AppAlive();
    /**
      * @brief 获取GD32固件版本.
     * @param out 输出参数，版本结构体指针.
     */
    static void getGd32Version(gd32_version_t *out);
    /**
      * @brief 设置中继状态.
     * @param s 目标状态值.
     */
    static void setState(relay_state_t s);
    /**
      * @brief 设置中继当前序号.
     * @param seq 序号值.
     */
    static void setRelaySeq(uint32_t seq);
    /**
      * @brief 设置中继总包数.
     * @param total 总包数.
     */
    static void setRelayTotalPackets(uint32_t total);
    /**
      * @brief 设置GD32 APP存活标志位.
     * @param alive 存活状态.
     */
    static void setGd32AppAlive(bool alive);
    /**
      * @brief 获取GD32固件版本.
     * @param ver 待设置的版本结构体引用.
     */
    static void setGd32Version(const gd32_version_t &ver);

    /**
      * @brief 重置状态机.
     */
    void reset();

    /**
      * @brief Flash写入接口（支持非4字节对齐写入）.
     * @param offset 分区内写入偏移.
     * @param data 待写入的数据指针.
     * @param len 数据长度（字节）.
     * @return ESP_OK 成功，其他失败.
     */
    static esp_err_t flashWriteChunk(uint32_t offset, const uint8_t *data, size_t len);
    /**
      * @brief 从OTA缓存分区读取数据块.
     * @param offset 分区内读取偏移.
     * @param data 读取缓冲区指针.
     * @param len 读取长度（字节）.
     * @return ESP_OK 成功，其他失败.
     */
    static esp_err_t flashReadChunk(uint32_t offset, uint8_t *data, size_t len);
    /**
      * @brief 从Flash加载元数据.
     * @param meta 输出参数，加载后的元数据结构体指针.
     * @return ESP_OK 成功，其他失败.
     */
    static esp_err_t metaLoad(ota_meta_t *meta);
    /**
      * @brief 保存元数据到Flash.
     * @param meta 待保存的元数据结构体指针.
     * @return ESP_OK 成功，其他失败.
     */
    static esp_err_t metaSave(const ota_meta_t *meta);
    /**
      * @brief 传输过程中更新元数据.
     * @param stored_size 已存储的固件大小（字节）.
     * @param last_seq 最后成功的分块序号.
     * @return ESP_OK 成功，其他失败.
     */
    static esp_err_t metaUpdate(uint32_t stored_size, uint32_t last_seq);
    /**
      * @brief OTA完成或中止时清除元数据.
     * @return ESP_OK 成功，其他失败.
     */
    static esp_err_t metaClear();
    /**
      * @brief 计算gd32_ota分区中已存储固件的SHA-256.
     * @param out_sha256 输出32字节SHA-256摘要.
     * @return ESP_OK 成功，其他失败.
     */
    static esp_err_t computeFlashSha256(uint8_t out_sha256[32]);
    /**
      * @brief 固件接收完毕后计算SHA-256并写入元数据.
     * @return ESP_OK 成功，其他失败.
     */
    static esp_err_t metaFinalizeChecksum();
    /**
      * @brief 校验固件完整性（SHA-256）.
     * @return ESP_OK 校验通过，ESP_ERR_INVALID_CRC 校验失败.
     */
    static esp_err_t metaVerifyChecksum();

    // 公开状态（状态机内部直接访问；跨任务请用 getter/setter）
    static relay_state_t relayState;
    static uint32_t relaySeq;
    static uint32_t relayTotalPackets;
    static volatile bool gd32AppAlive;
    static gd32_version_t gd32Version;
    static QueueHandle_t dispatchQueue;
    static QueueHandle_t resultQueue;

    // 线程安全锁
    static SemaphoreHandle_t s_stateMutex;
    /**
      * @brief 初始化状态互斥锁.
     */
    static void initMutex();
    static constexpr TickType_t kLockTimeout = pdMS_TO_TICKS(50);

    class StateLockGuard {
    public:
        /**
          * @brief 构造时获取互斥锁.
         * @param timeout 等待超时（时钟节拍），默认50ms.
         */
        explicit StateLockGuard(TickType_t timeout = kLockTimeout) {
            m_locked = (s_stateMutex && xSemaphoreTake(s_stateMutex, timeout) == pdTRUE);
        }
        /**
          * @brief 析构时释放互斥锁.
         */
        ~StateLockGuard() {
            if (m_locked && s_stateMutex) { xSemaphoreGive(s_stateMutex); }
        }
        /**
          * @brief 检查是否成功获取锁.
         * @return true 已锁定，false 未锁定.
         */
        bool isLocked() const { return m_locked; }
        StateLockGuard(const StateLockGuard&) = delete;
        StateLockGuard& operator=(const StateLockGuard&) = delete;
    private:
        bool m_locked = false;
    };

    // 实例状态（public，供状态机方法访问）
    const esp_partition_t *m_gd32Partition = NULL;
    session_ctx_t m_sessionCtx;
    uint8_t m_nonceServer[16] = {0};
    uint8_t m_kSession[16] = {0};
    uint8_t m_gd32UidRuntime[12] = {0};

    static constexpr size_t GD32_UID_LEN = 12;
    static constexpr size_t GD32_CHALLENGE_RESPONSE_LEN = 36;

    /**
      * @brief 请求GD32 APP进入Bootloader.
     * @return 操作结果码.
     */
    esp_err_t requestGd32AppEnterBootloader();
    /**
      * @brief 发送握手请求.
     * @return 操作结果码.
     */
    ota_result_t sendHandshakeReq();
    /**
      * @brief 发送随机数并与GD32相互认证.
     * @return 操作结果码.
     */
    ota_result_t sendNonceAndAuthenticate();
    /**
      * @brief 向GD32发送固件总大小.
     * @param size 固件总字节数.
     * @return 操作结果码.
     */
    ota_result_t sendFirmwareSize(uint32_t size);
    /**
      * @brief 从Flash向GD32转发固件包.
     * @param deadline_tick 截止时间戳（可选），0表示无超时.
     * @return 操作结果码.
     */
    ota_result_t relayPacketsFromFlash(TickType_t deadline_tick = 0);
    /**
      * @brief 完成OTA并发送结束指令.
     * @return 操作结果码.
     */
    ota_result_t finishOta();

    /**
      * @brief 使用AES-128-ECB加密单个16字节块.
     * @param key 16字节密钥.
     * @param in 16字节输入数据.
     * @param out 16字节输出数据.
     * @return 0 成功，-1 失败.
     */
    static int aes128EcbEncryptBlockLocal(const uint8_t *key, const uint8_t *in, uint8_t *out);
    /**
      * @brief 记录响应载荷日志.
     * @param stage 阶段名称字符串.
     * @param resp_type 响应类型.
     * @param payload 载荷指针.
     * @param len 载荷长度.
     */
    static void logRespPayload(const char *stage, uint8_t resp_type, const uint8_t *payload, size_t len);
    /**
      * @brief 循环等待直到响应不再是WAIT状态.
     * @param stage 阶段名称字符串.
     * @param resp_type 响应类型输出.
     * @param resp_payload 响应载荷缓冲区.
     * @param resp_max 缓冲区最大长度.
     * @param resp_len 实际响应长度输出.
     * @param timeout_ms 单次轮询超时毫秒.
     * @param max_wait_rounds 最大WAIT轮次.
     * @return ESP_OK 成功，其他失败.
     */
    esp_err_t waitUntilNotWait(const char *stage, uint8_t *resp_type,
                               uint8_t *resp_payload, size_t resp_max,
                               size_t *resp_len, uint32_t timeout_ms,
                               uint32_t max_wait_rounds);

    /**
      * @brief 判断是否为带序号的结构化ACK.
     * @param payload 载荷指针.
     * @param len 载荷长度.
     * @param expected_seq 期望的序号.
     * @return true 匹配成功，false 不匹配.
     */
    static bool isStructuredAckWithSeq(const uint8_t *payload, size_t len, uint32_t expected_seq);
    /**
      * @brief 解析结构化错误帧.
     * @param payload 载荷指针.
     * @param len 载荷长度.
     * @param seq_out 输出错误序号.
     * @param err_out 输出错误码.
     * @return true 解析成功，false 解析失败.
     */
    static bool parseStructuredError(const uint8_t *payload, size_t len, uint32_t *seq_out, uint8_t *err_out);
    /**
      * @brief 判断是否为握手回显ACK.
     * @param payload 载荷指针.
     * @param len 载荷长度.
     * @return true 是握手回显ACK，false 不是.
     */
    static bool isHandshakeEchoAck(const uint8_t *payload, size_t len);
    /**
      * @brief 判断是否为挑战应答ACK.
     * @param payload 载荷指针.
     * @param len 载荷长度.
     * @return true 是挑战应答ACK，false 不是.
     */
    static bool isChallengePayloadAck(const uint8_t *payload, size_t len);
    /**
      * @brief 判断是否为普通确认ACK.
     * @param payload 载荷指针.
     * @param len 载荷长度.
     * @return true 是确认ACK，false 不是.
     */
    static bool isConfirmPlainAck(const uint8_t *payload, size_t len);
    /**
      * @brief 判断是否为大小序号0的ACK.
     * @param payload 载荷指针.
     * @param len 载荷长度.
     * @return true 是大小序号0ACK，false 不是.
     */
    static bool isSizeSeq0Ack(const uint8_t *payload, size_t len);
    /**
      * @brief 判断是否为普通的结束ACK.
     * @param payload 载荷指针.
     * @param len 载荷长度.
     * @return true 是结束ACK，false 不是.
     */
    static bool isFinishPlainAck(const uint8_t *payload, size_t len);

    /**
      * @brief 轮询直到收到预期的ACK形状.
     * @param stage 阶段名称字符串.
     * @param resp_type 响应类型输出.
     * @param resp_payload 响应载荷缓冲区.
     * @param resp_max 响应缓冲区最大长度.
     * @param resp_len 实际响应长度输出.
     * @param poll_timeout_ms 轮询超时毫秒.
     * @param wait_rounds WAIT状态最大轮询次数.
     * @param maxExtraPolls 额外轮询次数.
     * @param matcher 匹配回调函数.
     * @return ESP_OK 成功，其他失败.
     */
    static esp_err_t pollUntilExpectedAckShape(const char *stage,
                                               uint8_t *resp_type,
                                               uint8_t *resp_payload,
                                               size_t resp_max,
                                               size_t *resp_len,
                                               uint32_t poll_timeout_ms,
                                               uint32_t wait_rounds,
                                               uint32_t maxExtraPolls,
                                               bool (*matcher)(const uint8_t *, size_t));
    /**
      * @brief 解析并验证挑战应答载荷.
     * @param resp_payload 响应载荷指针.
     * @param resp_len 响应长度.
     * @param out_k_session 输出会话密钥.
     * @param out_uid 输出GD32 UID.
     * @return true 验证通过，false 验证失败.
     */
    static bool parseAndVerifyChallengePayload(const uint8_t *resp_payload, size_t resp_len,
                                               uint8_t *out_k_session, uint8_t *out_uid);
    /**
      * @brief 使用AES-128-ECB加密48字节数据.
     * @param key 16字节密钥.
     * @param in 48字节输入数据.
     * @param out 48字节输出数据.
     * @return true 加密成功，false 加密失败.
     */
    static bool aes128EcbEncrypt48Bytes(const uint8_t *key, const uint8_t *in, uint8_t *out);
    /**
      * @brief 使用密钥计算48字节数据的CRC32.
     * @param key 16字节密钥.
     * @param data48 48字节输入数据.
     * @param output 输出4字节CRC32结果.
     */
    static void calculateCrc32WithKey(const uint8_t *key, const uint8_t *data48, uint8_t *output);

private:
    OtaRelay() = default;
    ~OtaRelay() = default;
    OtaRelay(const OtaRelay&) = delete;
    OtaRelay& operator=(const OtaRelay&) = delete;
};

#endif // OTA_RELAY_H