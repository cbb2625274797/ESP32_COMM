import sys

path = r'main\server\ble_ota.cpp'
with open(path, 'r', encoding='utf-8-sig') as f:
    data = f.read()

# 1. 文件头注释：补充 0xFA 说明
old = ' *   - CTRL (0x0001, Write):     0xF7=触发GD32中继, 0xF8=触发ESP32 OTA升级\n'
new = ' *   - CTRL (0x0001, Write):     0xF7=触发GD32中继, 0xF8=触发ESP32 OTA升级（含 32B SHA-256 校验并 reboot），0xFA=重启ESP32\n'
assert old in data, 'header old not found'
data = data.replace(old, new, 1)

# 2. 0xF8 分支：移除 "无 SHA 跳过比对" 路径，强制要求 32B SHA
old_f8 = '''    } else if (buf[0] == BLE_OTA_CTRL_TRIGGER_ESP32) {
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
            // 前端未传 SHA，说明已经走过 0xF9 先校验、收尾不再重复比对
            DEBUG_INFO(TAG, "CTRL 0xF8 未传入 SHA-256，finalize 将跳过 SHA 比对与重启前控制流");
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
    } else if (buf[0] == BLE_OTA_CTRL_VERIFY_SHA256) {
        // 仅校验：从 OTA 分区读回计算 SHA-256，通过 0xC1 上报。不 end / 不 set_boot / 不重启。
        Esp32OtaCore &core = Esp32OtaCore::instance();
        if (!core.isActive()) {
            DEBUG_WARN(TAG, "0xF9 请求被拒绝：ESP32 OTA 会话未激活");
            sendFinishEvent(BLE_OTA_FINISH_ERR_OTA_END, (uint32_t)ESP_ERR_INVALID_STATE);
            return 0;
        }
        char device_sha_hex[65] = {0};
        esp_err_t sha_err = core.computeDeviceSha(device_sha_hex);
        if (sha_err != ESP_OK) {
            DEBUG_ERR(TAG, "0xF9 请求：从 OTA 分区计算 SHA-256 失败: %s", esp_err_to_name(sha_err));
            sendFinishEvent(BLE_OTA_FINISH_ERR_OTA_END, (uint32_t)sha_err);
            return 0;
        }
        uint8_t sha_pkt[33] = {0};
        sha_pkt[0] = BLE_OTA_NOTIFY_SHA256;
        for (int i = 0; i < 32; ++i) {
            unsigned int byte = 0;
            sscanf(&device_sha_hex[i * 2], "%02x", &byte);
            sha_pkt[1 + i] = (uint8_t)byte;
        }
        esp_err_t notify_err = BleOta::instance().notifyResponse(sha_pkt, sizeof(sha_pkt));
        if (notify_err != ESP_OK) {
            DEBUG_WARN(TAG, "0xF9 请求：上报 SHA-256 notify 失败: %s", esp_err_to_name(notify_err));
        } else {
            DEBUG_INFO(TAG, "0xF9 请求：已上报设备端 SHA-256，前端可继续比对，%s%s",
                       device_sha_hex, BleOta::instance().m_esp32RebootPending ? "" : "");
        }
    } else if (buf[0] == BLE_OTA_CTRL_RESTART_ESP32) {'''

new_f8 = '''    } else if (buf[0] == BLE_OTA_CTRL_TRIGGER_ESP32) {
        if (!Esp32OtaCore::instance().isActive()) {
            DEBUG_WARN(TAG, "ESP32 OTA 未激活，无法触发升级");
            return 0;
        }
        if (BleOta::instance().m_esp32RebootPending) {
            DEBUG_WARN(TAG, "ESP32 OTA 收尾已在进行中，忽略重复 CTRL");
            return 0;
        }

        // 强制要求 32B 前端 SHA-256：与 WiFi 流程对齐，设备端读回分区计算并比对，
        // 一致走 end+set_boot+reboot，不一致返回 0x85 ERR_SHA_MISMATCH。
        if (len < 33) {
            DEBUG_WARN(TAG, "CTRL 0xF8 数据长度不足 33（前端 SHA-256 必传），拒绝触发");
            sendFinishEvent(BLE_OTA_FINISH_ERR_OTA_END, (uint32_t)ESP_ERR_INVALID_ARG);
            return 0;
        }

        char *frontend_sha_hex = (char *)malloc(65);
        if (!frontend_sha_hex) {
            DEBUG_ERR(TAG, "CTRL 0xF8 申请内存失败");
            sendFinishEvent(BLE_OTA_FINISH_ERR_OTA_END, (uint32_t)ESP_ERR_NO_MEM);
            return 0;
        }
        for (int i = 0; i < 32; ++i) {
            snprintf(&frontend_sha_hex[i * 2], 3, "%02x", buf[1 + i]);
        }
        frontend_sha_hex[64] = '\\0';
        DEBUG_INFO(TAG, "前端 SHA-256: %s", frontend_sha_hex);

        BleOta::instance().m_esp32RebootPending = true;
        BaseType_t ok = xTaskCreate(ble_esp32_finish_and_reboot_task,
                                    "ble_ota_finish",
                                    4096,
                                    frontend_sha_hex,  // heap string; task 负责 free
                                    5,
                                    NULL);
        if (ok != pdPASS) {
            DEBUG_ERR(TAG, "创建 ESP32 OTA 收尾任务失败");
            free(frontend_sha_hex);
            BleOta::instance().m_esp32RebootPending = false;
        } else {
            DEBUG_INFO(TAG, "ESP32 OTA 收尾任务已创建（前端 SHA 已传入）");
        }
    } else if (buf[0] == BLE_OTA_CTRL_RESTART_ESP32) {'''
assert old_f8 in data, '0xF8 block old not found'
data = data.replace(old_f8, new_f8, 1)

# 3. 结尾协议说明注释：删除 0xF9 行
old_tail = '''    //   0xF7 = 触发 GD32 中继
    //   0xF8 = 触发 ESP32 本机 OTA 收尾（end + set_boot + reboot，默认不重复校验 SHA，前端需先走 0xF9 确认）
    //   0xF9 = 仅从 OTA 分区读回计算 SHA-256，通过 0xC1 notify 上报前端（不 end / 不 reboot）
    //   0xFA = 重启 ESP32（配套前端 expectedReboot 自动重连）'''
new_tail = '''    //   0xF7 = 触发 GD32 中继
    //   0xF8 = 触发 ESP32 本机 OTA 收尾：设备端读 OTA 分区算 SHA 并与前端 32B SHA 比对，
    //         一致走 end+set_boot+reboot，不一致返回 0x85 ERR_SHA_MISMATCH（不重启）
    //   0xFA = 重启 ESP32（配套前端 expectedReboot 自动重连）'''
assert old_tail in data, 'tail old not found'
data = data.replace(old_tail, new_tail, 1)

with open(path, 'w', encoding='utf-8-sig') as f:
    f.write(data)
print('ble_ota.cpp updated')
