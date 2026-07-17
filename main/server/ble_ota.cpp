/*
 * ble_ota.cpp - BLE OTA 公开 API + 收尾任务 + WiFi 共存
 * 数据路径：ble_ota_data.cpp；GATT/GAP：ble_ota_host.cpp
 */
#include "ble_ota_priv.h"

QueueHandle_t s_otaDataQueue = nullptr;
bool s_wifi_ps_boosted = false;

void drainOtaDataQueue()
{
    BleOtaDataBatch stale;
    while (s_otaDataQueue && xQueueReceive(s_otaDataQueue, &stale, 0) == pdTRUE) {
        if (stale.data) free(stale.data);
    }
}

BleOta& BleOta::instance()
{
    static BleOta inst;
    return inst;
}

// File-scope val_handle for GATT service definition (struct literal needs compile-time address)

// ESP32 自身 OTA 收尾任务（OTA 写入/校验/重启逻辑委托给 Esp32OtaCore）

/**
 * @brief 异步完成 ESP32 OTA 收尾并触发重启。
 *
 * 实际的 SHA 校验 / esp_ota_end / esp_ota_set_boot_partition 全部由
 * Esp32OtaCore::finalize 完成。本任务仅负责：补发 ACK → 调 finalize → 上报 SHA → 通知前端。
 * @param pvParameters 任务参数，当前未使用。
 */
/**
 * @brief 0xFA 重启任务：为了不卡 NimBLE host task，仅在这里推送 FINISH 后 500ms 重启。
 * @param pvParameters 任务参数，未使用。
 */
void ble_esp32_restart_task(void *pvParameters)
{
    (void)pvParameters;
    DEBUG_INFO(TAG, "[RESTART] 推送 FINISH_REBOOT_NOW 后 500ms 重启");
    sendFinishEvent(BLE_OTA_FINISH_REBOOT_NOW, 0);
    OtaCoordinator::instance().restartEsp32();   // 内部 notify + delay + esp_restart，不返回
}

/**
  * @brief 异步完成 ESP32 OTA 收尾并触发重启。
  * @param pvParameters BleEsp32FinishCtx（堆分配，任务负责释放）。
  */
void ble_esp32_finish_and_reboot_task(void *pvParameters)
{
    BleEsp32FinishCtx *ctx = (BleEsp32FinishCtx *)pvParameters;
    // RAII：保证所有 return 路径自动释放 ctx（restartEsp32 不返回路径除外）
    BleFinishCtxDeleter ctx_guard(ctx);
    char *frontend_sha_hex = ctx ? ctx->sha_hex : nullptr;
    uint16_t session_id = ctx ? ctx->session_id : 0;
    DEBUG_DBG(TAG, "ESP32 OTA 异步收尾任务启动");

    Esp32OtaCore &core = Esp32OtaCore::instance();

    // 补发最终 ACK，让前端尽快得知最后一包已收到
    if (BleOta::instance().m_esp32LastSeq != 0xFFFFU) {
        sendWindowAck(BleOta::instance().m_esp32LastCmd,
                      BleOta::instance().m_esp32LastSeq,
                      (uint32_t)core.getReceived());
        DEBUG_DBG(TAG, "ESP32 最终 ACK 已补发: seq=%u", BleOta::instance().m_esp32LastSeq);
    }
    vTaskDelay(pdMS_TO_TICKS(150));

    if (!core.isActive()) {
        DEBUG_WARN(TAG, "异步收尾时 OTA 状态无效，放弃重启");
        sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE,
                        packBleFinishValue(session_id, 1));
        BleOta::instance().m_esp32RebootPending = false;
        // 无效会话：协调器可能仍假占用，走统一会话复位
        ble_ota_session_reset();
        ble_ota_restore_wifi_ps();
        vTaskDelete(NULL);
        return;
    }

    DEBUG_DBG(TAG, "ESP32 OTA 收尾: 共 %u 字节", (unsigned)core.getReceived());

    // ★ 收尾序列（notify + finalize）由协调器统一封装，reboot=false 让 device_sha 可被读取
    Esp32FinalizeResult fr = OtaCoordinator::instance().finalizeForReboot(frontend_sha_hex);

    if (fr.err == ESP_OK) {
        // 1) 把设备端 SHA 通过 SHA256 通知推给前端
        if (fr.device_sha_hex[0] != '\0') {
            uint8_t device_sha[32] = {0};
            for (int i = 0; i < 32; ++i) {
                unsigned int byte = 0;
                sscanf(&fr.device_sha_hex[i * 2], "%02x", &byte);
                device_sha[i] = (uint8_t)byte;
            }
            sendSha256Notify(device_sha, session_id);
        }
        // 2) 等待 SHA 通知从 NimBLE host 队列送出
        vTaskDelay(pdMS_TO_TICKS(150));

        // 3) 推送"OTA 收尾完成"事件
        sendFinishEvent(BLE_OTA_FINISH_DONE, packBleFinishValue(session_id, 0));
        sendFinishEvent(BLE_OTA_FINISH_REBOOT_NOW, packBleFinishValue(session_id, 0));

        // 4) 通知全部发出后由协调器统一重启（notify + delay + esp_restart）
        DEBUG_INFO(TAG, "[FINISH] ESP32 OTA 收尾完成，重启中");
        OtaCoordinator::instance().restartEsp32();   // 不返回，ctx 由系统回收
    } else if (fr.err == ESP_ERR_INVALID_VERSION) {
        DEBUG_ERR(TAG, "[FINISH] SHA-256 不匹配，OTA 已中止");
        sendFinishEvent(BLE_OTA_FINISH_ERR_SHA_MISMATCH, packBleFinishValue(session_id, 0));
        BleOta::instance().m_esp32RebootPending = false;
        // ★ finalizeForReboot 失败路径已 abortCurrent（协调器→IDLE + core 释放）。
        //   此处只清 BLE 会话侧 reorder，避免二次 abort 干扰新会话。
        esp32_reorder_free();
        BleOta::instance().m_esp32LastSeq = 0xFFFFU;
        BleOta::instance().m_esp32LastCmd = 0;
        ble_ota_restore_wifi_ps();
    } else {
        DEBUG_ERR(TAG, "[FINISH] OTA 收尾失败: %s", esp_err_to_name(fr.err));
        sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE,
                        packBleFinishValue(session_id, 1));
        BleOta::instance().m_esp32RebootPending = false;
        // ★ 同上：协调器已由 finalizeForReboot 复位，仅清 BLE 侧残留
        esp32_reorder_free();
        BleOta::instance().m_esp32LastSeq = 0xFFFFU;
        BleOta::instance().m_esp32LastCmd = 0;
        ble_ota_restore_wifi_ps();
    }

    vTaskDelete(NULL);
}

// ============================================================
// BLE OTA 期间 WiFi 共存：保持 WIFI_PS_NONE；暂停 RSSI 轮询
// ============================================================


/**
 * @brief BLE OTA 开始时的 WiFi 共存处理（暂停 RSSI 轮询，不切省电模式）。
 */
void ble_ota_boost_wifi_ps()
{
    if (s_wifi_ps_boosted) return;
    s_wifi_ps_boosted = true;
    // 通过协调器通知 WiFi 层
    OtaCoordinator::instance().notifyBleCoexStart();
    DEBUG_DBG(TAG, "BLE OTA 数据传输进行中：暂停 WiFi RSSI 轮询（保持 WIFI_PS_NONE，STA 仍可响应 HTTP）");
}

/**
 * @brief BLE OTA 结束/中止时恢复 WiFi RSSI 轮询。
 */
void ble_ota_restore_wifi_ps()
{
    if (!s_wifi_ps_boosted) return;
    s_wifi_ps_boosted = false;
    // 通过协调器通知 WiFi 层
    OtaCoordinator::instance().notifyBleCoexEnd();
    DEBUG_DBG(TAG, "BLE OTA 数据传输结束：恢复 WiFi RSSI 轮询（GD32 中继仍可能进行中）");
}

// ============================================================
// NimBLE 工具: 从 mbuf 读取原始数据
/**
  * @brief 将 NimBLE mbuf 中的数据拷贝到普通字节缓冲区。
 * @param om NimBLE mbuf 数据指针。
 * @param buf 输出缓冲区。
 * @param buf_len 缓冲区最大长度。
 * @return 拷贝的字节数，失败返回 -1。
 */
int ble_ota_mbuf_to_buf(struct os_mbuf *om, uint8_t *buf, size_t buf_len)
{
    if (!om || !buf) return -1;
    uint16_t om_len = OS_MBUF_PKTLEN(om);
    uint16_t copy_len = (om_len > buf_len) ? (uint16_t)buf_len : om_len;
    os_mbuf_copydata(om, 0, copy_len, buf);
    return (int)copy_len;
}


esp_err_t BleOta::serverStart()
{
    int rc;

    if (m_stackRunning) {
        DEBUG_INFO(TAG, "serverStart: 栈已在运行，跳过");
        return ESP_OK;
    }

    DEBUG_INFO(TAG, "=== 初始化 BLE OTA GATT 服务器 ===");
    DEBUG_INFO(TAG, "  Device: %s", BLE_DEVICE_NAME);
    DEBUG_INFO(TAG, "  MTU:    %" PRIu16, BLE_MTU_SIZE);
    DEBUG_INFO(TAG, "  Svc:    0x%04X", BLE_OTA_SERVICE_UUID);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    DEBUG_INFO(TAG, "nimble_port_init 完成");

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    if (rc != 0) {
        DEBUG_WARN(TAG, "设置设备名称失败: %d", rc);
    }

    rc = ble_att_set_preferred_mtu(BLE_MTU_SIZE);
    if (rc != 0) {
        DEBUG_WARN(TAG, "设置首选 MTU 失败: %d", rc);
    } else {
        DEBUG_INFO(TAG, "首选 ATT MTU 已设置: %u", (unsigned)BLE_MTU_SIZE);
    }

    // 5. 配置 Host 回调
    //    sync_cb: NimBLE 事件循环就绪时调用 (开始广播)
    //    reset_cb: NimBLE 重置时调用
    ble_hs_cfg.sync_cb  = ble_ota_on_sync;
    ble_hs_cfg.reset_cb = ble_ota_on_reset;
    // 不做 bonding，不需要 store 回调

    rc = ble_gatts_count_cfg(gatt_ota_svcs);
    if (rc != 0) {
        DEBUG_ERR(TAG, "GATT 服务计数失败: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(gatt_ota_svcs);
    if (rc != 0) {
        DEBUG_ERR(TAG, "GATT 服务注册失败: %d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(ble_ota_nimble_host_task);

    // s_data_out_val_handle 由 on_sync 回调填充，此处不复制
    BleOta::instance().m_stackRunning = true;
    BleOta::instance().m_suspendedForWifiOta = false;
    DEBUG_INFO(TAG, "BLE OTA 服务器初始化完成");
    return ESP_OK;
}

esp_err_t BleOta::suspendStack()
{
    /*
     * ESP32-C3 WiFi/BLE 共用射频：此处仅软挂起（停广播、断连、拒收新数据），
     * 不调用 nimble_port_stop / deinit / controller_disable。
     */
    if (m_suspendedForWifiOta) {
        DEBUG_DBG(TAG, "suspendStack: 已挂起，跳过");
        return ESP_OK;
    }


    if (m_stackRunning) {
        (void)ble_gap_adv_stop();
        if (m_bleConnected && m_connHandle != 0xFFFFU) {
            (void)ble_gap_terminate(m_connHandle, BLE_ERR_REM_USER_CONN_TERM);
            /* 短等即可，勿长时间阻塞 httpd */
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }

    m_bleConnected = false;
    m_connHandle = 0xFFFFU;
    /* 保持 m_bleReady / m_stackRunning：栈仍在，仅业务拒绝连接与写 */
    m_suspendedForWifiOta = true;

    if (s_otaDataQueue) {
        drainOtaDataQueue();
    }

    return ESP_OK;
}

esp_err_t BleOta::resumeStack()
{
    if (!m_suspendedForWifiOta) {
        return ESP_OK;
    }

    m_suspendedForWifiOta = false;

    if (!m_stackRunning) {
        /* 仅当曾 hard-deinit 过才需要完整 serverStart */
        DEBUG_INFO(TAG, "WiFi OTA 结束：BLE 栈未运行，尝试 serverStart free=%lu",
                   (unsigned long)esp_get_free_heap_size());
        esp_err_t err = serverStart();
        if (err != ESP_OK) {
            DEBUG_ERR(TAG, "resumeStack/serverStart 失败: %s", esp_err_to_name(err));
            m_suspendedForWifiOta = true;
            return err;
        }
        return ESP_OK;
    }

    /* 软挂起恢复：重新开广播即可 */
    DEBUG_INFO(TAG, "WiFi OTA 结束：恢复 BLE 广播 free=%lu",
               (unsigned long)esp_get_free_heap_size());
    m_bleReady = true;
    ble_ota_start_advertising();
    return ESP_OK;
}

/**
  * @brief 通过 BLE Notify 将 GD32 响应推送给上位机。
 * @param data 待推送的数据指针。
 * @param len 数据长度（字节）。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t BleOta::notifyResponse(const uint8_t *data, size_t len)
{
    if (!BleOta::instance().m_bleConnected) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_data_out_val_handle == 0) {
        DEBUG_WARN(TAG, "DATA_OUT 值句柄未设置");
        return ESP_ERR_INVALID_STATE;
    }

    if (len == 0) return ESP_OK;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        DEBUG_ERR(TAG, "分配 mbuf 失败 (%zu 字节)", len);
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gattc_notify_custom(BleOta::instance().m_connHandle, s_data_out_val_handle, om);
    if (rc != 0) {
        DEBUG_WARN(TAG, "通知发送失败: rc=%d", rc);
        return ESP_FAIL;
    }

    DEBUG_DBG(TAG, "通知已发送: %zu 字节", len);
    return ESP_OK;
}

/**
  * @brief 检查当前是否有 BLE 客户端连接。
 * @return true 已连接，false 未连接。
 */
bool BleOta::isConnected()
{
    return BleOta::instance().m_bleConnected;
}

// ============================================================
// BLE OTA FreeRTOS 任务入口
//
// 注意: 此任务的职责是初始化 BLE 服务器并保持存活。
// 实际的 BLE 事件循环在独立的 NimBLE Host 任务中运行。
// 为避免与 ota_relay_task 竞争，此任务不从 OtaRelay::dispatchQueue 消费。
// ============================================================
/**
  * @brief BLE OTA FreeRTOS 任务，初始化 NimBLE 并运行 GATT 服务。
 * @param pvParameters FreeRTOS 任务参数，未使用。
 */
void BleOta::taskEntry(void *pvParameters)
{
    (void)pvParameters;
    BleOta::instance().m_taskHandle = xTaskGetCurrentTaskHandle();
    DEBUG_INFO(TAG, "=== BLE OTA 任务启动 ===");

    // 创建 OTA 数据队列和工作线程（在 NimBLE 初始化之前完成）
    s_otaDataQueue = xQueueCreate(BLE_OTA_DATA_QUEUE_LEN, sizeof(BleOtaDataBatch));
    if (s_otaDataQueue) {
        BaseType_t wk_ok = xTaskCreate(ble_ota_data_worker_task,
                                       "ble_ota_worker",
                                       TASK_STACK_BLE_WORKER,  // 实测 peak~1.1KB，负载峰值曾~1.9KB
                                       NULL,
                                       12,     // 略提高：尽快排空队列，减少 withoutResponse 连发丢包
                                       NULL);
        if (wk_ok != pdPASS) {
            DEBUG_ERR(TAG, "OTA 数据工作线程创建失败，BLE OTA 数据传输不可用");
            vQueueDelete(s_otaDataQueue);
            s_otaDataQueue = nullptr;
        }
    } else {
        DEBUG_ERR(TAG, "OTA 数据队列创建失败，BLE OTA 数据传输不可用");
    }

    esp_err_t err = BleOta::instance().serverStart();
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "BLE 服务器启动失败: %s → BLE OTA 不可用",
                  esp_err_to_name(err));
    }

    // 保持任务存活 — 所有 BLE 事件由 NimBLE Host 任务处理
    uint32_t heartbeat = 0;
    while (1) {
        heartbeat++;

        // 每 60 秒打印一次心跳（确认任务存活）
        if (heartbeat % 6 == 0) {
            DEBUG_DBG(TAG, "心跳 #%lu | BLE %s | 连接: %s",
                      (unsigned long)(heartbeat / 6),
                      BleOta::instance().m_bleReady ? "就绪" : "未就绪",
                      BleOta::instance().m_bleConnected ? "已连接" : "未连接");
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

