import sys

with open('main/server/ble_ota.cpp','r',encoding='utf-8-sig') as f:
    data=f.read()

# 1. 添加 SHA_MISMATCH 错误码宏
old='#define BLE_OTA_FINISH_ERR_GD32_SIZE 0x84   // GD32 固件超出分区大小'
new='#define BLE_OTA_FINISH_ERR_GD32_SIZE 0x84   // GD32 固件超出分区大小\n#define BLE_OTA_FINISH_ERR_SHA_MISMATCH 0x85 // ESP32 OTA SHA-256 不匹配'
assert old in data, 'macro old not found'
data=data.replace(old,new)

# 2. 修改 ble_esp32_finish_and_reboot_task 接受 frontend SHA 参数
old_task='''static void ble_esp32_finish_and_reboot_task(void *pvParameters)
{
    (void)pvParameters;
    DEBUG_INFO(TAG, "ESP32 OTA 异步收尾任务启动");

    Esp32OtaCore &core = Esp32OtaCore::instance();

    // 补发最终 ACK，让前端尽快得知最后一包已收到
    if (BleOta::instance().m_esp32LastSeq != 0xFFFFU) {
        sendWindowAck(BleOta::instance().m_esp32LastCmd,
                      BleOta::instance().m_esp32LastSeq,
                      (uint32_t)core.getReceived());
        DEBUG_INFO(TAG, "ESP32 最终 ACK 已补发: seq=%u", BleOta::instance().m_esp32LastSeq);
    }
    vTaskDelay(pdMS_TO_TICKS(150));

    if (!core.isActive()) {
        DEBUG_WARN(TAG, "异步收尾时 OTA 状态无效，放弃重启");
        BleOta::instance().m_esp32RebootPending = false;
        vTaskDelete(NULL);
        return;
    }

    DEBUG_INFO(TAG, "ESP32 OTA 收尾: 共 %u 字节", (unsigned)core.getReceived());

    // 通知前端：开始 finalize
    sendFinishEvent(BLE_OTA_FINISH_BEGIN_OTA_END, 0);

    // 委托核心完成: esp_ota_end → SHA 计算 → set_boot_partition → esp_restart
    // BLE 不传前端 SHA（BLE 协议无 finalize 前校验机制），直接应用；SHA 通过 0xC1 上报供前端比对
    char device_sha_hex[65] = {0};
    esp_err_t err = core.finalize(/*frontend_sha_hex=*/nullptr,
                                  /*reboot=*/true,
                                  device_sha_hex);
    // reboot=true 时 finalize 正常不会返回（core 内部 esp_restart）

    if (err == ESP_OK) {
        // 极端情况：reboot 未执行（理论上不会发生），仍上报并标记
        if (device_sha_hex[0] != '\\0') {
            // 上报设备端 SHA-256 供前端比对显示（[0xC1] + [32 字节]）
            uint8_t sha_pkt[33] = {0};
            sha_pkt[0] = BLE_OTA_NOTIFY_SHA256;
            for (int i = 0; i < 32; ++i) {
                unsigned int byte = 0;
                sscanf(&device_sha_hex[i * 2], "%02x", &byte);
                sha_pkt[1 + i] = (uint8_t)byte;
            }
            BleOta::instance().notifyResponse(sha_pkt, sizeof(sha_pkt));
        }
        sendFinishEvent(BLE_OTA_FINISH_DONE_OTA_END, 0);
        sendFinishEvent(BLE_OTA_FINISH_REBOOT_NOW, 0);
    } else {
        DEBUG_ERR(TAG, "[FINISH] OTA 收尾失败: %s", esp_err_to_name(err));
        sendFinishEvent(BLE_OTA_FINISH_ERR_OTA_END, (uint32_t)err);
        BleOta::instance().m_esp32RebootPending = false;
    }

    vTaskDelete(NULL);
}'''

new_task='''static void ble_esp32_finish_and_reboot_task(void *pvParameters)
{
    // pvParameters: heap-allocated char[65] frontend SHA hex string (or nullptr)
    char *frontend_sha_hex = (char *)pvParameters;
    DEBUG_INFO(TAG, "ESP32 OTA 异步收尾任务启动");

    Esp32OtaCore &core = Esp32OtaCore::instance();

    // 补发最终 ACK，让前端尽快得知最后一包已收到
    if (BleOta::instance().m_esp32LastSeq != 0xFFFFU) {
        sendWindowAck(BleOta::instance().m_esp32LastCmd,
                      BleOta::instance().m_esp32LastSeq,
                      (uint32_t)core.getReceived());
        DEBUG_INFO(TAG, "ESP32 最终 ACK 已补发: seq=%u", BleOta::instance().m_esp32LastSeq);
    }
    vTaskDelay(pdMS_TO_TICKS(150));

    if (!core.isActive()) {
        DEBUG_WARN(TAG, "异步收尾时 OTA 状态无效，放弃重启");
        if (frontend_sha_hex) free(frontend_sha_hex);
        BleOta::instance().m_esp32RebootPending = false;
        vTaskDelete(NULL);
        return;
    }

    DEBUG_INFO(TAG, "ESP32 OTA 收尾: 共 %u 字节", (unsigned)core.getReceived());

    // 通知前端：开始 finalize
    sendFinishEvent(BLE_OTA_FINISH_BEGIN_OTA_END, 0);

    char device_sha_hex[65] = {0};
    esp_err_t err = core.finalize(frontend_sha_hex,
                                  /*reboot=*/true,
                                  device_sha_hex);
    // reboot=true 时 finalize 正常不会返回（core 内部 esp_restart）

    if (err == ESP_OK) {
        // 极端情况：reboot 未执行（理论上不会发生），仍上报并标记
        if (device_sha_hex[0] != '\\0') {
            uint8_t sha_pkt[33] = {0};
            sha_pkt[0] = BLE_OTA_NOTIFY_SHA256;
            for (int i = 0; i < 32; ++i) {
                unsigned int byte = 0;
                sscanf(&device_sha_hex[i * 2], "%02x", &byte);
                sha_pkt[1 + i] = (uint8_t)byte;
            }
            BleOta::instance().notifyResponse(sha_pkt, sizeof(sha_pkt));
        }
        sendFinishEvent(BLE_OTA_FINISH_DONE_OTA_END, 0);
        sendFinishEvent(BLE_OTA_FINISH_REBOOT_NOW, 0);
    } else if (err == ESP_ERR_INVALID_VERSION) {
        DEBUG_ERR(TAG, "[FINISH] SHA-256 不匹配，OTA 已中止");
        sendFinishEvent(BLE_OTA_FINISH_ERR_SHA_MISMATCH, 0);
        BleOta::instance().m_esp32RebootPending = false;
    } else {
        DEBUG_ERR(TAG, "[FINISH] OTA 收尾失败: %s", esp_err_to_name(err));
        sendFinishEvent(BLE_OTA_FINISH_ERR_OTA_END, (uint32_t)err);
        BleOta::instance().m_esp32RebootPending = false;
    }

    if (frontend_sha_hex) free(frontend_sha_hex);
    vTaskDelete(NULL);
}'''
assert old_task in data, 'old_task not found'
data=data.replace(old_task,new_task)

# 3. 修改 ble_ota_ctrl_access: 扩展 buf 到 36，解析前端 SHA
old_ctrl='''    uint8_t buf[4] = {0};
    int len = ble_ota_mbuf_to_buf(ctxt->om, buf, sizeof(buf));
    if (len < 1) {
        DEBUG_WARN(TAG, "CTRL 数据为空");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    DEBUG_INFO(TAG, "CTRL 写入: cmd=0x%02X", buf[0]);

    if (buf[0] == BLE_OTA_CTRL_TRIGGER_GD32) {
        // 检查固件是否存储完毕 → 触发 GD32 OTA 中继
        ota_meta_t meta;
        esp_err_t err = OtaRelay::metaLoad(&meta);
        if (err == ESP_OK &&
            meta.total_size > 0 &&
            meta.stored_size >= meta.total_size) {
            DEBUG_INFO(TAG, "GD32 固件就绪 (%" PRIu32 " / %" PRIu32 " 字节)，触发中继",
                       meta.stored_size, meta.total_size);
            BleOta::instance().signalRelay();
        } else {
            DEBUG_WARN(TAG, "GD32 固件未就绪 (stored=%" PRIu32 ", total=%" PRIu32 ")",
                       meta.stored_size, meta.total_size);
        }
    } else if (buf[0] == BLE_OTA_CTRL_TRIGGER_ESP32) {
        if (!Esp32OtaCore::instance().isActive()) {
            DEBUG_WARN(TAG, "ESP32 OTA 未激活，无法触发升级");
            return 0;
        }
        if (BleOta::instance().m_esp32RebootPending) {
            DEBUG_WARN(TAG, "ESP32 OTA 收尾已在进行中，忽略重复 CTRL");
            return 0;
        }

        BleOta::instance().m_esp32RebootPending = true;
        BaseType_t ok = xTaskCreate(ble_esp32_finish_and_reboot_task,
                                    "ble_ota_finish",
                                    4096,
                                    NULL,
                                    5,
                                    NULL);
        if (ok != pdPASS) {
            DEBUG_ERR(TAG, "创建 ESP32 OTA 收尾任务失败");
            BleOta::instance().m_esp32RebootPending = false;
        } else {
            DEBUG_INFO(TAG, "ESP32 OTA 收尾任务已创建（由设备端异步完成）");
        }
    }
    // 注: BLE_OTA_CTRL_VERIFY_SHA256 (0xF9) 路径已移除。
    //     SHA-256 现由 Esp32OtaCore 在 finalize 时统一从分区读回计算，
    //     并通过 BLE_OTA_NOTIFY_SHA256 (0xC1) 通知前端比对，校验逻辑与 WiFi 完全一致。'''

new_ctrl='''    uint8_t buf[36] = {0};
    int len = ble_ota_mbuf_to_buf(ctxt->om, buf, sizeof(buf));
    if (len < 1) {
        DEBUG_WARN(TAG, "CTRL 数据为空");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    DEBUG_INFO(TAG, "CTRL 写入: cmd=0x%02X len=%d", buf[0], len);

    if (buf[0] == BLE_OTA_CTRL_TRIGGER_GD32) {
        // 检查固件是否存储完毕 → 触发 GD32 OTA 中继
        ota_meta_t meta;
        esp_err_t err = OtaRelay::metaLoad(&meta);
        if (err == ESP_OK &&
            meta.total_size > 0 &&
            meta.stored_size >= meta.total_size) {
            DEBUG_INFO(TAG, "GD32 固件就绪 (%" PRIu32 " / %" PRIu32 " 字节)，触发中继",
                       meta.stored_size, meta.total_size);
            BleOta::instance().signalRelay();
        } else {
            DEBUG_WARN(TAG, "GD32 固件未就绪 (stored=%" PRIu32 ", total=%" PRIu32 ")",
                       meta.stored_size, meta.total_size);
        }
    } else if (buf[0] == BLE_OTA_CTRL_TRIGGER_ESP32) {
        if (!Esp32OtaCore::instance().isActive()) {
            DEBUG_WARN(TAG, "ESP32 OTA 未激活，无法触发升级");
            return 0;
        }
        if (BleOta::instance().m_esp32RebootPending) {
            DEBUG_WARN(TAG, "ESP32 OTA 收尾已在进行中，忽略重复 CTRL");
            return 0;
        }

        // 解析前端 SHA-256: [0xF8] + [32 bytes binary SHA]
        char *frontend_sha_hex = nullptr;
        if (len >= 33) {
            frontend_sha_hex = (char *)malloc(65);
            if (frontend_sha_hex) {
                for (int i = 0; i < 32; ++i) {
                    snprintf(&frontend_sha_hex[i * 2], 3, "%02x", buf[1 + i]);
                }
                frontend_sha_hex[64] = '\\0';
                DEBUG_INFO(TAG, "前端 SHA-256: %s", frontend_sha_hex);
            }
        } else {
            DEBUG_WARN(TAG, "CTRL 0xF8 数据长度不足 33，无法获取前端 SHA-256");
        }

        BleOta::instance().m_esp32RebootPending = true;
        BaseType_t ok = xTaskCreate(ble_esp32_finish_and_reboot_task,
                                    "ble_ota_finish",
                                    4096,
                                    frontend_sha_hex,  // heap string; task 负责 free
                                    5,
                                    NULL);
        if (ok != pdPASS) {
            DEBUG_ERR(TAG, "创建 ESP32 OTA 收尾任务失败");
            if (frontend_sha_hex) free(frontend_sha_hex);
            BleOta::instance().m_esp32RebootPending = false;
        } else {
            DEBUG_INFO(TAG, "ESP32 OTA 收尾任务已创建（前端 SHA 已传入）");
        }
    }
    // 注: BLE_OTA_CTRL_VERIFY_SHA256 (0xF9) 路径已移除。
    //     SHA-256 现由 Esp32OtaCore 在 finalize 时统一从分区读回计算，
    //     并通过 BLE_OTA_NOTIFY_SHA256 (0xC1) 通知前端比对，校验逻辑与 WiFi 完全一致。'''
assert old_ctrl in data, 'old_ctrl not found'
data=data.replace(old_ctrl,new_ctrl)

# 4. 移除 DATA_IN 中的自动收尾代码（ESP32 收到最后一包后自动创建 finish task）
old_auto='''            if ((total_size > 0 && Esp32OtaCore::instance().getReceived() >= total_size) || isLastPacket) {
                DEBUG_INFO(TAG, "★ ESP32 固件接收完毕: %u / %" PRIu32 " 字节 (共 %u 块)%s",
                           (unsigned int)Esp32OtaCore::instance().getReceived(), total_size, seq + 1,
                           isLastPacket ? " [尾包]" : "");
                BleOta::instance().m_esp32LastCmd = cmd;
                BleOta::instance().m_esp32LastSeq = seq;

                if (!BleOta::instance().m_esp32RebootPending) {
                    BleOta::instance().m_esp32RebootPending = true;
                    BaseType_t ok = xTaskCreate(ble_esp32_finish_and_reboot_task,
                                                "ble_ota_finish",
                                                4096,
                                                NULL,
                                                5,
                                                NULL);
                    if (ok != pdPASS) {
                        DEBUG_ERR(TAG, "创建 ESP32 OTA 收尾任务失败");
                        ble_esp32_ota_abort();  // core 内部完成 abort + mutex release + 状态复位
                        BleOta::instance().m_esp32RebootPending = false;
                        return BLE_ATT_ERR_UNLIKELY;
                    }
                    DEBUG_INFO(TAG, "ESP32 OTA 收尾任务已创建，等待设备端自动重启");
                } else {
                    DEBUG_WARN(TAG, "ESP32 OTA 收尾任务已存在，忽略重复完成触发");
                }
            }
        }'''

new_auto='''            if ((total_size > 0 && Esp32OtaCore::instance().getReceived() >= total_size) || isLastPacket) {
                DEBUG_INFO(TAG, "★ ESP32 固件接收完毕: %u / %" PRIu32 " 字节 (共 %u 块)%s",
                           (unsigned int)Esp32OtaCore::instance().getReceived(), total_size, seq + 1,
                           isLastPacket ? " [尾包]" : "");
                BleOta::instance().m_esp32LastCmd = cmd;
                BleOta::instance().m_esp32LastSeq = seq;
                // 注：与 WiFi 流程对齐，传输完成后由前端发送 0xF8 + SHA-256 触发 finalize，
                //     设备端不再自动创建收尾任务。
            }
        }'''
assert old_auto in data, 'old_auto not found'
data=data.replace(old_auto,new_auto)

with open('main/server/ble_ota.cpp','w',encoding='utf-8-sig') as f:
    f.write(data)
print('ble_ota.cpp updated')
