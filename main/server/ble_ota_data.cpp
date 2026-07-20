/*
 * ble_ota_data.cpp - BLE OTA 数据路径：reorder、写 Flash、worker、DATA_IN
 */
#include "ble_ota_priv.h"

// ============================================================
// ESP32 OTA 包序号顺序保护：Reorder Buffer（TCP 接收窗口模式）
//
// 背景：
//   esp_ota_write 是顺序追加写入，不支持 seek。
//   客户端窗口内使用 writeValueWithoutResponse（fire-and-forget），
//   BLE 空口会导致同一窗口的包乱序到达设备端。
//
// 策略（窗口级 reorder buffer）：
//   - seq < expected                → 重复包，丢弃
//   - seq == expected               → 写入 flash，expected++，
//                                     并 flush 连续紧邻的缓存包
//   - seq > expected 且在窗口范围内 → 暂存到 slot，不写 flash
//   - seq 超出窗口范围              → 防御性丢弃 + ACK 报告缺口
//
// 与旧"丢弃策略"的区别：乱序包不再丢弃，而是暂存到 buffer，
// 当缺口（expected）的包到达时按序 flush，从而吸收窗口内乱序，
// 彻底消除"重传包与原发包再次乱序"导致的重传风暴。
//
// 内存：slots 指针/长度数组在 OTA begin 时分配（常驻 ~384B），
//       每个 slot 的 payload 缓冲区按需 lazy malloc（最多 480B），
//       flush 后立即 free。OTA 结束/abort 时统一释放。
//       若 begin 时分配失败 → active=false，自动回退到丢弃策略。
//
// ACK 协议不变：getReceived() 仅在真正按序写入 flash 时增长，
//              客户端 resumeSeq = ceil(bytes/payloadSize) 仍正确。
// ============================================================

/**
  * @brief ESP32 OTA reorder buffer 状态。
  *        窗口级缓存，吸收 BLE 乱序到达的包。
  */
struct Esp32OtaReorderBuf {
    bool      active = false;      ///< 是否已启用（分配成功）
    bool      fallback = false;    ///< 是否回退到丢弃策略（buffer 分配失败时）
    uint32_t  expected = 0;        ///< 下一个期望按序写入的 seq
    uint32_t  windowBase = 0;      ///< 当前窗口起始 seq（= 上次 flush 后的 expected）
    uint32_t  windowSize = 0;      ///< 窗口大小（= BLE_OTA_REORDER_WINDOW）
    uint8_t  **slots = nullptr;    ///< slots[i]：窗口内第 i 个包的 payload 拷贝（nullptr=未占）
    size_t   *lens = nullptr;      ///< lens[i]：slots[i] 的 payload 长度
    size_t    payloadCap = 0;      ///< 每个 slot 的容量（= BLE_OTA_CHUNK_PAYLOAD）
    uint32_t  cachedCount = 0;     ///< 当前已缓存的乱序包数
};

Esp32OtaReorderBuf s_reorder;
uint32_t s_fallback_ooo_count = 0;  // 回退模式下自上次 ACK 以来的乱序包计数
uint32_t s_gd32_ooo_count = 0;      // GD32 路径自上次 ACK 以来的乱序/重复包计数
ota_meta_t s_ble_gd32_meta = {};
bool s_ble_gd32_meta_active = false;
bool s_conn_fallback_requested = false;

// ============================================================
// DATA_IN 丢包负反馈：把 malloc 失败 / 队列满从"静默丢包"变成
// "主动发缺口 ACK 让前端按 completed_bytes 续传退避"。
//
// 现状：writeValueWithoutResponse 窗口内丢包时，NimBLE 不会向对端
//       回 ATT Error Response（即发即忘），返回非零 ATT 错误对窗口内
//       包无效，只能走 DATA_OUT 通知通道发窗口 ACK。
//       静默丢包会让前端等满 8s 窗口超时后整窗口重发 → 重传风暴叠加
//       低堆时可能二次失败。这里在丢包路径补发一次缺口 ACK（seq 取
//       s_reorder.expected-1，completed 取实际落盘字节），让前端立刻
//       从 lastAckBytes 续传，客户端已有续传语义无需改动。
//
// 节流：丢包路径无"进度/窗口边界"可复用，用"丢包计数 ≥ ACK_WINDOW
//       或距上次 ≥ NEG_ACK_MIN_INTERVAL_US"双约束，防止重传叠加时
//       ACK 通知打爆 NimBLE mbuf 池（与现有正常路径节流同动机）。
//       状态仅在 NimBLE host task 回调中读写（单线程），无需锁。
// ============================================================
static uint32_t s_ota_drop_malloc_fail = 0;  // DATA_IN malloc 失败累计
static uint32_t s_ota_drop_queue_full  = 0;  // DATA_IN 队列满累计
static uint32_t s_neg_ack_sent         = 0;  // 负 ACK 发送累计
static uint32_t s_neg_ack_drop_count   = 0;  // 自上次发负 ACK 以来累计丢包数
static int64_t  s_last_neg_ack_us      = 0;  // 上次负 ACK 时刻（esp_timer_get_time）
// 节流常量：与正常 ACK 路径同窗口量级（BLE_OTA_ACK_WINDOW=64），
// 最小间隔 200ms 兜底，避免极端低堆时负 ACK 抖动。
static constexpr uint32_t NEG_ACK_DROP_WINDOW   = 64;       // 丢包数阈值
static constexpr int64_t  NEG_ACK_MIN_INTERVAL_US = 200 * 1000;  // 200ms

// 前向声明（alloc/free 互相引用）
void esp32_reorder_free();

uint16_t ble_ota_log_conn_params(uint16_t conn_handle, const char *phase)
{
    struct ble_gap_conn_desc desc = {};
    int rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        DEBUG_WARN(TAG, "%sBLE param读失败 rc=%d", phase, rc);
        return 0;
    }

    DEBUG_DBG(TAG, "%sitv=%.2f lat=%u",
               phase,
               desc.conn_itvl * 1.25f,
               (unsigned)desc.conn_latency);
    return desc.conn_itvl;
}

int ble_ota_request_conn_params(uint16_t conn_handle, bool windows_compatible)
{
    struct ble_gap_upd_params params = {};
    if (windows_compatible) {
        /* 回退：10–12.5ms（itvl 8..10）。比原 15–20ms 更激进，让 Windows 回退也不至于掉到 20ms。
         * 10ms 是多数 Windows 主机可接受的较激进下限，仍留 3 档让对端有选择。 */
        params.itvl_min = 8;
        params.itvl_max = 10;
        params.min_ce_len = 0;
        params.max_ce_len = 0;
    } else {
        /* 高速：7.5–10ms（itvl 6..8）。比原 7.5–15ms 激进收窄，让对端尽量落在 7.5–10ms
         * 而不是被对端选到 15ms。仍保留 3 档（6/7/8）让对端有选择，避免锁死 7.5 被拒
         * 然后掉到更慢默认值。ce_len 6..12 保证事件时间不被 OS 砍得更短。 */
        params.itvl_min = 6;
        params.itvl_max = 8;
        params.min_ce_len = 6;
        params.max_ce_len = 12;
    }
    params.latency = 0;
    params.supervision_timeout = 200;

    int rc = ble_gap_update_params(conn_handle, &params);
    DEBUG_DBG(TAG, "BLEp(%s) %.1f-%.1f rc=%d",
               windows_compatible ? "Win" : "Hi",
               params.itvl_min * 1.25f,
               params.itvl_max * 1.25f,
               rc);
    return rc;
}

/**
 * @brief 分配 reorder buffer（slots 指针/长度数组）。
 *        在 ble_esp32_ota_begin() 中调用。分配失败则置 fallback=true，
 *        后续自动回退到丢弃策略。
 */
void esp32_reorder_alloc()
{
    // 已分配则先释放（防御性）
    if (s_reorder.slots || s_reorder.lens) {
        esp32_reorder_free();
    }

    s_reorder.expected    = 0;
    s_reorder.windowBase  = 0;
    s_reorder.windowSize  = (uint32_t)BLE_OTA_REORDER_WINDOW;
    s_reorder.payloadCap  = BLE_OTA_CHUNK_PAYLOAD;
    s_reorder.cachedCount = 0;
    s_reorder.slots = (uint8_t **)calloc(s_reorder.windowSize, sizeof(uint8_t *));
    s_reorder.lens  = (size_t  *)calloc(s_reorder.windowSize, sizeof(size_t));
    s_fallback_ooo_count  = 0;

    if (!s_reorder.slots || !s_reorder.lens) {
        DEBUG_WARN(TAG, "Reorder分配失败,回退丢弃 free=%u",
                   (unsigned)esp_get_free_heap_size());
        // 释放可能部分成功的分配
        free(s_reorder.slots); s_reorder.slots = nullptr;
        free(s_reorder.lens);  s_reorder.lens  = nullptr;
        s_reorder.active   = false;
        s_reorder.fallback = true;
        return;
    }
    s_reorder.active   = true;
    s_reorder.fallback = false;
    DEBUG_DBG(TAG, "Reorder ok win=%u cap=%u heap=%u",
               (unsigned)s_reorder.windowSize, (unsigned)s_reorder.payloadCap,
               (unsigned)esp_get_free_heap_size());
}

/**
 * @brief 释放 reorder buffer 全部内存（含已占用的 slot）。
 *        同时复位 expected/windowBase 等状态，供下次 OTA begin 重新分配。
 */
void esp32_reorder_free()
{
    if (s_reorder.slots) {
        for (uint32_t i = 0; i < s_reorder.windowSize; ++i) {
            if (s_reorder.slots[i]) {
                free(s_reorder.slots[i]);
                s_reorder.slots[i] = nullptr;
            }
        }
        free(s_reorder.slots);
        s_reorder.slots = nullptr;
    }
    if (s_reorder.lens) {
        free(s_reorder.lens);
        s_reorder.lens = nullptr;
    }
    s_reorder.expected    = 0;
    s_reorder.windowBase  = 0;
    s_reorder.active      = false;
    s_reorder.fallback    = false;
    s_reorder.windowSize  = 0;
    s_reorder.payloadCap  = 0;
    s_reorder.cachedCount = 0;
    s_fallback_ooo_count  = 0;
}

/**
 * @brief 取当前 reorder buffer 已缓存的乱序包数（每个 slot 持有 malloc 的 payload）。
 *        供堆低水位快照采样，定位"OTA 期低谷是不是 BLE reorder 引起"。
 *        非 OTA 期 active=false，返回 0。
 */
uint32_t ble_ota_get_reorder_cached_count()
{
    return s_reorder.active ? s_reorder.cachedCount : 0;
}

/**
 * @brief flush expected 对应的连续缓存包到 flash，并释放对应 slot。
 *        由 esp32_write_ordered 在每次 expected 推进后调用。
 * @return flush 的包数（不含当前已写入的包）。
 */
uint32_t esp32_reorder_flush()
{
    uint32_t flushed = 0;
    while (s_reorder.active) {
        uint32_t idx = s_reorder.expected - s_reorder.windowBase;
        // idx 应始终 < windowSize（窗口未越界）
        if (idx >= s_reorder.windowSize) break;
        if (!s_reorder.slots[idx]) break;  // 缺口：后续虽有缓存但不连续

        uint8_t *data = s_reorder.slots[idx];
        size_t   len  = s_reorder.lens[idx];
        esp_err_t err = Esp32OtaCore::instance().write(data, len);
        // 释放 slot
        free(s_reorder.slots[idx]);
        s_reorder.slots[idx] = nullptr;
        s_reorder.lens[idx]  = 0;
        if (s_reorder.cachedCount > 0) {
            s_reorder.cachedCount--;
        }

        if (err != ESP_OK) {
            DEBUG_ERR(TAG, "Reorder flush 写入失败: seq=%u err=%s",
                      (unsigned)s_reorder.expected, esp_err_to_name(err));
            // 写入失败不推进 expected，交由上层 abort
            return flushed;
        }
        s_reorder.expected++;
        flushed++;
    }
    return flushed;
}

/**
 * @brief 按序号顺序写入 ESP32 OTA 数据。
 *
 * 启用 reorder buffer 时：吸收窗口内乱序，缺口填满后按序 flush。
 * 回退模式（buffer 分配失败）：执行旧丢弃策略（乱序包丢弃 + ACK 报告缺口）。
 *
 * @param seq        包序号。
 * @param cmd        数据命令字节（用于 ACK）。
 * @param data       负载数据指针。
 * @param data_len   负载数据长度。
 * @return ESP_OK 写入成功或被安全跳过，其他为失败。
 */
static esp_err_t esp32_write_ordered(uint16_t seq, uint8_t cmd,
                                     const uint8_t *data, size_t data_len)
{
    // ── 回退模式：旧丢弃策略 ──
    if (s_reorder.fallback) {
        // 重复包
        if (seq < s_reorder.expected) {
            DEBUG_DBG(TAG, "ESP32 重复包: seq=%u < expected=%u，跳过",
                      seq, (unsigned)s_reorder.expected);
            return ESP_OK;
        }
        // 乱序包：丢弃 + 节流 ACK 报告缺口
        // ★ 节流策略：每 BLE_OTA_ACK_WINDOW 个乱序包才发一次 ACK，
        //   或在窗口边界处发 ACK。避免大量乱序包涌入时触发 ACK 风暴，
        //   耗尽 NimBLE mbuf 池导致 ESP_ERR_NO_MEM。
        if (seq > s_reorder.expected) {
            s_fallback_ooo_count++;
            bool shouldAck =
                (s_fallback_ooo_count >= BLE_OTA_ACK_WINDOW) ||
                ((((uint32_t)seq + 1U) % BLE_OTA_ACK_WINDOW) == 0U);
            if (shouldAck) {
                uint32_t curReceived = (uint32_t)Esp32OtaCore::instance().getReceived();
                DEBUG_WARN(TAG, "乱序 s=%u e=%u ACK=%u ooo=%u",
                           seq, (unsigned)s_reorder.expected, (unsigned)curReceived,
                           (unsigned)s_fallback_ooo_count);
                sendWindowAck(cmd, s_reorder.expected > 0 ? (uint16_t)(s_reorder.expected - 1) : 0,
                              curReceived);
                s_fallback_ooo_count = 0;
            } else {
                DEBUG_DBG(TAG, "ESP32乱序 s=%u e=%u 抑ACK ooo=%u",
                          seq, (unsigned)s_reorder.expected,
                          (unsigned)s_fallback_ooo_count);
            }
            return ESP_OK;
        }
        // seq == expected：正常写入
        esp_err_t err = Esp32OtaCore::instance().write(data, data_len);
        if (err != ESP_OK) return err;
        s_reorder.expected++;
        return ESP_OK;
    }

    // ── Reorder buffer 模式 ──
    // 窗口推进检测：若 expected 已超出当前窗口基址，滑动窗口
    if (s_reorder.expected >= s_reorder.windowBase + s_reorder.windowSize) {
        s_reorder.windowBase = s_reorder.expected;
    }

    // 重复包：seq < expected → 已写入，跳过
    if (seq < s_reorder.expected) {
        DEBUG_DBG(TAG, "ESP32 重复包: seq=%u < expected=%u，跳过",
                  seq, (unsigned)s_reorder.expected);
        return ESP_OK;
    }

    // seq == expected：正常写入 + flush 连续缓存包
    if (seq == s_reorder.expected) {
        esp_err_t err = Esp32OtaCore::instance().write(data, data_len);
        if (err != ESP_OK) return err;
        s_reorder.expected++;
        uint32_t flushed = esp32_reorder_flush();
        if (flushed > 0) {
            DEBUG_INFO(TAG, "Reorder补齐 seq=%u flush=%u exp→%u",
                       seq, (unsigned)flushed, (unsigned)s_reorder.expected);
        }
        return ESP_OK;
    }

    // seq > expected：暂存到 slot（若在窗口范围内）
    uint32_t idx = seq - s_reorder.windowBase;
    if (idx < s_reorder.windowSize) {
        if (s_reorder.slots[idx]) {
            // 该 slot 已被占用（重复的乱序包），跳过
            DEBUG_DBG(TAG, "Reorder占 s=%u i=%u 跳过",
                      seq, (unsigned)idx);
            return ESP_OK;
        }
        // data 可能来自静态 buf（ble_ota_data_in_access 的合并缓冲区），
        // 后续包处理会覆盖它，必须拷贝一份
        uint8_t *copy = (uint8_t *)malloc(data_len);
        if (!copy) {
            uint32_t curReceived = (uint32_t)Esp32OtaCore::instance().getReceived();
            DEBUG_ERR(TAG, "Reorder OOM s=%u e=%u n=%u ACK+abort",
                       seq, (unsigned)s_reorder.expected, (unsigned)curReceived);
            sendWindowAck(cmd,
                          s_reorder.expected > 0 ? (uint16_t)(s_reorder.expected - 1) : 0,
                          curReceived);
            return ESP_ERR_NO_MEM;
        }
        memcpy(copy, data, data_len);
        s_reorder.slots[idx] = copy;
        s_reorder.lens[idx]  = data_len;
        s_reorder.cachedCount++;
        if (s_reorder.cachedCount == 1U) {
            DEBUG_INFO(TAG, "Reorder缺口 exp=%u cache seq=%u",
                       (unsigned)s_reorder.expected, seq);
        }
        DEBUG_DBG(TAG, "Reorder cache s=%u i=%u exp=%u",
                  seq, (unsigned)idx, (unsigned)s_reorder.expected);
        return ESP_OK;
    }

    // seq 超出窗口范围（防御性，理论不会发生）
    uint32_t curReceived = (uint32_t)Esp32OtaCore::instance().getReceived();
    DEBUG_WARN(TAG, "超窗 s=%u e=%u w=%u ACK=%u",
               seq, (unsigned)s_reorder.expected,
               (unsigned)s_reorder.windowSize, (unsigned)curReceived);
    sendWindowAck(cmd, s_reorder.expected > 0 ? (uint16_t)(s_reorder.expected - 1) : 0,
                  curReceived);
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────
// ESP32 OTA 薄包装：实际 begin/write/finalize 已抽离到 Esp32OtaCore
// 此处仅保留 BLE 侧的状态复位与日志，供 data_in_access 调用。
// ─────────────────────────────────────────────────────────────

/**
 * @brief 初始化 ESP32 本机 OTA 写入会话。
 *        委托 Esp32OtaCore::begin，并复位 BLE 传输相关状态。
 * @param total_size 固件总大小（字节）。
 * @return ESP_OK 成功，其他为失败错误码。
 */
esp_err_t ble_esp32_ota_begin(size_t total_size)
{
    // ★ 通过协调器发起（内部已含 forceCleanup 重试）
    esp_err_t err = OtaCoordinator::instance().beginEsp32OtaWithRetry(total_size, OTA_CHANNEL_BLE);
    if (err != ESP_OK) {
        return err;
    }
    // 复位 BLE 传输相关状态
    BleOta::instance().m_esp32RebootPending = false;
    BleOta::instance().m_esp32LastCmd = 0;
    BleOta::instance().m_esp32LastSeq = 0xFFFFU;
    // ★ 分配 reorder buffer（失败则自动回退丢弃策略）
    esp32_reorder_alloc();
    // ★ BLE OTA 开始：降低 WiFi 射频占用
    ble_ota_boost_wifi_ps();
    /* 刷写前再要一次高速连接参数（连接时若未谈妥，此处补刀） */
    if (BleOta::instance().m_bleConnected &&
        BleOta::instance().m_connHandle != 0xFFFFU) {
        (void)ble_ota_request_conn_params(BleOta::instance().m_connHandle, false);
    }
    return ESP_OK;
}

/**
 * @brief BLE OTA 会话状态统一复位（共用清理内核）。
 *
 * 集中处理三个清理动作：
 *   1. abortCurrent()：通知协调器按当前状态分发清理
 *      （GD32_STORING→metaClear, ESP32_RECEIVING→core.abort, IDLE→空操作），
 *      确保下次 begin() 不会被残留状态假拒绝。
 *   2. core.abort() 兜底：若 Esp32OtaCore 仍 isActive（异常路径），单独再 abort。
 *   3. BLE OTA 会话状态字段复位：m_esp32LastSeq/m_esp32LastCmd/m_esp32RebootPending
 *      + esp32_reorder_free() 释放 reorder buffer + s_gd32_ooo_count 归零。
 *
 * 不做：OTA 数据队列清理（调用方按需做）、WiFi PS 恢复（调用方按需做）。
 * 这两个动作在不同调用路径下时序/前置条件不同，交给调用方更安全。
 *
 * 调用方：
 *   - ble_esp32_ota_abort(): ESP32 OTA 异常/中止路径
 *   - 0xFB handler: 用户主动中止
 *   - BLE_GAP_EVENT_DISCONNECT handler: BLE 断开（含 GD32 存储断开）
 */
void ble_ota_session_reset()
{
    // ★ 无条件通知协调器释放通道占用（m_sysState → IDLE）。
    //   abortCurrent() 对 IDLE 状态是空操作，对其他状态均复位到 IDLE，
    //   确保后续 begin() 不会被残留的 ESP32_RECEIVING / ERROR 等状态假拒绝。
    OtaCoordinator::instance().abortCurrent();
    // 若 ESP32 OTA core 仍活跃（兜底），单独再 abort 一次
    if (Esp32OtaCore::instance().isActive()) {
        Esp32OtaCore::instance().abort();
    }
    // 复位 BLE OTA 会话状态，避免下次会话读到旧值
    BleOta::instance().m_esp32LastSeq = 0xFFFFU;
    BleOta::instance().m_esp32LastCmd = 0;
    BleOta::instance().m_esp32RebootPending = false;
    // 释放 ESP32 reorder buffer（无 buffer 时为空操作）
    esp32_reorder_free();
    // 复位 GD32 路径乱序包计数器（节流 ACK 计数），保证下次 GD32 上传从干净状态开始
    s_gd32_ooo_count = 0;
    s_ble_gd32_meta = {};
    s_ble_gd32_meta_active = false;
    // 复位 DATA_IN 丢包负反馈计数/节流状态，下次会话从干净状态开始
    s_ota_drop_malloc_fail = 0;
    s_ota_drop_queue_full  = 0;
    s_neg_ack_sent         = 0;
    s_neg_ack_drop_count   = 0;
    s_last_neg_ack_us      = 0;
}

/**
 * @brief ESP32 OTA 异常/中止时调用：会话复位 + WiFi PS 恢复。
 *
 * 关键：必须先通知协调器释放 m_sysState（避免下次 begin 被 ESP32_RECEIVING
 *   状态假拒绝），再调 core.abort() 释放 mutex。两者重复 abort 幂等安全。
 */
void ble_esp32_ota_abort()
{
    ble_ota_session_reset();
    // ★ BLE OTA 中止：恢复 WiFi 射频
    ble_ota_restore_wifi_ps();
}

/**
 * @brief 发送窗口 ACK 通知，回报已处理的分块序号与累计字节数。
 * @param cmd 对应的数据命令字节。
 * @param seq 当前确认的分块序号。
 * @param completed_bytes 已完成写入的累计字节数。
 */
void sendWindowAck(uint8_t cmd, uint16_t seq, uint32_t completed_bytes)
{
    uint8_t ack[8] = {0};
    ack[0] = BLE_OTA_NOTIFY_ACK;
    ack[1] = cmd;
    ack[2] = (uint8_t)(seq & 0xFFU);
    ack[3] = (uint8_t)((seq >> 8) & 0xFFU);
    ack[4] = (uint8_t)(completed_bytes & 0xFFU);
    ack[5] = (uint8_t)((completed_bytes >> 8) & 0xFFU);
    ack[6] = (uint8_t)((completed_bytes >> 16) & 0xFFU);
    ack[7] = (uint8_t)((completed_bytes >> 24) & 0xFFU);

    esp_err_t err = BleOta::instance().notifyResponse(ack, sizeof(ack));
    if (err != ESP_OK) {
        DEBUG_WARN(TAG, "ACK失败 cmd=0x%02X seq=%u %s",
                   cmd, seq, esp_err_to_name(err));
    } else {
        DEBUG_DBG(TAG, "已发送 ACK: cmd=0x%02X seq=%u bytes=%lu",
                  cmd, seq, (unsigned long)completed_bytes);
    }
}

/**
 * @brief 从 DATA_IN batch 头部解析 cmd 字节。
 *
 * DATA_IN 回调只把 mbuf 拷到 buf，未解析协议头；负 ACK 需要 cmd 字节
 * 区分 ESP32/GD32 续传分支，避免误报让前端走错路径。与 worker 中
 * ble_ota_process_data_batch 的包头解析保持一致（buf[0] = cmd）。
 *
 * @param buf      数据缓冲区。
 * @param recv_len 数据长度。
 * @return cmd 字节；非法长度返回 0（GD32 命令），调用方据此不发误导性 ACK。
 */
static uint8_t parse_cmd_from_batch(const uint8_t *buf, size_t recv_len)
{
    // 与 ble_ota_process_data_batch 一致：至少 7 字节头才算合法包
    if (recv_len < 7 || buf == nullptr) {
        return 0;
    }
    return buf[0];
}

/**
 * @brief 丢包路径负 ACK：发缺口 ACK 让前端按 completed_bytes 续传退避。
 *
 * 触发于 DATA_IN 回调中 malloc 失败 / 队列满。窗口内包用
 * writeValueWithoutResponse（即发即忘），收不到 ATT Error Response，
 * 只能走 DATA_OUT 通知通道。负 ACK 复用 sendWindowAck，seq 取
 * s_reorder.expected-1（最近连续落盘序号），completed 取实际落盘字节，
 * 前端 resumeSeq = ceil(bytes/payloadSize) 续传，无需改客户端。
 *
 * 节流：丢包数 ≥ NEG_ACK_DROP_WINDOW 或距上次 ≥ NEG_ACK_MIN_INTERVAL_US
 *       才发一次，防止重传风暴期间 ACK 通知打爆 NimBLE mbuf 池。
 *       状态仅在 NimBLE host task 回调中读写，无需锁。
 *
 * 仅 ESP32 路径发负 ACK（s_reorder 状态在此文件内可见）；GD32 中继转发
 * 的缺口反馈由 GD32 侧自行处理，本函数对 GD32 cmd 仅累计不发送。
 */
static void sendNegativeAckIfDue(uint8_t cmd)
{
    // 累计丢包（无论是否发送，计数都增长，供可观测性）
    s_neg_ack_drop_count += 1;

    // 节流判定：丢包数达阈值 或 距上次发送超过最小间隔
    const int64_t now_us = esp_timer_get_time();
    bool by_count = (s_neg_ack_drop_count >= NEG_ACK_DROP_WINDOW);
    bool by_time  = ((now_us - s_last_neg_ack_us) >= NEG_ACK_MIN_INTERVAL_US);
    // 首次（s_last_neg_ack_us==0）也允许立即发，避免低堆首包丢就憋住
    bool first_time = (s_last_neg_ack_us == 0);
    if (!(by_count || by_time || first_time)) {
        return;
    }

    // 仅 ESP32 路径发负 ACK；GD32 中继缺口由 GD32 侧反馈，这里只计不发
    if (cmd == BLE_OTA_CMD_ESP32_DATA) {
        // expected>0：已落盘过包，ackSeq=expected-1，completed=实际落盘字节
        // expected==0：首包都没落盘，发 ackSeq=0/completed=0，等价"从头续发"
        //   （客户端 seq=0 用 write-with-response，本就等首包 ACK，不会误导）
        uint16_t ackSeq = (s_reorder.expected > 0U)
                        ? (uint16_t)(s_reorder.expected - 1U)
                        : 0U;
        uint32_t completed = (s_reorder.expected > 0U)
                           ? (uint32_t)Esp32OtaCore::instance().getReceived()
                           : 0U;
        sendWindowAck(cmd, ackSeq, completed);
        s_neg_ack_sent += 1;
        DEBUG_WARN(TAG, "DATA_IN 负ACK cmd=0x%02X seq=%u bytes=%lu (drop:malloc=%u qfull=%u)",
                   cmd, ackSeq, (unsigned long)completed,
                   (unsigned)s_ota_drop_malloc_fail, (unsigned)s_ota_drop_queue_full);
    }
    // 复位节流状态
    s_neg_ack_drop_count = 0;
    s_last_neg_ack_us = now_us;
}

/**
 * @brief 发送收尾阶段事件通知（开始/完成/失败/重启等）。
 * @param stage 阶段标识码。
 * @param value 阶段附带值（错误码或耗时等）。
 */
void sendFinishEvent(uint8_t stage, uint32_t value)
{
    uint8_t pkt[6] = {0};
    pkt[0] = BLE_OTA_NOTIFY_FINISH;
    pkt[1] = stage;
    pkt[2] = (uint8_t)(value & 0xFFU);
    pkt[3] = (uint8_t)((value >> 8) & 0xFFU);
    pkt[4] = (uint8_t)((value >> 16) & 0xFFU);
    pkt[5] = (uint8_t)((value >> 24) & 0xFFU);
    esp_err_t err = BleOta::instance().notifyResponse(pkt, sizeof(pkt));
    if (err != ESP_OK) {
        DEBUG_WARN(TAG, "发送 FINISH 事件失败: stage=0x%02X err=%s",
                   stage, esp_err_to_name(err));
    }
}

/** @brief 将 session_id 和 code 打包为 32 位 BLE FINISH 事件值。 */
uint32_t packBleFinishValue(uint16_t session_id, uint16_t code)
{
    return ((uint32_t)code << 16) | (uint32_t)session_id;
}

/** @brief 将 32 字节 SHA-256 转为大写十六进制字符串（65字节含'\0'）。 */
void shaBytesToHexUpper(const uint8_t sha[32], char out_hex[65])
{
    if (!sha || !out_hex) return;
    for (int i = 0; i < 32; ++i) {
        snprintf(&out_hex[i * 2], 3, "%02X", sha[i]);
    }
    out_hex[64] = '\0';
}

/** @brief 通过 BLE Notify 推送设备端 SHA-256 给前端（用于比对验证）。 */
void sendSha256Notify(const uint8_t sha[32], uint16_t session_id)
{
    if (!sha) return;

    uint8_t sha_pkt[35] = {0};
    sha_pkt[0] = BLE_OTA_NOTIFY_SHA256;
    memcpy(&sha_pkt[1], sha, 32);
    sha_pkt[33] = (uint8_t)(session_id & 0xFFU);
    sha_pkt[34] = (uint8_t)((session_id >> 8) & 0xFFU);
    BleOta::instance().notifyResponse(sha_pkt, sizeof(sha_pkt));
}

/** @brief BLE 触发 GD32 中继后等待结果的 FreeRTOS 任务。 */
void ble_gd32_relay_wait_task(void *pvParameters)
{
    BleGd32RelayWaitCtx *ctx = (BleGd32RelayWaitCtx *)pvParameters;
    uint16_t session_id = ctx ? ctx->session_id : 0;
    if (ctx) free(ctx);

    uint32_t result_msg = (uint32_t)OTA_RESULT_ERR_TIMEOUT;
    bool got_result = false;
    QueueHandle_t rq = OtaRelay::getResultQueue();
    if (rq) {
        got_result = (xQueueReceive(rq, &result_msg, pdMS_TO_TICKS(120000)) == pdTRUE);
    }

    if (!got_result) {
        DEBUG_ERR(TAG, "GD32 BLE OTA 等待中继结果超时");
        sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                        packBleFinishValue(session_id, (uint16_t)(-OTA_RESULT_ERR_TIMEOUT)));
        vTaskDelete(NULL);
        return;
    }

    ota_result_t result = (ota_result_t)((int32_t)result_msg);
    if (result == OTA_RESULT_OK) {
        DEBUG_INFO(TAG, "GD32中继完成 stage=0x11 sess=%u", session_id);
        sendFinishEvent(BLE_OTA_FINISH_GD32_RELAY_DONE,
                        packBleFinishValue(session_id, 0));
    } else {
        uint16_t code = (uint16_t)(result < 0 ? -result : result);
        DEBUG_ERR(TAG, "GD32 BLE OTA 中继失败: result=%d", (int)result);
        sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                        packBleFinishValue(session_id, code));
    }
    vTaskDelete(NULL);
}


// ============================================================
// GATT 访问回调
// ============================================================

/*
 * CTRL (0x0001) - Write
 * 写入 0xF7 触发 OTA 中继
 */
/**
 * @brief GATT CTRL 特征访问回调，处理控制命令写入。
 * @param conn_handle BLE 连接句柄。
 * @param attr_handle GATT 属性句柄。
 * @param ctxt GATT 访问上下文。
 * @param arg 回调用户参数。
 * @return NimBLE ATT 错误码，0 表示成功。
 */

int ble_ota_data_in_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (BleOta::instance().isSuspendedForWifiOta()) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }

    // 使用较大静态缓冲区：writeValueWithoutResponse 快速发送时，
    // NimBLE 可能将多个写请求合并到同一个 mbuf 链中
    // ★ static: NimBLE host task 栈只有 4096 字节，不能用栈分配
    // ★ 4096 字节可容纳 8 个合并包（8×491=3928），避免截断丢包
    static constexpr size_t BLE_OTA_BUF_SIZE = 4096;
    static uint8_t buf[BLE_OTA_BUF_SIZE];
    int recv_len = ble_ota_mbuf_to_buf(ctxt->om, buf, sizeof(buf));

    if (recv_len < 8) {
        DEBUG_WARN(TAG, "DATA_IN 数据太短: %d 字节 (最少 8: 7头+1数据)", recv_len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    // 将数据拷贝到堆上并送入工作队列，避免在 NimBLE host task 中执行
    // 耗时的 flash 操作（esp_ota_begin / esp_ota_write）导致 BLE 连接断开。
    if (s_otaDataQueue) {
        // 解析 cmd 字节供负 ACK 区分 ESP32/GD32 续传分支（与 worker 解析一致）
        uint8_t cmd = parse_cmd_from_batch(buf, (size_t)recv_len);
        uint8_t *heap_buf = (uint8_t *)malloc((size_t)recv_len);
        if (heap_buf) {
            memcpy(heap_buf, buf, (size_t)recv_len);
            BleOtaDataBatch item = { heap_buf, (size_t)recv_len };
            if (xQueueSend(s_otaDataQueue, &item, 0) != pdTRUE) {
                // 队列满：发缺口 ACK 让前端按 completed_bytes 续传退避，
                // 而非等满 8s 窗口超时后整窗口重发 → 重传风暴叠加低堆二次失败
                s_ota_drop_queue_full += 1;
                DEBUG_WARN(TAG, "DATA_IN满 丢%dB 等重传", recv_len);
                free(heap_buf);
                sendNegativeAckIfDue(cmd);
            }
        } else {
            // malloc 失败：同上，发缺口 ACK 形成可恢复协议反馈，避免静默丢包
            s_ota_drop_malloc_fail += 1;
            DEBUG_ERR(TAG, "DATA_IN 堆内存不足 (%d 字节)，丢弃", recv_len);
            sendNegativeAckIfDue(cmd);
        }
    }

    return 0;
}

/**
 * @brief 处理单个 ESP32 OTA 数据包（ble_ota_process_data_batch 子步骤）。
 * @return true 中止整个 batch；false 继续下一包（offset 由外层统一推进）。
 */
static bool process_esp32_packet(uint8_t cmd, uint16_t seq, uint32_t total_size,
                                  const uint8_t *buf, size_t data_offset, size_t data_len,
                                  uint32_t expected_payload)
{
    if (seq == 0) {
        if (total_size == 0) {
            DEBUG_ERR(TAG, "ESP32 固件大小非法: %" PRIu32, total_size);
            return true;
        }
        // 活跃 BLE 会话中的 seq=0 一律按重传处理。窗口 0 的 ACK 丢失时，
        // 前端会从累计字节 0 续发；此时重置 OTA 会丢掉已缓存的后续包。
        // 新上传由 0xFB/断连先清理旧会话，不能用 seq=0 猜测会话边界。
        if (Esp32OtaCore::instance().isActive()) {
            ota_channel_t curCh = OtaCoordinator::instance().getOtaChannel();
            if (curCh != OTA_CHANNEL_BLE) {
                DEBUG_WARN(TAG, "ESP32被ch=%d占用,BLE拒抢",
                           (int)curCh);
                sendFinishEvent(BLE_OTA_FINISH_ERR_MUTEX, 0);
                return true;
            }
            DEBUG_INFO(TAG, "ESP32已激活 seq=0重传,保留reorder");
        } else {
            // 分区选择/大小校验/互斥由 Esp32OtaCore::begin 内部完成
            esp_err_t err = ble_esp32_ota_begin(total_size);
            if (err != ESP_OK) {
                if (err == ESP_ERR_INVALID_STATE) {
                    sendFinishEvent(BLE_OTA_FINISH_ERR_MUTEX, 0);
                }
                return true;
            }
        }
    }

    // ★ 会话有效性检查：如果 begin() 失败（如互斥拒绝），后续包也应丢弃
    if (!Esp32OtaCore::instance().isActive()) {
        DEBUG_WARN(TAG, "ESP32 OTA 会话无效，丢弃 seq=%u", seq);
        return false;  // 外层推进 offset，继续下一包
    }

    // ★ 防重复写入保护：如果已接收字节 >= 声明大小，跳过（重试引起的重复包）
    uint32_t curReceived = (uint32_t)Esp32OtaCore::instance().getReceived();
    if (total_size > 0 && curReceived >= total_size) {
        DEBUG_WARN(TAG, "ESP32 OTA 已收满 (%u >= %" PRIu32 ")，跳过重复 seq=%u",
                   (unsigned)curReceived, total_size, seq);
        sendWindowAck(cmd, seq, curReceived);
        return false;  // 外层推进 offset，继续下一包
    }

    // ★ 使用带序号顺序保护的写入：拒绝乱序包并立即 ACK 通知发送端
    uint32_t beforeReceived = (uint32_t)Esp32OtaCore::instance().getReceived();
    esp_err_t err = esp32_write_ordered(seq, cmd, &buf[data_offset], data_len);
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "ESP32 OTA 写入失败: seq=%u err=%s",
                  seq, esp_err_to_name(err));
        ble_esp32_ota_abort();  // core 内部完成 abort + mutex release + 状态复位
        return true;
    }

    // ★ 仅在实际写入新字节时才触发 ACK：重复包/乱序缓存包不写入 flash，
    //   若在窗口边界触发 ACK 会携带陈旧字节数，被发送端误判为丢包。
    uint32_t afterReceived = (uint32_t)Esp32OtaCore::instance().getReceived();
    bool progressMade = (afterReceived > beforeReceived);


    // 完成判定：以实际按序写入字节数为准。
    bool isLastPacket = (expected_payload < BLE_OTA_CHUNK_PAYLOAD);
    bool transferComplete = (total_size > 0 && afterReceived >= total_size);

    // ACK 按累计连续进度触发，而不是按当前到达包的 seq 触发。缺口包到达后
    // reorder 可能一次 flush 到下个窗口边界，例如 seq=99 使 expected 直达128；
    // 此时必须立即 ACK 127，否则后续重复包都不产生 progress，前端只能超时。
    //
    // ★ seq=0 强制 ACK：begin 后首包需尽快 ACK，前端才能继续窗口。
    //   前端必须等“首包已落盘”ACK 后再发窗口内其余包，否则队列满丢包→重传极慢。
    uint32_t completedPackets = (afterReceived + BLE_OTA_CHUNK_PAYLOAD - 1U) /
                                BLE_OTA_CHUNK_PAYLOAD;
    bool reachedAckBoundary = completedPackets > 0U &&
                              (completedPackets % BLE_OTA_ACK_WINDOW) == 0U;
    bool firstPacketReady = (seq == 0U) && progressMade;
    if (progressMade &&
        (reachedAckBoundary ||
         transferComplete ||
         isLastPacket ||
         firstPacketReady)) {
        if (isLastPacket || transferComplete) {
            DEBUG_DBG(TAG, "ESP32尾包 s=%u n=%zu rx=%u tot=%" PRIu32,
                       seq, data_len, (unsigned int)afterReceived, total_size);
        } else if (firstPacketReady) {
            DEBUG_DBG(TAG, "ESP32 begin 就绪 ACK: seq=0 received=%u total=%" PRIu32,
                       (unsigned)afterReceived, total_size);
        }
        uint16_t ackSeq = s_reorder.expected > 0U
                        ? (uint16_t)(s_reorder.expected - 1U)
                        : seq;
        sendWindowAck(cmd, ackSeq, afterReceived);
    }

    if (transferComplete) {
        DEBUG_INFO(TAG, "ESP32收完 %u/%" PRIu32 "B chunks=%u",
                   (unsigned int)Esp32OtaCore::instance().getReceived(), total_size, seq + 1);
        OtaCoordinator::instance().notifyEsp32ReceiveComplete();
        BleOta::instance().m_esp32LastCmd = cmd;
        BleOta::instance().m_esp32LastSeq = seq;
        ble_ota_restore_wifi_ps();
    }
    return false;
}

/**
 * @brief 处理单个 GD32 OTA 数据包（ble_ota_process_data_batch 子步骤）。
 * @return true 中止整个 batch；false 继续下一包（offset 由外层统一推进）。
 */
/** GD32 路径硬失败：与 WiFi abortCurrentIfEpoch 对齐，复位协调器 + 本通道 RAM meta。 */
void ble_gd32_fail_abort(const char *why)
{
    DEBUG_ERR(TAG, "GD32 OTA 中止: %s", why ? why : "?");
    OtaCoordinator::instance().abortCurrent();
    s_ble_gd32_meta = {};
    s_ble_gd32_meta_active = false;
    s_gd32_ooo_count = 0;
    ble_ota_restore_wifi_ps();
}

static bool process_gd32_packet(uint8_t cmd, uint16_t seq, uint32_t total_size,
                                 const uint8_t *buf, size_t data_offset, size_t data_len,
                                 uint32_t expected_payload)
{
    if (seq == 0 && total_size > 0) {
        // 与 ESP32 BLE 路径一致：活跃会话中的 seq=0 视为首包重传，禁止再次 prepare
        // （否则会 erase 分区、清 meta，毁掉已写入进度）。
        if (s_ble_gd32_meta_active && s_ble_gd32_meta.total_size == total_size) {
            if (s_ble_gd32_meta.stored_size > 0) {
                DEBUG_INFO(TAG, "GD32已激活 seq=0重传 skip stored=%" PRIu32,
                           s_ble_gd32_meta.stored_size);
                sendWindowAck(cmd, 0, s_ble_gd32_meta.stored_size);
                return false;
            }
            DEBUG_INFO(TAG, "GD32未落盘 seq=0重传首块(不prepare)");
            // fall through：stored_size==0 时重写 offset 0
        } else {
            // 与 WiFi 共用 prepareGd32Storage：validate + begin + erase + clearMeta + saveMeta
            esp_err_t prep_err = OtaCoordinator::instance().prepareGd32Storage(
                total_size, OTA_CHANNEL_BLE);
            if (prep_err == ESP_ERR_INVALID_SIZE) {
                DEBUG_ERR(TAG, "GD32 固件大小非法: raw=%" PRIu32 " max=%zu",
                          total_size, OTA_MAX_FIRMWARE_SIZE);
                sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_SIZE, total_size);
                return true;
            } else if (prep_err == ESP_ERR_INVALID_STATE) {
                DEBUG_ERR(TAG, "GD32 OTA 协调器拒绝存储（系统忙）");
                sendFinishEvent(BLE_OTA_FINISH_ERR_MUTEX, 0);
                return true;
            } else if (prep_err != ESP_OK) {
                DEBUG_ERR(TAG, "GD32 OTA 准备失败: %s", esp_err_to_name(prep_err));
                sendFinishEvent(BLE_OTA_FINISH_ERR_MUTEX, 0);
                return true;
            }

            DEBUG_INFO(TAG, "GD32 固件存储开始: total=%" PRIu32 ", payload=%zu",
                       total_size, data_len);
            s_ble_gd32_meta = {};
            s_ble_gd32_meta.total_size = total_size;
            s_ble_gd32_meta_active = true;
            ble_ota_boost_wifi_ps();
            if (BleOta::instance().m_bleConnected &&
                BleOta::instance().m_connHandle != 0xFFFFU) {
                (void)ble_ota_request_conn_params(BleOta::instance().m_connHandle, false);
            }
        }
    }

    if (!s_ble_gd32_meta_active || s_ble_gd32_meta.total_size == 0) {
        DEBUG_ERR(TAG, "GD32 元数据无效，拒绝写入: seq=%u", seq);
        return true;
    }
    ota_meta_t &meta = s_ble_gd32_meta;

    // ── 顺序检查：GD32 Flash 必须按序写入，无 reorder buffer ──
    // 改进：乱序/重复包不再中止整个 batch，而是丢弃当前包 + 节流 ACK 报告缺口，
    //       让前端尽快知道设备端实际进度（last_seq）并重传缺口。
    //       策略与 ESP32 fallback 路径一致（参考 esp32_write_ordered 行 459-477）。
    if (seq > 0 && meta.last_seq != (uint32_t)(seq - 1)) {
        // 重复包：seq <= last_seq（已写入过）
        // ★ 重复包静默跳过，不发送 ACK：发送端已持有上一窗口的有效 ACK 进度，
        //   重复包的 ACK 会携带未增长的字节数，被发送端误判为当前窗口丢包。
        if (seq <= meta.last_seq) {
            DEBUG_DBG(TAG, "GD32 重复包: seq=%u <= last_seq=%" PRIu32 "，跳过",
                      seq, meta.last_seq);
            return false;  // 外层推进 offset，继续下一包
        }

        // 乱序包：seq > last_seq + 1（缺口）
        // 节流策略：每 BLE_OTA_ACK_WINDOW 个乱序包才发一次 ACK，或在窗口边界处发 ACK。
        // 避免大量乱序包涌入时触发 ACK 风暴，耗尽 NimBLE mbuf 池导致 ESP_ERR_NO_MEM。
        s_gd32_ooo_count++;
        bool shouldAck =
            (s_gd32_ooo_count >= BLE_OTA_ACK_WINDOW) ||
            ((((uint32_t)seq + 1U) % BLE_OTA_ACK_WINDOW) == 0U);
        if (shouldAck) {
            DEBUG_WARN(TAG, "GD32乱序 s=%u last=%" PRIu32 " ACK=%" PRIu32 " ooo=%u",
                       seq, meta.last_seq, meta.stored_size, (unsigned)s_gd32_ooo_count);
            sendWindowAck(cmd, (uint16_t)meta.last_seq, meta.stored_size);
            s_gd32_ooo_count = 0;
        } else {
            DEBUG_DBG(TAG, "GD32乱序 s=%u last=%" PRIu32 " 抑ACK ooo=%u",
                      seq, meta.last_seq, (unsigned)s_gd32_ooo_count);
        }
        return false;  // 外层推进 offset，继续下一包
    }

    // 顺序包到达，复位乱序计数器
    if (s_gd32_ooo_count != 0) {
        s_gd32_ooo_count = 0;
    }

    // --- GD32 写入 Flash ---
    uint32_t flash_offset = meta.stored_size;
    if (flash_offset >= meta.total_size) {
        DEBUG_WARN(TAG, "GD32 OTA 已收满 (%" PRIu32 " >= %" PRIu32 ")，跳过重复 seq=%u",
                   flash_offset, meta.total_size, seq);
        sendWindowAck(cmd, seq, flash_offset);
        return false;  // 外层推进 offset，继续下一包
    }
    if (flash_offset + (uint32_t)data_len > meta.total_size) {
        DEBUG_ERR(TAG, "GD32 写入越界: offset=%" PRIu32 " len=%zu total=%" PRIu32,
                  flash_offset, data_len, meta.total_size);
        ble_gd32_fail_abort("write OOB");
        sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE, 0);
        return true;
    }

    esp_err_t err = OtaCoordinator::instance().writeGd32Chunk(flash_offset, &buf[data_offset], data_len);
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "Flash 写入失败: seq=%u offset=%" PRIu32 " err=%s",
                  seq, flash_offset, esp_err_to_name(err));
        ble_gd32_fail_abort(esp_err_to_name(err));
        sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE, (uint32_t)err);
        return true;
    }

    uint32_t stored_size = flash_offset + (uint32_t)data_len;
    meta.stored_size = stored_size;
    meta.last_seq = seq;
    const bool transfer_complete = stored_size >= meta.total_size;
    // seq=0：prepare 后写完首包必须 ACK，前端才能继续发后续窗口
    const bool checkpoint =
        (seq == 0U) ||
        ((((uint32_t)seq + 1U) % BLE_OTA_ACK_WINDOW) == 0U) ||
        transfer_complete ||
        (expected_payload < BLE_OTA_CHUNK_PAYLOAD);

    // NVS commit 是毫秒级操作，不能每 480 字节执行一次。仅在 ACK 窗口边界
    // 和尾包持久化；正常完成时尾包一定落盘，后续 SHA/中继读取到的语义不变。
    if (checkpoint) {
        err = OtaCoordinator::instance().updateGd32Meta(stored_size, seq);
        if (err != ESP_OK) {
            DEBUG_ERR(TAG, "GD32 元数据 checkpoint 失败 (seq=%u): %s",
                      seq, esp_err_to_name(err));
            ble_gd32_fail_abort(esp_err_to_name(err));
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE, (uint32_t)err);
            return true;
        }
        sendWindowAck(cmd, seq, stored_size);
    }

    DEBUG_DBG(TAG, "Flash 写入成功: seq=%u offset=%" PRIu32 " stored=%" PRIu32,
              seq, flash_offset, stored_size);

    // --- 检查 GD32 固件是否接收完毕 ---
    if (transfer_complete) {
        DEBUG_INFO(TAG, "GD32收完 %" PRIu32 "/%" PRIu32 "B chunks=%u",
                   meta.stored_size, meta.total_size, seq + 1);
        s_ble_gd32_meta_active = false;
        ble_ota_restore_wifi_ps();
        // 与 WiFi 共用 finishGd32StorageAfterReceive：complete + SHA
        esp_err_t sha_err = OtaCoordinator::instance().finishGd32StorageAfterReceive();
        if (sha_err != ESP_OK) {
            DEBUG_ERR(TAG, "GD32 SHA失败:%s", esp_err_to_name(sha_err));
        }
    }
    return false;
}

/**
 * @brief 处理一批 OTA 数据包（在工作线程上下文中调用）。
 *        包含 ESP32 和 GD32 的数据接收逻辑。
 * @param buf      数据缓冲区（包含一个或多个合并的包）。
 * @param recv_len 数据长度。
 */
void ble_ota_process_data_batch(const uint8_t *buf, size_t recv_len)
{
    // 循环处理 mbuf 中的所有完整数据包
    size_t offset = 0;
    while (offset + 7 <= recv_len) {
        // 解析包头
        uint8_t  cmd        = buf[offset + 0];
        uint16_t seq        = (uint16_t)(buf[offset + 1] | (buf[offset + 2] << 8));
        uint32_t total_size = (uint32_t)(buf[offset + 3] | (buf[offset + 4] << 8) |
                                         (buf[offset + 5] << 16) | (buf[offset + 6] << 24));

        size_t data_offset = offset + 7;

        // 根据包序号和固件总大小计算期望 payload，而非依赖 recv_len
        // 这样即使 NimBLE mbuf 合并导致缓冲区截断，也不会误判包长度
        uint32_t expected_payload = (total_size > 0)
            ? ((uint32_t)BLE_OTA_CHUNK_PAYLOAD < (total_size - (uint32_t)seq * (uint32_t)BLE_OTA_CHUNK_PAYLOAD)
               ? (uint32_t)BLE_OTA_CHUNK_PAYLOAD
               : (total_size - (uint32_t)seq * (uint32_t)BLE_OTA_CHUNK_PAYLOAD))
            : (uint32_t)BLE_OTA_CHUNK_PAYLOAD;

        size_t data_len = (size_t)expected_payload;
        size_t remaining_after_header = recv_len - data_offset;

        // 如果缓冲区中实际可用数据不足，截断 data_len
        if (remaining_after_header < expected_payload) {
            if (remaining_after_header == 0) {
                // 没有更多数据，退出循环
                break;
            }
            // 缓冲区截断，使用实际可用数据
            data_len = remaining_after_header;
        }

        // 当前包总长度: header(7) + payload(data_len)
        size_t packet_len = 7 + data_len;

        DEBUG_DBG(TAG, "DATA_IN c=0x%02X s=%u tot=%" PRIu32 " n=%zu",
                  cmd, seq, total_size, data_len);

        // 分发到对应通道的包处理器
        bool abort_batch = false;
        if (cmd == BLE_OTA_CMD_ESP32_DATA) {
            abort_batch = process_esp32_packet(cmd, seq, total_size,
                                               buf, data_offset, data_len, expected_payload);
        } else if (cmd == BLE_OTA_CMD_GD32_DATA) {
            abort_batch = process_gd32_packet(cmd, seq, total_size,
                                              buf, data_offset, data_len, expected_payload);
        } else {
            DEBUG_WARN(TAG, "DATA_IN 未知 cmd: 0x%02X offset=%zu", cmd, offset);
        }

        if (abort_batch) {
            return;
        }

        // 推进到下一个包
        offset += packet_len;

        // 检查剩余数据是否足够组成一个完整的包头
        if (offset + 7 > recv_len) break;
    }
}

/**
 * @brief OTA 数据工作线程：从队列接收数据并调用处理函数。
 *        独立于 NimBLE host task，避免 flash 操作阻塞 BLE 协议栈。
 */
void ble_ota_data_worker_task(void *pvParameters)
{
    (void)pvParameters;
    BleOta::instance().m_workerTaskHandle = xTaskGetCurrentTaskHandle();
    DEBUG_INFO(TAG, "OTA 数据工作线程已启动");

    while (1) {
        BleOtaDataBatch item;
        if (xQueueReceive(s_otaDataQueue, &item, portMAX_DELAY) == pdTRUE) {
            if (!item.data || item.len == 0) {
                free(item.data);
                continue;
            }

            // 若 BLE 已断开或 WiFi OTA 挂起，丢弃队列中的残留数据
            if (!BleOta::instance().isConnected() ||
                BleOta::instance().isSuspendedForWifiOta()) {
                DEBUG_DBG(TAG, "BLE不可用 丢%zuB队列", item.len);
                free(item.data);
                continue;
            }

            ble_ota_process_data_batch(item.data, item.len);
            free(item.data);
        }
    }
}

/*
 * VERSION (0x0004) - Read
 * 返回固件版本字符串
 */
/**
 * @brief GATT VERSION 特征访问回调，返回固件版本字符串。
 * @param conn_handle BLE 连接句柄。
 * @param attr_handle GATT 属性句柄。
 * @param ctxt GATT 访问上下文。
 * @param arg 回调用户参数。
 * @return NimBLE ATT 错误码，0 表示成功。
 */
