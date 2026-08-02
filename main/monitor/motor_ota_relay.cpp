/*
 * motor_ota_relay.cpp - 电机 OTA relay ESP32 侧实现
 */
#include "monitor/motor_ota_relay.h"
#include "monitor/debug_log.h"
#include "transport/spi_master.h"
#include "storage/gd32_flash_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "MOTOR_OTA";

/* SPI 类型常量（与 GD32 app_spi_command_defs.h 对齐） */
static constexpr uint8_t kSpiMotorOtaCmd    = 0xD0U;
static constexpr uint8_t kSpiMotorOtaData   = 0xD1U;
static constexpr uint8_t kSpiMotorOtaStatus = 0xD2U;
static constexpr uint8_t kRspAck  = 0x80U;
static constexpr uint8_t kRspWait = 0x81U;

/* 内部状态 */
static SemaphoreHandle_t s_mutex = nullptr;
static motor_ota_status_t s_status = {};
/* 固件不再整体缓冲在堆里：HTTP handler 流式写入 gd32_ota flash 分区，
 * feedTask 经 flashReadChunk 分块读取（见 feedTask）。 */
static uint32_t s_fw_size = 0;
static uint32_t s_fw_offset = 0;          /* 已喂给 GD32 的偏移 */
static TaskHandle_t s_feed_task = nullptr;
static volatile bool s_feed_running = false;

static int64_t nowMs() {
    return esp_timer_get_time() / 1000;
}

/* ── 共享辅助：终态判断 + QUERY SPI 事务 ── */

/** @brief GD32 侧已进终态（IDLE/FINISHED/ERROR），喂数据应停止。 */
static inline bool isTerminalState(motor_ota_state_t s)
{
    return s == MOTOR_OTA_IDLE || s == MOTOR_OTA_FINISHED || s == MOTOR_OTA_ERROR;
}

/**
 * @brief 发 SPI 0xD0 QUERY 查询 GD32 真实状态。
 * @param out_state   非 null 时写入 GD32 返回的 state
 * @param out_err     非 null 时写入 error_code
 * @param update_status  true 时把结果同步写回 s_status（持锁）
 * @return true=QUERY 成功且 ACK 有有效数据；false=SPI 失败或无 ACK
 *
 * 消除 feedTask 中 4 处重复的 qcmd[1]={2} + transact + parse qp[0..5] 块。
 */
static bool queryMotorOtaStatus(motor_ota_state_t *out_state = nullptr,
                                uint8_t *out_err = nullptr,
                                bool update_status = false)
{
    uint8_t qcmd[1] = {2U};  /* cmd=QUERY */
    uint8_t qt = 0, qp[16] = {0};
    size_t ql = 0;
    if (SpiMaster::instance().transact(
            kSpiMotorOtaCmd, qcmd, 1, &qt, qp, sizeof(qp), &ql, 300U) != ESP_OK
        || qt != kRspAck || ql < 1U) {
        return false;
    }
    const motor_ota_state_t qst = static_cast<motor_ota_state_t>(qp[0]);
    if (out_state) *out_state = qst;
    if (out_err)   *out_err   = (ql >= 2U) ? qp[1] : 0U;
    if (update_status && s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_status.state      = qst;
        s_status.error_code = (ql >= 2U) ? qp[1] : 0U;
        if (ql >= 6U) {
            s_status.packets_acked = static_cast<uint16_t>(qp[2])
                | (static_cast<uint16_t>(qp[3]) << 8);
            s_status.total_packets = static_cast<uint16_t>(qp[4])
                | (static_cast<uint16_t>(qp[5]) << 8);
        }
        if (ql >= 10U) {
            s_status.fw_size = static_cast<uint32_t>(qp[6])
                | (static_cast<uint32_t>(qp[7]) << 8)
                | (static_cast<uint32_t>(qp[8]) << 16)
                | (static_cast<uint32_t>(qp[9]) << 24);
        }
        s_status.last_update_ms = nowMs();
        xSemaphoreGive(s_mutex);
    }
    return true;
}

/* ═══════════════════ 喂数据 task ═══════════════════ */

static void feedTask(void *arg)
{
    (void)arg;
    uint8_t resp_payload[16];
    uint8_t resp_type = 0;
    size_t resp_len = 0;
/* 栈上 chunk：[offset:u32 LE][data…]，避免整固件堆缓冲 */
	    uint8_t chunk_buf[MOTOR_OTA_CHUNK_SIZE];

    /* 整体超时保护：feedTask 最长存活 120s，超时自动退出。
     * 防止 GD32 侧不回 ACK 或持续回 WAIT 时，task 永久卡在循环里，
     * 导致 motorOtaIsActive() 一直为 true、阻塞其他 OTA。 */
    const int64_t deadline_ms = nowMs() + 120000LL;

    /* 启动宽限期：只需覆盖“suppress 生效前在途的陈旧 0xD2 push”这一极小窗口——
     * s_status 刚被 motorOtaStart 置为 HANDSHAKE_REQ，下方 suppress 后 ring 不再读，
     * 残留 ERROR 只可能在这几十 ms 内覆写。原 1500ms 基于 kBootSettleMs=800ms 时代，
     * GD32 v1.5.1 已移除残帧窗口（收 0x103 即进握手，握手后不再发 ERROR push），
     * 100ms 足够（用户指定）。 */
    const int64_t grace_end_ms = nowMs() + 100LL;

    /* 重新启用 suppressRingService：ring service 抢占 SPI 带宽导致吐量从 ~5 chunks/s 降到 ~1.5 chunks/s。
     * 进度显示由 feedTask 自己基于 s_fw_offset 更新(近似值)，不依赖 GD32 push。 */
    SpiMaster::instance().setSuppressRingService(true);

    /* suppress 期间 s_status 不被 0xD2 push 更新（worker 不读 ring），
     * feedTask 无法靠 motorOtaGetStatus() 检测 GD32 是否已进 ERROR/FINISHED 终态。
     * 连续 SPI 失败计数：超过阈值时主动 QUERY GD32 真实状态，避免盲重试空转到 120s。 */
    uint32_t consec_fail = 0U;
    constexpr uint32_t kQueryAfterFails = 8U;  /* 连续 8 次失败(~2s)后 QUERY 一次 */
    uint32_t consec_wait = 0U;
    constexpr uint32_t kQueryAfterWaits = 150U; /* 连续 150 次 WAIT(~300ms)后 QUERY：
                                                  * 正常 buffer-full WAIT <100 次即恢复；
                                                  * 超限说明 GD32 可能因 auto-retry 重置了
                                                  * s_fw_fed=0，当前 offset 被 gap 拒收。 */
    uint32_t ack_n = 0U;  /* 成功 ACK 计数，用于降频写 s_status */
    bool handshake_confirmed = false;  /* GD32 已进 kTransfer/kErase，可标 TRANSFER */

    while (s_feed_running && s_fw_offset < s_fw_size) {
        if (nowMs() >= deadline_ms) {
            ESP_LOGE(TAG, "feedTask 整体超时 (120s)，强制退出 offset=%u/%u",
                     (unsigned)s_fw_offset, (unsigned)s_fw_size);
            break;
        }

        /* GD32 已进终态：停止喂，避免盲重试空转到 120s 超时。
         * 启动宽限期(100ms)内跳过终态检查：避免上一次 OTA 残留的 ERROR 状态推送
         * (0xD2 ring 延迟到达) 覆写 s_status 导致 feedTask 误判退出发 CANCEL。 */
        const motor_ota_status_t st = motorOtaGetStatus();
        if (nowMs() >= grace_end_ms) {
            if (isTerminalState(st.state)) {
                ESP_LOGW(TAG, "GD32 终态 0x%02X，停止喂数据 offset=%u/%u",
                         (int)st.state, (unsigned)s_fw_offset, (unsigned)s_fw_size);
                break;
            }
        }
        /* GD32 auto-retry 检测（v1.5.5+）：GD32 SPI 断流/电机超时后自动重新握手，
         * 重置 s_fw_fed=0。ESP32 侧通过连续 WAIT→QUERY 检测到 state!=kTransfer 后
         * 重置 s_fw_offset=0 从头喂（见下方 kRspWait 分支）。
         * 注：v1.4.38~v1.5.4 曾移除此逻辑（当时无 auto-retry，握手失败即中止）。 */

const uint32_t remaining = s_fw_size - s_fw_offset;
	        const uint32_t data_len = (remaining < MOTOR_OTA_CHUNK_DATA)
	                                     ? remaining : MOTOR_OTA_CHUNK_DATA;
	        const uint32_t frame_len = MOTOR_OTA_CHUNK_HDR + data_len;

	        /* [0..3]=绝对 offset LE，[4..]=固件数据。重传同 offset 时 GD32 幂等。 */
	        chunk_buf[0] = static_cast<uint8_t>(s_fw_offset & 0xFFU);
	        chunk_buf[1] = static_cast<uint8_t>((s_fw_offset >> 8) & 0xFFU);
	        chunk_buf[2] = static_cast<uint8_t>((s_fw_offset >> 16) & 0xFFU);
	        chunk_buf[3] = static_cast<uint8_t>((s_fw_offset >> 24) & 0xFFU);
	        esp_err_t rd = Gd32FlashStore::instance().flashReadChunk(
	            s_fw_offset, chunk_buf + MOTOR_OTA_CHUNK_HDR, data_len);
	        if (rd != ESP_OK) {
	            ESP_LOGE(TAG, "flash read fail @%u: %s", (unsigned)s_fw_offset, esp_err_to_name(rd));
	            break;
	        }

	        esp_err_t ret = SpiMaster::instance().transact(
	            kSpiMotorOtaData,
	            chunk_buf, frame_len,
	            &resp_type, resp_payload, sizeof(resp_payload), &resp_len,
	            200U);  /* 200ms: v1.4.59成功时的确切值。poll有~95ms 做多次盲读 */

	        if (ret != ESP_OK) {
	            ESP_LOGW(TAG, "SPI data fail: %s offset=%u", esp_err_to_name(ret), (unsigned)s_fw_offset);
	            vTaskDelay(pdMS_TO_TICKS(10));  /* 快速重试（原 100ms 太慢导致吐量低） */
	            /* suppress 期间 s_status 不更新，连续失败时主动 QUERY GD32 真实状态，
	             * 避免 GD32 已进 ERROR 终态但 feedTask 不知道、盲重试空转到 120s 超时。 */
                    if (++consec_fail >= kQueryAfterFails) {
                        consec_fail = 0U;
                        motor_ota_state_t qst;
                        uint8_t qerr;
                        if (queryMotorOtaStatus(&qst, &qerr, true) && isTerminalState(qst)) {
                            ESP_LOGW(TAG, "QUERY 检测到 GD32 终态 0x%02X err=0x%02X，停止喂数据 offset=%u/%u",
                                     (int)qst, (unsigned)qerr,
                                     (unsigned)s_fw_offset, (unsigned)s_fw_size);
                            break;
                        }
                    }
	            continue;  /* 不推进 offset，同 offset 重发 */
	        }
	        consec_fail = 0U;
	        consec_wait = 0U;

        if (resp_type == kRspAck) {
            s_fw_offset += data_len;
            /* 进度：每 8 chunk 写 s_status。
             * ★ 不再喂数据一成功就强标 TRANSFER：GD32 feedData() 在握手阶段
             *   (kPreCancel/kHandshakeReq 等) 也接收预填充数据，若此刻强标
             *   TRANSFER，无电机的空通道会让网页误显示"数据传输 N/912"而非
             *   "握手中…→超时"。改为：只在 GD32 真正进 kErase/kTransfer 后
             *   才标 TRANSFER；握手未完成时保持握手态显示。
             *   握手未确认时每 8 个 ACK 重新 QUERY 一次（GD32 握手+擦除
             *   通常 1~3s，8 个 ACK ~16 个 chunk ≈ 3700B <1s，需多轮确认）。 */
            ++ack_n;
            const bool touch_status = ((ack_n & 7U) == 0U) || (s_fw_offset >= s_fw_size);

            /* 握手未确认时定期 QUERY：确认 GD32 已过握手阶段才标 TRANSFER */
            if (!handshake_confirmed && (ack_n & 7U) == 0U) {
                motor_ota_state_t qst;
                if (queryMotorOtaStatus(&qst, nullptr, false)) {
                    if (qst == MOTOR_OTA_ERASE || qst == MOTOR_OTA_TRANSFER ||
                        qst == MOTOR_OTA_FINISH_OTA || qst == MOTOR_OTA_WAIT_APP_RUN) {
                        handshake_confirmed = true;
                        if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
                            s_status.state = MOTOR_OTA_TRANSFER;
                            s_status.last_update_ms = nowMs();
                            xSemaphoreGive(s_mutex);
                        }
                    }
                    /* 终态（ERROR/IDLE/FINISHED）交给下方终态检测处理 */
                }
            }
            if (touch_status) {
                const uint16_t fed_pkts = static_cast<uint16_t>((s_fw_offset + 47U) / 48U);
                if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
                    /* 握手未确认时不标 TRANSFER、不更新 packets_acked：
                     * 此刻 fed_pkts 是喂给 GD32 环缓冲的字节数（预填充），
                     * 不是电机真实 ACK，握手未成时显示会让用户误以为在传输。 */
                    if (handshake_confirmed) {
                        s_status.state = MOTOR_OTA_TRANSFER;
                        s_status.packets_acked = fed_pkts;
                    }
                    s_status.last_update_ms = nowMs();
                    xSemaphoreGive(s_mutex);
                }
            }
	        } else if (resp_type == kRspWait) {
	            /* 缓冲满 / gap 拒收：不推进 offset，稍候重试。
	             * WAIT 路径 SPI 层已推进 seq，避免同 seq 重发被 GD32 当重复请求死锁。 */
	            vTaskDelay(pdMS_TO_TICKS(2));
	            /* 连续 WAIT 超限：可能 GD32 auto-retry 重置了 s_fw_fed=0，
	             * 当前 offset 被 gap 拒收（永远不会变 ACK）。QUERY 确认真实状态。 */
            if (++consec_wait >= kQueryAfterWaits) {
                consec_wait = 0U;
                motor_ota_state_t qst;
                uint8_t qerr;
                if (queryMotorOtaStatus(&qst, &qerr, false)) {
                    if (qst != MOTOR_OTA_TRANSFER) {
                        /* GD32 不在传输态（auto-retry 回了 kPreCancel/握手）：
                         * 重置 offset 从头喂，配合 GD32 重新擦除+握手。 */
                        ESP_LOGW(TAG, "GD32 retry detected: state=%u err=0x%02X, reset offset %u->0",
                                 (unsigned)qst, (unsigned)qerr, (unsigned)s_fw_offset);
                        s_fw_offset = 0U;
                        ack_n = 0U;
                        handshake_confirmed = false;  /* 重新握手，需重新确认 */
                    } else {
                        handshake_confirmed = true;  /* GD32 在 transfer，确认握手完成 */
                    }
                    /* 终态仍 break */
                    if (isTerminalState(qst)) {
                        ESP_LOGW(TAG, "QUERY(WAIT) 终态 0x%02X err=0x%02X，停止",
                                 (unsigned)qst, (unsigned)qerr);
                        if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            s_status.state = qst;
                            s_status.error_code = qerr;
                            s_status.last_update_ms = nowMs();
                            xSemaphoreGive(s_mutex);
                        }
                        break;
                    }
                }
            }
	        } else {
	            /* 非 ACK/WAIT（含 ERROR/陈旧 type）：不 break 整次 OTA。
	             * 曾因 poll 读到 EVENT/错 type 直接 break → fed 卡首 chunk。 */
	            ESP_LOGW(TAG, "SPI data unexpected type=0x%02X offset=%u，重试",
	                     resp_type, (unsigned)s_fw_offset);
	            vTaskDelay(pdMS_TO_TICKS(5));
	        }
	    }

/* 恢复 ring service，让后续 GD32 push(FINISHED/ERROR) 正常到达 */
	    SpiMaster::instance().setSuppressRingService(false);

	    /* feedTask 字节喂完 ≠ GD32/电机已完成。suppress 期间 0xD2 被丢；
	     * 末包 CAN ACK / 跳转 APP 还要数百 ms。只 QUERY 一次会把前端卡在 kTransfer。
	     * 退出后轮询 QUERY，直到 GD32 进 FINISHED/ERROR/IDLE，或最多 ~8s。 */
	    {
	        constexpr uint32_t kExitPollMs = 8000U;
	        constexpr uint32_t kExitGapMs  = 150U;
	        const uint32_t exit_deadline = nowMs() + kExitPollMs;
	        bool got_terminal = false;
	        while (nowMs() < exit_deadline) {
            /* 优先吃 ring 推送（已恢复 suppress） */
            {
                const motor_ota_status_t st = motorOtaGetStatus();
                if (isTerminalState(st.state)) {
                    ESP_LOGI(TAG, "feedTask 退出，ring 终态: state=%u err=%u pkt=%u/%u",
                             (unsigned)st.state, (unsigned)st.error_code,
                             (unsigned)st.packets_acked, (unsigned)st.total_packets);
                    got_terminal = true;
                    break;
                }
            }
            motor_ota_state_t qst;
            uint8_t qerr;
            if (queryMotorOtaStatus(&qst, &qerr, true)) {
                if (isTerminalState(qst)) {
                    const motor_ota_status_t snap = motorOtaGetStatus();
                    ESP_LOGI(TAG, "feedTask 退出，QUERY 终态: state=%u err=%u pkt=%u/%u",
                             (unsigned)qst, (unsigned)qerr,
                             (unsigned)snap.packets_acked, (unsigned)snap.total_packets);
                    got_terminal = true;
                    break;
                }
            }
	            vTaskDelay(pdMS_TO_TICKS(kExitGapMs));
	        }
	        if (!got_terminal) {
	            ESP_LOGW(TAG, "feedTask 退出轮询超时，前端可能仍显示传输中 offset=%u/%u",
	                     (unsigned)s_fw_offset, (unsigned)s_fw_size);
	        }
	    }

	    s_feed_running = false;
	    s_feed_task = nullptr;
	    vTaskDelete(NULL);
	}

/* ═══════════════════ 公开接口 ═══════════════════ */

void motorOtaRelayInit(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

void onMotorOtaStatusPush(const uint8_t *payload, size_t len)
{
    if (len < 12U || !s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

    s_status.state         = static_cast<motor_ota_state_t>(payload[0]);
    s_status.error_code    = payload[1];
    s_status.packets_acked = static_cast<uint16_t>(payload[2]) | (static_cast<uint16_t>(payload[3]) << 8);
    s_status.total_packets = static_cast<uint16_t>(payload[4]) | (static_cast<uint16_t>(payload[5]) << 8);
    s_status.fw_size       = static_cast<uint32_t>(payload[6])
                           | (static_cast<uint32_t>(payload[7]) << 8)
                           | (static_cast<uint32_t>(payload[8]) << 16)
                           | (static_cast<uint32_t>(payload[9]) << 24);
    s_status.buf_used      = static_cast<uint16_t>(payload[10]) | (static_cast<uint16_t>(payload[11]) << 8);
    s_status.last_update_ms = nowMs();

    xSemaphoreGive(s_mutex);
}

int motorOtaStart(uint32_t fw_size, uint8_t channel)
{
    if (fw_size == 0 || fw_size > MOTOR_OTA_MAX_FW_SIZE || channel > 2) {
        return -1;
    }
    if (s_feed_running) {
        return -2;
    }

    /* 固件已由 HTTP handler 流式写入 gd32_ota 分区；此处不再堆缓冲 */
    s_fw_size = fw_size;
    s_fw_offset = 0;

    /* 发 SPI start 命令（best-effort，不阻塞等 ACK）。
     * SPI "延迟一拍"协议下，START 的 ACK 偶发被 send 阶段当作 stale 吃掉、
     * poll 反读空 → transact 报 TIMEOUT，但 GD32 实际已收到 START 进握手
     * （GD32 日志 "SPI 0xD0 START recv" + "START v150" 每次稳定出现）。
     * 重发 START 会被 GD32 拒（已在进行中→0xE5），QUERY 的 ACK 同样受
     * delayed-by-one 影响不可靠。改为 fire-and-forget：命令发出即继续，
     * 由 feedTask 靠 GD32 push 的状态 + 超时保护自决去留。 */
    uint8_t cmd[6];
    cmd[0] = 0U;  /* cmd=START */
    cmd[1] = channel;
    cmd[2] = static_cast<uint8_t>(fw_size & 0xFF);
    cmd[3] = static_cast<uint8_t>((fw_size >> 8) & 0xFF);
    cmd[4] = static_cast<uint8_t>((fw_size >> 16) & 0xFF);
    cmd[5] = static_cast<uint8_t>((fw_size >> 24) & 0xFF);

    uint8_t resp_type = 0;
    uint8_t resp_payload[16];
    size_t resp_len = 0;
    esp_err_t ret = SpiMaster::instance().transact(
        kSpiMotorOtaCmd, cmd, 6,
        &resp_type, resp_payload, sizeof(resp_payload), &resp_len,
        300U);  /* 300ms: fire-and-forget，让 worker 有足够时间完成 START。
                 * 原 500ms 偏保守；GD32 收到 START 立即进 kPreCancel，<50ms 响应。 */
    if (ret != ESP_OK || resp_type != kRspAck) {
        DEBUG_WARN(TAG, "START SPI 未确认 ACK (ret=%s resp=0x%02X)——fire-and-forget 继续",
                   esp_err_to_name(ret), resp_type);
    }

    /* 初始化状态 */
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(&s_status, 0, sizeof(s_status));
        s_status.state = MOTOR_OTA_HANDSHAKE_REQ;
        s_status.fw_size = fw_size;
        s_status.total_packets = static_cast<uint16_t>((fw_size + 47U) / 48U);
        s_status.last_update_ms = nowMs();
        xSemaphoreGive(s_mutex);
    }

/* 启动喂数据 task。prio=7 > spi_worker(6) > httpd(5)：
	     * 传输期优先抢 SPI 队列；实际总线访问仍经 worker 串行。 */
	    s_feed_running = true;
	    BaseType_t ok = xTaskCreate(feedTask, "ota_feed", 4096, nullptr, 7, &s_feed_task);
    if (ok != pdPASS) {
        s_feed_running = false;
        return -3;
    }

    ESP_LOGI(TAG, "Motor OTA started: %lu B, ch=%u", (unsigned long)fw_size, channel);
    return 0;
}

void motorOtaCancel(void)
{
    DEBUG_WARN(TAG, "cancel: feed_running=%d task=%p offset=%u/%u",
               (int)s_feed_running, s_feed_task,
               (unsigned)s_fw_offset, (unsigned)s_fw_size);

    /* 立即标记 false，让 feedTask 在下次循环检查时退出。
     * motorOtaIsActive() 依赖此标志，设 false 后后续 OTA 不再被阻塞。 */
    s_feed_running = false;

    /* 标记状态为 IDLE，让前端 /motor/ota/status 立即看到空闲 */
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_status.state = MOTOR_OTA_IDLE;
        s_status.last_update_ms = nowMs();
        xSemaphoreGive(s_mutex);
    }

    /* 通知 GD32 取消 relay 状态机 + 等 feedTask 退出是耗时操作
     *（SPI transact 最多 1s + feedTask 退出最多 2s），不能在 httpd 线程
     * 同步阻塞，否则 cancel HTTP 请求本身会超时（"Failed to fetch"），
     * 且 httpd 单线程会饿死其他端点。
     * 用独立 task 异步执行，HTTP handler 立即返回。 */
    static TaskHandle_t cancel_task = nullptr;
    if (cancel_task != nullptr) {
        /* 上一次 cancel 还在跑，不重复创建 */
        DEBUG_WARN(TAG, "cancel: previous cancel task still running, skip");
        return;
    }

    auto do_cancel = [](void *) {
        /* 通知 GD32 取消 relay 状态机 */
        uint8_t cmd[1] = {1U};  /* cmd=CANCEL */
        uint8_t resp_type = 0;
        uint8_t resp_payload[4];
        size_t resp_len = 0;
        esp_err_t ret = SpiMaster::instance().transact(
            kSpiMotorOtaCmd, cmd, 1,
            &resp_type, resp_payload, sizeof(resp_payload), &resp_len,
            1000U);
        DEBUG_WARN(TAG, "cancel: GD32 resp ret=%s type=0x%02X", esp_err_to_name(ret), resp_type);

        /* 等待 feedTask 真正退出（最多 2s），避免 s_feed_running 已清但 task
         * 仍卡在 SpiMaster::transact 阻塞中，导致 motorOtaIsActive() 瞬时仍 true。 */
        TaskHandle_t t = s_feed_task;
        if (t) {
            for (int i = 0; i < 200 && s_feed_task != nullptr; i++) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (s_feed_task != nullptr) {
                DEBUG_ERR(TAG, "cancel: feedTask 未在 2s 内退出，强制删除");
                vTaskDelete(s_feed_task);
                s_feed_task = nullptr;
            }
        }
        s_feed_running = false;  /* 双保险 */
        cancel_task = nullptr;
        vTaskDelete(NULL);
    };

    BaseType_t ok = xTaskCreate(do_cancel, "ota_cancel", 3072, nullptr, 4, &cancel_task);
    if (ok != pdPASS) {
        DEBUG_ERR(TAG, "cancel: failed to create cancel task, doing sync cancel");
        cancel_task = nullptr;
        /* 降级：同步执行（极端 OOM 情况） */
        uint8_t cmd[1] = {1U};
        uint8_t resp_type = 0;
        uint8_t resp_payload[4];
        size_t resp_len = 0;
        (void)SpiMaster::instance().transact(
            kSpiMotorOtaCmd, cmd, 1,
            &resp_type, resp_payload, sizeof(resp_payload), &resp_len,
            1000U);
        TaskHandle_t t = s_feed_task;
        if (t) {
            for (int i = 0; i < 200 && s_feed_task != nullptr; i++) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (s_feed_task != nullptr) {
                vTaskDelete(s_feed_task);
                s_feed_task = nullptr;
            }
        }
        s_feed_running = false;
    }
    DEBUG_WARN(TAG, "cancel: dispatched feed_running=%d", (int)s_feed_running);
}

bool motorOtaIsActive(void)
{
    return s_feed_running;
}

motor_ota_status_t motorOtaGetStatus(void)
{
    motor_ota_status_t out = {};
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        out = s_status;
        xSemaphoreGive(s_mutex);
    }
    return out;
}
