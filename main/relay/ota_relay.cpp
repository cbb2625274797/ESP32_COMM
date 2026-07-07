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
#include "monitor/can_feedback_task.h"
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "aes/esp_aes.h"

#include "ota_relay.h"
#include "storage/gd32_flash_store.h"
#include "spi_master.h"
#include "protocol.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"
// ★ H4 解耦：OtaRelay 不再直接 #include "ota_coordinator.h"，
//   通过 IRelayListener 接口回调通知协调器，依赖方向变为单向。

// ============================================================
// 公共拒绝事件判断（文件内工具）
// ============================================================
// 统一各状态 handler 头部的 ABORT/TIMEOUT/HEALTH_CRIT 拒绝判断。
// 返回 OTA_RESULT_OK 表示非拒绝事件；其他为对应错误码。
// allowHealth: 是否将 RELAY_EVT_HEALTH_CRIT 视为拒绝（仅 TRANSFERRING 传 true）。
static ota_result_t commonReject(relay_event_t e, bool allowHealth = false)
{
    if (e == RELAY_EVT_ABORT)   return OTA_RESULT_ERR_USER_ABORT;
    if (e == RELAY_EVT_TIMEOUT) return OTA_RESULT_ERR_TIMEOUT;
    if (allowHealth && e == RELAY_EVT_HEALTH_CRIT) return OTA_RESULT_ERR_HEALTH;
    return OTA_RESULT_OK;
}

// ============================================================
// doRelayPackets 重试阈值（与历史代码字面值保持一致，仅命名化）
// ============================================================
// 原代码中 retry_count 阈值：行 1015/1075/1084 用 10U，行 1032/1050 用 3U。
// 含义：NACK/轮询耗尽属于"通信异常"，给予较多重试机会（10次）；
//       结构化错误属于"GD32 报告具体错误"，较少重试（3次）。
static constexpr uint32_t kMaxResendOnNack        = 10U;  // ACK 失败/轮询耗尽重发上限
static constexpr uint32_t kMaxResendOnStructError = 3U;   // 结构化错误重发上限
static constexpr uint32_t kLastPacketAckSeq        = 0xF0F0F0F0U;  // 末包 ACK 序号约定

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
TaskHandle_t OtaRelay::taskHandle = NULL;
IRelayListener *OtaRelay::s_relayListener = nullptr;

// ── 中继事件监听器（解决 H4 双向耦合）──
void OtaRelay::setRelayListener(IRelayListener *listener)
{
    s_relayListener = listener;
}

IRelayListener* OtaRelay::getRelayListener()
{
    return s_relayListener;
}

// ── 队列访问器（M3: 封装 public static 成员）──
QueueHandle_t OtaRelay::getDispatchQueue()
{
    return dispatchQueue;
}

QueueHandle_t OtaRelay::getResultQueue()
{
    return resultQueue;
}

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

RelaySnapshot OtaRelay::getSnapshot()
{
    StateLockGuard lock;
    RelaySnapshot s;
    s.state = relayState;
    s.seq = relaySeq;
    s.total_packets = relayTotalPackets;
    s.gd32_app_alive = gd32AppAlive;
    s.version = gd32Version;
    return s;
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

TaskHandle_t OtaRelay::getTaskHandle()
{
    return taskHandle;
}

// ============================================================
// 事件分发核心：processEvent()
// ============================================================
ota_result_t OtaRelay::processEvent(relay_event_t event, const uint8_t *payload, size_t len)
{
    relay_state_t cur = getState();

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

    ota_result_t rej = commonReject(event);
    if (rej != OTA_RESULT_OK) {
        DEBUG_ERR("OTA", "HANDSHAKING 拒绝事件: %d", (int)event);
        setState(RELAY_ERROR);
        return rej;
    }

    if (event != RELAY_EVT_START && event != RELAY_EVT_RESP_OK) {
        DEBUG_WARN("OTA", "HANDSHAKING 忽略事件: %d", (int)event);
        return OTA_RESULT_OK;
    }

    // 进入 HANDSHAKING 阶段：执行完整的握手 + 认证流程
    DEBUG_INFO("OTA", "=== 阶段1: 握手与认证开始 ===");

    // 步骤1: 请求 GD32 进入 Bootloader
    // 0xB0 对 APP：触发写标志+复位 → BOOT 启动
    // 0xB0 对 BOOT：返回 0xB5 + resetToIdle()（IDLE 状态下无害）
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

    ota_result_t rej = commonReject(event);
    if (rej != OTA_RESULT_OK) {
        DEBUG_ERR("OTA", "SIZE_NEG 拒绝事件: %d", (int)event);
        setState(RELAY_ERROR);
        return rej;
    }

    // 加载元数据获取固件大小
    ota_meta_t meta;
    if (Gd32FlashStore::instance().metaLoad(&meta) != ESP_OK || meta.total_size == 0) {
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
    setRelayTotalPackets(m_transferTotalPackets);
    setRelaySeq(0);

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

    ota_result_t rej = commonReject(event, /*allowHealth=*/true);
    if (rej != OTA_RESULT_OK) {
        DEBUG_ERR("OTA", "TRANSFERRING 拒绝事件: %d", (int)event);
        setState(RELAY_ERROR);
        return rej;
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

    ota_result_t rej = commonReject(event);
    if (rej != OTA_RESULT_OK) {
        DEBUG_ERR("OTA", "FINISHING 拒绝事件: %d", (int)event);
        setState(RELAY_ERROR);
        return rej;
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
    if (Gd32FlashStore::instance().metaLoad(&meta) != ESP_OK) {
        return OTA_RESULT_ERR_STORAGE;
    }

    DEBUG_INFO("OTA", "开始固件转发: %" PRIu32 " 字节", meta.total_size);

    // 固件完整性校验
    {
        esp_err_t verify_err = Gd32FlashStore::instance().metaVerifyChecksum();
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
    // 5 步顺序推进：IDLE → HANDSHAKING → SIZE_NEG → TRANSFERRING → FINISHING → DONE
    // 每步同步执行对应 handler（内部完成协议交互并推进状态）。
    // 任一步返回非 OK 即跳出并复位。
    ota_result_t res = OTA_RESULT_OK;
    while (true) {
        relay_state_t cur = getState();
        if (cur == RELAY_DONE || cur == RELAY_ERROR) break;
        res = processEvent(RELAY_EVT_START, nullptr, 0);
        if (res != OTA_RESULT_OK) {
            break;
        }
    }

    if (res != OTA_RESULT_OK) {
        DEBUG_ERR("OTA", "中继失败: result=%d, state=%d", (int)res, (int)getState());
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
/**
 * @brief 检查缓冲区中所有字节是否为指定值。
 * @param buf 数据缓冲区。
 * @param len 缓冲区长度。
 * @param value 目标字节值。
 * @return true 所有字节均为指定值，false 否则。
 */
static bool bytesAllValue(const uint8_t *buf, size_t len, uint8_t value)
{
    if (!buf || len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] != value) return false;
    }
    return true;
}

/**
 * @brief 本地 AES-128-ECB 加密单块（16字节），使用硬件加速。
 * @param key 16 字节密钥。
 * @param in 16 字节明文输入。
 * @param out 16 字节密文输出。
 * @return 0 成功，-1 失败。
 */
int OtaRelay::aes128EcbEncryptBlockLocal(const uint8_t *key, const uint8_t *in, uint8_t *out)
{
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    if (esp_aes_setkey(&ctx, key, 128) != 0) { esp_aes_free(&ctx); return -1; }
    int ret = esp_aes_crypt_ecb(&ctx, ESP_AES_ENCRYPT, in, out);
    esp_aes_free(&ctx);
    return ret;
}

/**
 * @brief 记录 GD32 SPI 响应的十六进制负载（调试用）。
 * @param stage 当前阶段名称。
 * @param resp_type 响应类型字节。
 * @param payload 响应负载数据。
 * @param len 负载长度。
 */
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

/**
 * @brief 循环轮询 SPI 直到响应不再是 WAIT 类型，超过最大轮询次数后放弃。
 * @param stage 当前阶段名称（日志用）。
 * @param resp_type 输入/输出响应类型（初始为 SPI_OTA_RSP_WAIT）。
 * @param resp_payload 输出响应负载缓冲区。
 * @param resp_max 响应负载缓冲区最大长度。
 * @param resp_len 输出实际响应长度。
 * @param timeout_ms 单次 SPI 轮询超时（毫秒）。
 * @param max_wait_rounds 最大 WAIT 轮询次数。
 * @return ESP_OK 成功，ESP_ERR_TIMEOUT 超时。
 */
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

/**
 * @brief 检查响应负载是否为带指定序号的结构化 ACK（0xBA 0xC0 + seq32）。
 * @param payload 响应负载数据。
 * @param len 负载长度。
 * @param expected_seq 期望的序号。
 * @return true 匹配，false 不匹配。
 */
bool OtaRelay::isStructuredAckWithSeq(const uint8_t *payload, size_t len, uint32_t expected_seq)
{
    if (!payload || len < 6) return false;
    if (payload[0] != 0xBA || payload[1] != 0xC0) return false;
    uint32_t seq = (uint32_t)payload[2] | ((uint32_t)payload[3] << 8) |
                   ((uint32_t)payload[4] << 16) | ((uint32_t)payload[5] << 24);
    return seq == expected_seq;
}

/**
 * @brief 解析结构化错误响应（0xBA 0xC0 + seq32 + err_code）。
 * @param payload 响应负载数据。
 * @param len 负载长度。
 * @param seq_out 输出序号（可为 nullptr）。
 * @param err_out 输出错误码（可为 nullptr）。
 * @return true 成功解析为结构化错误，false 不是。
 */
bool OtaRelay::parseStructuredError(const uint8_t *payload, size_t len, uint32_t *seq_out, uint8_t *err_out)
{
    if (!payload || len < 7) return false;
    if (payload[0] != 0xBA || payload[1] != 0xC0) return false;
    if (seq_out) *seq_out = (uint32_t)payload[2] | ((uint32_t)payload[3] << 8) |
                   ((uint32_t)payload[4] << 16) | ((uint32_t)payload[5] << 24);
    if (err_out) *err_out = payload[6];
    return true;
}

/** @brief 检查响应是否为握手回显 ACK（0x00 0xAC）。 */
bool OtaRelay::isHandshakeEchoAck(const uint8_t *payload, size_t len)
{ return payload && len >= 2U && payload[0] == 0x00U && payload[1] == 0xACU; }

/** @brief 检查响应是否为挑战认证 ACK（36字节挑战响应）。 */
bool OtaRelay::isChallengePayloadAck(const uint8_t *payload, size_t len)
{ return payload && len == GD32_CHALLENGE_RESPONSE_LEN; }

/** @brief 检查响应是否为确认明文 ACK（0xBA 0xC0）。 */
bool OtaRelay::isConfirmPlainAck(const uint8_t *payload, size_t len)
{ return payload && len == 2U && payload[0] == 0xBAU && payload[1] == 0xC0U; }

/** @brief 检查响应是否为固件大小 seq=0 的结构化 ACK。 */
bool OtaRelay::isSizeSeq0Ack(const uint8_t *payload, size_t len)
{ return isStructuredAckWithSeq(payload, len, 0U); }

/** @brief 检查响应是否为结束指令 ACK（0x0A 0xAC）。 */
bool OtaRelay::isFinishPlainAck(const uint8_t *payload, size_t len)
{
    if (!payload) return false;
    if (len == 2U && payload[0] == 0x0AU && payload[1] == 0xACU) return true;
    return false;
}

/**
 * @brief 轮询 SPI 直到匹配指定的 ACK 形状，处理 WAIT 和过期 ACK。
 * @param stage 当前阶段名称。
 * @param resp_type 输入/输出响应类型。
 * @param resp_payload 输入/输出响应负载。
 * @param resp_max 响应负载缓冲区最大长度。
 * @param resp_len 输入/输出实际响应长度。
 * @param poll_timeout_ms 单次 SPI 轮询超时。
 * @param wait_rounds 最大 WAIT 轮询次数。
 * @param maxExtraPolls 最大额外轮询次数。
 * @param matcher ACK 形状匹配函数。
 * @return ESP_OK 成功，其他错误码表示失败。
 */
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

/**
 * @brief 解析挑战响应负载，验证 nonce、派生会话密钥和 UID。
 * @param resp_payload GD32 响应负载（36字节）。
 * @param resp_len 响应负载长度。
 * @param out_k_session 输出 16 字节会话密钥。
 * @param out_uid 输出 12 字节 GD32 UID。
 * @return true 验证通过，false 失败。
 */
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

/**
 * @brief AES-128-ECB 加密 48 字节（分 3 个 16 字节块）。
 * @param key 16 字节密钥。
 * @param in 48 字节明文输入。
 * @param out 48 字节密文输出。
 * @return true 全部加密成功，false 任一块失败。
 */
bool OtaRelay::aes128EcbEncrypt48Bytes(const uint8_t *key, const uint8_t *in, uint8_t *out)
{
    for (int i = 0; i < 3; ++i)
        if (aes128EcbEncryptBlockLocal(key, in + i * 16, out + i * 16) != 0) return false;
    return true;
}

/**
 * @brief 使用 CRC32（IEEE 多项式 0xEDB88320）对 key+data48 计算 4 字节校验值。
 * @param key 16 字节密钥（混入 CRC）。
 * @param data48 48 字节数据。
 * @param output 输出 4 字节 CRC32 值（小端序）。
 */
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
// 协议子步骤实现
// ============================================================
/** @brief 协议子步骤：请求 GD32 APP 进入 Bootloader OTA 模式。 */
esp_err_t OtaRelay::requestGd32AppEnterBootloader()
{
    DEBUG_INFO("OTA", "尝试通知 GD32 APP 进入 Bootloader OTA 窗口");
    SpiMaster::instance().appRequestBootloader();

    /* 方案A: 通过 PB6 LOW→HIGH 序列等待 Bootloader 就绪
     * APP 模式: 复位前拉低 PB6 → BOOT 极早拉低 → init 完成释放 HIGH
     * BOOT 模式: PB6 一直 HIGH，Phase1 超时后直接握手
     * 超时退化为固定延时（容错旧固件） */
    esp_err_t wait_err = SpiMaster::instance().waitForBootloaderReady(3000);
    if (wait_err != ESP_OK) {
        DEBUG_WARN("OTA", "硬件就绪信号超时，退化为固定延时");
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    return ESP_OK;
}

/** @brief 协议子步骤：发送握手请求（0x00 0xAC）并等待 GD32 回显 ACK。
 *  @param attempts 最大尝试次数，默认 20。先用 3 次快速探测 GD32 是否已在 BOOT。 */
ota_result_t OtaRelay::doHandshakeReq(uint32_t attempts)
{
    DEBUG_INFO("OTA", "--- 握手请求 (max=%" PRIu32 ") ---", attempts);

    uint8_t payload[] = {0x00, 0xAC};
    uint8_t last_resp_type = 0U;
    size_t last_resp_len = 0U;
    esp_err_t last_err = ESP_OK;
    constexpr uint32_t kHandshakeReqTimeoutMs = 200U;
    constexpr uint32_t kHandshakePollTimeoutMs = 200U;
    constexpr uint32_t kHandshakeRetryGapMs = 100U;

    for (uint32_t attempt = 0U; attempt < attempts; ++attempt) {
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
                   attempt + 1U, attempts, last_err, last_resp_type, (unsigned)last_resp_len);
        vTaskDelay(pdMS_TO_TICKS(kHandshakeRetryGapMs));
    }

    DEBUG_ERR("OTA", "HANDSHAKE_REQ 失败, err=%d, resp_type=0x%02X, resp_len=%zu",
              last_err, last_resp_type, last_resp_len);
    return OTA_RESULT_ERR_TIMEOUT;
}

/** @brief 协议子步骤：执行挑战认证（发送 nonce、验证响应、派生会话密钥、发送确认）。 */
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

/** @brief 协议子步骤：发送固件总大小给 GD32 并等待 seq=0 的确认。 */
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

/**
 * @brief 协议子步骤：逐包转发固件数据给 GD32。
 * 流程：从 Flash 读取 → AES 加密 → SPI 发送 → 等待 ACK → 失败则重发。
 * 每 OTA_PACKET_HEALTH_CHECK_INTERVAL 包执行一次健康检查。
 */
ota_result_t OtaRelay::prepareRelayPacket(uint32_t seq, const ota_meta_t &meta, uint8_t tx_buf[64], size_t *out_chunk_size)
{
    uint8_t plain48[GD32_OTA_PACKET_SIZE];
    memset(plain48, 0xFF, sizeof(plain48));
    size_t chunk_size = GD32_OTA_PACKET_SIZE;
    if (seq == m_transferTotalPackets - 1) {
        chunk_size = meta.total_size - (seq * GD32_OTA_PACKET_SIZE);
    }

    esp_err_t err = Gd32FlashStore::instance().flashReadChunk(m_transferFlashOffset, plain48, chunk_size);
    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "Flash 读取失败, offset=%" PRIu32 ", err=%d", m_transferFlashOffset, err);
        return OTA_RESULT_ERR_STORAGE;
    }

    /* session_num = nonce_server 前 4 字节，GD32 端做校验防重放/乱序 */
    memcpy(&tx_buf[0], m_nonceServer, 4);
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
    if (out_chunk_size) *out_chunk_size = chunk_size;
    return OTA_RESULT_OK;
}

bool OtaRelay::sendRelayPacketWithRetry(uint32_t seq, const uint8_t tx_buf[64], size_t chunk_size, ota_result_t *out_result)
{
    uint8_t resp_type = 0;
    uint8_t resp_payload[32] = {0};
    size_t resp_len = 0;
    uint32_t retry_count = 0;
    ota_result_t result = OTA_RESULT_OK;
    bool got_expected_credit_ok = false;
    const uint32_t expected_ack = (seq == m_transferTotalPackets - 1) ? kLastPacketAckSeq : (seq + 1U);

    // ★ 单包耗时统计，用于定位 OTA 速度瓶颈。
    TickType_t pkt_start = xTaskGetTickCount();

    // 注意：goto resend_packet 保持原有语义，仅作用范围缩小到本 helper。
    // 完全去除 goto 需配合 GD32 全链路 OTA 回归验证后再进行。
resend_packet:
    while (1) {
        resp_len = 0;
        esp_err_t err = SpiMaster::instance().otaTransfer(SPI_OTA_CMD_DATA, tx_buf, 64,
                              &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 3000);
        logRespPayload("DATA_PACKET", resp_type, resp_payload, resp_len);
        if (err == ESP_OK) {
            err = waitUntilNotWait("DATA_PACKET", &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 1000, 10);
        }
        if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK) {
            DEBUG_WARN("OTA", "数据包 %" PRIu32 " 发送失败, err=%d, resp_type=0x%02X, resp_len=%zu", seq, err, resp_type, resp_len);
            if (++retry_count >= kMaxResendOnNack) {
                DEBUG_ERR("OTA", "包 %" PRIu32 " 重发次数超限", seq);
                result = OTA_RESULT_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;  // 重发本包
        }

        if (!isStructuredAckWithSeq(resp_payload, resp_len, expected_ack)) {
            uint32_t err_seq = 0;
            uint8_t err_code = 0;
            if (parseStructuredError(resp_payload, resp_len, &err_seq, &err_code)) {
                if (err_seq == seq) {
                    DEBUG_WARN("OTA", "包 %" PRIu32 " 收到结构化错误 err=0x%02X，准备重发", seq, err_code);
                    if (++retry_count >= kMaxResendOnStructError) {
                        DEBUG_ERR("OTA", "包 %" PRIu32 " 重发次数超限", seq);
                        result = (err_code == 0x04U) ? OTA_RESULT_ERR_CRC : OTA_RESULT_ERR_GD32;
                        break;
                    }
                    continue;
                }
            }

            /* CREDIT 感知：若 GD32 报 CREDIT=0，说明它在写 Flash 缓冲，
             * 此时重发只会被丢弃。改为耐心 POLL 等待 CREDIT 恢复，最多等 500ms。 */
            if (SpiMaster::instance().remoteCredit() == 0U) {
                DEBUG_DBG("OTA", "包 %" PRIu32 " GD32 CREDIT=0(写Flash中)，POLL 等待...", seq);
                for (uint32_t cw = 0; cw < 50U; ++cw) {
                    err = SpiMaster::instance().otaPollResponse(&resp_type, resp_payload,
                                                                sizeof(resp_payload), &resp_len, 100);
                    if (err == ESP_OK && resp_type == SPI_OTA_RSP_ACK &&
                        isStructuredAckWithSeq(resp_payload, resp_len, expected_ack)) {
                        got_expected_credit_ok = true;
                        break;
                    }
                    if (err == ESP_OK && SpiMaster::instance().remoteCredit() > 0U) {
                        break;  /* CREDIT 恢复，可以继续轮询或重发 */
                    }
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }

            bool got_expected = false;
            if (got_expected_credit_ok) {
                m_transferFlashOffset += (uint32_t)chunk_size;
                break;
            }
            for (uint32_t retry = 0; retry < 10; ++retry) {
                DEBUG_DBG("OTA", "包 %" PRIu32 " 收到非目标ACK(seq不匹配)，继续轮询(%" PRIu32 "/5)...", seq, retry + 1);
                err = SpiMaster::instance().otaPollResponse(&resp_type, resp_payload, sizeof(resp_payload), &resp_len, 2000);
                if (err == ESP_OK && resp_type == SPI_OTA_RSP_WAIT) {
                    err = waitUntilNotWait("DATA_PACKET", &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 100, 5);
                }
                if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK) {
                    DEBUG_WARN("OTA", "包 %" PRIu32 " poll 超时/非ACK，重发 (retry=%" PRIu32 "/3)", seq, retry_count + 1);
                    if (++retry_count >= kMaxResendOnStructError) {
                        DEBUG_ERR("OTA", "包 %" PRIu32 " 重发次数超限", seq);
                        result = OTA_RESULT_ERR_GD32;
                        break;
                    }
                    // ★ resend 前延迟
                    vTaskDelay(pdMS_TO_TICKS(1));
                    got_expected_credit_ok = false;
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
                if (++retry_count >= kMaxResendOnNack) {
                    DEBUG_ERR("OTA", "包 %" PRIu32 " 重发次数超限", seq);
                    result = (err_code == 0x04U) ? OTA_RESULT_ERR_CRC : OTA_RESULT_ERR_GD32;
                    break;
                }
                continue;
            }

            DEBUG_WARN("OTA", "包 %" PRIu32 " 轮询耗尽未匹配，重发 (retry=%" PRIu32 "/10)", seq, retry_count + 1);
            if (++retry_count >= kMaxResendOnNack) {
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

    TickType_t pkt_elapsed = xTaskGetTickCount() - pkt_start;
    DEBUG_DBG("OTA", "包 %" PRIu32 " 完成: elapsed=%" PRIu32 " ms, retries=%" PRIu32, seq,
              (uint32_t)(pkt_elapsed * portTICK_PERIOD_MS), retry_count);

    if (result != OTA_RESULT_OK) {
        *out_result = result;
        return true;  // 中止外层循环
    }
    return false;  // 继续下一包
}

ota_result_t OtaRelay::doRelayPackets(TickType_t deadline_tick)
{
    DEBUG_INFO("OTA", "开始转发 %" PRIu32 " 包", m_transferTotalPackets);

    ota_meta_t meta;
    if (Gd32FlashStore::instance().metaLoad(&meta) != ESP_OK) {
        DEBUG_ERR("OTA", "元数据加载失败");
        return OTA_RESULT_ERR_STORAGE;
    }

    uint8_t tx_buf[64] = {0};
    ota_result_t result = OTA_RESULT_OK;

    for (uint32_t seq = 0; seq < m_transferTotalPackets; seq++) {
        // 健康检查（每 OTA_PACKET_HEALTH_CHECK_INTERVAL 包 + 首末包）
        if ((seq % OTA_PACKET_HEALTH_CHECK_INTERVAL) == 0 || seq == 0 || seq == m_transferTotalPackets - 1) {
            health_status_t health = HealthMonitor::instance().checkBeforeOtaRelay();
            if (health == HEALTH_MON_SHUTDOWN || health == HEALTH_MON_CRITICAL) {
                DEBUG_ERR("OTA", "健康检查危险，中止 OTA");
                return OTA_RESULT_ERR_HEALTH;
            }
        }

        // 全局超时检查
        if (deadline_tick != 0 && xTaskGetTickCount() >= deadline_tick) {
            DEBUG_ERR("OTA", "全局超时: 包 %" PRIu32 "/%" PRIu32 " 已超限，中止转发", seq, m_transferTotalPackets);
            return OTA_RESULT_ERR_USER_ABORT;
        }

        // 准备单包：读 Flash + 加密 + 组装
        size_t chunk_size = 0;
        result = prepareRelayPacket(seq, meta, tx_buf, &chunk_size);
        if (result != OTA_RESULT_OK) {
            return result;
        }

        // 发送单包并等待 ACK（内含重试循环）
        if (sendRelayPacketWithRetry(seq, tx_buf, chunk_size, &result)) {
            return result;  // 中止
        }

        setRelaySeq(seq + 1);
        // 每 5% 或前 5 包/最后 5 包打印进度，避免 9 秒+静默期无法判断是否卡死
        uint32_t step = m_transferTotalPackets / 20 + 1;  // ~5% 间隔
        if (seq < 5 || seq >= m_transferTotalPackets - 5 || seq % step == 0) {
            // 进度日志直接用本地 seq+1，避免再次加锁读 relaySeq
            uint32_t done = seq + 1;
            uint32_t progress = (done * 100U) / m_transferTotalPackets;
            DEBUG_INFO("OTA", "转发进度: %" PRIu32 "/%" PRIu32 " 包 (%" PRIu32 "%%)", done, m_transferTotalPackets, progress);
        }

        // ★ 包间延迟：给 GD32 Bootloader 充足时间完成 flash 写入并准备下一包 ACK。
        //   无此延迟时，ESP32 立即 poll 会读到上一包的 stale ACK，导致误判为丢包。
        if (seq < m_transferTotalPackets - 1 && CONFIG_DCU_OTA_PACKET_INTERVAL_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_DCU_OTA_PACKET_INTERVAL_MS));
        }
    }
    return result;
}

// ============================================================
// ============================================================
// 统一 OtaRelay 内部两处 APP 探测逻辑（doWaitForAppAlive 轮询 + taskEntry 启动探测）。
// 成功：setGd32AppAlive(true) + setGd32Version + 打印版本号；失败：打印 warning。
// 注：OtaCoordinator::probeGd32AppOnce 逻辑相似，但因依赖方向不调用本函数。
bool OtaRelay::probeAndCacheAppAlive(uint32_t timeout_ms, uint32_t *out_uptime)
{
    uint32_t uptime = 0;
    gd32_version_t ver = {};

    esp_err_t err = SpiMaster::instance().appPing(timeout_ms, &uptime, &ver);
    if (err == ESP_OK) {
        // 持锁一次性写入两个状态字段，保证 getSnapshot() 读取一致
        StateLockGuard lock;
        gd32AppAlive = true;
        gd32Version  = ver;
        if (out_uptime) *out_uptime = uptime;
        DEBUG_INFO("OTA", "GD32 APP 存活确认: uptime=%" PRIu32 " sec, APP=v%d.%d.%d.%d, BOOT=v%d.%d.%d.%d",
                   uptime,
                   ver.app_major, ver.app_minor, ver.app_patch, ver.app_build,
                   ver.boot_major, ver.boot_minor, ver.boot_patch, ver.boot_build);
        return true;
    }
    if (out_uptime) *out_uptime = 0;
    DEBUG_WARN("OTA", "GD32 APP 探测失败: %s", esp_err_to_name(err));
    return false;
}

/** @brief 协议子步骤：发送 FINISH_OTA 指令并等待 GD32 ACK（10 次重试）。 */
ota_result_t OtaRelay::doSendFinishOta()
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
                                         &resp_type, resp_payload, sizeof(resp_payload), &resp_len,
                                         CONFIG_DCU_OTA_FINISH_TIMEOUT_MS);
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

    return OTA_RESULT_OK;
}

/** @brief 协议子步骤：轮询等待 GD32 APP 存活（30 次重试）。 */
ota_result_t OtaRelay::doWaitForAppAlive(uint32_t timeout_ms)
{
    // ★ APP 存活验证
    setGd32AppAlive(false);

    for (uint32_t attempt = 0; attempt < 30; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (probeAndCacheAppAlive(timeout_ms)) {
            DEBUG_INFO("OTA", "=== OTA 全流程完成 (APP 已启动) ===");
            break;
        }
        DEBUG_INFO("OTA", "等待 GD32 APP 启动... (%" PRIu32 "/30)", attempt + 1);
    }

    if (!getGd32AppAlive()) {
        DEBUG_ERR("OTA", "GD32 APP alive confirm timeout, OTA failed");
        reset();
        return OTA_RESULT_ERR_GD32;
    }

    return OTA_RESULT_OK;
}

/** @brief 协议子步骤：发送 FINISH_OTA + 等待 APP 存活（顺序两阶段 + 会话清理）。 */
ota_result_t OtaRelay::doFinishAndVerify()
{
    // 阶段1: 发送 FINISH_OTA 并等待 ACK
    ota_result_t res = doSendFinishOta();
    if (res != OTA_RESULT_OK) {
        return res;
    }

    // 会话密钥/上下文清理
    Protocol::sessionKeyClear(&m_sessionCtx);
    memset(&m_sessionCtx, 0, sizeof(session_ctx_t));
    memset(m_kSession, 0, sizeof(m_kSession));
    memset(m_nonceServer, 0, sizeof(m_nonceServer));
    memset(m_gd32UidRuntime, 0, sizeof(m_gd32UidRuntime));

    DEBUG_INFO("OTA", "=== OTA 完成，等待 GD32 APP 启动 ===");

    Gd32FlashStore::instance().metaClear();

    // 阶段2: APP 存活轮询（单次 appPing 超时 1000ms，共 30 次）
    return doWaitForAppAlive(1000);
}

// ============================================================
// FreeRTOS 任务入口
// ============================================================
void OtaRelay::taskEntry(void *pvParameters)
{
    (void)pvParameters;
    taskHandle = xTaskGetCurrentTaskHandle();
    DEBUG_INFO("OTA", "OTA 中继任务启动");

    initMutex();

    dispatchQueue = xQueueCreate(4, sizeof(uint32_t));
    resultQueue = xQueueCreate(4, sizeof(uint32_t));

    if (!dispatchQueue || !resultQueue) {
        DEBUG_ERR("OTA", "OTA 队列创建失败");
        vTaskDelete(NULL);
        return;
    }

    // ★ 启动时初始 GD32 探测：读取 GD32 APP 版本号与存活状态
    //   GD32 APP 启动需要一定时间，给其 3 秒准备窗口后发起探测
    {
        DEBUG_INFO("OTA", "启动初始 GD32 探测...");
        vTaskDelay(pdMS_TO_TICKS(3000));  // 等待 GD32 APP 就绪
        probeAndCacheAppAlive(2000);
    }

    while (1) {
        uint32_t msg = 0;
        BaseType_t ret = xQueueReceive(dispatchQueue, &msg, portMAX_DELAY);
        if (ret != pdTRUE) {
            continue;
        }

        DEBUG_INFO("OTA", "收到 OTA 调度信号: %" PRIu32 "，开始转发...", msg);
        // ★ H4: 通过 IRelayListener 接口回调通知协调器（不再直接依赖 OtaCoordinator）
        IRelayListener *listener = getRelayListener();
        pauseCanFeedbackTask(true);
        if (listener) listener->onRelayStarted();
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
        // ★ H4: 通过 IRelayListener 接口回调通知协调器中继结束
        listener = getRelayListener();
        if (listener) listener->onRelayComplete(result == OTA_RESULT_OK);
        pauseCanFeedbackTask(false);
    }
}
