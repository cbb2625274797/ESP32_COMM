# 死函数清理与兼容标记计划

## 目标
逐函数扫描 `main/`（C++）、`tools/`（JS）及根目录 Python 脚本，识别：
1. **死函数**：已定义但无调用、未注册、未绑定，直接清理。
2. **旧版本兼容函数**：仍被调用但无实际作用，或注释/命名表明为兼容保留，进行明确标记（保留并加注释，或连同调用点一起移除）。

扫描已完成，共覆盖约 349 个 C++ 定义、~130 个 JS 函数、2 个 Python 脚本。

---

## 一、C++ 死函数（建议删除）

| # | 函数/方法 | 文件 | 行 | 说明 |
|---|-----------|------|----|------|
| 1 | `esp_err_t ota_prepare_handler(httpd_req_t *req)` | `main/server/http_handlers.cpp` | 202 | 完整实现，但 `wifi_ota.cpp` 的 `kHttpRoutes` 未注册 `/ota/prepare`，无任何调用。 |
| 2 | `bool WifiOta::getGd32StorageDone() const` | `main/server/wifi_ota.h` | 73 | 内联 getter；`setGd32StorageDone()` 在用，getter 无调用。 |
| 3 | `bool OtaRelay::StateLockGuard::isLocked() const` | `main/relay/ota_relay.h` | 241 | 公共嵌套类方法，无调用。 |
| 4 | `int canFdSendNoReply(...)` | `main/transport/canfd_bridge.cpp/h` | 93/61 | 无调用者。 |
| 5 | `int canFdPullIdSnapshot(...)` | `main/transport/canfd_bridge.cpp/h` | 183/158 | 无调用者。 |
| 6 | `bool getLatestIdSnapshot(...)` | `main/monitor/can_feedback_task.cpp/h` | 156/44 | 无调用者；`getLatestCanFeedback()` 已覆盖同一需求。 |
| 7 | `const char* OtaCoordinator::getSystemStateString() const` | `main/ota/ota_coordinator.cpp/h` | 711/438 | 注释已写明“保持向后兼容”，但无任何调用。 |
| 8 | `int OtaCoordinator::esp32FinalizeErrCategory(esp_err_t err)` | `main/ota/ota_coordinator.cpp/h` | 563/466 | 声明、定义齐全，无调用。 |
| 9 | `bool OtaCoordinator::isGd32Busy() const` | `main/ota/ota_coordinator.cpp/h` | 680/424 | 无调用；`isGd32BusyState()` 仍保留供内部使用。 |
| 10 | `bool OtaCoordinator::isEsp32Busy() const` | `main/ota/ota_coordinator.cpp/h` | 686/431 | 无调用；`isEsp32BusyState()` 仍保留。 |
| 11 | `bool OtaCoordinator::isGd32StorageDone() const` | `main/ota/ota_coordinator.cpp/h` | 723/？ | 无调用；`getSnapshot()` 直接读取 `m_gd32StorageDone`。 |
| 12 | `static void OtaRelay::getGd32Version(gd32_version_t *out)` | `main/relay/ota_relay.cpp/h` | 144/156 | 无调用；`getSnapshot()` 已包含 version。 |
| 13 | `static uint32_t OtaRelay::getRelaySeq()` | `main/relay/ota_relay.cpp/h` | 126/141 | 无调用；`getSnapshot()` 已包含 seq。 |
| 14 | `static uint32_t OtaRelay::getRelayTotalPackets()` | `main/relay/ota_relay.cpp/h` | 132/146 | 无调用；`getSnapshot()` 已包含 total_packets。 |

### C++ 清理动作
- 删除上述函数的实现与对应头文件声明。
- 删除 `ota_prepare_handler` 后，检查并清理其专用的 `clear_esp32_prepare` / `clear_gd32_prepare` 静态状态变量是否仍被其他 handler 使用；若仅由 `ota_prepare_handler` 使用则一并移除。
- 删除 `getLatestIdSnapshot` 后，若 `id_snapshot_cache_t` 类型不再被使用，保留在头文件中（作为公共类型）或同步删除。

---

## 二、C++ 旧版本兼容标记（建议保留并强化注释）

| 函数/成员 | 文件 | 说明 |
|-----------|------|------|
| `ble_ota_start_advertising(void)` | `main/server/ble_ota.cpp` | 注释已写“kept as free function for BLE callback compatibility”，确实被注册使用，保留并维持注释。 |
| `volatile bool m_wifiConnected` | `main/monitor/health_monitor.h` | 注释已写“保留为历史兼容”，保留。 |
| CANFD/SPI/Flash 迁移注释 | `protocol.cpp`、`spi_master.h/.cpp`、`ota_relay.h` | 已是历史迁移说明，无需改动。 |
| NVS 兼容 fallback | `gd32_flash_store.cpp` | 注释“视为空元数据，保持兼容”，保留。 |

---

## 三、JS 死函数（建议删除）

### `tools/app.js`
| 函数 | 行 | 说明 |
|------|----|------|
| `sendCanFrame()` | 925 | 引用已移除的 `#canChannel/#canId/#canDlc/#canData`。 |
| `clearCanFields()` | 968 | 同上。 |
| `updateCanHint()` | 376 | 同上；事件绑定也指向已移除 DOM。 |
| `setHelper(id, text, type)` | 370 | 仅被 `updateCanHint()` 调用。 |
| `retryEsp32Upload()` | 1629 | 无按钮/调用者。 |
| `cancelEsp32Ota()` | 1643 | 无按钮/调用者。 |
| `applyEsp32Firmware()` | 1655 | 当前流程由 `verifyEsp32Firmware()` 直接 `/ota/update` 应用。 |
| `triggerRelay()` | 1759 | 仅转发 `triggerGd32Relay()`，注释写“旧页面兜底”。 |

### `tools/app.js` 兼容/空壳处理
| 函数 | 行 | 建议 |
|------|----|------|
| `showVerifyActions(type)` | 179 | 空壳，被 `verifyEsp32Firmware()` 等调用。删除函数体及所有调用点（因为 UI 按钮已移除）。 |
| `setOtaProgressBadge()` | 782 | 空壳局部函数，被多处调用。删除函数及所有调用点。 |

### `tools/ble_ota.js`
| 函数 | 行 | 说明 |
|------|----|------|
| `restartEsp32()` | 837 | IIFE 内部定义，无绑定/调用；全局 `restartEsp32()` 来自 `app.js` 已绑定按钮。 |

### `tools/can.js`
| 函数 | 行 | 说明 |
|------|----|------|
| `metaCard(label, value)` | 747 | 从未调用。 |
| `renderSignalGrid(/* message */)` | 751 | 空壳，无调用者。 |
| `renderSelectedMessage()` | 742 | 空壳但**仍被 `selectMessage` 等多个调用点调用**，是兼容 shim。保留并强化注释“旧版信号编辑面板兼容 shim”。 |

### `tools/index.html`
| 函数 | 行 | 说明 |
|------|----|------|
| `window.attachSharedFirmwareDropBridge` | 639 | 定义但从未调用；`app.js` 的 `setupDropZone()` 已处理 drop。 |

---

## 四、Python 脚本

- `ota_upload_test.py`、`serial_monitor.py` 均为纯顺序脚本，无函数定义，无需改动。

---

## 五、验证步骤

1. 修改后执行 `idf.py build`（或等价的 `idf.py fullclean` + `build`）验证 C++ 无编译/链接错误。
2. 浏览器打开 `tools/index.html`、`tools/can.html`，验证控制台无 `xxx is not defined` 报错。
3. 检查 C++ 中是否产生新的 `-Wunused-function` / `-Wunused-variable` 警告。
4. 对存疑项做二次 grep 确认无调用。

---

## 六、风险与保留项

- `canFdSendNoReply` / `canFdPullIdSnapshot` 是公共 API，当前无调用。若你计划后续通过 HTTP/SPI 暴露它们，可以改为加 `[[maybe_unused]]` 或保留注释；否则按死函数删除。
- `ota_prepare_handler` 实现完整，若你打算未来启用 `/ota/prepare` 端点，应改为注册而非删除。本次计划按“当前死代码”删除处理。
- `renderSelectedMessage()` 仍被调用，保留作为明确的旧版兼容 shim。

---

## 七、交付物

1. 修改后的 C++ 源文件与头文件（死函数已删除，兼容项已标记/保留）。
2. 修改后的 JS 文件（死函数已删除，空壳 shim 已清理）。
3. 一份简短变更摘要，列出删除的函数及保留的兼容 shim。
4. `idf.py build` 成功输出。