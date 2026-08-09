/*
 * ota_relay.cpp - OTA 中继事件驱动状态机实现
 *
 * 7 状态事件驱动架构：
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
#include "aes/esp_aes.h"

#include "ota_relay.h"
#include "storage/gd32_flash_store.h"
#include "spi_master.h"
#include "protocol.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"
// OtaRelay 不再直接 #include "ota_coordinator.h"，
//   通过 IRelayListener 接口回调通知协调器，依赖方向为单向。

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
// doRelayPackets 重试阈值（仅命名化字面值）
// ============================================================
// retry_count 阈值：NACK/轮询耗尽属于"通信异常"，给予较多重试机会（10次）；
//       结构化错误属于"GD32 报告具体错误"，较少重试（3次）。
static constexpr uint32_t kMaxResendOnNack        = 10U;  // ACK 失败/轮询耗尽重发上限
static constexpr uint32_t kMaxResendOnStructError = 3U;   // 结构化错误重发上限
static constexpr uint32_t kLastPacketAckSeq        = 0xF0F0F0F0U;  // 末包 ACK 序号约定
// 10 秒无成功帧停刷：跨包级别的背压/卡死检测，比单包 kAckWaitMs 更早触发。
static constexpr uint32_t kStallTimeoutMs = 10000U;

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

// ── 中继事件监听器 ──
void OtaRelay::setRelayListener(IRelayListener *listener)
{
    s_relayListener = listener;
}

IRelayListener* OtaRelay::getRelayListener()
{
    return s_relayListener;
}

// ── 队列访问器 ──
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

bool OtaRelay::getGd32AppAlive()
{
    StateLockGuard lock;
    return gd32AppAlive;
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

    // 步骤1: 请求 GD32 进入 Bootloader（或确认已在 BOOT）
    // 必须 HELLO role=boot 成功，禁止“未确认就盲发握手”
    esp_err_t err = requestGd32AppEnterBootloader();
    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "Bootloader 未就绪: %s", esp_err_to_name(err));
        setState(RELAY_ERROR);
        return OTA_RESULT_ERR_TIMEOUT;
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

    m_transferTotalPackets = (meta.total_size + GD32_OTA_PACKET_SIZE - 1U) / GD32_OTA_PACKET_SIZE;
    m_transferCurrentSeq = 0;
    m_transferFlashOffset = 0;
    /* total 提前写入，前端可显示「共 N 包」；seq 保持 0 直到真正开始转发 */
    setRelayTotalPackets(m_transferTotalPackets);
    setRelaySeq(0);

    ota_result_t res = doSendFirmwareSize(meta.total_size);
    if (res != OTA_RESULT_OK) {
        setState(RELAY_ERROR);
        return res;
    }

    /* 进入转发前先切状态，避免 status 长时间停在 SIZE_NEG 导致前端像卡在 0/N */
    setState(RELAY_TRANSFERRING);
    setRelaySeq(0);
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

    /* 转发预算：自本阶段起算。正常 ~2ms/包（12MHz+ 读预算收紧后实测），
     * 单包失败重发最坏 ~95ms/轮 × 3 轮 ≈ 285ms（底层 3 次尝试 + relay 50ms 窗口）。
     * 预算按"正常 + 重发余量"：25ms/包 ≈ 正常 2ms + 约 10% 包重发 1 次的平均，
     * 底 10s 覆盖慢启动/握手余量（777 包正常仅 ~1.6s）；上限 120s 兜极端慢速。
     * 跨包卡死（10s 无成功帧）由 kStallTimeoutMs 单独检测，总预算不重复兜底。 */
    constexpr uint32_t kXferMinMs = 10U * 1000U;
    constexpr uint32_t kXferMaxMs = 120U * 1000U;
    constexpr uint32_t kXferPerPktMs = 25U;
    uint32_t xfer_ms = kXferMinMs + m_transferTotalPackets * kXferPerPktMs;
    if (xfer_ms > kXferMaxMs) {
        xfer_ms = kXferMaxMs;
    }
    m_transferDeadline = xTaskGetTickCount() + pdMS_TO_TICKS(xfer_ms);
    DEBUG_INFO("OTA", "转发超时预算: %" PRIu32 " ms (%" PRIu32 " 包)", xfer_ms, m_transferTotalPackets);

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

    DEBUG_INFO("OTA", "结束中继，等待 GD32 APP 启动");

    ota_result_t res = doFinishAndVerify();
    if (res != OTA_RESULT_OK) {
        setState(RELAY_ERROR);
        return res;
    }

    setState(RELAY_DONE);
    // 成功总结只在 doWaitForAppAlive 打一条，避免多层「完成」刷屏
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

    /* 完整性门控（避免与 /ota/relay·BLE 触发前的 verifyGd32Firmware 二次全盘 SHA）：
     * - meta 必须完整 stored≥total
     * - checksum 非全零：触发路径已 finalize+比对，此处信任 meta，跳过再扫 Flash
     * - checksum 全零：兜底做 metaVerifyChecksum（或 finalize 后再比） */
    if (meta.total_size == 0U || meta.stored_size < meta.total_size) {
        DEBUG_ERR("OTA", "固件未完整存储 stored=%" PRIu32 " total=%" PRIu32,
                  meta.stored_size, meta.total_size);
        abortRemoteAndReset(true);  /* 顺带清上次远端半会话 */
        return OTA_RESULT_ERR_STORAGE;
    }
    {
        static const uint8_t kZeroSha[32] = {0};
        if (memcmp(meta.checksum, kZeroSha, sizeof(meta.checksum)) == 0) {
            DEBUG_WARN("OTA", "meta.checksum 为空，中继前补做全盘 SHA 校验");
            esp_err_t verify_err = Gd32FlashStore::instance().metaVerifyChecksum();
            if (verify_err != ESP_OK) {
                DEBUG_ERR("OTA", "固件完整性校验失败，中止转发");
                abortRemoteAndReset(true);
                return OTA_RESULT_ERR_CRC;
            }
        } else {
            DEBUG_INFO("OTA", "跳过二次全盘 SHA（meta.checksum 已就绪，%" PRIu32 " B）",
                       meta.total_size);
        }
    }

    /* 全局 deadline 在进入 TRANSFERRING 时才启动（见 handleTransferring）。
     * 握手 + Boot 擦 Flash（SIZE 阶段长时间 WAIT）不得占用转发预算，
     * 否则 1117 包会在擦除后仅剩十余秒被“全局超时”误杀。 */
    m_transferDeadline = 0;

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
        /* 必须通知 GD32 CANCEL，否则远端停在 WAITING_* 半会话 */
        abortRemoteAndReset(true);
    }
    /* 成功/失败统一由 reset() 解除 ring 抑制（relay task 侧持有 ref） */
    return res;
}

// ============================================================
// 会话密钥/上下文清理（reset 与 doFinishAndVerify 共用）
// ============================================================
void OtaRelay::clearSessionCryptoState()
{
    /* 抗优化清零：避免编译器去掉「无读回」的敏感缓冲擦除 */
    auto wipe = [](void *p, size_t n) {
        if (p == nullptr || n == 0U) {
            return;
        }
        volatile uint8_t *v = static_cast<volatile uint8_t *>(p);
        while (n > 0U) {
            *v++ = 0;
            --n;
        }
    };
    Protocol::sessionKeyClear(&m_sessionCtx);
    wipe(&m_sessionCtx, sizeof(m_sessionCtx));
    wipe(m_kSession, sizeof(m_kSession));
    wipe(m_nonceServer, sizeof(m_nonceServer));
    wipe(m_gd32UidRuntime, sizeof(m_gd32UidRuntime));
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
    m_transferDeadline = 0;
    clearSessionCryptoState();
    SpiMaster::instance().setSuppressRingService(false);
    DEBUG_INFO("OTA", "状态机已复位 → IDLE");
}

/**
 * @brief 向 GD32 发送 OTA CANCEL，驱动远端 resetToIdle（含 CREDIT/缓冲清理）。
 *
 * 失败路径必须调用：本端 reset() 只清 ESP32 状态机，GD32 仍可能停在
 * WAITING_RANDOM / WAITING_CONFIRM 等半会话，导致下次握手/进度错乱。
 */
esp_err_t OtaRelay::sendRemoteCancel()
{
    uint8_t resp_type = 0;
    uint8_t resp_payload[8] = {0};
    size_t resp_len = 0;
    /* payload 可空；GD32 CANCEL 分支无条件 resetToIdle 并 ACK 0x03 */
    esp_err_t err = SpiMaster::instance().transact(
        SPI_OTA_CMD_CANCEL, nullptr, 0,
        &resp_type, resp_payload, sizeof(resp_payload), &resp_len,
        1500U);
    if (err != ESP_OK) {
        DEBUG_WARN("OTA", "远端 CANCEL 发送失败: %s（本端仍复位）", esp_err_to_name(err));
        return err;
    }
    DEBUG_INFO("OTA", "远端 CANCEL 已发送, resp t=0x%02X n=%zu", resp_type, resp_len);
    return ESP_OK;
}

void OtaRelay::abortRemoteAndReset(bool notify_remote)
{
    if (notify_remote) {
        /* 中继失败时 suppress 可能仍为 true，CANCEL 需能走 SPI 事务 */
        (void)sendRemoteCancel();
    }
    reset();
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
    /* 默认 logLevel=INFO 时 DBG 不输出；勿先建 hex[193] 白占 ota_relay 栈 */
    if (DebugLog::instance().getLogLevel() < DEBUG_LOG_DBG) {
        return;
    }
    char hx[17] = {0};
    size_t n = (len > 8U) ? 8U : len;
    if (payload) {
        for (size_t i = 0; i < n; ++i) {
            snprintf(&hx[i * 2], sizeof(hx) - i * 2, "%02X", payload[i]);
        }
    } else {
        n = 0;
    }
    /* stage 名截断，避免整行超 128B */
    char stg[12];
    snprintf(stg, sizeof(stg), "%.11s", stage ? stage : "?");
    DEBUG_DBG("OTA", "%s rsp t=%02X n=%u",
              stg, resp_type, (unsigned)len);
    if (n > 0) {
        DEBUG_DBG("OTA", "  hx=%s%s", hx, len > n ? ".." : "");
    }
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
        /* 进度/等待文案只走前端；此处不打 INFO，避免 /logs 刷屏 */
        esp_err_t err = SpiMaster::instance().otaPollResponse(resp_type, resp_payload, resp_max, resp_len, timeout_ms);
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
        if (*resp_type == SPI_OTA_RSP_ACK) {
            if (matcher(resp_payload, *resp_len)) return ESP_OK;
            uint32_t err_seq = 0; uint8_t err_code = 0;
            if (parseStructuredError(resp_payload, *resp_len, &err_seq, &err_code)) return ESP_OK;
            DEBUG_WARN("OTA", "%s stale ACK, poll more: len=%u", stage, (unsigned)*resp_len);
        } else if (*resp_type == SPI_OTA_RSP_ERROR || *resp_type == 0x7FU) {
            /* 明确错误帧：交给调用方判失败，不再空转 poll */
            return ESP_OK;
        } else {
            /* 非 ACK（含 0xC1 事件/空 type）：继续 poll 等延迟响应，勿提前返回 */
            DEBUG_WARN("OTA", "%s unexpected type=0x%02X, poll more", stage, *resp_type);
        }
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
    /* 单次 init/setkey 加密 3 块，避免每块重建 AES 上下文（转发热路径） */
    if (!key || !in || !out) {
        return false;
    }
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    if (esp_aes_setkey(&ctx, key, 128) != 0) {
        esp_aes_free(&ctx);
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (esp_aes_crypt_ecb(&ctx, ESP_AES_ENCRYPT, in + i * 16, out + i * 16) != 0) {
            esp_aes_free(&ctx);
            return false;
        }
    }
    esp_aes_free(&ctx);
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

    /* 1) 探测：已在 BOOT 则零开销直接成功 */
    esp_err_t hello_err = SpiMaster::instance().hello(1500U, 0U);
    if (hello_err == ESP_OK && SpiMaster::instance().gd32Role() == V2_ROLE_BOOT) {
        DEBUG_INFO("OTA", "GD32 已在 Bootloader (HELLO role=boot)，跳过 ENTER_BOOT");
        return ESP_OK;
    }

    /* 2) 发 ENTER_BOOT（ACK 可选；发送后静默，门控是 BOOT HELLO） */
    if (hello_err != ESP_OK) {
        DEBUG_WARN("OTA", "探测 HELLO 未成功: %s，仍尝试 ENTER_BOOT",
                   esp_err_to_name(hello_err));
    }
    esp_err_t boot_err = SpiMaster::instance().appRequestBootloader();
    if (boot_err != ESP_OK) {
        DEBUG_WARN("OTA", "ENTER_BOOT 发送失败: %s", esp_err_to_name(boot_err));
    }

    /* 3) 硬门控：HELLO role=boot（成功时 SPI waitForBootloaderReady 已打就绪日志） */
    esp_err_t wait_err = SpiMaster::instance().waitForBootloaderReady(12000);
    if (wait_err == ESP_OK) {
        return ESP_OK;
    }

    /* 4) 一次重试：仍为 APP 时再 HELLO + ENTER_BOOT */
    DEBUG_WARN("OTA", "BOOT HELLO 超时，二次 HELLO+ENTER_BOOT 重试...");
    hello_err = SpiMaster::instance().hello(2000U, 0U);
    if (hello_err == ESP_OK && SpiMaster::instance().gd32Role() == V2_ROLE_BOOT) {
        DEBUG_INFO("OTA", "二次探测已在 Bootloader，继续");
        return ESP_OK;
    }
    boot_err = SpiMaster::instance().appRequestBootloader();
    if (boot_err != ESP_OK) {
        DEBUG_WARN("OTA", "二次 ENTER_BOOT 未确认 ACK: %s（仍等 BOOT HELLO）",
                   esp_err_to_name(boot_err));
    }
    wait_err = SpiMaster::instance().waitForBootloaderReady(12000);
    if (wait_err != ESP_OK) {
        DEBUG_ERR("OTA", "Bootloader HELLO 就绪失败(重试后): %s", esp_err_to_name(wait_err));
        return wait_err;
    }
    /* 成功日志由 SPI waitForBootloaderReady 统一输出 */
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
    constexpr uint32_t kHandshakeReqTimeoutMs = 800U;
    constexpr uint32_t kHandshakePollTimeoutMs = 800U;
    constexpr uint32_t kHandshakeRetryGapMs = 50U;

    for (uint32_t attempt = 0U; attempt < attempts; ++attempt) {
        uint8_t resp_type = 0U;
        uint8_t resp_payload[64] = {0};
        size_t resp_len = 0U;

        /* 每若干次失败做一次 HELLO 重同步（session/从机状态） */
        if (attempt > 0U && (attempt % 5U) == 0U) {
            esp_err_t hr = SpiMaster::instance().hello(1500U, V2_ROLE_BOOT);
            DEBUG_WARN("OTA", "HANDSHAKE 前 HELLO 重同步: %s role=%u",
                       esp_err_to_name(hr), SpiMaster::instance().gd32Role());
        }

        /* transact 已含延迟一拍两步法；再经 pollUntil 处理 WAIT/陈旧 ACK。 */
        const uint32_t total_timeout_ms = kHandshakeReqTimeoutMs + kHandshakePollTimeoutMs;
        esp_err_t err = SpiMaster::instance().transact(SPI_OTA_CMD_REQ, payload, sizeof(payload),
                                         &resp_type, resp_payload, sizeof(resp_payload), &resp_len,
                                         total_timeout_ms);
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

        DEBUG_WARN("OTA", "HANDSHAKE重试 %" PRIu32 "/%" PRIu32 " err=%d t=0x%02X",
                   attempt + 1U, attempts, last_err, last_resp_type);
        vTaskDelay(pdMS_TO_TICKS(kHandshakeRetryGapMs));
    }

    DEBUG_ERR("OTA", "HANDSHAKE_REQ 失败, err=%d, resp_type=0x%02X, resp_len=%zu",
              last_err, last_resp_type, last_resp_len);
    return OTA_RESULT_ERR_TIMEOUT;
}

/**
 * @brief 统一 SPI 事务 + 轮询 ACK 形状的 helper（CHALLENGE / CONFIRM / SEND_SIZE 共用）。
 * @return OTA_RESULT_OK 成功；OTA_RESULT_ERR_TIMEOUT 发送或轮询失败。
 */
ota_result_t OtaRelay::sendAndWaitAckShape(const char *stage, uint8_t cmd,
                                           const uint8_t *payload, size_t payload_len,
                                           uint32_t transact_timeout_ms,
                                           uint32_t poll_timeout_ms, uint32_t wait_rounds, uint32_t max_extra_polls,
                                           uint8_t resp_payload[], size_t resp_max, size_t *resp_len,
                                           bool (*matcher)(const uint8_t *, size_t))
{
    uint8_t resp_type = 0;
    esp_err_t err = SpiMaster::instance().transact(cmd, payload, payload_len,
                                                   &resp_type, resp_payload, resp_max, resp_len, transact_timeout_ms);
    logRespPayload(stage, resp_type, resp_payload, *resp_len);
    /* transact 已超时/失败时勿再 pollUntil：否则 resp_type 仍为 0，会刷
     * "unexpected type=0x00" 并二次空等，掩盖真实超时。 */
    if (err == ESP_OK) {
        err = pollUntilExpectedAckShape(stage, &resp_type, resp_payload, resp_max, resp_len,
                                        poll_timeout_ms, wait_rounds, max_extra_polls, matcher);
    }
    if (err != ESP_OK || resp_type != SPI_OTA_RSP_ACK || !matcher(resp_payload, *resp_len)) {
        DEBUG_ERR("OTA", "%s fail err=%d t=0x%02X n=%zu",
                  stage, err, resp_type, *resp_len);
        return OTA_RESULT_ERR_TIMEOUT;
    }
    return OTA_RESULT_OK;
}

/** @brief 协议子步骤：执行挑战认证（发送 nonce、验证响应、派生会话密钥、发送确认）。 */
ota_result_t OtaRelay::doChallengeAndAuth()
{
    uint8_t resp_payload[64] = {0};
    size_t resp_len = 0;

    uint32_t timestamp = xTaskGetTickCount();
    uint32_t mem_addr = (uint32_t)(uintptr_t)m_nonceServer;
    uint32_t rand_seed = 0x12345678;

    Protocol::instance().scrambleGenerate(timestamp, mem_addr, rand_seed, m_nonceServer);

    DEBUG_INFO("OTA", "--- 认证步骤 1: 发送 CHALLENGE ---");
    DEBUG_INFO("OTA", "发送 nonce_server (16B), timestamp=0x%08" PRIX32, timestamp);

    ota_result_t res = sendAndWaitAckShape("CHALLENGE", SPI_OTA_CMD_REQ,
                                           m_nonceServer, 16, 5000,
                                           1000, 12, 8,
                                           resp_payload, sizeof(resp_payload), &resp_len,
                                           OtaRelay::isChallengePayloadAck);
    if (res != OTA_RESULT_OK) return res;

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

    res = sendAndWaitAckShape("CONFIRM", SPI_OTA_CMD_REQ,
                              encrypted_confirm, 16, 3000,
                              1000, 8, 8,
                              resp_payload, sizeof(resp_payload), &resp_len,
                              OtaRelay::isConfirmPlainAck);
    if (res != OTA_RESULT_OK) return res;

    DEBUG_INFO("OTA", "CONFIRM ok, resp_len=%zu", resp_len);

    Protocol::sessionKeyInit(&m_sessionCtx, m_kSession);
    return OTA_RESULT_OK;
}

/** @brief 协议子步骤：发送固件总大小给 GD32 并等待 seq=0 的确认。 */
ota_result_t OtaRelay::doSendFirmwareSize(uint32_t size)
{
    uint8_t resp_payload[32] = {0};
    size_t resp_len = 0;

    DEBUG_INFO("OTA", "固件大小: %" PRIu32 " 字节", size);

    uint8_t size_payload[4];
    size_payload[0] = (uint8_t)(size & 0xFF);
    size_payload[1] = (uint8_t)((size >> 8) & 0xFF);
    size_payload[2] = (uint8_t)((size >> 16) & 0xFF);
    size_payload[3] = (uint8_t)((size >> 24) & 0xFF);

    /* GD32 收到 SIZE 后擦 Flash，先 WAIT 再 ACK(seq=0)；进度由前端读 status，不打过程 INFO */
    ota_result_t res = sendAndWaitAckShape("SEND_SIZE", SPI_OTA_CMD_DATA,
                                           size_payload, sizeof(size_payload), 3000,
                                           500, 120, 40,
                                           resp_payload, sizeof(resp_payload), &resp_len,
                                           OtaRelay::isSizeSeq0Ack);
    if (res != OTA_RESULT_OK) return res;

    SpiMaster::instance().confirmTxSeqAccepted();
    DEBUG_INFO("OTA", "大小协商完成: %" PRIu32 " 字节，GD32 已就绪收包", size);
    return OTA_RESULT_OK;
}

/**
 * @brief 准备单包：读 Flash → AES → 填 64B tx_buf。
 * flash_offset 显式传入，便于在等 ACK 时预组下一包（不推进 m_transferFlashOffset）。
 */
ota_result_t OtaRelay::prepareRelayPacket(uint32_t seq, uint32_t flash_offset, const ota_meta_t &meta,
                                          uint8_t tx_buf[64], size_t *out_chunk_size)
{
    if (!tx_buf) {
        return OTA_RESULT_ERR_STORAGE;
    }

    size_t chunk_size = GD32_OTA_PACKET_SIZE;
    if (seq + 1U >= m_transferTotalPackets) {
        const uint32_t done = seq * GD32_OTA_PACKET_SIZE;
        if (done >= meta.total_size) {
            return OTA_RESULT_ERR_STORAGE;
        }
        chunk_size = meta.total_size - done;
    }

    uint8_t plain48[GD32_OTA_PACKET_SIZE];
    /* 仅末包需要 0xFF 填充；满包直接读满 48B */
    if (chunk_size < GD32_OTA_PACKET_SIZE) {
        memset(plain48, 0xFF, sizeof(plain48));
    }

    esp_err_t err = Gd32FlashStore::instance().flashReadChunk(flash_offset, plain48, chunk_size);
    if (err != ESP_OK) {
        DEBUG_ERR("OTA", "Flash 读取失败, offset=%" PRIu32 ", err=%d", flash_offset, err);
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

    const uint32_t if_end = (seq + 1U >= m_transferTotalPackets) ? 0xFFFFFFFFU : 0x00000000U;
    tx_buf[56] = (uint8_t)(if_end & 0xFFU);
    tx_buf[57] = (uint8_t)((if_end >> 8) & 0xFFU);
    tx_buf[58] = (uint8_t)((if_end >> 16) & 0xFFU);
    tx_buf[59] = (uint8_t)((if_end >> 24) & 0xFFU);
    calculateCrc32WithKey(m_kSession, plain48, &tx_buf[60]);

    DEBUG_DBG("OTA", "包%" PRIu32 " n=%u end=%08" PRIX32,
              seq, (unsigned)chunk_size, if_end);

    /* 不在此写 m_transferCurrentSeq：预组下一包时不应污染“当前发送序号” */
    if (out_chunk_size) {
        *out_chunk_size = chunk_size;
    }
    return OTA_RESULT_OK;
}

bool OtaRelay::sendRelayPacketWithRetry(uint32_t seq, const uint8_t tx_buf[64], size_t chunk_size,
                                        ota_result_t *out_result, NextPacketPrep *next_prep)
{
    uint8_t resp_type = 0;
    uint8_t resp_payload[32] = {0};
    size_t resp_len = 0;
    const uint32_t expected_ack = (seq == m_transferTotalPackets - 1) ? kLastPacketAckSeq : (seq + 1U);
    /*
     * 协议要点（v2，GD32 侧缓存修复后）：
     * - GD32 ISR 收 DATA 后立刻回 WAIT，主循环 AES/Flash 后再回 ACK。
     * - 一旦见过 WAIT，包已进从机；但那帧 ACK 可能因时序（尤其 8MHz）丢失。
     * - GD32 主循环发 ACK 时已同步更新重发缓存（见 SpiOta::sender），
     *   且主循环对已写入包号去重（receive_packet_num < next_expected_packet 只回 ACK，
     *   不二次写 flash）——故重发 DATA 安全。
     * - 因此：见 WAIT 后若 poll 超时，允许有限次重发 DATA 触发 DUP-SEQ 回放最终 ACK，
     *   实现“重发快且不漏”（旧逻辑见 WAIT 后只能干 poll 到 stall，ACK 丢了就永丢）。
     * - 等 ACK 期间可预组下一包（与从机处理重叠）。
     */
    constexpr uint32_t kMaxDataSends = 5U;
    /* 单次发送后等 ACK 上限：GD32 正常 ms 级回 ACK（ISR 回 WAIT、主循环回 ACK），
     * 50ms 未到即重发（DUP-SEQ 回放捞回），不再干等 6s（用户指定 50ms）。
     * credit==0 的排空积压由 transact 内部 credit 背压消化，此处无需长等。 */
    constexpr uint32_t kAckWaitMs = 50U;
    constexpr uint32_t kPollSliceMs = 50U;   /* 略紧：DATA_READY 驱动，缩短空切片 */

    TickType_t pkt_start = xTaskGetTickCount();
    bool saw_wait = false;

    auto maybe_prep_next = [&]() {
        if (!next_prep || !next_prep->ready || *next_prep->ready) {
            return;
        }
        if (!next_prep->meta || !next_prep->buf || !next_prep->chunk_size) {
            return;
        }
        if (next_prep->next_seq >= m_transferTotalPackets) {
            return;
        }
        ota_result_t pr = prepareRelayPacket(next_prep->next_seq, next_prep->next_flash_offset,
                                             *next_prep->meta, next_prep->buf, next_prep->chunk_size);
        if (pr == OTA_RESULT_OK) {
            *next_prep->ready = true;
        }
        /* 预组失败不致命：外层会再 prepare */
    };

    for (uint32_t send_i = 0; send_i < kMaxDataSends; ++send_i) {
        /* saw_wait 后仍允许重发 DATA：触发 GD32 DUP-SEQ 回放最终 ACK（缓存已修复）。
         * 重发的 DATA 进 GD32 主循环会因包号去重只回 ACK，不二次写 flash。 */
        {
            resp_len = 0;
            resp_type = 0;
            /* 等首响应上限 10ms：一次 SPI 事务最多 5ms（用户指定），
             * 超时立即进入下方 50ms poll 窗口，不再干等 200ms。 */
            esp_err_t err = SpiMaster::instance().transact(
                SPI_OTA_CMD_DATA, tx_buf, 64,
                &resp_type, resp_payload, sizeof(resp_payload), &resp_len, 10);
            logRespPayload("DATA_PACKET", resp_type, resp_payload, resp_len);
            if (err == ESP_OK && resp_type == SPI_OTA_RSP_WAIT) {
                saw_wait = true;
                /* 从机已收下本包，立刻利用等待时间预组下一包 */
                maybe_prep_next();
            }
            if (err == ESP_OK && resp_type == SPI_OTA_RSP_ACK &&
                isStructuredAckWithSeq(resp_payload, resp_len, expected_ack)) {
                SpiMaster::instance().confirmTxSeqAccepted();
                m_transferFlashOffset += (uint32_t)chunk_size;
                return false;
            }
        }

        const TickType_t poll_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kAckWaitMs);
        TickType_t last_hb = xTaskGetTickCount();
        while (xTaskGetTickCount() < poll_deadline) {
            if ((xTaskGetTickCount() - last_hb) >= pdMS_TO_TICKS(1000)) {
                DEBUG_INFO("OTA", "包%" PRIu32 "等ACK send=%" PRIu32 " t=0x%02X cr=%u",
                           seq, send_i, resp_type,
                           (unsigned)SpiMaster::instance().remoteCredit());
                last_hb = xTaskGetTickCount();
            }

            /* 优先在 poll 间隙预组，再读 SPI（READY 未拉时也能吃掉 Flash/AES 时间） */
            maybe_prep_next();

            esp_err_t err = SpiMaster::instance().otaPollResponse(
                &resp_type, resp_payload, sizeof(resp_payload), &resp_len, kPollSliceMs);

            if (err == ESP_OK && resp_type == SPI_OTA_RSP_WAIT) {
                saw_wait = true;
                continue;
            }
            if (err == ESP_OK && resp_type == SPI_OTA_RSP_ACK) {
                if (isStructuredAckWithSeq(resp_payload, resp_len, expected_ack)) {
                    SpiMaster::instance().confirmTxSeqAccepted();
                    m_transferFlashOffset += (uint32_t)chunk_size;
                    return false;
                }
                uint32_t err_seq = 0;
                uint8_t err_code = 0;
                if (parseStructuredError(resp_payload, resp_len, &err_seq, &err_code) &&
                    err_seq == seq) {
                    DEBUG_WARN("OTA", "包 %" PRIu32 " 结构化错误 err=0x%02X", seq, err_code);
                    if (err_code == 0x04U) {
                        *out_result = OTA_RESULT_ERR_CRC;
                        return true;
                    }
                    saw_wait = false; /* 允许带错误码的重发 */
                    /* 结构化错误后下一包内容可能变化，丢弃预组 */
                    if (next_prep && next_prep->ready) {
                        *next_prep->ready = false;
                    }
                    break;
                }
                /* 陈旧 ACK，继续 poll */
            }
        }

        /* 超时：无论是否见过 WAIT 都允许重发 DATA。
         * 见过 WAIT 时重发靠 GD32 DUP-SEQ 回放缓存的最终 ACK 来捞回丢失的 ACK；
         * 未见 WAIT 时重发则是重新送注。两者均安全（GD32 对已写包号去重）。 */
        if (send_i + 1U < kMaxDataSends) {
            DEBUG_WARN("OTA", "包 %" PRIu32 " 超时重发 DATA (%" PRIu32 "/%" PRIu32 ") saw_wait=%d",
                       seq, send_i + 1U, kMaxDataSends, (int)saw_wait);
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    DEBUG_ERR("OTA", "包%" PRIu32 "失败 el=%" PRIu32 "ms t=0x%02X cr=%u",
              seq,
              (uint32_t)((xTaskGetTickCount() - pkt_start) * portTICK_PERIOD_MS),
              resp_type,
              (unsigned)SpiMaster::instance().remoteCredit());
    *out_result = OTA_RESULT_ERR_TIMEOUT;
    return true;
}

ota_result_t OtaRelay::doRelayPackets(TickType_t deadline_tick)
{
    ota_meta_t meta;
    if (Gd32FlashStore::instance().metaLoad(&meta) != ESP_OK) {
        DEBUG_ERR("OTA", "元数据加载失败");
        return OTA_RESULT_ERR_STORAGE;
    }

    /* 双缓冲：当前发送槽 + 预组槽（等 ACK 时填充） */
    uint8_t tx_slot[2][64] = {};
    size_t chunk_slot[2] = {0, 0};
    int cur = 0;
    bool next_ready = false;
    size_t next_chunk = 0;

    uint32_t flash_off = 0;
    m_transferFlashOffset = 0;

    ota_result_t result = prepareRelayPacket(0, flash_off, meta, tx_slot[cur], &chunk_slot[cur]);
    if (result != OTA_RESULT_OK) {
        return result;
    }

    TickType_t last_success_tick = xTaskGetTickCount();
    /* 进度：首包 / 每 8 包 / 末包更新，减 status 锁竞争 */
    constexpr uint32_t kSeqReportMask = 7U;

    for (uint32_t seq = 0; seq < m_transferTotalPackets; seq++) {
        if ((seq % OTA_PACKET_HEALTH_CHECK_INTERVAL) == 0 ||
            seq == 0 || seq + 1U >= m_transferTotalPackets) {
            health_status_t health = HealthMonitor::instance().checkBeforeOtaRelay();
            if (health == HEALTH_MON_SHUTDOWN || health == HEALTH_MON_CRITICAL) {
                DEBUG_ERR("OTA", "健康检查危险，中止 OTA");
                return OTA_RESULT_ERR_HEALTH;
            }
        }

        if (deadline_tick != 0 && xTaskGetTickCount() >= deadline_tick) {
            DEBUG_ERR("OTA", "全局超时: 包 %" PRIu32 "/%" PRIu32 " 已超限，中止转发",
                      seq, m_transferTotalPackets);
            return OTA_RESULT_ERR_TIMEOUT;
        }

        if ((xTaskGetTickCount() - last_success_tick) >= pdMS_TO_TICKS(kStallTimeoutMs)) {
            DEBUG_ERR("OTA", "%" PRIu32 " ms 无成功帧, 中止转发: seq=%" PRIu32 "/%" PRIu32,
                      kStallTimeoutMs, seq, m_transferTotalPackets);
            return OTA_RESULT_ERR_TIMEOUT;
        }

        /* 当前包必须已在 cur 槽；若预组未命中则现场准备 */
        if (seq > 0U) {
            if (next_ready) {
                cur ^= 1;
                chunk_slot[cur] = next_chunk;
                next_ready = false;
            } else {
                result = prepareRelayPacket(seq, flash_off, meta, tx_slot[cur], &chunk_slot[cur]);
                if (result != OTA_RESULT_OK) {
                    return result;
                }
            }
        }

        m_transferCurrentSeq = seq;
        if ((seq & kSeqReportMask) == 0U || seq + 1U >= m_transferTotalPackets) {
            setRelaySeq(seq);
        }

        NextPacketPrep prep{};
        const int prep_idx = cur ^ 1;
        prep.next_seq = seq + 1U;
        prep.next_flash_offset = flash_off + (uint32_t)chunk_slot[cur];
        prep.meta = &meta;
        prep.buf = tx_slot[prep_idx];
        prep.chunk_size = &next_chunk;
        prep.ready = &next_ready;
        NextPacketPrep *prep_arg = (seq + 1U < m_transferTotalPackets) ? &prep : nullptr;

        if (sendRelayPacketWithRetry(seq, tx_slot[cur], chunk_slot[cur], &result, prep_arg)) {
            DEBUG_ERR("OTA", "中止于包 %" PRIu32 "/%" PRIu32 " result=%d",
                      seq, m_transferTotalPackets, (int)result);
            return result;
        }

        flash_off += (uint32_t)chunk_slot[cur];
        /* 与 sendRelayPacketWithRetry 内累加对齐（绝对赋值，防漂移） */
        m_transferFlashOffset = flash_off;

        if ((seq & kSeqReportMask) == 0U || seq + 1U >= m_transferTotalPackets) {
            setRelaySeq(seq + 1U);
        }
        last_success_tick = xTaskGetTickCount();
    }
    setRelaySeq(m_transferTotalPackets);
    DEBUG_INFO("OTA", "全部数据包已转发: %" PRIu32 " 包", m_transferTotalPackets);
    return OTA_RESULT_OK;
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
        // 启动探测等场景用 DBG；OTA 成功路径由 doWaitForAppAlive 统一 INFO 总结
        DEBUG_DBG("OTA", "GD32 APP up=%" PRIu32 "s APP=v%d.%d.%d.%d",
                  uptime,
                  ver.app_major, ver.app_minor, ver.app_patch, ver.app_build);
        return true;
    }
    if (out_uptime) *out_uptime = 0;
    DEBUG_WARN("OTA", "GD32 APP 探测失败: %s", esp_err_to_name(err));
    return false;
}

/**
 * @brief 统一 "HELLO(role=APP) → 可选 PING 探测" 循环（doWaitForAppAlive + taskEntry 共用）。
 *
 * 两处原循环差异仅：taskEntry 在 HELLO 已带身份(gd32IdKnown)时跳过 PING 直接成功。
 * 用 allow_hello_shortcut 参数统一：true 时启用 HELLO 身份快路径。
 *
 * @param timeout_ms      PING 超时
 * @param ping_interval_ms 每轮间隔
 * @param max_attempts    最大轮数
 * @param allow_hello_shortcut  true=HELLO 带身份即成功（taskEntry 场景）
 * @return true=APP 存活已确认
 */
bool OtaRelay::helloAndProbeApp(uint32_t timeout_ms, uint32_t ping_interval_ms,
                                uint32_t max_attempts, bool allow_hello_shortcut)
{
    bool session_ok = false;
    for (uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(ping_interval_ms));

        if (!session_ok) {
            if (SpiMaster::instance().hello(1500U, V2_ROLE_APP) == ESP_OK &&
                SpiMaster::instance().gd32Role() == V2_ROLE_APP) {
                session_ok = true;
                /* HELLO 已带身份时直接缓存版本/开关，无需再等 PING 才亮版本 */
                if (allow_hello_shortcut && SpiMaster::instance().gd32IdKnown()) {
                    StateLockGuard lock;
                    gd32Version  = SpiMaster::instance().gd32VersionCached();
                    gd32AppAlive = true;
                    DEBUG_INFO("OTA", "HELLO就绪 APP=v%d.%d.%d.%d",
                               gd32Version.app_major, gd32Version.app_minor,
                               gd32Version.app_patch, gd32Version.app_build);
                    DEBUG_INFO("OTA", "HELLO就绪 BOOT=v%d.%d.%d.%d",
                               gd32Version.boot_major, gd32Version.boot_minor,
                               gd32Version.boot_patch, gd32Version.boot_build);
                    return true;
                }
            } else {
                DEBUG_INFO("OTA", "等待 GD32 APP 启动... (%" PRIu32 "/%u)", attempt + 1U, (unsigned)max_attempts);
                continue;
            }
        }
        /* HELLO 成功但无身份（旧固件），或 doWaitForAppAlive 路径：回退 APP PING */
        if (probeAndCacheAppAlive(timeout_ms)) {
            return true;
        }
        /* PING 失败：清 session，下轮重 HELLO（避免 sticky 假会话） */
        session_ok = false;
        DEBUG_INFO("OTA", "等待 GD32 APP 存活... (%" PRIu32 "/%u)", attempt + 1U, (unsigned)max_attempts);
    }
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

        esp_err_t err = SpiMaster::instance().transact(SPI_OTA_CMD_DATA, finish_payload, sizeof(finish_payload),
                                         &resp_type, resp_payload, sizeof(resp_payload), &resp_len,
                                         CONFIG_DCU_OTA_FINISH_TIMEOUT_MS);
        logRespPayload("FINISH_OTA", resp_type, resp_payload, resp_len);
        if (err != ESP_OK) {
            DEBUG_WARN("OTA", "FINISH_OTA 传输失败, err=%d, retry=%" PRIu32 "/10", err, retry + 1);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        err = pollUntilExpectedAckShape("FINISH_OTA", &resp_type, resp_payload,
                                            sizeof(resp_payload), &resp_len,
                                            1000, 10, 8, OtaRelay::isFinishPlainAck);
        if (err != ESP_OK) {
            DEBUG_WARN("OTA", "FINISH_OTA poll 失败, err=%d, retry=%" PRIu32 "/10", err, retry + 1);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (resp_type == SPI_OTA_RSP_ACK && isFinishPlainAck(resp_payload, resp_len)) {
            break;
        }

        if (resp_type == SPI_OTA_RSP_WAIT) {
            DEBUG_INFO("OTA", "FINISH_OTA 收到 WAIT, GD32 正在排空 Flash 缓冲, retry=%" PRIu32 "/10", retry + 1);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint32_t err_seq = 0;
        uint8_t err_code = 0;
        if (parseStructuredError(resp_payload, resp_len, &err_seq, &err_code)) {
            DEBUG_ERR("OTA", "FINISH_OTA 收到错误帧 err=0x%02X, retry=%" PRIu32 "/10", err_code, retry + 1);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        DEBUG_WARN("OTA", "FINISH_OTA异常 t=0x%02X n=%zu r=%" PRIu32,
                   resp_type, resp_len, retry + 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (resp_type != SPI_OTA_RSP_ACK || !isFinishPlainAck(resp_payload, resp_len)) {
        DEBUG_ERR("OTA", "FINISH_OTA 失败, err=0, resp_type=0x%02X, resp_len=%zu", resp_type, resp_len);
        return OTA_RESULT_ERR_GD32;
    }

    return OTA_RESULT_OK;
}

/** @brief 协议子步骤：轮询等待 GD32 APP 存活。 */
ota_result_t OtaRelay::doWaitForAppAlive(uint32_t timeout_ms)
{
    setGd32AppAlive(false);

    /* FINISH 后短暂安静，避免 jump/APP SPI arm 窗口被 HELLO 打乱（对齐既有 300ms 节拍）。 */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* doWaitForAppAlive 不启用 HELLO 身份快路径：OTA 完成后必须 PING 确认 APP 真正运行 */
    if (!helloAndProbeApp(timeout_ms, 300U, 20U, /*allow_hello_shortcut=*/false)) {
        DEBUG_ERR("OTA", "GD32 APP 启动确认超时，OTA 失败");
        /* APP 未起来时 CANCEL 可能无效，仍尽力清理 BOOT 半会话 */
        abortRemoteAndReset(true);
        return OTA_RESULT_ERR_GD32;
    }

    /* 版本已在 HELLO/PING 身份（SPI ID APP/BOOT）打印，此处不重复 */
    DEBUG_INFO("OTA", "GD32 OTA完成");
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
    clearSessionCryptoState();

    // 元数据清理安静进行；等待/成功总结由 doWaitForAppAlive 统一输出
    Gd32FlashStore::instance().metaClear();

    // 阶段2: APP 存活轮询
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

    /* 启动 GD32 探测：先 hello 建 v2 session，再 appPing。 */
    {
        DEBUG_INFO("OTA", "启动初始 GD32 探测 (hello+probe)...");
        if (!helloAndProbeApp(2000U, 300U, 20U, /*allow_hello_shortcut=*/true)) {
            DEBUG_WARN("OTA", "启动 GD32 探测未成功（中继前仍可再 hello）");
        }
    }

    while (1) {
        uint32_t msg = 0;
        BaseType_t ret = xQueueReceive(dispatchQueue, &msg, portMAX_DELAY);
        if (ret != pdTRUE) {
            continue;
        }

        DEBUG_INFO("OTA", "收到 OTA 调度信号: %" PRIu32 "，开始转发...", msg);
        /* 尽早抑制 ring service：onRelayStarted 可能因协调器锁（/system/status
         * 等争锁）阻塞，此窗口内 worker 会把上一阶段残留 RESPONSE 当 EVENT 偷读
         * （线上 log#1：TRANSFERRING 起始瞬间 serviceRing 读到 type=0x80）。
         * forwardFirmware 不再重复设置；释放统一走 reset()。 */
        SpiMaster::instance().setSuppressRingService(true);
        // 通过 IRelayListener 接口回调通知协调器
        IRelayListener *listener = getRelayListener();
        if (listener) listener->onRelayStarted();
        ota_result_t result = instance().forwardFirmware();

        uint32_t result_msg = (uint32_t)result;
        xQueueSend(resultQueue, &result_msg, pdMS_TO_TICKS(100));

        if (result == OTA_RESULT_OK) {
            // 成功日志已在 doWaitForAppAlive 输出，立即通知协调器置完成态
            instance().reset();  /* 本端清会话；GD32 已跳 APP */
        } else if (result == OTA_RESULT_ERR_TIMEOUT) {
            DEBUG_ERR("OTA", "OTA超时中止 TIMEOUT=%d", (int)result);
            /* forwardFirmware 失败路径已 abortRemoteAndReset；再调一次无害（二次 CANCEL） */
            instance().abortRemoteAndReset(true);
        } else if (result == OTA_RESULT_ERR_USER_ABORT) {
            DEBUG_ERR("OTA", "OTA 用户中止 (USER_ABORT=%d)", (int)result);
            instance().abortRemoteAndReset(true);
        } else {
            DEBUG_ERR("OTA", "OTA 转发失败，错误码: %d", (int)result);
            instance().abortRemoteAndReset(true);
        }
        // 通过 IRelayListener 接口回调通知协调器中继结束
        listener = getRelayListener();
        if (listener) listener->onRelayComplete(result == OTA_RESULT_OK);
    }
}
