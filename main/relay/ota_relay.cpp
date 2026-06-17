/*
 * ota_relay.cpp - OTA 中继事件驱动状态机实现
 *
 * 将原 17 状态"声明式赋值"重构为 7 状态事件驱动架构：
 *   - 每个协议阶段封装为一个状态 handler
 *   - processEvent() 根据当前状态分发事件
 *   - 状态转换显式通过 setState() 完成
 *   - forwardFirmware() 保留为便捷入口，内部驱动状态机循环
 *
 * 协议子步骤与加密/Flash 工具方法保持不变。
 */
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "aes/esp_aes.h"
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/sha256.h"

#include "ota_relay.h"
#include "spi_master.h"
#include "protocol.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"
#include "ota_coordinator.h"   // ★ 通知 OtaCoordinator 中继开始/完成

// ============================================================
// 静态成员定义
// ============================================================
relay_state_t OtaRelay::relayState = RELAY_IDLE;
uint32_t OtaRelay::relaySeq = 0;
uint32_t OtaRelay::relayTotalPackets = 0;
volatile bool OtaRelay::gd32AppAlive = false;
gd32_version_t OtaRelay::gd32Version = {};
QueueHandle_t OtaRelay::dispatchQueue = NULL;
QueueHandle_t OtaRelay::resultQueue = NULL;
SemaphoreHandle_t OtaRelay::s_stateMutex = NULL;

// ============================================================
// 状态表：将每个 relay_state_t 映射到对应的 handler
// ============================================================
const OtaRelay::RelayStateDef OtaRelay::s_stateTable[] = {
    { "IDLE",          &OtaRelay::handleIdle },
    { "HANDSHAKING",   &OtaRelay::handleHandshaking },
    { "SIZE_NEG",      &OtaRelay::handleSizeNeg },
    { "TRANSFERRING",  &OtaRelay::handleTransferring },
    { "FINISHING",     &OtaRelay::handleFinishing },
    { "DONE",          &OtaRelay::handleDone },
    // RELAY_ERROR = 0xFF, 索引特殊处理
};

// ============================================================
// 单例 & 线程安全
// ============================================================
OtaRelay& OtaRelay::instance()
{
    static OtaRelay inst;
    return inst;
}

void OtaRelay::initMutex()
{
    if (!s_stateMutex) {
        s_stateMutex = xSemaphoreCreateMutex();
    }
}

relay_state_t OtaRelay::getState()
{
    StateLockGuard lock;
    return relayState;
}

uint32_t OtaRelay::getRelaySeq()
{
    StateLockGuard lock;
    return relaySeq;
}

uint32_t OtaRelay::getRelayTotalPackets()
{
    StateLockGuard lock;
    return relayTotalPackets;
}

bool OtaRelay::getGd32AppAlive()
{
    StateLockGuard lock;
    return gd32AppAlive;
}

void OtaRelay::getGd32Version(gd32_version_t *out)
{
    if (!out) return;
    StateLockGuard lock;
    *out = gd32Version;
}

void OtaRelay::setState(relay_state_t s)
{
    StateLockGuard lock;
    relayState = s;
    DEBUG_DBG("OTA", "状态转换: → %s",
              (s < RELAY_ERROR) ? s_stateTable[s].name : "ERROR");
}

void OtaRelay::setRelaySeq(uint32_t seq)
{
    StateLockGuard lock;
    relaySeq = seq;
}

void OtaRelay::setRelayTotalPackets(uint32_t total)
{
    StateLockGuard lock;
    relayTotalPackets = total;
}

void OtaRelay::setGd32AppAlive(bool alive)
{
    StateLockGuard lock;
    gd32AppAlive = alive;
}

void OtaRelay::setGd32Version(const gd32_version_t &ver)
{
    StateLockGuard lock;
    gd32Version = ver;
}

// ============================================================
// 事件分发核心：processEvent()
// ============================================================
ota_result_t OtaRelay::processEvent(relay_event_t event, const uint8_t *payload, size_t len)
{
    relay_state_t cur = relayState;

    // ERROR 状态：仅接受 reset（通过 handleError 的 EVT_START → IDLE）
    if (cur == RELAY_ERROR) {
        return handleError(event, payload, len);
    }

    // DONE 状态：仅接受 EVT_START（重新开始）
    if (cur == RELAY_DONE) {
        return handleDone(event, payload, len);
    }

    // 查找状态表
    if (cur > RELAY_DONE) {
        DEBUG_ERR("OTA", "processEvent: 未知状态 %d", (int)cur);
        setState(RELAY_ERROR);
        return OTA_RESULT_ERR_GD32;
    }

    StateHandler handler = s_stateTable[cur].handler;
    if (!handler) {
        DEBUG_ERR("OTA", "processEvent: 状态 %s 无 handler", s_stateTable[cur].name);
        setState(RELAY_ERROR);
        return OTA_RESULT_ERR_GD32;
    }

    DEBUG_DBG("OTA", "processEvent: state=%s event=%d",
              s_stateTable[cur].name, (int)event);

    ota_result_t result = (this->*handler)(event, payload, len);
    return result;
}

// ============================================================
// IDLE 状态 handler
// ============================================================
ota_result_t OtaRelay::handleIdle(relay_event_t event, const uint8_t *payload, size_t len)
{
    (void)payload;
    (void)len;

    if (event == RELAY_EVT_START) {
        DEBUG_INFO("OTA", "=== 中继状态机启动: IDLE → HANDSHAKING ===");
        reset();
        setState(RELAY_HANDSHAKING);
        return OTA_RESULT_OK;
    }

    DEBUG_WARN("OTA", "IDLE 忽略事件: %d", (int)event);
    return OTA_RESULT_OK;
}

// ============================================================
// HANDSHAKING 状态 handler（阶段1: 握手 + 挑战认证）
// ============================================================
ota_result_t OtaRelay::handleHandshaking(relay_event_t event, const uint8_t *payload, size_t len)
{
    (void)payload;
    (void)len;

    if (event == RELAY_EVT_ABORT) {
        DEBUG_ERR("OTA", "HANDSHAKING 收到 ABORT");
        setState(RELAY_ERROR);
        return OTA_RESULT_ERR_USER_ABORT;
    }

    if (event == RELAY_EVT_TIMEOUT) {
        DEBUG_ERR("OTA", "HANDSHAKING 超时");
        setState(RELAY_ERROR);
        return OTA_RESULT_ERR_TIMEOUT;
    }

    if (event != RELAY_EVT_START && event != RELAY_EVT_RESP_OK) {
        DEBUG_WARN("OTA", "HANDSHAKING 忽略事件: %d", (int)event);
        return OTA_RESULT_OK;
    }

    // 进入 HANDSHAKING 阶段：执行完整的握手 + 认证流程
    DEBUG_INFO("OTA", "=== 阶段1: 握手与认证开始 ===");

    // 步骤1: 请求 GD32 进入 Bootloader
    esp_err_t err = requestGd32AppEnterBootloader();
    if (err != ESP_OK) {
        DEBUG_WARN("OTA", "GD32 进入 Bootloader 请求未确认，继续尝试握手");
    }

    // 步骤2: 发送握手请求
    ota_result_t res = doHandshakeReq();
    if (res != OTA_RESULT_OK) {
        setState(RELAY_ERROR);
        return res;
    }

    // 步骤3: 挑战认证
    res = doChallengeAndAuth();
    if (res != OTA_RESULT_OK) {
        setState(RELAY_ERROR);
        return res;
    }

    // 认证完成 → 进入大小协商
    setState(RELAY_SIZE_NEG);
    DEBUG_INFO("OTA", "=== 阶段1 认证完成: 安全通道已建立, → SIZE_NEG ===");
    return OTA_RESULT_OK;
}

// ============================================================
// SIZE_NEG 状态 handler（阶段2: 固件大小协商）
// ============================================================
ota_result_t OtaRelay::handleSizeNeg(relay_event_t event, const uint8_t *payload, size_t len)
{
    (void)payload;
    (void)len;

    if (event == RELAY_EVT_ABORT) {
        DEBUG_ERR("OTA", "SIZE_NEG 收到 ABORT");
        setState(RELAY_ERROR);
        return OTA_RESULT_ERR_USER_ABORT;
    }

    if (event == RELAY_EVT_TIMEOUT) {
        DEBUG_ERR("OTA", "SIZE_NEG 超时");
        setState(RELAY_ERROR);
        return OTA_RESULT_ERR_TIMEOUT;
    }

    // 加载元数据获取固件大小
    ota_meta_t meta;
    if (OtaRelay::metaLoad(&meta) != ESP_OK || meta.total_size == 0) {
        DEBUG_ERR("OTA", "SIZE_NEG: 元数据无效");
        setState(RELAY_ERROR);
        return OTA_RESULT_ERR_STORAGE;
    }

    DEBUG_INFO("OTA", "=== 阶段2: 固件大小协商 (%" PRIu32 " 字节) ===", meta.total_size);

    ota_result_t res = doSendFirmwareSize(meta.total_size);
    if (res != OTA_RESULT_OK) {
        setState(RELAY_ERROR);
        return res;
    }

    // 初始化传输进度
    m_transferTotalPackets = (meta.total_size + GD32_OTA_PACKET_SIZE - 1U) / GD32_OTA_PACKET_SIZE;
    m_transferCurrentSeq = 0;
    m_transferFlashOffset = 0;
    m_transferRetryCount = 0;
    relayTotalPackets = m_transferTotalPackets;
    relaySeq = 0;

    setState(RELAY_TRANSFERRING);
    DEBUG_INFO("OTA", "=== 阶段2 完成: 总包数=%" PRIu32 ", → TRANSFERRING ===", m_transferTotalPackets);
    return OTA_RESULT_OK;
}

// ============================================================
// TRANSFERRING 状态 handler（阶段3: 数据包传输）
// ============================================================
ota_result_t OtaRelay::handleTransferring(relay_event_t event, const uint8_t *payload, size_t len)
{
    (void)payload;
    (void)len;

    if (event == RELAY_EVT_ABORT) {
        DEBUG_ERR("OTA", "TRANSFERRING 收到 ABORT");
        setState(RELAY_ERROR);
        return OTA_RESULT_ERR_USER_ABORT;
    }

    if (event == RELAY_EVT_HEALTH_CRIT) {
        DEBUG_ERR("OTA", "TRANSFERRING 健康检查危险，中止");
        setState(RELAY_ERROR);
        return OTA_RESULT_ERR_HEALTH;
    }

    DEBUG_INFO("OTA", "=== 阶段3: 数据包传输 (%" PRIu32 " 包) ===", m_transferTotalPackets);

    ota_result_t res = doRelayPackets(m_transferDeadline);
    if (res != OTA_RESULT_OK) {
        setState(RELAY_ERROR);
        return res;
    }

    setState(RELAY_FINISHING);
    DEBUG_INFO("OTA", "=== 阶段3 完成: → FINISHING ===");
    return OTA_RESULT_OK;
}

// ============================================================
// FINISHING 状态 handler（阶段4-5: 结束 + APP 验证）
// ============================================================
ota_result_t OtaRelay::handleFinishing(relay_event_t event, const uint8_t *payload, size_t len)
{
    (void)payload;
    (void)len;

    if (event == RELAY_EVT_ABORT) {
        DEBUG_ERR("OTA", "FINISHING 收到 ABORT");
        setState(RELAY_ERROR);
        return OTA_RESULT_ERR_USER_ABORT;
    }

    if (event == RELAY_EVT_TIMEOUT) {
        DEBUG_ERR("OTA", "FINISHING 超时");
        setState(RELAY_ERROR);
        return OTA_RESULT_ERR_TIMEOUT;
    }

    DEBUG_INFO("OTA", "=== 阶段4-5: 结束OTA + APP验证 ===");

    ota_result_t res = doFinishAndVerify();
    if (res != OTA_RESULT_OK) {
        setState(RELAY_ERROR);
        return res;
    }

    setState(RELAY_DONE);
    DEBUG_INFO("OTA", "=== OTA 全流程完成: → DONE ===");
    return OTA_RESULT_OK;
}

// ============================================================
// DONE 状态 handler
// ============================================================
ota_result_t OtaRelay::handleDone(relay_event_t event, const uint8_t *payload, size_t len)
{
    (void)payload;
    (void)len;

    if (event == RELAY_EVT_START) {
        // 允许从 DONE 重新开始
        DEBUG_INFO("OTA", "DONE → 重新开始");
        setState(RELAY_IDLE);
        return OTA_RESULT_OK;
    }

    DEBUG_DBG("OTA", "DONE 忽略事件: %d", (int)event);
    return OTA_RESULT_OK;
}

// ============================================================
// ERROR 状态 handler
// ============================================================
ota_result_t OtaRelay::handleError(relay_event_t event, const uint8_t *payload, size_t len)
{
    (void)payload;
    (void)len;

    if (event == RELAY_EVT_START || event == RELAY_EVT_ABORT) {
        DEBUG_INFO("OTA", "ERROR → 复位到 IDLE");
        reset();
        return OTA_RESULT_OK;
    }

    DEBUG_DBG("OTA", "ERROR 忽略事件: %d", (int)event);
    return OTA_RESULT_ERR_GD32;
}

// ============================================================
// forwardFirmware() - 便捷入口，驱动状态机循环
// ============================================================
ota_result_t OtaRelay::forwardFirmware()
{
    ota_meta_t meta;
    if (OtaRelay::metaLoad(&meta) != ESP_OK) {
        return OTA_RESULT_ERR_STORAGE;
    }

    DEBUG_INFO("OTA", "开始固件转发: %" PRIu32 " 字节", meta.total_size);

    // 固件完整性校验
    {
        esp_err_t verify_err = metaVerifyChecksum();
        if (verify_err != ESP_OK) {
            DEBUG_ERR("OTA", "固件完整性校验失败，中止转发");
            reset();
            return OTA_RESULT_ERR_CRC;
        }
    }

    // 全局超时计算
    constexpr uint32_t kMinGlobalTimeoutMs = 20U * 1000U;        // 20 s 最小超时
    constexpr uint32_t kMaxGlobalTimeoutMs = 60U * 1000U;        // 60 s 硬上限
    uint32_t dynamic_ms = kMinGlobalTimeoutMs + (meta.total_size / 1024U) * 50U;  // 50 ms / KB
    uint32_t global_timeout_ms = (dynamic_ms > kMaxGlobalTimeoutMs) ? kMaxGlobalTimeoutMs : dynamic_ms;
    TickType_t start_tick = xTaskGetTickCount();
    m_transferDeadline = start_tick + pdMS_TO_TICKS(global_timeout_ms);

    DEBUG_INFO("OTA", "全局超时: %" PRIu32 " ms, start_tick=%" PRIu32 ", deadline tick=%" PRIu32,
               global_timeout_ms, (uint32_t)start_tick, (uint32_t)m_transferDeadline);

    // ── 驱动状态机 ──
    // 步骤0: IDLE → HANDSHAKING（handleIdle 内部 reset() + setState(HANDSHAKING)）
    ota_result_t res = processEvent(RELAY_EVT_START, nullptr, 0);
    if (res != OTA_RESULT_OK) goto cleanup;

    // 步骤1: HANDSHAKING（内部完成握手+认证 → SIZE_NEG）
    // ★ 注意：processEvent(START) 在 HANDSHAKING handler 内同步执行 doHandshakeReq + doChallengeAndAuth
    //   期间 SPI 阻塞轮询 8~30 秒。超时检查必须放在同步 handler 执行之后。
    res = processEvent(RELAY_EVT_START, nullptr, 0);
    if (res != OTA_RESULT_OK) goto cleanup;

    // 步骤2: SIZE_NEG（内部完成大小协商 → TRANSFERRING）
    res = processEvent(RELAY_EVT_START, nullptr, 0);
    if (res != OTA_RESULT_OK) goto cleanup;

    // 步骤3: TRANSFERRING（内部完成所有数据包 → FINISHING）
    res = processEvent(RELAY_EVT_START, nullptr, 0);
    if (res != OTA_RESULT_OK) goto cleanup;

    // 步骤4: FINISHING（结束OTA + APP验证 → DONE）
    res = processEvent(RELAY_EVT_START, nullptr, 0);
    if (res != OTA_RESULT_OK) goto cleanup;

cleanup:
    if (res != OTA_RESULT_OK) {
        DEBUG_ERR("OTA", "中继失败: result=%d, state=%d", (int)res, (int)relayState);
        reset();
    }
    return res;
}

// ============================================================
// 状态机复位
// ============================================================
void OtaRelay::reset()
{
    StateLockGuard lock;
    relayState = RELAY_IDLE;
    relaySeq = 0;
    relayTotalPackets = 0;
    m_transferTotalPackets = 0;
    m_transferCurrentSeq = 0;
    m_transferFlashOffset = 0;
    m_transferRetryCount = 0;
    m_transferDeadline = 0;
    Protocol::sessionKeyClear(&m_sessionCtx);
    memset(&m_sessionCtx, 0, sizeof(session_ctx_t));
    memset(m_kSession, 0, sizeof(m_kSession));
    memset(m_nonceServer, 0, sizeof(m_nonceServer));
    memset(m_gd32UidRuntime, 0, sizeof(m_gd32UidRuntime));
    DEBUG_INFO("OTA", "状态机已复位 → IDLE");
}

// ============================================================
// 内部工具函数（不变）
// ============================================================
static bool bytesAllValue(const uint8_t *buf, size_t len, uint8_t value)
{
    if (!buf || len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] != value) return false;
    }
    return true;
}

int OtaRelay::aes128EcbEncryptBlockLocal(const uint8_t *key, const uint8_t *in, uint8_t *out)
{
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    if (esp_aes_setkey(&ctx, key, 128) != 0) { esp_aes_free(&ctx); return -1; }
    int ret = esp_aes_crypt_ecb(&ctx, ESP_AES_ENCRYPT, in, out);
    esp_aes_free(&ctx);
    return ret;
}

void OtaRelay::logRespPayload(const char *stage, uint8_t resp_type, const uint8_t *payload, size_t len)
{
    char hex[193] = {0};
    size_t n = (len > 32U) ? 32U : len;
    for (size_t i = 0; i < n; ++i)
        snprintf(&hex[i * 2], sizeof(hex) - i * 2, "%02X", payload[i]);
    DEBUG_DBG("OTA", "%s resp: type=0x%02X len=%u payload[%u]=%s%s",
              stage, resp_type, (unsigned)len, (unsigned)n,
              n > 0 ? hex : "<empty>", len > n ? "..." : "");
}

esp_err_t OtaRelay::waitUntilNotWait(const char *stage, uint8_t *resp_type,
                                     uint8_t *resp_payload, size_t resp_max,
                                     size_t *resp_len, uint32_t timeout_ms, uint32_t max_wait_rounds)
{
    if (!resp_type) return ESP_ERR_INVALID_ARG;
    uint32_t round = 0;
    while (*resp_type == SPI_OTA_RSP_WAIT && round < max_wait_rounds) {
        ++round;
        DEBUG_WARN("OTA", "%s WAIT, poll #%" PRIu32 "...", stage, round);
        esp_err_t err = SpiMaster::instance().otaPollResponse(resp_type, resp_payload, resp_max, resp_len, 100);
        if (err == ESP_ERR_TIMEOUT) { *resp_type = SPI_OTA_RSP_WAIT; continue; }
        if (err != ESP_OK) return err;
        logRespPayload(stage, *resp_type, resp_payload, resp_len ? *resp_len : 0);
    }
    return ESP_OK;
}

bool OtaRelay::isStructuredAckWithSeq(const uint8_t *payload, size_t len, uint32_t expected_seq)
{
    if (!payload || len < 6) return false;
    if (payload[0] != 0xBA || payload[1] != 0xC0) return false;
    uint32_t seq = (uint32_t)payload[2] | ((uint32_t)payload[3] << 8) |
                   ((uint32_t)payload[4] << 16) | ((uint32_t)payload[5] << 24);
    return seq == expected_seq;
}

bool OtaRelay::parseStructuredError(const uint8_t *payload, size_t len, uint32_t *seq_out, uint8_t *err_out)
{
    if (!payload || len < 7) return false;
    if (payload[0] != 0xBA || payload[1] != 0xC0) return false;
    if (seq_out) *seq_out = (uint32_t)payload[2] | ((uint32_t)payload[3] << 8) |
                   ((uint32_t)payload[4] << 16) | ((uint32_t)payload[5] << 24);
    if (err_out) *err_out = payload[6];
    return true;
}

bool OtaRelay::isHandshakeEchoAck(const uint8_t *payload, size_t len)
{ return payload && len >= 2U && payload[0] == 0x00U && payload[1] == 0xACU; }

bool OtaRelay::isChallengePayloadAck(const uint8_t *payload, size_t len)
{ return payload && len == GD32_CHALLENGE_RESPONSE_LEN; }

bool OtaRelay::isConfirmPlainAck(const uint8_t *payload, size_t len)
{ return payload && len == 2U && payload[0] == 0xBAU && payload[1] == 0xC0U; }

bool OtaRelay::isSizeSeq0Ack(const uint8_t *payload, size_t len)
{ return isStructuredAckWithSeq(payload, len, 0U); }

bool OtaRelay::isFinishPlainAck(const uint8_t *payload, size_t len)
{
    if (!payload) return false;
    if (len == 2U && payload[0] == 0x0AU && payload[1] == 0xACU) return true;
    return false;
}

esp_err_t OtaRelay::pollUntilExpectedAckShape(const char *stage,
                                               uint8_t *resp_type, uint8_t *resp_payload,
                                               size_t resp_max, size_t *resp_len,
                                               uint32_t poll_timeout_ms, uint32_t wait_rounds,
                                               uint32_t maxExtraPolls,
                                               bool (*matcher)(const uint8_t *, size_t))
{
    if (!stage || !resp_type || !resp_payload || !resp_len || !matcher)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;
    if (*resp_type == SPI_OTA_RSP_WAIT) {
        err = instance().waitUntilNotWait(stage, resp_type, resp_payload, resp_max, resp_len, poll_timeout_ms, wait_rounds);
        if (err != ESP_OK) return err;
    }
    for (uint32_t extra = 0; extra < maxExtraPolls; ++extra) {
        if (*resp_type != SPI_OTA_RSP_ACK) return ESP_OK;
        if (matcher(resp_payload, *resp_len)) return ESP_OK;
        uint32_t err_seq = 0; uint8_t err_code = 0;
        if (parseStructuredError(resp_payload, *resp_len, &err_seq, &err_code)) return ESP_OK;
        DEBUG_WARN("OTA", "%s stale ACK, poll more: len=%u", stage, (unsigned)*resp_len);
        *resp_len = 0;
        err = SpiMaster::instance().otaPollResponse(resp_type, resp_payload, resp_max, resp_len, poll_timeout_ms);
        if (err != ESP_OK) return err;
        logRespPayload(stage, *resp_type, resp_payload, *resp_len);
        if (*resp_type == SPI_OTA_RSP_WAIT) {
            err = instance().waitUntilNotWait(stage, resp_type, resp_payload, resp_max, resp_len, poll_timeout_ms, wait_rounds);
            if (err != ESP_OK) return err;
        }
    }
    return ESP_OK;
}

bool OtaRelay::parseAndVerifyChallengePayload(const uint8_t *resp_payload, size_t resp_len,
                                               uint8_t *out_k_session, uint8_t *out_uid)
{
    if (!resp_payload || !out_k_session || !out_uid) return false;
    if (resp_len != GD32_CHALLENGE_RESPONSE_LEN) {
        DEBUG_ERR("OTA", "CHALLENGE: invalid len=%u", (unsigned)resp_len); return false;
    }
    const uint8_t *uid = resp_payload;
    const uint8_t *nonce_frag = resp_payload + 12;
    const uint8_t *encrypted_ksession = resp_payload + 20;
    if (memcmp(nonce_frag, instance().m_nonceServer + 8, 8) != 0) {
        DEBUG_ERR("OTA", "CHALLENGE: nonce mismatch"); return false;
    }
    uint8_t K_session[16] = {0};
    if (aes128EcbEncryptBlockLocal(MASTER_KEY, instance().m_nonceServer, K_session) != 0) {
        DEBUG_ERR("OTA", "CHALLENGE: derive K_session failed"); return false;
    }
    uint8_t expected_encrypted[16] = {0};
    if (aes128EcbEncryptBlockLocal(MASTER_KEY, K_session, expected_encrypted) != 0) {
        DEBUG_ERR("OTA", "CHALLENGE: encrypt K_session failed"); return false;
    }
    if (memcmp(encrypted_ksession, expected_encrypted, 16) != 0) {
        DEBUG_ERR("OTA", "CHALLENGE: K_session mismatch"); return false;
    }
    memcpy(out_k_session, K_session, 16);
    memcpy(out_uid, uid, GD32_UID_LEN);
    if (bytesAllValue(out_uid, GD32_UID_LEN, 0x00U) || bytesAllValue(out_uid, GD32_UID_LEN, 0xFFU)) {
        DEBUG_ERR("OTA", "CHALLENGE: UID all 0 or FF"); return false;
    }
    return true;
}

bool OtaRelay::aes128EcbEncrypt48Bytes(const uint8_t *key, const uint8_t *in, uint8_t *out)
{
    for (int i = 0; i < 3; ++i)
        if (aes128EcbEncryptBlockLocal(key, in + i * 16, out + i * 16) != 0) return false;
    return true;
}

void OtaRelay::calculateCrc32WithKey(const uint8_t *key, const uint8_t *data48, uint8_t *output)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (int i = 0; i < 16; ++i) { crc ^= key[i]; for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U))); }
    for (size_t i = 0; i < 48U; ++i) { crc ^= data48[i]; for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U))); }
    crc ^= 0xFFFFFFFFU;
    output[0] = (uint8_t)(crc & 0xFFU); output[1] = (uint8_t)((crc >> 8) & 0xFFU);
    output[2] = (uint8_t)((crc >> 16) & 0xFFU); output[3] = (uint8_t)((crc >> 24) & 0xFFU);
}

// ============================================================
// Flash / NVS 操作（不变）
// ============================================================
esp_err_t OtaRelay::metaLoad(ota_meta_t *meta)
{
    if (!meta) return ESP_ERR_INVALID_ARG;
    memset(meta, 0, sizeof(ota_meta_t));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_META_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return ESP_OK;
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

esp_err_t OtaRelay::metaSave(const ota_meta_t *meta)
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

esp_err_t OtaRelay::metaUpdate(uint32_t stored_size, uint32_t last_seq)
{
    ota_meta_t meta;
    esp_err_t err = OtaRelay::metaLoad(&meta);
    if (err != ESP_OK) return err;

    meta.stored_size = stored_size;
    meta.last_seq = last_seq;

    return OtaRelay::metaSave(&meta);
}

esp_err_t OtaRelay::metaClear()
{
    DEBUG_INFO("OTA", "清除 OTA 元数据");
    ota_meta_t meta = {};
    return OtaRelay::metaSave(&meta);
}

esp_err_t OtaRelay::computeFlashSha256(uint8_t out_sha256[32])
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

esp_err_t OtaRelay::metaFinalizeChecksum()
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

esp_err_t OtaRelay::metaVerifyChecksum()
{
    ota_meta_t meta;
    esp_err_t err = metaLoad(&meta);
    if (err != ESP_OK) return err;

    if (bytesAllValue(meta.checksum, sizeof(meta.checksum), 0x00)) {
        DEBUG_WARN("OTA", "metaVerify: checksum 未计算，跳过校验（旧数据兼容）");
        return ESP_OK;
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

esp_err_t OtaRelay::flashWriteChunk(uint32_t offset, const uint8_t *data, size_t len)
{
    if (!instance().m_gd32Partition) {
        instance().m_gd32Partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "gd32_ota");
        if (!instance().m_gd32Partition) {
            DEBUG_ERR("OTA", "找不到 gd32_ota 分区");
            return ESP_ERR_NOT_FOUND;
        }
    }

    const esp_partition_t *part = instance().m_gd32Partition;

    // ★ 计算 head/tail padding，使写入地址和长度都对齐到 4 字节
    //   WiFi HTTP chunk 边界不一定 4 对齐，flash 写入必须对齐。
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

    // 读回 head padding（offset 之前的字节，属于上一次写入的尾部）
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

    // 读回 tail padding（数据末尾到 4 字节边界的字节）
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

esp_err_t OtaRelay::flashReadChunk(uint32_t offset, uint8_t *data, size_t len)
{
    if (!instance().m_gd32Partition) {
        instance().m_gd32Partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "gd32_ota");
        if (!instance().m_gd32Partition) {
            DEBUG_ERR("OTA", "找不到 gd32_ota 分区");
            return ESP_ERR_NOT_FOUND;
        }
    }

    return esp_partition_read(instance().m_gd32Partition, offset, data, len);
}

// ============================================================
// 协议子步骤实现
// ============================================================
esp_err_t OtaRelay::requestGd32AppEnterBootloader()
{
    DEBUG_INFO("OTA", "尝试通知 GD32 APP 进入 Bootloader OTA 窗口");
    esp_err_t err = SpiMaster::instance().appRequestBootloader();
    if (err == ESP_OK) {
        DEBUG_INFO("OTA", "GD32 已确认进入 Bootloader，等待就绪");
        vTaskDelay(pdMS_TO_TICKS(1000));
        return ESP_OK;
    }
    DEBUG_WARN("OTA", "GD32 APP 未确认进入 Bootloader: %s", esp_err_to_name(err));
    return err;
}

ota_result_t OtaRelay::doHandshakeReq()
{
    DEBUG_INFO("OTA", "--- 握手请求 ---");

    uint8_t payload[] = {0x00, 0xAC};
    uint8_t last_resp_type = 0U;
    size_t last_resp_len = 0U;
    esp_err_t last_err = ESP_OK;
    constexpr uint32_t kHandshakeAttempts = 10U;
    constexpr uint32_t kHandshakeReqTimeoutMs = 200U;
    constexpr uint32_t kHandshakePollTimeoutMs = 200U;
    constexpr uint32_t kHandshakeRetryGapMs = 100U;

    for (uint32_t attempt = 0U; attempt < kHandshakeAttempts; ++attempt) {
        uint8_t resp_type = 0U;
        uint8_t resp_payload[64] = {0};
        size_t resp_len = 0U;

        esp_err_t err = SpiMaster::instance().otaTransfer(SPI_OTA_CMD_REQ, payload, sizeof(payload),
                                         &resp_type, resp_payload, sizeof(resp_payload), &resp_len,
                                         kHandshakeReqTimeoutMs);
        last_err = err;
        last_resp_type = resp_type;
        last_resp_len = resp_len;

        if (err == ESP_OK) {
            logRespPayload("HANDSHAKE_REQ", resp_type, resp_payload, resp_len);
            err = pollUntilExpectedAckShape("HANDSHAKE_REQ", &resp_type, resp_payload,
                                                sizeof(resp_payload), &resp_len,
                                                kHandshakePollTimeoutMs, 2, 2, OtaRelay::isHandshakeEchoAck);
            last_err = err;
            last_resp_type = resp_type;
            last_resp_len = resp_len;

            if (err == ESP_OK && resp_type == SPI_OTA_RSP_ACK && isHandshakeEchoAck(resp_payload, resp_len)) {
                DEBUG_INFO("OTA", "握手请求已确认 (attempt=%" PRIu32 ")", attempt + 1U);
                return OTA_RESULT_OK;
            }
        }

        DEBUG_WARN("OTA", "HANDSHAKE_REQ 未就绪，重试 %" PRIu32 "/%" PRIu32 ": err=%d resp_type=0x%02X resp_len=%u",
                   attempt + 1U, kHandshakeAttempts, last_err, last_resp_type, (unsigned)last_resp_len);
        vTaskDelay(pdMS_TO_TICKS(kHandshakeRetryGapMs));
    }

    DEBUG_ERR("OTA", "HANDSHAKE_REQ 失败, err=%d, resp_type=0x%02X, resp_len=%zu",
              last_err, last_resp_type, last_resp_len);
    return OTA_RESULT_ERR_TIMEOUT;
}

ota_result_t OtaRelay::doChallengeAndAuth()
{
    uint8_t resp_type = 0;
    uint8_t resp_payload[64] = {0};
    size_t resp_len = 0;

    uint32_t timestamp = xTaskGetTickCount();
    uint32_t mem_addr = (uint32_t)(uintptr_t)m_nonceServer;
    uint32_t rand_seed = 0x12345678;

    Protocol::instance().scrambleGenerate(timestamp, mem_addr, rand_seed, m_nonceServer);

    DEBUG_INFO("OTA", "--- 认证步骤 1: 发送 CHALLENGE ---");
    DEBUG_INFO("OTA", "发送 nonce_server (16B), timestamp=0x%08" PRIX32, timestamp);

    esp_err_t err = SpiMaster::instance().otaTransfer(SPI_OTA_CMD_REQ, m_nonceServer, 16,
                                     &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 5000);
    logRespPayload("CHALLENGE", resp_type, resp_payload, resp_len);
    err = pollUntilExpectedAckShape("CHALLENGE", &resp_type, resp_payload,
                                        sizeof(resp_payload), &resp_len,
                                        1000, 12, 8, OtaRelay::isChallengePayloadAck);
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || !isChallengePayloadAck(resp_payload, resp_len)) {
        DEBUG_ERR("OTA", "CHALLENGE 发送失败, err=%d, resp_type=0x%02X, resp_len=%zu", err, resp_type, resp_len);
        return OTA_RESULT_ERR_TIMEOUT;
    }

    DEBUG_INFO("OTA", "已收到 GD32 握手数据包");

    if (!parseAndVerifyChallengePayload(resp_payload, resp_len, m_kSession, m_gd32UidRuntime)) {
        DEBUG_ERR("OTA", "CHALLENGE 解包/校验失败");
        return OTA_RESULT_ERR_AUTH;
    }

    uint8_t plaintext[16];
    for (int i = 0; i < 16; ++i) {
        plaintext[i] = m_nonceServer[i] ^ 0x55U;
    }
    uint8_t encrypted_confirm[16] = {0};
    if (aes128EcbEncryptBlockLocal(m_kSession, plaintext, encrypted_confirm) != 0) {
        DEBUG_ERR("OTA", "加密确认块失败");
        return OTA_RESULT_ERR_AUTH;
    }

    DEBUG_INFO("OTA", "--- 认证步骤 2: 发送 SESSION_KEY/CONFIRM ---");

    resp_len = 0;
    err = SpiMaster::instance().otaTransfer(SPI_OTA_CMD_REQ, encrypted_confirm, 16,
                           &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
    logRespPayload("CONFIRM", resp_type, resp_payload, resp_len);
    err = pollUntilExpectedAckShape("CONFIRM", &resp_type, resp_payload,
                                        sizeof(resp_payload), &resp_len,
                                        1000, 8, 8, OtaRelay::isConfirmPlainAck);
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || !isConfirmPlainAck(resp_payload, resp_len)) {
        DEBUG_ERR("OTA", "SESSION_KEY/CONFIRM 失败, err=%d, resp_type=0x%02X, resp_len=%zu", err, resp_type, resp_len);
        return OTA_RESULT_ERR_TIMEOUT;
    }

    {
        char hex[73] = {0};
        size_t n = (resp_len > 36U) ? 36U : resp_len;
        for (size_t i = 0; i < n; ++i) {
            snprintf(&hex[i * 2], sizeof(hex) - i * 2, "%02X", resp_payload[i]);
        }
        DEBUG_INFO("OTA", "CONFIRM 验证通过, resp_len=%zu, payload=%s", resp_len, hex);
    }

    Protocol::sessionKeyInit(&m_sessionCtx, m_kSession);
    return OTA_RESULT_OK;
}

ota_result_t OtaRelay::doSendFirmwareSize(uint32_t size)
{
    uint8_t resp_type = 0;
    uint8_t resp_payload[32] = {0};
    size_t resp_len = 0;

    DEBUG_INFO("OTA", "固件大小: %" PRIu32 " 字节", size);

    uint8_t size_payload[4];
    size_payload[0] = (uint8_t)(size & 0xFF);
    size_payload[1] = (uint8_t)((size >> 8) & 0xFF);
    size_payload[2] = (uint8_t)((size >> 16) & 0xFF);
    size_payload[3] = (uint8_t)((size >> 24) & 0xFF);

    esp_err_t err = SpiMaster::instance().otaTransfer(SPI_OTA_CMD_DATA, size_payload, sizeof(size_payload),
                                     &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
    logRespPayload("SEND_SIZE", resp_type, resp_payload, resp_len);
    DEBUG_WARN("OTA", "SEND_SIZE poll config: poll_timeout_ms=2000 wait_rounds=300 maxExtraPolls=24");
    err = pollUntilExpectedAckShape("SEND_SIZE", &resp_type, resp_payload,
                                        sizeof(resp_payload), &resp_len,
                                        2000, 300, 24, OtaRelay::isSizeSeq0Ack);
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || !isSizeSeq0Ack(resp_payload, resp_len)) {
        DEBUG_ERR("OTA", "SEND_SIZE 失败, err=%d, resp_type=0x%02X, resp_len=%zu", err, resp_type, resp_len);
        return OTA_RESULT_ERR_TIMEOUT;
    }

    DEBUG_INFO("OTA", "大小已发送: %" PRIu32 " 字节，等待 GD32 确认...", size);
    return OTA_RESULT_OK;
}

ota_result_t OtaRelay::doRelayPackets(TickType_t deadline_tick)
{
    DEBUG_INFO("OTA", "开始转发 %" PRIu32 " 包", m_transferTotalPackets);

    ota_meta_t meta;
    if (OtaRelay::metaLoad(&meta) != ESP_OK) {
        DEBUG_ERR("OTA", "元数据加载失败");
        return OTA_RESULT_ERR_STORAGE;
    }

    uint8_t tx_buf[64] = {0};
    uint8_t resp_type = 0;
    uint8_t resp_payload[32] = {0};
    size_t resp_len = 0;
    ota_result_t result = OTA_RESULT_OK;

    for (uint32_t seq = 0; seq < m_transferTotalPackets; seq++) {
        uint32_t retry_count = 0;
        if ((seq % OTA_PACKET_HEALTH_CHECK_INTERVAL) == 0 || seq == 0 || seq == m_transferTotalPackets - 1) {
            health_status_t health = HealthMonitor::instance().checkBeforeOtaRelay();
            if (health == HEALTH_MON_SHUTDOWN || health == HEALTH_MON_CRITICAL) {
                DEBUG_ERR("OTA", "健康检查危险，中止 OTA");
                return OTA_RESULT_ERR_HEALTH;
            }
        }

        if (deadline_tick != 0 && xTaskGetTickCount() >= deadline_tick) {
            DEBUG_ERR("OTA", "全局超时: 包 %" PRIu32 "/%" PRIu32 " 已超限，中止转发", seq, m_transferTotalPackets);
            return OTA_RESULT_ERR_USER_ABORT;
        }

        uint8_t plain48[GD32_OTA_PACKET_SIZE];
        memset(plain48, 0xFF, sizeof(plain48));
        size_t chunk_size = GD32_OTA_PACKET_SIZE;
        if (seq == m_transferTotalPackets - 1) {
            chunk_size = meta.total_size - (seq * GD32_OTA_PACKET_SIZE);
        }

        esp_err_t err = OtaRelay::flashReadChunk(m_transferFlashOffset, plain48, chunk_size);
        if (err != ESP_OK) {
            DEBUG_ERR("OTA", "Flash 读取失败, offset=%" PRIu32 ", err=%d", m_transferFlashOffset, err);
            return OTA_RESULT_ERR_STORAGE;
        }

        tx_buf[0] = 0x00; tx_buf[1] = 0x00; tx_buf[2] = 0x00; tx_buf[3] = 0x00;
        tx_buf[4] = (uint8_t)(seq & 0xFFU);
        tx_buf[5] = (uint8_t)((seq >> 8) & 0xFFU);
        tx_buf[6] = 0x00;
        tx_buf[7] = 0x00;

        if (!aes128EcbEncrypt48Bytes(m_kSession, plain48, &tx_buf[8])) {
            DEBUG_ERR("OTA", "包 %" PRIu32 " 加密失败", seq);
            return OTA_RESULT_ERR_AUTH;
        }

        uint32_t if_end = (seq == m_transferTotalPackets - 1) ? 0xFFFFFFFFU : 0x00000000U;
        tx_buf[56] = (uint8_t)(if_end & 0xFFU);
        tx_buf[57] = (uint8_t)((if_end >> 8) & 0xFFU);
        tx_buf[58] = (uint8_t)((if_end >> 16) & 0xFFU);
        tx_buf[59] = (uint8_t)((if_end >> 24) & 0xFFU);
        calculateCrc32WithKey(m_kSession, plain48, &tx_buf[60]);

        DEBUG_DBG("OTA", "包 %" PRIu32 ": chunk=%u if_end=0x%08" PRIX32 " crc32=%02X%02X%02X%02X",
                   seq, (unsigned)chunk_size, if_end,
                   tx_buf[60], tx_buf[61], tx_buf[62], tx_buf[63]);

        m_transferCurrentSeq = seq;

    resend_packet:
        while (1) {
            resp_len = 0;
            err = SpiMaster::instance().otaTransfer(SPI_OTA_CMD_DATA, tx_buf, sizeof(tx_buf),
                                   &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
            logRespPayload("DATA_PACKET", resp_type, resp_payload, resp_len);
            if (err == ESP_OK) {
                err = waitUntilNotWait("DATA_PACKET", &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 1000, 10);
            }
            if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK) {
                DEBUG_WARN("OTA", "数据包 %" PRIu32 " 发送失败, err=%d, resp_type=0x%02X, resp_len=%zu", seq, err, resp_type, resp_len);
                result = OTA_RESULT_ERR_TIMEOUT;
                break;
            }

            uint32_t expected_ack = (seq == m_transferTotalPackets - 1) ? 0xF0F0F0F0U : (seq + 1U);

            if (!isStructuredAckWithSeq(resp_payload, resp_len, expected_ack)) {
                uint32_t err_seq = 0;
                uint8_t err_code = 0;
                if (parseStructuredError(resp_payload, resp_len, &err_seq, &err_code)) {
                    if (err_seq == seq) {
                        DEBUG_WARN("OTA", "包 %" PRIu32 " 收到结构化错误 err=0x%02X，准备重发", seq, err_code);
                        if (++retry_count >= 3U) {
                            DEBUG_ERR("OTA", "包 %" PRIu32 " 重发次数超限", seq);
                            result = (err_code == 0x04U) ? OTA_RESULT_ERR_CRC : OTA_RESULT_ERR_GD32;
                            break;
                        }
                        continue;
                    }
                }

                bool got_expected = false;
                for (uint32_t retry = 0; retry < 10; ++retry) {
                    DEBUG_DBG("OTA", "包 %" PRIu32 " 收到非目标ACK(seq不匹配)，继续轮询(%" PRIu32 "/5)...", seq, retry + 1);
                    err = SpiMaster::instance().otaPollResponse(&resp_type, resp_payload, sizeof(resp_payload), &resp_len, 2000);
                    if (err == ESP_OK && resp_type == SPI_OTA_RSP_WAIT) {
                        err = waitUntilNotWait("DATA_PACKET", &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 100, 5);
                    }
                    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK) {
                        DEBUG_WARN("OTA", "包 %" PRIu32 " poll 超时/非ACK，重发 (retry=%" PRIu32 "/3)", seq, retry_count + 1);
                        if (++retry_count >= 3U) {
                            DEBUG_ERR("OTA", "包 %" PRIu32 " 重发次数超限", seq);
                            result = OTA_RESULT_ERR_GD32;
                            break;
                        }
                        // ★ resend 前延迟
                        vTaskDelay(pdMS_TO_TICKS(1));
                        goto resend_packet;
                    }
                    if (isStructuredAckWithSeq(resp_payload, resp_len, expected_ack)) {
                        got_expected = true;
                        break;
                    }
                    if (parseStructuredError(resp_payload, resp_len, &err_seq, &err_code) && err_seq == seq) {
                        break;
                    }
                }

                if (got_expected) {
                    m_transferFlashOffset += (uint32_t)chunk_size;
                    break;
                }

                if (parseStructuredError(resp_payload, resp_len, &err_seq, &err_code) && err_seq == seq) {
                    DEBUG_WARN("OTA", "包 %" PRIu32 " 收到结构化错误 err=0x%02X，准备重发", seq, err_code);
                    if (++retry_count >= 10U) {
                        DEBUG_ERR("OTA", "包 %" PRIu32 " 重发次数超限", seq);
                        result = (err_code == 0x04U) ? OTA_RESULT_ERR_CRC : OTA_RESULT_ERR_GD32;
                        break;
                    }
                    continue;
                }

                DEBUG_WARN("OTA", "包 %" PRIu32 " 轮询耗尽未匹配，重发 (retry=%" PRIu32 "/10)", seq, retry_count + 1);
                if (++retry_count >= 10U) {
                    DEBUG_ERR("OTA", "包 %" PRIu32 " 重发次数超限", seq);
                    result = OTA_RESULT_ERR_GD32;
                    break;
                }
                // ★ resend 前延迟：给 GD32 时间处理上一包并更新 SPI 响应缓冲区
                vTaskDelay(pdMS_TO_TICKS(1));
                goto resend_packet;
            }

            m_transferFlashOffset += (uint32_t)chunk_size;
            break;
        }

        if (result != OTA_RESULT_OK) {
            break;
        }

        relaySeq = seq + 1;
        if (seq % (m_transferTotalPackets / 10 + 1) == 0) {
            uint32_t progress = (relaySeq * 100U) / m_transferTotalPackets;
            DEBUG_INFO("OTA", "转发进度: %" PRIu32 "/%" PRIu32 " 包 (%" PRIu32 "%%)", relaySeq, m_transferTotalPackets, progress);
        }

        // ★ 包间延迟：给 GD32 Bootloader 充足时间完成 flash 写入并准备下一包 ACK。
        //   无此延迟时，ESP32 立即 poll 会读到上一包的 stale ACK，导致误判为丢包。
        if (seq < m_transferTotalPackets - 1) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return result;
}

ota_result_t OtaRelay::doFinishAndVerify()
{
    DEBUG_INFO("OTA", "发送 FINISH_OTA (0x0A 0xAC)，等待 GD32 确认即将跳转 APP");

    uint8_t finish_payload[2] = {0x0A, 0xAC};
    uint8_t resp_type = 0;
    uint8_t resp_payload[32] = {0};
    size_t resp_len = 0;

    for (uint32_t retry = 0; retry < 10; ++retry) {
        resp_type = 0;
        resp_len = 0;
        memset(resp_payload, 0, sizeof(resp_payload));

        esp_err_t err = SpiMaster::instance().otaTransfer(SPI_OTA_CMD_DATA, finish_payload, sizeof(finish_payload),
                                         &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
        logRespPayload("FINISH_OTA", resp_type, resp_payload, resp_len);
        if (err != ESP_OK) {
            DEBUG_WARN("OTA", "FINISH_OTA 传输失败, err=%d, retry=%" PRIu32 "/10", err, retry + 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        err = pollUntilExpectedAckShape("FINISH_OTA", &resp_type, resp_payload,
                                            sizeof(resp_payload), &resp_len,
                                            1000, 10, 8, OtaRelay::isFinishPlainAck);
        if (err != ESP_OK) {
            DEBUG_WARN("OTA", "FINISH_OTA poll 失败, err=%d, retry=%" PRIu32 "/10", err, retry + 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (resp_type == SPI_OTA_RSP_ACK && isFinishPlainAck(resp_payload, resp_len)) {
            break;
        }

        if (resp_type == SPI_OTA_RSP_WAIT) {
            DEBUG_INFO("OTA", "FINISH_OTA 收到 WAIT, GD32 正在排空 Flash 缓冲, retry=%" PRIu32 "/10", retry + 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        uint32_t err_seq = 0;
        uint8_t err_code = 0;
        if (parseStructuredError(resp_payload, resp_len, &err_seq, &err_code)) {
            DEBUG_ERR("OTA", "FINISH_OTA 收到错误帧 err=0x%02X, retry=%" PRIu32 "/10", err_code, retry + 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        DEBUG_WARN("OTA", "FINISH_OTA 响应异常, resp_type=0x%02X, resp_len=%zu, retry=%" PRIu32 "/10",
                   resp_type, resp_len, retry + 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (resp_type != SPI_OTA_RSP_ACK || !isFinishPlainAck(resp_payload, resp_len)) {
        DEBUG_ERR("OTA", "FINISH_OTA 失败, err=0, resp_type=0x%02X, resp_len=%zu", resp_type, resp_len);
        return OTA_RESULT_ERR_GD32;
    }

    Protocol::sessionKeyClear(&m_sessionCtx);
    memset(&m_sessionCtx, 0, sizeof(session_ctx_t));
    memset(m_kSession, 0, sizeof(m_kSession));
    memset(m_nonceServer, 0, sizeof(m_nonceServer));
    memset(m_gd32UidRuntime, 0, sizeof(m_gd32UidRuntime));

    DEBUG_INFO("OTA", "=== OTA 完成，等待 GD32 APP 启动 ===");

    OtaRelay::metaClear();

    // ★ APP 存活验证
    gd32AppAlive = false;

    for (uint32_t attempt = 0; attempt < 30; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(500));
        uint32_t uptime = 0;
        gd32_version_t ping_ver = {};
        esp_err_t ping_err = SpiMaster::instance().appPing(1000, &uptime, &ping_ver);
        if (ping_err == ESP_OK) {
            gd32AppAlive = true;
            gd32Version = ping_ver;
            DEBUG_INFO("OTA", "GD32 APP 存活确认: uptime=%" PRIu32 " sec, APP=v%d.%d.%d.%d, BOOT=v%d.%d.%d.%d",
                       uptime,
                       ping_ver.app_major, ping_ver.app_minor, ping_ver.app_patch, ping_ver.app_build,
                       ping_ver.boot_major, ping_ver.boot_minor, ping_ver.boot_patch, ping_ver.boot_build);
            DEBUG_INFO("OTA", "=== OTA 全流程完成 (APP 已启动) ===");
            break;
        }
        DEBUG_INFO("OTA", "等待 GD32 APP 启动... (%" PRIu32 "/30)", attempt + 1);
    }

    if (!gd32AppAlive) {
        DEBUG_ERR("OTA", "GD32 APP alive confirm timeout, OTA failed");
        reset();
        return OTA_RESULT_ERR_GD32;
    }

    return OTA_RESULT_OK;
}

// ============================================================
// FreeRTOS 任务入口
// ============================================================
void OtaRelay::taskEntry(void *pvParameters)
{
    (void)pvParameters;
    DEBUG_INFO("OTA", "OTA 中继任务启动");

    initMutex();

    dispatchQueue = xQueueCreate(4, sizeof(uint32_t));
    resultQueue = xQueueCreate(4, sizeof(uint32_t));

    if (!dispatchQueue || !resultQueue) {
        DEBUG_ERR("OTA", "OTA 队列创建失败");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        uint32_t msg = 0;
        BaseType_t ret = xQueueReceive(dispatchQueue, &msg, portMAX_DELAY);
        if (ret != pdTRUE) {
            continue;
        }

        DEBUG_INFO("OTA", "收到 OTA 调度信号: %" PRIu32 "，开始转发...", msg);
        // ★ 通知协调器中继已开始（确保协调器状态与实际中继一致）
        OtaCoordinator::instance().notifyRelayStarted();
        ota_result_t result = instance().forwardFirmware();

        uint32_t result_msg = (uint32_t)result;
        xQueueSend(resultQueue, &result_msg, pdMS_TO_TICKS(100));

        if (result == OTA_RESULT_OK) {
            DEBUG_INFO("OTA", "OTA 转发成功完成");
            vTaskDelay(pdMS_TO_TICKS(3000));
        } else if (result == OTA_RESULT_ERR_USER_ABORT) {
            DEBUG_ERR("OTA", "OTA 全局超时中止 (OTA_RESULT_ERR_USER_ABORT=%d)", result);
        } else {
            DEBUG_ERR("OTA", "OTA 转发失败，错误码: %d", result);
        }
        instance().reset();
        // ★ 通知协调器中继结束（成功→GD32_COMPLETE，失败→ERROR→IDLE）
        //   避免 m_sysState 卡在 OTA_SYS_GD32_RELAYING，导致后续 beginGd32Storage() 全部被拒
        OtaCoordinator::instance().notifyRelayComplete(result == OTA_RESULT_OK);
    }
}
