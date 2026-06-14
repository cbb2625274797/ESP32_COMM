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
#define OTA_META_NVS_KEY "meta"

typedef struct {
    uint32_t total_size;
    uint32_t stored_size;
    uint32_t last_seq;
    uint32_t chunk_count;
    uint8_t  checksum[32];
} ota_meta_t;

typedef enum {
    RELAY_IDLE = 0,
    RELAY_HANDSHAKE_IDLE,
    RELAY_HANDSHAKE_SENT,
    RELAY_CHALLENGE_SENT,
    RELAY_CHALLENGE_RESPOND,
    RELAY_SESSION_KEY_SENT,
    RELAY_AUTH_COMPLETE,
    RELAY_SIZE_SENT,
    RELAY_SIZE_ACK,
    RELAY_RECEIVING,
    RELAY_RELAY_ACTIVE,
    RELAY_PACKET_SENT,
    RELAY_PACKET_ACK_OK,
    RELAY_PACKET_ACK_RETRY,
    RELAY_RECEIVE_DONE,
    RELAY_TRANSFER_DONE,
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
    static OtaRelay& instance();

    static void taskEntry(void *pvParameters);
    ota_result_t startHandshake();
    ota_result_t forwardFirmware();

    /* ---- 线程安全的状态访问接口 ----
     * OTA 任务写、HTTP 任务读，通过 mutex 保证一致性。
     * 状态机内部（OTA 任务）可直接写 static 成员，
     * 跨任务读/写必须走这些 API。
     */
    static relay_state_t getState();
    static uint32_t getRelaySeq();
    static uint32_t getRelayTotalPackets();
    static bool getGd32AppAlive();
    static void getGd32Version(gd32_version_t *out);
    static void setState(relay_state_t s);
    static void setRelaySeq(uint32_t seq);
    static void setRelayTotalPackets(uint32_t total);
    static void setGd32AppAlive(bool alive);
    static void setGd32Version(const gd32_version_t &ver);

    void reset();

    static esp_err_t flashWriteChunk(uint32_t offset, const uint8_t *data, size_t len);
    static esp_err_t flashReadChunk(uint32_t offset, uint8_t *data, size_t len);
    static esp_err_t metaLoad(ota_meta_t *meta);
    static esp_err_t metaSave(const ota_meta_t *meta);
    static esp_err_t metaUpdate(uint32_t stored_size, uint32_t last_seq);
    static esp_err_t metaClear();
    static esp_err_t computeFlashSha256(uint8_t out_sha256[32]);
    static esp_err_t metaFinalizeChecksum();
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
    static void initMutex();
    static constexpr TickType_t kLockTimeout = pdMS_TO_TICKS(50);

    class StateLockGuard {
    public:
        explicit StateLockGuard(TickType_t timeout = kLockTimeout) {
            m_locked = (s_stateMutex && xSemaphoreTake(s_stateMutex, timeout) == pdTRUE);
        }
        ~StateLockGuard() {
            if (m_locked && s_stateMutex) { xSemaphoreGive(s_stateMutex); }
        }
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

    esp_err_t requestGd32AppEnterBootloader();
    ota_result_t probeBootHandshake();
    ota_result_t sendHandshakeReq();
    ota_result_t sendNonceAndAuthenticate();
    ota_result_t sendFirmwareSize(uint32_t size);
    ota_result_t relayPacketsFromFlash(TickType_t deadline_tick = 0);
    ota_result_t finishOta();

    static int aes128EcbEncryptBlockLocal(const uint8_t *key, const uint8_t *in, uint8_t *out);
    static void logRespPayload(const char *stage, uint8_t resp_type, const uint8_t *payload, size_t len);
    esp_err_t waitUntilNotWait(const char *stage, uint8_t *resp_type,
                               uint8_t *resp_payload, size_t resp_max,
                               size_t *resp_len, uint32_t timeout_ms,
                               uint32_t max_wait_rounds);

    static bool isStructuredAckWithSeq(const uint8_t *payload, size_t len, uint32_t expected_seq);
    static bool parseStructuredError(const uint8_t *payload, size_t len, uint32_t *seq_out, uint8_t *err_out);
    static bool isHandshakeEchoAck(const uint8_t *payload, size_t len);
    static bool isChallengePayloadAck(const uint8_t *payload, size_t len);
    static bool isConfirmPlainAck(const uint8_t *payload, size_t len);
    static bool isSizeSeq0Ack(const uint8_t *payload, size_t len);
    static bool isFinishPlainAck(const uint8_t *payload, size_t len);
    static bool isTransferDoneAck(const uint8_t *payload, size_t len);
    static bool isStructuredAckWithPayload(const uint8_t *payload, size_t len);

    static esp_err_t pollUntilExpectedAckShape(const char *stage,
                                               uint8_t *resp_type,
                                               uint8_t *resp_payload,
                                               size_t resp_max,
                                               size_t *resp_len,
                                               uint32_t poll_timeout_ms,
                                               uint32_t wait_rounds,
                                               uint32_t maxExtraPolls,
                                               bool (*matcher)(const uint8_t *, size_t));
    static bool parseAndVerifyChallengePayload(const uint8_t *resp_payload, size_t resp_len,
                                               uint8_t *out_k_session, uint8_t *out_uid);
    static bool aes128EcbEncrypt48Bytes(const uint8_t *key, const uint8_t *in, uint8_t *out);
    static void calculateCrc32WithKey(const uint8_t *key, const uint8_t *data48, uint8_t *output);

private:
    OtaRelay() = default;
    ~OtaRelay() = default;
    OtaRelay(const OtaRelay&) = delete;
    OtaRelay& operator=(const OtaRelay&) = delete;
};

#endif // OTA_RELAY_H