/*
 * ble_ota_host.cpp - BLE OTA GATT/GAP/广播/NimBLE Host
 */
#include "ble_ota_priv.h"

uint16_t s_data_out_val_handle = 0;

int ble_ota_ctrl_access(uint16_t conn_handle, uint16_t attr_handle,
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

    uint8_t buf[36] = {0};
    int len = ble_ota_mbuf_to_buf(ctxt->om, buf, sizeof(buf));
    if (len < 1) {
        DEBUG_WARN(TAG, "CTRL 数据为空");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    DEBUG_DBG(TAG, "CTRL 写入: cmd=0x%02X len=%d", buf[0], len);

    if (buf[0] == BLE_OTA_CTRL_TRIGGER_GD32) {
        uint16_t session_id = 0;
        if (len >= 35) {
            session_id = (uint16_t)(buf[33] | (buf[34] << 8));
        } else if (len >= 3) {
            session_id = (uint16_t)(buf[1] | (buf[2] << 8));
        }

        if (len < 35) {
            DEBUG_WARN(TAG, "CTRL 0xF7 len<35 缺SHA,拒GD32中继");
            sendFinishEvent(BLE_OTA_FINISH_ERR_SHA_MISMATCH,
                            packBleFinishValue(session_id, 1));
            return 0;
        }

        // 检查固件是否存储完毕 → 通过协调器触发 GD32 OTA 中继
        ota_meta_t meta = {};
        esp_err_t err = OtaCoordinator::instance().loadGd32Meta(&meta);
        if (err == ESP_OK &&
            meta.total_size > 0 &&
            meta.stored_size >= meta.total_size) {
            const uint8_t *frontend_sha = &buf[1];
            char frontend_sha_hex[65] = {0};
            char device_sha_hex[65] = {0};
            shaBytesToHexUpper(frontend_sha, frontend_sha_hex);

            // ★ 统一走共享 helper：计算设备端 SHA → 比对前端 SHA
            uint8_t device_sha[32] = {0};
            err = OtaCoordinator::instance().verifyGd32Firmware(frontend_sha, device_sha);
            shaBytesToHexUpper(device_sha, device_sha_hex);
            sendSha256Notify(device_sha, session_id);
            if (err == ESP_ERR_INVALID_CRC) {
                DEBUG_ERR(TAG, "GD32 SHA-256 不匹配");
                DEBUG_ERR(TAG, "frontend=%s", frontend_sha_hex);
                DEBUG_ERR(TAG, "device=%s", device_sha_hex);
                sendFinishEvent(BLE_OTA_FINISH_ERR_SHA_MISMATCH,
                                packBleFinishValue(session_id, 0));
                return 0;
            } else if (err != ESP_OK) {
                DEBUG_ERR(TAG, "GD32 SHA 失败: %s", esp_err_to_name(err));
                sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                                packBleFinishValue(session_id, 3));
                return 0;
            }

            DEBUG_INFO(TAG, "GD32就绪 SHA匹配 %" PRIu32 "/%" PRIu32 ",触发中继",
                       meta.stored_size, meta.total_size);
            if (OtaRelay::getResultQueue()) {
                uint32_t stale = 0;
                while (xQueueReceive(OtaRelay::getResultQueue(), &stale, 0) == pdTRUE) {
                    DEBUG_WARN(TAG, "清理旧 GD32 中继结果: %" PRIu32, stale);
                }
            }
            esp_err_t trig_err = OtaCoordinator::instance().triggerGd32Relay();
            if (trig_err != ESP_OK) {
                DEBUG_ERR(TAG, "GD32 中继触发失败: %s", esp_err_to_name(trig_err));
                sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                                packBleFinishValue(session_id, 1));
                return 0;
            }

            sendFinishEvent(BLE_OTA_FINISH_GD32_RELAY_START,
                            packBleFinishValue(session_id, 0));
            DEBUG_INFO(TAG, "GD32中继开始 stage=0x10 sess=%u", session_id);

            BleGd32RelayWaitCtx *ctx = (BleGd32RelayWaitCtx *)malloc(sizeof(BleGd32RelayWaitCtx));
            if (!ctx) {
                DEBUG_ERR(TAG, "GD32 中继等待任务上下文分配失败");
                sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                                packBleFinishValue(session_id, 2));
                return 0;
            }
            ctx->session_id = session_id;
            BaseType_t ok = xTaskCreate(ble_gd32_relay_wait_task,
                                        "ble_gd32_wait",
                                        TASK_STACK_BLE_GD32_WAIT,
                                        ctx,
                                        5,
                                        NULL);
            if (ok != pdPASS) {
                DEBUG_ERR(TAG, "创建 GD32 中继等待任务失败");
                free(ctx);
                sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                                packBleFinishValue(session_id, 2));
            }
        } else {
            DEBUG_WARN(TAG, "GD32 固件未就绪 (stored=%" PRIu32 ", total=%" PRIu32 ")",
                       meta.stored_size, meta.total_size);
            sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                            packBleFinishValue(session_id, 1));
        }
    } else if (buf[0] == BLE_OTA_CTRL_TRIGGER_ESP32) {
        uint16_t session_id = 0;
        if (len >= 35) {
            session_id = (uint16_t)(buf[33] | (buf[34] << 8));
        }
        if (!Esp32OtaCore::instance().isActive()) {
            DEBUG_WARN(TAG, "ESP32 OTA 未激活，无法触发升级");
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE,
                            packBleFinishValue(session_id, 1));
            return 0;
        }
        if (BleOta::instance().m_esp32RebootPending) {
            DEBUG_WARN(TAG, "ESP32 OTA 收尾已在进行中，忽略重复 CTRL");
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE,
                            packBleFinishValue(session_id, 1));
            return 0;
        }

        // 强制要求 32B 前端 SHA-256：与 WiFi 流程对齐，设备端读回分区计算并比对，
        // 一致走 end+set_boot+reboot，不一致返回 0x85 ERR_SHA_MISMATCH。
        if (len < 33) {
            DEBUG_WARN(TAG, "CTRL 0xF8 len<33 缺SHA,拒绝");
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE, (uint32_t)ESP_ERR_INVALID_ARG);
            return 0;
        }

        BleEsp32FinishCtx *finish_ctx = (BleEsp32FinishCtx *)malloc(sizeof(BleEsp32FinishCtx));
        if (!finish_ctx) {
            DEBUG_ERR(TAG, "CTRL 0xF8 申请内存失败");
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE,
                            packBleFinishValue(session_id, (uint16_t)ESP_ERR_NO_MEM));
            return 0;
        }
        for (int i = 0; i < 32; ++i) {
            snprintf(&finish_ctx->sha_hex[i * 2], 3, "%02x", buf[1 + i]);
        }
        finish_ctx->sha_hex[64] = '\0';
        finish_ctx->session_id = session_id;
        DEBUG_DBG(TAG, "feSHA=%s sess=%u", finish_ctx->sha_hex, session_id);

        BleOta::instance().m_esp32RebootPending = true;
        BaseType_t ok = xTaskCreate(ble_esp32_finish_and_reboot_task,
                                    "ble_ota_finish",
                                    TASK_STACK_BLE_FINISH,
                                    finish_ctx,  // heap context; task 负责 free
                                    5,
                                    NULL);
        if (ok != pdPASS) {
            DEBUG_ERR(TAG, "创建 ESP32 OTA 收尾任务失败");
            free(finish_ctx);
            BleOta::instance().m_esp32RebootPending = false;
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE,
                            packBleFinishValue(session_id, (uint16_t)ESP_ERR_NO_MEM));
        } else {
            DEBUG_DBG(TAG, "ESP32 OTA 收尾任务已创建（前端 SHA 已传入）");
        }
    } else if (buf[0] == BLE_OTA_CTRL_RESTART_ESP32) {
        // 重启 ESP32：GATT 回调中仅推送 FINISH，延时 + 重启交给独立 task，避免卡 NimBLE host
        DEBUG_INFO(TAG, "0xFA重启,建restart task(500ms)");
        if (BleOta::instance().m_esp32RebootPending) {
            DEBUG_WARN(TAG, "0xFA 重启已在进行中，忽略重复 CTRL");
            return 0;
        }
        BleOta::instance().m_esp32RebootPending = true;
        BaseType_t ok = xTaskCreate(ble_esp32_restart_task,
                                    "ble_ota_restart",
                                    TASK_STACK_BLE_RESTART,
                                    NULL,
                                    5,
                                    NULL);
        if (ok != pdPASS) {
            DEBUG_ERR(TAG, "创建 0xFA 重启任务失败");
            BleOta::instance().m_esp32RebootPending = false;
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE, 0);
        }
    } else if (buf[0] == BLE_OTA_CTRL_ABORT_OTA) {
        // 0xFB: 中止当前 OTA（保持 BLE 连接）
        //   - 数据接收阶段（GD32_STORING / ESP32_RECEIVING）：立即中止 + 清队列 + 回复 ABORTED
        //   - 中继/收尾阶段（GD32_RELAYING / ESP32_FINALIZING）：拒绝中止（SPI 转发不可中断）
        //   - IDLE/已完成状态：幂等回复 ABORTED
        uint16_t session_id = 0;
        if (len >= 3) {
            session_id = (uint16_t)(buf[1] | (buf[2] << 8));
        }

        ota_system_state_t cur_state = OtaCoordinator::instance().getSystemState();
        DEBUG_WARN(TAG, "0xFB abort state=%s sess=%u",
                   OtaCoordinator::stateToString(cur_state), session_id);

        if (cur_state == OTA_SYS_GD32_RELAYING) {
            DEBUG_WARN(TAG, "GD32 SPI中继中,无法中止,请等待");
            sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                            packBleFinishValue(session_id, 2));
            return 0;
        }
        if (cur_state == OTA_SYS_ESP32_FINALIZING) {
            DEBUG_WARN(TAG, "ESP32 OTA 收尾进行中，无法中止");
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE,
                            packBleFinishValue(session_id, 2));
            return 0;
        }

        // ── 可中止状态：清队列 + abort core + 复位协调器 ──
        if (s_otaDataQueue) {
            BleOtaDataBatch stale;
            size_t dropped = 0;
            while (xQueueReceive(s_otaDataQueue, &stale, 0) == pdTRUE) {
                free(stale.data);
                dropped++;
            }
            if (dropped > 0) {
                DEBUG_INFO(TAG, "中止时清理 OTA 数据队列: %zu 项", dropped);
            }
        }

        // abortCurrent 内部按状态分发：GD32_STORING→metaClear, ESP32_RECEIVING→core.abort, IDLE→空操作
        // ble_ota_session_reset() 统一负责 abortCurrent + core.abort 兜底 + 会话状态复位
        // （m_esp32LastSeq/m_esp32LastCmd/m_esp32RebootPending/reorder/s_gd32_ooo_count）
        ble_ota_session_reset();

        ble_ota_restore_wifi_ps();

        DEBUG_INFO(TAG, "OTA 已中止（BLE 保持连接），通知前端 (stage=0x20)");
        sendFinishEvent(BLE_OTA_FINISH_ABORTED,
                        packBleFinishValue(session_id, 0));
    }
    //   0xF7 = 触发 GD32 中继
    //   0xF8 = 触发 ESP32 本机 OTA 收尾：设备端读 OTA 分区算 SHA 并与前端 32B SHA 比对，
    //         一致走 end+set_boot+reboot，不一致返回 0x85 ERR_SHA_MISMATCH（不重启）
    //   0xFA = 重启 ESP32（配套前端 expectedReboot 自动重连）
    //   0xFB = 中止当前 OTA（不断开 BLE）：仅数据接收阶段有效，中继/收尾阶段拒绝

    return 0; // BLE_ATT_ERR_OK 在 NimBLE 中就是 0
}

/*
 * DATA_IN (0x0002) - Write
 * 数据块格式:
 *   [0]      cmd          (0x00=数据)
 *   [1-2]    seq          uint16 LE - 分块序号
 *   [3-6]    total_size   uint32 LE - 固件总大小 (仅 seq=0 时有效)
 *   [7..n]   payload      固件数据
 * 注: 已移除CRC字段（BLE链路层已有CRC校验）
 */
/**
 * @brief GATT DATA_IN 特征访问回调，接收并处理 OTA 数据块。
 * @param conn_handle BLE 连接句柄。
 * @param attr_handle GATT 属性句柄。
 * @param ctxt GATT 访问上下文。
 * @param arg 回调用户参数。
 * @return NimBLE ATT 错误码，0 表示成功。
 */

int ble_ota_version_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }

    const char *version = FW_VERSION;
    // 版本字符串: "vX.Y.Z YYYY-MM-DD HH:MM:SS"
    char ver_buf[64];
    int ver_len = snprintf(ver_buf, sizeof(ver_buf), "%s %s", FW_VERSION, fwBuildTime());
    if (ver_len < 0 || ver_len >= (int)sizeof(ver_buf)) {
        ver_len = strlen(version);
        int rc = os_mbuf_append(ctxt->om, version, ver_len);
        return (rc == 0) ? 0 : BLE_ATT_ERR_UNLIKELY;
    }
    int rc = os_mbuf_append(ctxt->om, ver_buf, ver_len);
    return (rc == 0) ? 0 : BLE_ATT_ERR_UNLIKELY;
}

/*
 * DATA_OUT (0x0003) - Notify only
 * 访问回调不做读写处理，通知由 BleOta::instance().notifyResponse() 发送
 */
/**
 * @brief GATT DATA_OUT 特征访问回调（Notify-only，占位实现）。
 * @param conn_handle BLE 连接句柄。
 * @param attr_handle GATT 属性句柄。
 * @param ctxt GATT 访问上下文。
 * @param arg 回调用户参数。
 * @return 0。
 */
int ble_ota_data_out_access(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return 0;
}

// ============================================================
// GATT 服务定义
// ============================================================

// BLE_UUID16_DECLARE 在 C++ 中产生 "taking address of rvalue" 编译错误。
// 改用 static ble_uuid16_t 变量，再取 .u 地址。
/** @brief OTA GATT 服务 UUID */
const ble_uuid16_t GATT_SVC_UUID      = BLE_UUID16_INIT(BLE_OTA_SERVICE_UUID);
/** @brief CTRL 特征 UUID，写入 0xF7 触发中继 */
const ble_uuid16_t GATT_CTRL_UUID     = BLE_UUID16_INIT(BLE_OTA_CTRL_UUID);
/** @brief DATA_IN 特征 UUID，接收固件数据块 */
const ble_uuid16_t GATT_DATA_IN_UUID  = BLE_UUID16_INIT(BLE_OTA_DATA_IN_UUID);
/** @brief DATA_OUT 特征 UUID，Notify 推送 GD32 响应 */
const ble_uuid16_t GATT_DATA_OUT_UUID = BLE_UUID16_INIT(BLE_OTA_DATA_OUT_UUID);
/** @brief VERSION 特征 UUID，返回固件版本字符串 */
const ble_uuid16_t GATT_VERSION_UUID  = BLE_UUID16_INIT(BLE_OTA_VERSION_UUID);

// NimBLE C 结构体在 C++ designated initializer 不能显式初始化全部字段。
// 用 pragma 局部关闭 -Wmissing-field-initializers 警告。
#ifdef __cplusplus
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

const struct ble_gatt_chr_def gatt_ota_chrs[] = {
    {
        .uuid       = &GATT_CTRL_UUID.u,
        .access_cb  = ble_ota_ctrl_access,
        .flags      = BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid       = &GATT_DATA_IN_UUID.u,
        .access_cb  = ble_ota_data_in_access,
        .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid       = &GATT_DATA_OUT_UUID.u,
        .access_cb  = ble_ota_data_out_access,
        .flags      = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_data_out_val_handle,
    },
    {
        .uuid       = &GATT_VERSION_UUID.u,
        .access_cb  = ble_ota_version_access,
        .flags      = BLE_GATT_CHR_F_READ,
    },
    {}  // 终止符
};

const struct ble_gatt_svc_def gatt_ota_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &GATT_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def *)gatt_ota_chrs,
    },
    {}  // 终止符
};

#ifdef __cplusplus
#pragma GCC diagnostic pop
#endif

// ============================================================
// GAP 事件回调
// ============================================================
/**
   * @brief BLE GAP 事件回调，处理连接和断开事件。
 * @param event
 * @param arg
 * @return 0 成功。
 */
int ble_ota_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            if (BleOta::instance().isSuspendedForWifiOta()) {
                /* WiFi OTA 中拒绝 BLE 连接，立刻踢掉 */
                DEBUG_WARN(TAG, "WiFi OTA 中，拒绝 BLE 连接 handle=%d",
                           event->connect.conn_handle);
                (void)ble_gap_terminate(event->connect.conn_handle,
                                        BLE_ERR_REM_USER_CONN_TERM);
                BleOta::instance().m_bleConnected = false;
                return 0;
            }
            // 连接成功
            BleOta::instance().m_bleConnected = true;
            BleOta::instance().m_connHandle   = event->connect.conn_handle;
            s_conn_fallback_requested = false;
            DEBUG_INFO(TAG, "BLE 客户端已连接: handle=%d", BleOta::instance().m_connHandle);

            ble_ota_log_conn_params(BleOta::instance().m_connHandle, "初始");
            int rc = ble_ota_request_conn_params(BleOta::instance().m_connHandle, false);
            if (rc != 0) {
                s_conn_fallback_requested = true;
                ble_ota_request_conn_params(BleOta::instance().m_connHandle, true);
            }
            /* 优先 LE 2M PHY（空中速率约翻倍）；对端不支持则保持 1M，失败仅打日志 */
            {
                const uint8_t phy_mask = BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK;
                int phy_rc = ble_gap_set_prefered_le_phy(
                    BleOta::instance().m_connHandle, phy_mask, phy_mask,
                    BLE_GAP_LE_PHY_CODED_ANY);
                DEBUG_DBG(TAG, "请求 LE PHY 1M|2M: rc=%d", phy_rc);
            }
        } else {
            DEBUG_WARN(TAG, "BLE 连接失败: status=%d", event->connect.status);
            BleOta::instance().m_bleConnected = false;
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        DEBUG_INFO(TAG, "BLE 客户端已断开: reason=%d",
                   event->disconnect.reason);

        // ★ 断开时清空数据队列，丢弃属于旧会话的残留包
        if (s_otaDataQueue) {
            drainOtaDataQueue();
        }

        BleOta::instance().m_bleConnected = false;
        BleOta::instance().m_connHandle   = BLE_HS_CONN_HANDLE_NONE;
        s_conn_fallback_requested = false;

        /* WiFi OTA 软挂起期间：主动踢连触发的 disconnect 不得 abort WiFi OTA，也不得再开广播 */
        if (BleOta::instance().isSuspendedForWifiOta()) {
            DEBUG_INFO(TAG, "WiFi OTA 挂起中：忽略 BLE 断开对 OTA 状态的影响");
            return 0;
        }

        // ★ BLE 断开时恢复 WiFi 射频
        ble_ota_restore_wifi_ps();

        // ★ 断开时清理"BLE 数据接收阶段"残留的 OTA 会话状态，避免下次刷写被
        //   系统 busy 假拒绝。注意区分可中止 / 不可中止状态：
        //   - GD32_STORING / ESP32_RECEIVING / ESP32_FINALIZING：BLE 是唯一数据源，
        //     断开即等价于中止，必须 abortCurrent() + 清理底层 core。
        //   - GD32_RELAYING / ESP32_REBOOTING：SPI 中继/重启不依赖 BLE，不能打断。
        //     这些状态保留，等中继/重启自然结束。
        //   - IDLE / STORED / COMPLETE / ERROR：abortCurrent 幂等安全，直接调即可。
        {
            ota_system_state_t cur = OtaCoordinator::instance().getSystemState();
            if (cur == OTA_SYS_GD32_RELAYING || cur == OTA_SYS_ESP32_REBOOTING) {
                DEBUG_WARN(TAG, "BLE断但%s进行中,保留状态",
                           OtaCoordinator::stateToString(cur));
            } else if (cur == OTA_SYS_ESP32_RECEIVING ||
                       cur == OTA_SYS_ESP32_FINALIZING ||
                       cur == OTA_SYS_GD32_STORING) {
                /* 仅当当前 OTA 通道是 BLE 时才因断连中止；WiFi 通道的 RECEIVING 不能被误杀 */
                ota_channel_t ch = OTA_CHANNEL_WIFI;
                {
                    /* get channel via snapshot if available */
                    OtaSnapshot snap = OtaCoordinator::instance().getSnapshot();
                    ch = snap.channel;
                }
                if (ch == OTA_CHANNEL_BLE) {
                    DEBUG_WARN(TAG, "BLE断,中止OTA state=%s",
                               OtaCoordinator::stateToString(cur));
                    ble_ota_session_reset();
                } else {
                    DEBUG_INFO(TAG, "BLE 断开但当前为 WiFi OTA，不中止会话");
                }
            } else {
                ble_ota_session_reset();
            }
        }
        // 恢复广播以接受新连接
        ble_ota_start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (BleOta::instance().isSuspendedForWifiOta()) {
            return 0; /* WiFi OTA 中不要自动再开广播 */
        }
        DEBUG_INFO(TAG, "广播周期结束，重新开始");
        ble_ota_start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        DEBUG_INFO(TAG, "CCC upd conn=%d attr=%d n=%d i=%d",
                   event->subscribe.conn_handle,
                   event->subscribe.attr_handle,
                   event->subscribe.cur_notify,
                   event->subscribe.cur_indicate);
        if (event->subscribe.cur_notify) {
            ble_ota_log_conn_params(event->subscribe.conn_handle, "订阅时");
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        DEBUG_DBG(TAG, "BLE 连接参数更新完成: status=%d", event->conn_update.status);
        uint16_t interval = ble_ota_log_conn_params(event->conn_update.conn_handle, "更新后");
        /* 回退阈值：高速分支目标是 itvl ≤ 8（≤10ms）。若协商失败或落在 > 8（>10ms），
         * 说明高速区间被对端拒绝/降级，尝试 Windows 兼容回退路径（10..12.5ms）。
         * 原阈值 > 16（>20ms）过宽松，收窄后让回退更早触发、避免停在 15ms 这种"慢但不算失败"的区间。 */
        if (!s_conn_fallback_requested &&
            (event->conn_update.status != 0 || interval == 0 || interval > 8)) {
            s_conn_fallback_requested = true;
            ble_ota_request_conn_params(event->conn_update.conn_handle, true);
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        DEBUG_INFO(TAG, "MTU 协商: conn=%d mtu=%d",
                   event->mtu.conn_handle, event->mtu.value);
        return 0;

#ifdef BLE_GAP_EVENT_PHY_UPDATE_COMPLETE
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE: {
        /* NimBLE: event type 名 *_COMPLETE，union 字段 phy_updated（非 phy_update）。
         * 2M 生效才能翻倍空中速率；Chrome/Win 常仍停在 1M。 */
        const uint8_t rx = event->phy_updated.rx_phy;
        const uint8_t tx = event->phy_updated.tx_phy;
        const char *rx_phy = (rx == BLE_GAP_LE_PHY_2M) ? "2M" :
                             (rx == BLE_GAP_LE_PHY_1M) ? "1M" :
                             (rx == BLE_GAP_LE_PHY_CODED) ? "CODED" : "?";
        const char *tx_phy = (tx == BLE_GAP_LE_PHY_2M) ? "2M" :
                             (tx == BLE_GAP_LE_PHY_1M) ? "1M" :
                             (tx == BLE_GAP_LE_PHY_CODED) ? "CODED" : "?";
        DEBUG_INFO(TAG, "PHY %d %s/%s",
                   event->phy_updated.status, rx_phy, tx_phy);
        return 0;
    }
#endif

    default:
        return 0;
    }
}

// ============================================================
// BLE 广播
// ============================================================
/**
   * @brief 启动 BLE 广播，使设备可被发现和连接。
 */
void ble_ota_start_advertising(void)  // kept as free function for BLE callback compatibility
{
    if (BleOta::instance().isSuspendedForWifiOta()) {
        DEBUG_DBG(TAG, "WiFi OTA 挂起中，跳过广播");
        return;
    }

    int rc;

    // 自动推断可用地址类型（优先 Public，不可用则生成 Random）
    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        DEBUG_ERR(TAG, "推断地址失败 rc=%d,试随机地址", rc);
        // 生成不可解析随机地址作为 fallback
        ble_addr_t rnd_addr;
        rc = ble_hs_id_gen_rnd(0, &rnd_addr);
        if (rc != 0) {
            DEBUG_ERR(TAG, "随机地址失败 rc=%d", rc);
            return;
        }
        own_addr_type = rnd_addr.type;
    }

    DEBUG_DBG(TAG, "BLE addr type: %s", own_addr_type == BLE_OWN_ADDR_PUBLIC ? "Public" : "Random");

    // --- 构建广播数据 ---
    uint8_t adv_data[31] = {0};
    int     adv_pos = 0;

    // AD Flags: LE General Discoverable + BR/EDR Not Supported
    adv_data[adv_pos++] = 0x02;   // 长度
    adv_data[adv_pos++] = 0x01;   // 类型 = Flags
    adv_data[adv_pos++] = 0x06;   // 值

    // Complete Local Name
    const char *name = BLE_DEVICE_NAME;
    size_t name_len  = strlen(name);
    if (name_len > 29 - adv_pos - 2) {
        name_len = 29 - adv_pos - 2;  // 截断以适配广播包
    }
    adv_data[adv_pos++] = (uint8_t)(name_len + 1);
    adv_data[adv_pos++] = 0x09;   // 类型 = Complete Local Name
    memcpy(&adv_data[adv_pos], name, name_len);
    adv_pos += name_len;

    rc = ble_gap_adv_set_data(adv_data, adv_pos);
    if (rc != 0) {
        DEBUG_ERR(TAG, "设置广播数据失败: %d", rc);
        return;
    }

    // 扫描响应: 包含 Service UUID 以帮助发现
    uint8_t scan_rsp[31] = {0};
    int     rsp_pos = 0;
    scan_rsp[rsp_pos++] = 0x11;   // 长度 (2 + 15)
    scan_rsp[rsp_pos++] = 0x09;   // 类型 = Complete Local Name
    name_len = strlen(name);
    if (name_len > (size_t)(31 - rsp_pos)) {
        name_len = 31 - rsp_pos;
    }
    memcpy(&scan_rsp[rsp_pos], name, name_len);
    rsp_pos += name_len;

    rc = ble_gap_adv_rsp_set_data(scan_rsp, rsp_pos);
    if (rc != 0) {
        DEBUG_WARN(TAG, "设置扫描响应数据失败: %d", rc);
    }

    // 开始广播
    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // own_addr_type 已在上面由 ble_hs_id_infer_auto 推断

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_ota_gap_event, NULL);
    if (rc != 0) {
        DEBUG_ERR(TAG, "启动广播失败: %d", rc);
    } else {
        /* 冷启动与 WiFi 共存恢复都会走这里；常态 INFO 不刷，避免上传日志夹杂 BLE */
        DEBUG_DBG(TAG, "BLE广播就绪: \"%s\"", name);
    }
}

// ============================================================
// NimBLE 同步/重置回调
// ============================================================
/**
   * @brief NimBLE 协议栈同步完成回调，初始化广播。
 */
void ble_ota_on_sync(void)
{
    DEBUG_INFO(TAG, "NimBLE 协议栈同步完成");

    // GATT 服务已在 serverStart() 中注册，不再重复注册
    // 重复调用 ble_gatts_count_cfg / ble_gatts_add_svcs 会导致 BLE_HS_EBUSY

    DEBUG_INFO(TAG, "DATA_OUT 值句柄: %u", s_data_out_val_handle);
    if (s_data_out_val_handle == 0) {
        DEBUG_WARN(TAG, "DATA_OUT 值句柄为 0，通知功能不可用");
    }

    BleOta::instance().m_bleReady = true;
    ble_ota_start_advertising();
}

/**
  * @brief NimBLE 协议栈重置回调。
 * @param reason
 */
void ble_ota_on_reset(int reason)
{
    DEBUG_WARN(TAG, "NimBLE 协议栈重置: reason=%d", reason);
    BleOta::instance().m_bleReady = false;
}

// ============================================================
// NimBLE Host 任务（独立 FreeRTOS 任务）
// ============================================================
/**
   * @brief NimBLE 主机任务入口，处理协议栈事件循环。
 * @param param
 */
void ble_ota_nimble_host_task(void *param)
{
    (void)param;
    DEBUG_INFO(TAG, "NimBLE Host 任务启动");
    nimble_port_run();
    // 正常情况下不会到达这里
    DEBUG_INFO(TAG, "NimBLE Host 任务退出");
}

// ============================================================
// 公开 API
// ============================================================
/**
  * @brief 启动 BLE GATT 服务器并注册 OTA 服务。
 * @return ESP_OK 成功，其他失败。
 */
