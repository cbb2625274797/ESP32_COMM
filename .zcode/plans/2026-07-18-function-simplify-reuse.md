# 函数精简与复用重构实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 通过提取共享工具、消除重复实现、拆分过长函数，系统性地精简 ESP32 固件 (`main/`) 与前端工具 (`tools/js/`) 中的函数，重点强化复用关系。

**Architecture:** 三层重构——(1) 抽取跨文件共享工具模块（hex/le/sha/progress/stage-ui/fetch）；(2) 消除同语义的重复实现并替换调用点；(3) 拆分超长/职责混杂函数。每个任务独立可测、可提交。C++ 侧与 JS 侧互不依赖，可并行推进。

**Tech Stack:** ESP-IDF (C++17, FreeRTOS, NimBLE), 原生 JS (浏览器 ES 模块，无构建步骤)，CMake。

**测试策略:**
- C++ 侧：`idf.py build` 编译通过；OTA 上传/转发/CAN 收发的人工冒烟测试（设备实测）。
- JS 侧：浏览器加载 `tools/index.html` 与 `tools/can.html`，控制台无报错；BLE OTA、WiFi OTA、CAN 监控、DBC 解析、手动发送、批量发送功能冒烟通过。
- 无单元测试框架，每个任务后需在浏览器/设备上做对应路径的冒烟验证。

**核心约束:**
- 不改变任何对外行为/协议/UUID/HTTP 路由。
- 不引入构建步骤（保持原生 ES 模块）。
- 保持现有代码风格（无分号、`var`/`const`/`let` 混用、中文注释）。
- 每个 task 一次提交，commit message 用 `refactor:` 前缀。

---

## File Structure

### 新建文件

| 文件 | 职责 |
|---|---|
| `main/include/hex_utils.h` | C++ 共享 hex/le 工具：`parseHexToBytes`、`bytesToHexUpper`、`le8`、`le32` |
| `tools/js/common/progress.js` | JS 共享进度条：`setProgress`、`markProgressDone`、`resetProgress`、`getProgressEls`（懒缓存） |
| `tools/js/common/stage-ui.js` | JS 共享阶段节点/状态栏：`resetStageNodes`、`advanceStage`、`markStageError`、`setStatusBar`、`getStageStatusBar`、`getStageStatusText`（懒缓存） |
| `tools/js/common/sha.js` | JS 共享 SHA：`computeFileSha256`、`sha256HexToBytes`、`compareShaHex` |
| `tools/js/common/fetch-json.js` | JS 共享 fetch 包装：`fetchJSON(url, opts)`、`abortableFetch(url, timeoutMs)` |
| `tools/js/common/device-ip.js` | JS 共享设备 IP：`getDeviceIp`、`setDeviceIp`、`getBaseUrl` |
| `tools/js/ble/gatt-helpers.js` | JS BLE GATT 包装：`discoverCharacteristics`、`writeWithResponse`、`writeWithoutResponse`、`readCharacteristic` |

### 修改文件（按 task 顺序）

C++ 侧：`main/server/ble_ota_data.cpp`、`main/server/ble_ota_host.cpp`、`main/server/ble_ota.cpp`、`main/server/http_handlers.cpp`、`main/transport/spi_master.cpp`、`main/include/app_config.h`、`main/CMakeLists.txt`（仅在新增 .cpp 时）。

JS 侧：`tools/js/common/utils.js`、`tools/js/common/connection.js`、`tools/js/common/polling.js`、`tools/js/ble/transfer.js`、`tools/js/ble/file.js`、`tools/js/ble/gatt.js`、`tools/js/ble/stages.js`、`tools/js/ble/state.js`、`tools/js/ble/verify.js`、`tools/js/ota/progress.js`、`tools/js/ota/stages.js`、`tools/js/ota/upload-esp32.js`、`tools/js/ota/upload-gd32.js`、`tools/js/ota/file-handle.js`、`tools/js/can.js`、`tools/js/can/send.js`、`tools/js/can/dbc.js`、`tools/js/can_monitor.js`、`tools/js/app.js`。

### 拆分文件

- `tools/js/ble/stages.js` → 保留目标配置相关，把通用节点操作移至 `stage-ui.js`。
- `tools/js/ota/progress.js` → 保留 esp32/gd32 专属阶段相位映射，把通用节点操作移至 `stage-ui.js`、进度条移至 `progress.js`（common）。

> 注意：新建 `tools/js/common/progress.js` 与现有 `tools/js/ota/progress.js` 同名不同目录，需在 HTML/导入路径中明确区分。

---

## 阶段总览

| 阶段 | 主题 | Task |
|---|---|---|
| A | C++ 共享工具与拆分 | T1–T5 |
| B | JS 共享工具模块建立 | T6–T11 |
| C | JS 调用点迁移与重复消除 | T12–T18 |
| D | JS 过长函数拆分 | T19–T22 |
| E | JS 死代码/一次性 helper 清理 | T23–T25 |

---

## 阶段 A：C++ 共享工具与拆分

### Task 1: 新建 `main/include/hex_utils.h` 共享 hex/le 工具

**Files:**
- Create: `main/include/hex_utils.h`

- [ ] **Step 1: 创建头文件**

```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// 将 hex 字符串解析为字节数组，返回写入字节数（<=outCap）。失败返回 0。
inline size_t parseHexToBytes(const char *hex, uint8_t *out, size_t outCap) {
    size_t len = strlen(hex);
    if (len % 2 != 0) return 0;
    size_t n = len / 2;
    if (n > outCap) return 0;
    for (size_t i = 0; i < n; ++i) {
        char h[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        out[i] = (uint8_t)strtoul(h, nullptr, 16);
    }
    return n;
}

// 字节数组 → 大写 hex 字符串（buf 至少 n*2+1 字节）。
inline void bytesToHexUpper(const uint8_t *in, size_t n, char *buf) {
    static const char *kHex = "0123456789ABCDEF";
    for (size_t i = 0; i < n; ++i) {
        buf[i * 2]     = kHex[(in[i] >> 4) & 0xF];
        buf[i * 2 + 1] = kHex[in[i] & 0xF];
    }
    buf[n * 2] = 0;
}

// 小端打包/解包
inline uint8_t le8(uint32_t v, uint8_t shift) { return (uint8_t)(v >> shift); }
inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
```

- [ ] **Step 2: 验证编译**

Run: `idf.py build`
Expected: 编译通过（头文件尚未被引用，不影响现有代码）

- [ ] **Step 3: 提交**

```bash
git add main/include/hex_utils.h
git commit -m "refactor(main): 新增 hex_utils.h 共享 hex/le 工具"
```

---

### Task 2: 迁移 http_handlers.cpp 使用 hex_utils.h

**Files:**
- Modify: `main/server/http_handlers.cpp:39-56`（删除本地 `parseHexToBytes`/`bytesToHexUpper`）
- Modify: `main/server/http_handlers.cpp:571-581`（GD32 relay handler 内联 hex 解析改用工具）

- [ ] **Step 1: 添加 include 并删除本地实现**

在 `http_handlers.cpp` 顶部 `#include` 区加入：
```cpp
#include "hex_utils.h"
```

删除 `parseHexToBytes()` (39-48) 与 `bytesToHexUpper()` (50-56) 的本地定义。保留所有调用点不变（函数名一致）。

- [ ] **Step 2: 替换 ota_relay_trigger_handler 内联 hex 解析**

将 `http_handlers.cpp:571-581` 处：
```cpp
for (size_t i = 0; i < sizeof(frontend_sha); ++i) {
    char hex[3] = { frontend_sha_hex[i*2], frontend_sha_hex[i*2+1], 0 };
    frontend_sha[i] = (uint8_t)strtoul(hex, nullptr, 16);
}
// ... 后续 inline hex 构造 device_sha_hex
```
改为：
```cpp
if (parseHexToBytes(frontend_sha_hex, frontend_sha, sizeof(frontend_sha)) != sizeof(frontend_sha)) {
    http_send_json_error(req, 400, "BAD_SHA", "SHA-256 hex 格式错误");
    return ESP_OK;
}
char device_sha_hex[65];
bytesToHexUpper(device_sha, sizeof(device_sha), device_sha_hex);
```

- [ ] **Step 3: 验证编译**

Run: `idf.py build`
Expected: PASS

- [ ] **Step 4: 冒烟测试**

设备上通过 HTTP 触发一次 `/ota/relay`（错误 SHA 也行，只要返回 400 而非崩溃）。

- [ ] **Step 5: 提交**

```bash
git add main/server/http_handlers.cpp main/include/hex_utils.h
git commit -m "refactor(http): 迁移 hex 工具至 hex_utils.h，消除内联 hex 解析"
```

---

### Task 3: 迁移 ble_ota_data.cpp 使用 hex_utils.h，删除 `shaBytesToHexUpper`

**Files:**
- Modify: `main/server/ble_ota_data.cpp:437-507`（`sendWindowAck`/`sendFinishEvent`/`sendSha256Notify` 使用 `le8`；删除 `shaBytesToHexUpper`，改用 `bytesToHexUpper`）
- Modify: `main/server/ble_ota.cpp:92-96`（`ble_esp32_finish_and_reboot_task` 中手写 hex 解析改用 `parseHexToBytes`）

- [ ] **Step 1: 添加 include**

在 `ble_ota_data.cpp` 与 `ble_ota.cpp` 顶部加入 `#include "hex_utils.h"`。

- [ ] **Step 2: 删除 `shaBytesToHexUpper` (487-494)**

将 `sendSha256Notify()` (497-507) 内的 `shaBytesToHexUpper(sha, sha_hex)` 调用替换为 `bytesToHexUpper(sha, 32, sha_hex)`。删除 `shaBytesToHexUpper` 函数定义。

- [ ] **Step 3: `sendWindowAck`/`sendFinishEvent` 使用 `le8`**

将 `ble_ota_data.cpp:441-447` 的 4x `(uint8_t)(value >> shift)` 与 `468-473` 的同类代码替换为 `le8(value, 0/8/16/24)`。

- [ ] **Step 4: `ble_ota.cpp:92-96` 使用 `parseHexToBytes`**

将手写循环改为：
```cpp
uint8_t sha[32];
if (parseHexToBytes(sha_hex, sha, sizeof(sha)) != sizeof(sha)) { /* 原错误分支 */ }
```

- [ ] **Step 5: 验证编译**

Run: `idf.py build`
Expected: PASS

- [ ] **Step 6: 冒烟测试**

BLE OTA 上传一次固件，观察 SHA256 notify 与 finish event 正常返回。

- [ ] **Step 7: 提交**

```bash
git add main/server/ble_ota_data.cpp main/server/ble_ota.cpp
git commit -m "refactor(ble_ota): 统一 hex/le 工具，删除 shaBytesToHexUpper"
```

---

### Task 4: 提取 `shouldSendThrottledAck` helper，消除 ESP32/GD32 重复 ACK 节流逻辑

**Files:**
- Modify: `main/server/ble_ota_data.cpp:245-263`（`esp32_write_ordered` fallback 节流）
- Modify: `main/server/ble_ota_data.cpp:808-821`（`process_gd32_packet` 节流）
- Modify: `main/server/ble_ota_priv.h`（新增 prototype）

- [ ] **Step 1: 在 ble_ota_priv.h 新增声明**

```cpp
// 节流 ACK 决策：计数器自增，命中窗口边界或倍数时返回 true。
bool shouldSendThrottledAck(uint32_t *counter, uint16_t seq, uint16_t window);
```

- [ ] **Step 2: 在 ble_ota_data.cpp 实现（放在 sendWindowAck 附近）**

```cpp
bool shouldSendThrottledAck(uint32_t *counter, uint16_t seq, uint16_t window) {
    ++(*counter);
    if (*counter >= window) { *counter = 0; return true; }
    if (*counter % (window / 4 == 0 ? 1 : window / 4) == 0) return true; // 与原逻辑等价
    (void)seq;
    return false;
}
```
> ⚠️ 实现前需先精读原 245-263 与 808-821 的精确条件（`>= BLE_OTA_ACK_WINDOW` 或 `% N == 0`），逐字复刻而非凭记忆。本 step 的代码块是占位骨架，正式实现以原文条件为准。

- [ ] **Step 3: 替换两处调用点**

将两处 `if (++s_xxx_ooo_count >= BLE_OTA_ACK_WINDOW || s_xxx_ooo_count % N == 0) { sendWindowAck(...); }` 替换为：
```cpp
if (shouldSendThrottledAck(&s_fallback_ooo_count, seq, BLE_OTA_ACK_WINDOW)) {
    sendWindowAck(...);
}
```
与 GD32 对应版本。

- [ ] **Step 4: 验证编译并对照原文输出**

Run: `idf.py build`
Expected: PASS

对照 git diff 确认两处节流条件与原逻辑完全等价（包括日志是否仍打印）。

- [ ] **Step 5: 冒烟测试**

BLE OTA 上传中制造乱序（可手动断连重连），观察 ACK 仍按原节奏发出。

- [ ] **Step 6: 提交**

```bash
git add main/server/ble_ota_data.cpp main/server/ble_ota_priv.h
git commit -m "refactor(ble_ota): 提取 shouldSendThrottledAck 消除 ESP32/GD32 重复节流"
```

---

### Task 5: 拆分 `ble_ota_ctrl_access` 与 `ota_upload_handler`，统一 BLE/HTTP GD32 relay 触发

> 本任务较大，按 5a/5b/5c 拆成三个连续提交。

#### Task 5a: 拆分 `ble_ota_ctrl_access` 为 4 个命令 handler

**Files:**
- Modify: `main/server/ble_ota_host.cpp:8-257`
- Modify: `main/server/ble_ota_priv.h`

- [ ] **Step 1: 在 ble_ota_priv.h 新增声明**

```cpp
void ble_ota_handle_gd32_relay(const uint8_t *buf, uint16_t len);
void ble_ota_handle_esp32_finish(const uint8_t *buf, uint16_t len);
void ble_ota_handle_restart();
void ble_ota_handle_abort();
```

- [ ] **Step 2: 在 ble_ota_host.cpp 提取 4 个 handler**

把 `ble_ota_ctrl_access` 中 0xF7/0xF8/0xFA/0xFB 四个分支体分别搬入上述 4 个函数（保持原有 task 创建、错误响应逻辑）。

- [ ] **Step 3: 重写 `ble_ota_ctrl_access` 为 dispatcher**

```cpp
static int ble_ota_ctrl_access(...) {
    // 保留 mbuf→buf 拷贝
    if (len < 1) return 0;
    switch (buf[0]) {
        case 0xF7: ble_ota_handle_gd32_relay(buf, len); break;
        case 0xF8: ble_ota_handle_esp32_finish(buf, len); break;
        case 0xFA: ble_ota_handle_restart(); break;
        case 0xFB: ble_ota_handle_abort(); break;
        default: /* 原日志 */ break;
    }
    return 0;
}
```

- [ ] **Step 4: 验证编译 + 冒烟**

Run: `idf.py build` → PASS。BLE 触发 4 类 ctrl 命令各一次。

- [ ] **Step 5: 提交**

```bash
git add main/server/ble_ota_host.cpp main/server/ble_ota_priv.h
git commit -m "refactor(ble_ota): 拆分 ble_ota_ctrl_access 为 4 个命令 handler"
```

#### Task 5b: 拆分 `ota_upload_handler` 为 `handle_esp32_upload`/`handle_gd32_upload`

**Files:**
- Modify: `main/server/http_handlers.cpp:233-532`
- Modify: `main/server/http_handlers.h`

- [ ] **Step 1: 在 http_handlers.h 新增声明**

```cpp
esp_err_t handle_esp32_upload(httpd_req_t *req, const char *body, size_t len);
esp_err_t handle_gd32_upload(httpd_req_t *req, const char *body, size_t len);
```

- [ ] **Step 2: 提取两个函数**

将 `ota_upload_handler` 中 ESP32 分支 (292-398) 与 GD32 分支 (400-531) 分别搬入两个新函数。共享的静态缓冲与 `otaUploadPaceIfHeapTight()` 调用保留在各自函数内（或提到 `ota_upload_handler` 入口）。`recv_json_header` 仍在 `ota_upload_handler` 中先调用。

- [ ] **Step 3: 重写 `ota_upload_handler` 为 dispatcher**

```cpp
esp_err_t ota_upload_handler(httpd_req_t *req) {
    // recv_json_header + 读取 type 字段
    if (type == "esp32") return handle_esp32_upload(req, body, len);
    return handle_gd32_upload(req, body, len);
}
```

- [ ] **Step 4: 验证编译 + 冒烟**

Run: `idf.py build` → PASS。HTTP 分别上传 ESP32 与 GD32 固件。

- [ ] **Step 5: 提交**

```bash
git add main/server/http_handlers.cpp main/server/http_handlers.h
git commit -m "refactor(http): 拆分 ota_upload_handler 为 esp32/gd32 handler"
```

#### Task 5c: 提取 `triggerGd32RelayCommon` 统一 BLE/HTTP GD32 relay 触发

**Files:**
- Create: `main/server/ota_relay_common.cpp` 或并入现有 `ota/ota_coordinator.cpp`（先查该文件是否更合适）
- Modify: `main/server/ble_ota_host.cpp:48-123`（`ble_ota_handle_gd32_relay`）
- Modify: `main/server/http_handlers.cpp:540-611`（`ota_relay_trigger_handler`）
- Modify: `main/CMakeLists.txt`（若新建 .cpp）
- Modify: `main/server/http_handlers.h` 或相关头文件

- [ ] **Step 1: 先确认 `ota/ota_coordinator.cpp`/`ota_relay.cpp` 是否已导出 `triggerGd32Relay`**

读 `ota/ota_coordinator.cpp` 与 `relay/ota_relay.cpp`，确认现有 `triggerGd32Relay`/`loadGd32Meta`/`verifyGd32Firmware` 签名。

- [ ] **Step 2: 设计共享签名**

```cpp
// 返回 ESP_OK/ESP_ERR_INVALID_CRC/其他；out_sha_hex 为设备侧 SHA 大写 hex。
esp_err_t triggerGd32RelayCommon(const uint8_t *frontend_sha,
                                 uint16_t session_id,
                                 char *out_sha_hex /*[65]*/,
                                 uint16_t *out_session_id);
```

- [ ] **Step 3: 实现共享函数**

把两条路径共有的 meta-load → verify → hex → trigger 序列合入。BLE 侧的 `sendSha256Notify` 在共享函数外由 BLE handler 调用（HTTP 不需要）。

- [ ] **Step 4: 改写两个调用点**

`ble_ota_handle_gd32_relay` 与 `ota_relay_trigger_handler` 改为调用 `triggerGd32RelayCommon`，BLE 侧额外调用 `sendSha256Notify`，HTTP 侧组装 JSON 响应。

- [ ] **Step 5: 验证编译 + 双路径冒烟**

Run: `idf.py build` → PASS。BLE 与 HTTP 各触发一次 GD32 relay。

- [ ] **Step 6: 提交**

```bash
git add main/server/ota_relay_common.cpp main/server/ble_ota_host.cpp main/server/http_handlers.cpp main/CMakeLists.txt
git commit -m "refactor(ota): 提取 triggerGd32RelayCommon 统一 BLE/HTTP GD32 relay"
```

---

## 阶段 B：JS 共享工具模块建立

> 阶段 B 的每个 task 创建一个新模块并写出导出，但**不**改任何调用点（阶段 C 再迁移）。这样每个模块可独立提交、独立冒烟（浏览器加载无报错即视为通过）。

### Task 6: `tools/js/common/progress.js` 统一进度条

**Files:**
- Create: `tools/js/common/progress.js`

- [ ] **Step 1: 创建模块（合并 BLE 与 OTA 两版 setProgress，取并集特性：NaN guard + .done toggle + 懒缓存）**

```js
// tools/js/common/progress.js
let _fill, _txt, _pct;
function getProgressEls() {
  if (!_fill || !_fill.isConnected) {
    _fill = document.getElementById('progressFill');
    _txt  = document.getElementById('progressTxt');
    _pct  = document.getElementById('progressPct');
  }
  return { fill: _fill, txt: _txt, pct: _pct };
}
function setProgress(percent, text) {
  var p = Number(percent);
  if (!isFinite(p)) p = 0;
  p = Math.max(0, Math.min(100, p));
  var els = getProgressEls();
  if (els.fill) els.fill.style.width = p + '%';
  if (els.pct)  els.pct.textContent  = p + '%';
  if (els.txt)  els.txt.textContent  = text || '';
  if (els.fill) els.fill.classList.toggle('done', p >= 100);
  return p;
}
function markProgressDone(text) {
  var els = getProgressEls();
  if (els.fill) { els.fill.style.width = '100%'; els.fill.classList.add('done'); }
  if (els.pct)  els.pct.textContent = '100%';
  if (text && els.txt) els.txt.textContent = text;
}
function resetProgress(text) {
  var els = getProgressEls();
  if (els.fill) { els.fill.style.width = '0%'; els.fill.classList.remove('done'); }
  if (els.pct)  els.pct.textContent = '0%';
  if (els.txt)  els.txt.textContent = text || '';
}
export { setProgress, markProgressDone, resetProgress, getProgressEls };
```

- [ ] **Step 2: 冒烟（仅加载）**

浏览器打开 `tools/index.html`，控制台执行：
```js
import('./js/common/progress.js').then(m => m.setProgress(42, 'test'));
```
Expected: 进度条显示 42%，无报错。

- [ ] **Step 3: 提交**

```bash
git add tools/js/common/progress.js
git commit -m "refactor(js): 新增 common/progress.js 共享进度条"
```

---

### Task 7: `tools/js/common/stage-ui.js` 统一阶段节点/状态栏

**Files:**
- Create: `tools/js/common/stage-ui.js`

- [ ] **Step 1: 创建模块**

```js
// tools/js/common/stage-ui.js
let _bar, _txt;
function getStageStatusBar() {
  if (!_bar || !_bar.isConnected) _bar = document.getElementById('stageStatusBar');
  return _bar;
}
function getStageStatusText() {
  if (!_txt || !_txt.isConnected) _txt = document.getElementById('stageStatusText');
  return _txt;
}
function resetStageNodes(stageIds, detailIds) {
  stageIds.forEach(function (id) {
    var el = document.getElementById(id);
    if (el) el.className = 'stage-node';
  });
  if (detailIds) detailIds.forEach(function (id) {
    var el = document.getElementById(id);
    if (el) el.textContent = '';
  });
}
function setStageClass(stageIds, untilIndex, cls) {
  // cls: 'done' | 'live' | 'err' → 完整类名 'stage-node done' 等
  for (var i = 0; i < stageIds.length; ++i) {
    var el = document.getElementById(stageIds[i]);
    if (!el) continue;
    if (i < untilIndex) el.className = 'stage-node done';
    else if (i === untilIndex) el.className = 'stage-node ' + cls;
    else el.className = 'stage-node';
  }
}
function markStageError(stageIds, index) {
  var el = document.getElementById(stageIds[index]);
  if (el) el.className = 'stage-node err';
}
function setStatusBar(state, text) {
  // state: 'ok' | 'active' | 'err' | ''
  var bar = getStageStatusBar();
  var t = getStageStatusText();
  if (bar) bar.className = 'stage-status-bar' + (state ? ' ' + state : '');
  if (t) t.textContent = text || '';
}
export { getStageStatusBar, getStageStatusText, resetStageNodes, setStageClass, markStageError, setStatusBar };
```

> ⚠️ `setStageClass` 的精确语义需对照 `ble/stages.js:60-94` `setBleStage` 与 `ota/progress.js:83-116` `updateEsp32StagesFromPhase` 的实际赋值规则（done/live/未到达）。本 step 为骨架，正式实现以原文为准。

- [ ] **Step 2: 冒烟（仅加载）**

控制台执行 `import('./js/common/stage-ui.js')` 无报错。

- [ ] **Step 3: 提交**

```bash
git add tools/js/common/stage-ui.js
git commit -m "refactor(js): 新增 common/stage-ui.js 共享阶段节点/状态栏"
```

---

### Task 8: `tools/js/common/sha.js` 统一 SHA-256 计算/比较

**Files:**
- Create: `tools/js/common/sha.js`

- [ ] **Step 1: 创建模块**

```js
// tools/js/common/sha.js
import { bytesToHex } from './utils.js';

async function computeFileSha256(file) {
  try {
    var buf = await file.arrayBuffer();
    var hash = await crypto.subtle.digest('SHA-256', buf);
    return bytesToHex(new Uint8Array(hash)).toUpperCase();
  } catch (e) {
    return null;
  }
}
async function computeBufferSha256(buffer) {
  var hash = await crypto.subtle.digest('SHA-256', buffer);
  return bytesToHex(new Uint8Array(hash)).toUpperCase();
}
function sha256HexToBytes(hex) {
  var out = new Uint8Array(32);
  for (var i = 0; i < 32; ++i) {
    out[i] = parseInt(hex.substr(i * 2, 2), 16);
  }
  return out;
}
function compareShaHex(a, b) {
  if (!a || !b) return false;
  return a.toUpperCase() === b.toUpperCase();
}
export { computeFileSha256, computeBufferSha256, sha256HexToBytes, compareShaHex };
```

- [ ] **Step 2: 冒烟（仅加载）**

控制台执行：
```js
import('./js/common/sha.js').then(async m => {
  var f = new File([new Uint8Array([1,2,3])], 'x.bin');
  var h = await m.computeFileSha256(f);
  console.log(h, m.compareShaHex(h, h));
});
```
Expected: 输出 64 字符 hex + true。

- [ ] **Step 3: 提交**

```bash
git add tools/js/common/sha.js
git commit -m "refactor(js): 新增 common/sha.js 共享 SHA-256 工具"
```

---

### Task 9: `tools/js/common/fetch-json.js` 统一 fetch 包装

**Files:**
- Create: `tools/js/common/fetch-json.js`

- [ ] **Step 1: 创建模块**

```js
// tools/js/common/fetch-json.js
async function fetchJSON(url, options) {
  options = options || {};
  var signal = options.signal;
  var resp = await fetch(url, options);
  var text = await resp.text();
  var data;
  try { data = text ? JSON.parse(text) : null; } catch (e) {
    var err = new Error('JSON 解析失败: ' + e.message);
    err.resp = resp; err.text = text; throw err;
  }
  if (!resp.ok) {
    var err2 = new Error('HTTP ' + resp.status);
    err2.resp = resp; err2.data = data; throw err2;
  }
  return data;
}
function abortableFetch(url, timeoutMs) {
  var ctrl = new AbortController();
  var timer = setTimeout(function () { ctrl.abort(); }, timeoutMs || 5000);
  var p = fetch(url, { signal: ctrl.signal }).finally(function () { clearTimeout(timer); });
  p.controller = ctrl;
  return p;
}
export { fetchJSON, abortableFetch };
```

- [ ] **Step 2: 冒烟（仅加载）**

控制台执行 `import('./js/common/fetch-json.js')` 无报错。

- [ ] **Step 3: 提交**

```bash
git add tools/js/common/fetch-json.js
git commit -m "refactor(js): 新增 common/fetch-json.js 共享 fetch 包装"
```

---

### Task 10: `tools/js/common/device-ip.js` 统一设备 IP/BaseUrl

**Files:**
- Create: `tools/js/common/device-ip.js`

- [ ] **Step 1: 创建模块（取 `can_monitor.js:31-45` 最完整版为基准）**

```js
// tools/js/common/device-ip.js
var DEFAULT_IP = '192.168.31.127';
var STORAGE_KEY = 'dcu_esp_ip';

function safeGet(key) { try { return localStorage.getItem(key); } catch (e) { return null; } }
function safeSet(key, v) { try { localStorage.setItem(key, v); } catch (e) {} }

function getDeviceIp(opts) {
  opts = opts || {};
  // 优先 DOM（可指定元素 ID），其次 localStorage，最后默认
  if (opts.inputId) {
    var el = document.getElementById(opts.inputId);
    if (el && el.value) { safeSet(STORAGE_KEY, el.value); return el.value; }
  }
  var stored = safeGet(STORAGE_KEY);
  if (stored) return stored;
  return DEFAULT_IP;
}
function setDeviceIp(ip) { safeSet(STORAGE_KEY, ip); }
function getBaseUrl(opts) { return 'http://' + getDeviceIp(opts); }

export { getDeviceIp, setDeviceIp, getBaseUrl, DEFAULT_IP, STORAGE_KEY };
```

> 现有 `app.js:52-64` 的 `getEspIp`/`getBaseUrl`、`can/dbc.js:38-60`、`can_monitor.js:31-45` 将在阶段 C 迁移到本模块（本 task 仅创建）。

- [ ] **Step 2: 冒烟（仅加载）**

控制台执行 `import('./js/common/device-ip.js').then(m => console.log(m.getBaseUrl()))` 输出 `http://192.168.31.127`。

- [ ] **Step 3: 提交**

```bash
git add tools/js/common/device-ip.js
git commit -m "refactor(js): 新增 common/device-ip.js 统一设备 IP/BaseUrl"
```

---

### Task 11: `tools/js/ble/gatt-helpers.js` 提取 GATT 发现/读写包装

**Files:**
- Create: `tools/js/ble/gatt-helpers.js`

- [ ] **Step 1: 创建模块**

```js
// tools/js/ble/gatt-helpers.js
import { UUIDS } from './constants.js';

async function discoverCharacteristics(server) {
  var service = await server.getPrimaryService(UUIDS.otaService);
  var ctrl    = await service.getCharacteristic(UUIDS.ctrlChar);
  var dataIn  = await service.getCharacteristic(UUIDS.dataInChar);
  var dataOut = await service.getCharacteristic(UUIDS.dataOutChar);
  var version = await service.getCharacteristic(UUIDS.versionChar);
  return { service: service, ctrl: ctrl, dataIn: dataIn, dataOut: dataOut, version: version };
}
function writeWithResponse(char, data) { return char.writeValueWithResponse(data); }
function writeWithoutResponse(char, data) { return char.writeValueWithoutResponse(data); }
function readCharacteristic(char) { return char.readValue(); }
export { discoverCharacteristics, writeWithResponse, writeWithoutResponse, readCharacteristic };
```

> ⚠️ 实现前需先 Read `tools/js/ble/gatt.js:98-102` 与 `:478-484` 与 `constants.js` 中 UUIDS 的精确字段名，本 step 为骨架。

- [ ] **Step 2: 冒烟（仅加载）**

控制台执行 `import('./js/ble/gatt-helpers.js')` 无报错。

- [ ] **Step 3: 提交**

```bash
git add tools/js/ble/gatt-helpers.js
git commit -m "refactor(js): 新增 ble/gatt-helpers.js 提取 GATT 发现/读写"
```

---

## 阶段 C：JS 调用点迁移与重复消除

> 阶段 C 每个 task 改调用点 + 删除旧实现，必须确保浏览器加载 + 对应路径冒烟通过。

### Task 12: 迁移所有 `setProgress` 调用点至 `common/progress.js`

**Files:**
- Modify: `tools/js/ble/transfer.js:12-23`（删除本地 `setProgress`，import 新版）
- Modify: `tools/js/ble/file.js:95,98`
- Modify: `tools/js/ble/gatt.js:277,287,301,313,328,349,356,363,376`
- Modify: `tools/js/ota/progress.js:228-241`（删除本地 `setProgress`，import 新版；保留 `resetProgress` 但内部调用新版）
- Modify: `tools/js/ota/stages.js:207,302,307,311,314,319,330`
- Modify: `tools/js/ota/upload-esp32.js:223,251,255,261,296,307`
- Modify: `tools/js/ota/upload-gd32.js:58,96,151,164`

- [ ] **Step 1: BLE 侧迁移**

`tools/js/ble/transfer.js` 顶部加 `import { setProgress } from '../common/progress.js';`，删除 12-23 本地定义。确认 `ble/file.js`、`ble/gatt.js` 通过 `window` 或 import 拿到 `setProgress`——若它们当前通过 `window.setProgress` 调用，需在 transfer.js 末尾 `window.setProgress = setProgress;` 暂时兼容（阶段 D 再清理 window 暴露）。

- [ ] **Step 2: OTA 侧迁移**

`tools/js/ota/progress.js` 顶部加 `import { setProgress as commonSetProgress } from '../common/progress.js';`，本地 `setProgress` (228-241) 删除。若其他文件通过 `window.setProgress` 调用 OTA 版，同样 `window.setProgress = commonSetProgress;`。

- [ ] **Step 3: 冒烟**

`tools/index.html`：BLE OTA + WiFi OTA 各走一次，进度条正常推进 + 100% 时 `.done` 类生效。

- [ ] **Step 4: 提交**

```bash
git add tools/js/ble/transfer.js tools/js/ble/file.js tools/js/ble/gatt.js tools/js/ota/progress.js tools/js/ota/stages.js tools/js/ota/upload-esp32.js tools/js/ota/upload-gd32.js
git commit -m "refactor(js): 迁移所有 setProgress 至 common/progress.js"
```

---

### Task 13: 迁移阶段节点/状态栏操作至 `common/stage-ui.js`

**Files:**
- Modify: `tools/js/ble/stages.js:15-123`（`getStageStatusBar`/`getStageStatusText`/`resetBleStages`/`setBleStage`/`setBleStageText`/`setBleStageStatus`/`setBleStageError` 改为薄包装或直接 re-export）
- Modify: `tools/js/ota/progress.js:56-128`（`resetEsp32StageNodes`/`resetGd32StageNodes`/`updateEsp32StagesFromPhase`/`updateEsp32StagesError`）
- Modify: `tools/js/ota/stages.js:49-102`（`updateStagesFromRelayState`）、`:243-249`（内联 err）
- Modify: `tools/js/ota/upload-gd32.js:97-100`（内联 ok 状态栏）

- [ ] **Step 1: BLE stages.js 迁移**

把 `getStageStatusBar`/`getStageStatusText` 改为 `export { getStageStatusBar, getStageStatusText } from '../common/stage-ui.js';`（删除本地实现）。`resetBleStages`/`setBleStage`/`setBleStageError` 内部改用 `stage-ui.js` 的 `resetStageNodes`/`setStageClass`/`markStageError`/`setStatusBar`，保留 BLE 专属的 stage-id 数组常量在 `ble/stages.js`。

- [ ] **Step 2: OTA progress.js 迁移**

`resetEsp32StageNodes`/`resetGd32StageNodes` 改为调用 `resetStageNodes(ESP32_STAGE_IDS, ESP32_DETAIL_IDS)` 等。`updateEsp32StagesFromPhase`/`updateEsp32StagesError` 改用 `setStageClass`/`markStageError`/`setStatusBar`。

- [ ] **Step 3: OTA stages.js 与 upload-gd32.js 迁移**

`updateStagesFromRelayState` 改用共享 helper；`:243-249` 与 `upload-gd32.js:97-100` 的内联状态栏赋值改用 `setStatusBar('err'/'ok', text)`。

- [ ] **Step 4: 冒烟**

`tools/index.html`：BLE OTA 完整跑一遍，观察 6 个阶段节点 done/live/err 切换 + 状态栏文案正确。CAN 页无需测。

- [ ] **Step 5: 提交**

```bash
git add tools/js/ble/stages.js tools/js/ota/progress.js tools/js/ota/stages.js tools/js/ota/upload-gd32.js
git commit -m "refactor(js): 迁移阶段节点/状态栏至 common/stage-ui.js"
```

---

### Task 14: 迁移 SHA-256 计算与比较至 `common/sha.js`

**Files:**
- Modify: `tools/js/ble/file.js:114-143`（`computeFileSha256` 删除，改 import）
- Modify: `tools/js/ble/transfer.js:59-66`（`sha256HexToBytes` 删除，改 import）、`:185-188`（手写 hex 改用 `computeBufferSha256`）
- Modify: `tools/js/ota/file-handle.js:501-528`（`_computeAndCacheFileSha` 改用 `computeFileSha256`/`computeBufferSha256`）
- Modify: `tools/js/ble/gatt.js:387,421`（SHA 比较改用 `compareShaHex`）
- Modify: `tools/js/ota/upload-esp32.js:534`（SHA 比较改用 `compareShaHex`）

- [ ] **Step 1: 替换 computeFileSha256 与 sha256HexToBytes**

各文件顶部 import，删除本地定义。`ble/transfer.js:185-188` 的手写 hex 删除，改 `state.fileSha256 = await computeBufferSha256(fileBytes.buffer)`。

- [ ] **Step 2: 替换 compareShaHex**

`gatt.js:387` `state.deviceSha256.toUpperCase() === state.fileSha256.toUpperCase()` → `compareShaHex(state.deviceSha256, state.fileSha256)`。`:421` 同理。`upload-esp32.js:534` `sha256.toLowerCase() === data.sha256.toLowerCase()` → `compareShaHex(sha256, data.sha256)`。

- [ ] **Step 3: 冒烟**

BLE OTA + WiFi OTA 各跑一次，验证 SHA 比对结果与改前一致（成功路径与故意错配路径）。

- [ ] **Step 4: 提交**

```bash
git add tools/js/ble/file.js tools/js/ble/transfer.js tools/js/ble/gatt.js tools/js/ota/file-handle.js tools/js/ota/upload-esp32.js
git commit -m "refactor(js): 迁移 SHA-256 计算/比较至 common/sha.js"
```

---

### Task 15: 迁移 GATT 发现/读写至 `ble/gatt-helpers.js`

**Files:**
- Modify: `tools/js/ble/gatt.js:98-102`（`connectBle` 中发现）、`:478-484`（`tryReconnect` 中发现）
- Modify: `tools/js/ble/transfer.js:228,254,314,329,374,416`（write 调用）
- Modify: `tools/js/ble/gatt.js:158`（readValue）

- [ ] **Step 1: 替换 discoverCharacteristics**

`gatt.js:98-102` 与 `:478-484` 两处 `getPrimaryService` + 4×`getCharacteristic` 改为 `var ch = await discoverCharacteristics(server);` 并把 `ch.ctrl`/`ch.dataIn` 等赋给 `state.*`。

- [ ] **Step 2: 替换 write/read 包装**

可选：`writeValueWithResponse` → `writeWithResponse(char, data)` 等。若收益不大可仅替换发现部分，write/read 保持原样（本步可降级为 optional）。

- [ ] **Step 3: 冒烟**

BLE 连接 + OTA 上传一次，观察发现/写入正常。

- [ ] **Step 4: 提交**

```bash
git add tools/js/ble/gatt.js tools/js/ble/transfer.js
git commit -m "refactor(js): 迁移 GATT 发现至 ble/gatt-helpers.js"
```

---

### Task 16: 迁移设备 IP/BaseUrl 至 `common/device-ip.js`

**Files:**
- Modify: `tools/js/app.js:52-64`（`getEspIp`/`getBaseUrl` 删除，import；保留 `window.getBaseUrl` 兼容）
- Modify: `tools/js/can/dbc.js:38-60`（`getDeviceIp`/`setDeviceIp`/`getBaseUrl` 删除，import）
- Modify: `tools/js/can_monitor.js:31-45`（删除本地 `getBaseUrl`，import）
- Modify: `tools/js/common/connection.js:367`、`tools/js/common/polling.js:190`（已通过 `window.getBaseUrl`，确认仍可用）

- [ ] **Step 1: app.js 迁移**

`import { getDeviceIp as getEspIp, getBaseUrl } from './common/device-ip.js';`，删除本地 52-64。末尾 `window.getBaseUrl = getBaseUrl; window.getEspIp = getEspIp;` 保留兼容（因 `connection.js`/`polling.js` 用 `window.*`）。

- [ ] **Step 2: dbc.js 迁移**

`import { getDeviceIp, setDeviceIp, getBaseUrl } from '../common/device-ip.js';`，删除 38-60。注意：`dbc.js` 原 `getDeviceIp` 读 `el.toolIp`，迁移后需用 `getDeviceIp({ inputId: 'toolIp' })` 调用，或确认 `el.toolIp` 在调用时已缓存。

- [ ] **Step 3: can_monitor.js 迁移**

删除 31-45，import。`getBaseUrl` 调用点改为 `getBaseUrl({ inputId: 'monIp' })` 或 `{ inputId: 'toolIp' }`（按原文优先级）。

- [ ] **Step 4: 冒烟**

`tools/can.html` 与 `tools/index.html` 各加载一次，确认连接的目标 IP 正确（默认 192.168.31.127），切换 IP 后 localStorage 持久化生效。

- [ ] **Step 5: 提交**

```bash
git add tools/js/app.js tools/js/can/dbc.js tools/js/can_monitor.js
git commit -m "refactor(js): 迁移设备 IP/BaseUrl 至 common/device-ip.js"
```

---

### Task 16b: 消除 `bytesToHex`/`formatTime` 重复实现

**Files:**
- Modify: `tools/js/can/dbc.js:79-86`（删除本地 `bytesToHex`，统一用 `common/utils.js` 的）
- Modify: `tools/js/can/send.js:23-29`（删除本地 `formatTime`，改用参数化 `common/utils.js` 版）

- [ ] **Step 1: 统一 bytesToHex**

先确认 `common/utils.js:51-60` 的 `bytesToHex` 输出小写，而 `can/dbc.js:79-86` 用 `padHex` 输出大写。需检查 `dbc.js` 的 `bytesToHex` 调用点（`can/send.js`、`can/signals.js`）对大小写的依赖。

- 若调用点只用于显示且显示要求大写：在 `common/utils.js` 给 `bytesToHex` 加 `{ upper: true }` 选项：
  ```js
  function bytesToHex(bytes, opts) {
    var hex = Array.prototype.map.call(new Uint8Array(bytes), function (b) {
      var h = b.toString(16).padStart(2, '0');
      return opts && opts.upper ? h.toUpperCase() : h;
    }).join('');
    return hex;
  }
  ```
  然后 `dbc.js` 调用 `bytesToHex(bytes, { upper: true })`。
- 删除 `dbc.js:79-86` 本地定义与 `padHex`（若 `padHex` 无其他调用点）。

- [ ] **Step 2: 统一 formatTime**

在 `common/utils.js:17-23` 的 `formatTime` 加 `opts.millis` 选项（默认 true）：
```js
function formatTime(date, opts) {
  var d = date instanceof Date ? date : new Date(date);
  var pad = function (n) { return n < 10 ? '0' + n : '' + n; };
  var base = pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
  if (opts && opts.millis === false) return base;
  return base + '.' + ('00' + d.getMilliseconds()).slice(-3);
}
```
`can/send.js:23-29` 删除本地定义，调用改为 `formatTime(date, { millis: false })`。

- [ ] **Step 3: 冒烟**

CAN 页发送一帧，历史记录时间显示 `HH:MM:SS`（无毫秒）；hex 预览/字节条显示大写。

- [ ] **Step 4: 提交**

```bash
git add tools/js/common/utils.js tools/js/can/dbc.js tools/js/can/send.js
git commit -m "refactor(js): 统一 bytesToHex/formatTime 至 common/utils.js"
```

---

### Task 17: 提取 `buildOtaBody` 消除 esp32/gd32 OTA body 重复组装

**Files:**
- Create: `tools/js/common/ota-body.js`
- Modify: `tools/js/ota/upload-esp32.js:219-242`

- [ ] **Step 1: 创建 ota-body.js**

```js
// tools/js/common/ota-body.js
function buildOtaBody(arrayBuffer, headerObj) {
  var encoder = new TextEncoder();
  var headerBytes = encoder.encode(JSON.stringify(headerObj));
  var payload = new Uint8Array(arrayBuffer);
  var combined = new Uint8Array(headerBytes.length + payload.length);
  combined.set(headerBytes, 0);
  combined.set(payload, headerBytes.length);
  return combined.buffer;
}
export { buildOtaBody };
```

- [ ] **Step 2: 替换 upload-esp32.js:219-242**

ESP32 分支：`buildOtaBody(payload, { type: 'esp32', total_size: payload.byteLength })`
GD32 分支：`buildOtaBody(payload, { total_size: payload.byteLength })`

- [ ] **Step 3: 冒烟**

WiFi OTA 上传 ESP32 与 GD32 固件各一次，服务端正常解析。

- [ ] **Step 4: 提交**

```bash
git add tools/js/common/ota-body.js tools/js/ota/upload-esp32.js
git commit -m "refactor(js): 提取 buildOtaBody 消除 esp32/gd32 body 重复"
```

---

### Task 18: 统一 SHA verify panel UI（BLE 与 OTA 共用同一套 DOM id）

**Files:**
- Create: `tools/js/common/verify-panel.js`
- Modify: `tools/js/ble/verify.js:3-26`（改为薄包装或 re-export）
- Modify: `tools/js/ota/upload-esp32.js:200-207,486-489,529-540,557-564,448-452,470-475`（内联 DOM 写改用共享 helper）

- [ ] **Step 1: 创建 verify-panel.js**

```js
// tools/js/common/verify-panel.js
function showVerifyInfo(frontSha, devSha, opts) {
  opts = opts || {};
  var panel = document.getElementById('stageVerifyInfo');
  if (panel) panel.hidden = false;
  var f = document.getElementById('sviFrontSha');
  var d = document.getElementById('sviDeviceSha');
  var r = document.getElementById('sviResult');
  if (f) f.textContent = frontSha || '';
  if (d) d.textContent = devSha || '';
  if (r) {
    r.textContent = opts.text || '';
    r.style.color = opts.ok ? '#2ecc71' : (opts.err ? '#e74c3c' : '');
  }
}
function hideVerifyInfo() {
  var panel = document.getElementById('stageVerifyInfo');
  if (panel) panel.hidden = true;
}
function resetVerifyInfo() {
  ['sviFrontSha', 'sviDeviceSha', 'sviResult'].forEach(function (id) {
    var el = document.getElementById(id);
    if (el) { el.textContent = ''; el.style.color = ''; }
  });
}
export { showVerifyInfo, hideVerifyInfo, resetVerifyInfo };
```

> ⚠️ 实现前需 Read `tools/js/ble/verify.js:3-26` 与 `ota/upload-esp32.js` 上述行段，确认精确 DOM id 与样式字段，本 step 为骨架。

- [ ] **Step 2: ble/verify.js 改为 re-export**

`export { showVerifyInfo as showBleVerifyInfo, hideVerifyInfo as hideBleVerifyInfo } from '../common/verify-panel.js';`（若 `ble/verify.js` 有 BLE 专属逻辑则保留包装）。

- [ ] **Step 3: ota/upload-esp32.js 内联 DOM 写改用 helper**

`:200-207` reset → `resetVerifyInfo()`；`:486-489` show → `showVerifyInfo(...)`；`:529-540` 比对成功 → `showVerifyInfo(front, dev, { text: '匹配', ok: true })`；`:557-564` 失败 → `showVerifyInfo(front, dev, { text: '不匹配', err: true })`。

- [ ] **Step 4: 冒烟**

BLE OTA 与 WiFi OTA 各跑一次，验证 SHA 面板显示与改前一致。

- [ ] **Step 5: 提交**

```bash
git add tools/js/common/verify-panel.js tools/js/ble/verify.js tools/js/ota/upload-esp32.js
git commit -m "refactor(js): 统一 SHA verify panel UI 至 common/verify-panel.js"
```

---

## 阶段 D：JS 过长函数拆分

### Task 19: 拆分 `ble/gatt.js:handleNotify` (262 行)

**Files:**
- Modify: `tools/js/ble/gatt.js:182-444`

- [ ] **Step 1: 识别子段**

按通知类型拆出三个本地函数：
- `handleAckNotify(buf)` — ACK 解析 + state + progress + waiter 解析
- `handleFinishNotify(buf)` — FINISH 解析 + 阶段推进 + 结果等待
- `handleSha256Notify(buf)` — SHA256 解析 + verify panel + 比较

- [ ] **Step 2: 重写 handleNotify 为 dispatcher**

```js
function handleNotify(event) {
  var buf = new Uint8Array(event.target.value.buffer);
  if (buf.length < 1) return;
  switch (buf[0]) {
    case BLE_OTA_NOTIFY_ACK:    return handleAckNotify(buf);
    case BLE_OTA_NOTIFY_FINISH: return handleFinishNotify(buf);
    case BLE_OTA_NOTIFY_SHA256: return handleSha256Notify(buf);
    default: bleLog('未知通知 0x' + buf[0].toString(16));
  }
}
```

- [ ] **Step 3: 冒烟**

BLE OTA 全流程，观察 ACK/FINISH/SHA256 三类通知处理正常。

- [ ] **Step 4: 提交**

```bash
git add tools/js/ble/gatt.js
git commit -m "refactor(ble): 拆分 handleNotify 为 ack/finish/sha256 三个 handler"
```

---

### Task 20: 拆分 `ble/transfer.js:uploadFirmware` (367 行)

**Files:**
- Modify: `tools/js/ble/transfer.js:107-474`

- [ ] **Step 1: 提取子函数**

- `refreshFileBeforeUpload()` — 文件重读逻辑 (109-120)
- `sendWindow(windowStart, endSeq, fileBytes, payloadSize, cfg)` — 单窗口发送 (219-302)
- `retryWindow(resumeSeq, endSeq, ...)` — 重传 (304-345) — 与 sendWindow 共享 per-packet 体
- `buildEsp32Trigger(fileSha)` — ESP32 触发包 (362-396)
- `buildGd32Trigger(fileSha, sessionId, cfg)` — GD32 触发包 (400-441)

- [ ] **Step 2: 重写 uploadFirmware 为编排**

保留外层窗口循环、retry 决策、stage 推进、结果等待；per-packet 体提到 `sendPacket(seq, fileBytes, payloadSize, cfg)` 共享。

- [ ] **Step 3: 冒烟**

BLE OTA 上传 ESP32 与 GD32 固件各一次，观察窗口推进/重传/触发/结果等待正常。

- [ ] **Step 4: 提交**

```bash
git add tools/js/ble/transfer.js
git commit -m "refactor(ble): 拆分 uploadFirmware 为窗口/触发子函数"
```

---

### Task 21: 拆分 `ota/upload-esp32.js:startUpload` (299 行)

**Files:**
- Modify: `tools/js/ota/upload-esp32.js:56-355`

- [ ] **Step 1: 提取子函数**

- `probeOtaMutex(chip)` — 4 次重试的 mutex 探测
- `probeStaleSession(chip)` — 旧会话探测
- `buildUploadBody(chip, payload)` — 用 Task 17 的 `buildOtaBody`
- `wireXhrHandlers(xhr, chip, payload)` — 5 个 XHR handler 绑定（progress/load/error/abort/timeout）

- [ ] **Step 2: 重写 startUpload 为编排**

- [ ] **Step 3: 冒烟**

WiFi OTA 上传 ESP32 与 GD32 各一次。

- [ ] **Step 4: 提交**

```bash
git add tools/js/ota/upload-esp32.js
git commit -m "refactor(ota): 拆分 startUpload 为 mutex/body/xhr 子函数"
```

---

### Task 22: 拆分 `common/polling.js:pollStatus` (122 行) 与 `ota/stages.js:applyGd32OtaSessionUi` (151 行)

**Files:**
- Modify: `tools/js/common/polling.js:182-303`
- Modify: `tools/js/ota/stages.js:182-333`

- [ ] **Step 1: pollStatus catch 拆分**

把 228-290 的 7 分支 if/else 拆出 `handlePollRecovery`/`handlePollReboot`/`handlePollSessionFailure`/`handlePollDeviceBusy`/`handlePollDisconnected`，`pollStatus` 的 catch 调用之。

- [ ] **Step 2: applyGd32OtaSessionUi 拆分**

按 `otaSessionState` 分支拆出 `applyGd32Failed`/`applyGd32Relaying`/`applyGd32Storing`/`applyGd32Done`。

- [ ] **Step 3: 冒烟**

模拟 OTA 失败/转发中/存储中/完成四态（可手动改 `window.otaSessionState`），观察 UI 切换正常；pollStatus 在断连/重启/失败下行为正常。

- [ ] **Step 4: 提交**

```bash
git add tools/js/common/polling.js tools/js/ota/stages.js
git commit -m "refactor(js): 拆分 pollStatus 与 applyGd32OtaSessionUi"
```

---

## 阶段 E：死代码/一次性 helper 清理

### Task 23: C++ 侧死代码与内联

**Files:**
- Modify: `main/server/ble_ota.cpp:346-349`（内联 `BleOta::isConnected()` 或保留——评估后决定）
- Modify: `main/server/ble_ota_host.cpp:228-236`（0xFB handler 内联队列 drain 改用 `drainOtaDataQueue()`）
- Modify: `main/include/app_config.h:126`（确认 `BLE_OTA_CTRL_VERIFY_SHA256 = 0xF9` 是否死代码）

- [ ] **Step 1: 0xFB handler 改用 drainOtaDataQueue**

`ble_ota_host.cpp:228-236` 的内联 drain 循环替换为 `drainOtaDataQueue();`。

- [ ] **Step 2: 检查 BLE_OTA_CTRL_VERIFY_SHA256**

`grep -rn "0xF9\|BLE_OTA_CTRL_VERIFY_SHA256" main/ tools/`。若全无引用，删除 `app_config.h:126` 该行。

- [ ] **Step 3: 评估 BleOta::isConnected**

若调用点仅 2 处且均为简单条件判断，内联；否则保留。本 step 可降级为 no-op。

- [ ] **Step 4: 验证编译 + 冒烟**

- [ ] **Step 5: 提交**

```bash
git add main/server/ble_ota_host.cpp main/include/app_config.h main/server/ble_ota.cpp
git commit -m "refactor(ble_ota): 清理 0xFB 内联 drain 与死常量"
```

---

### Task 24: 消除 `polling.js` 与 `app.js` 的重复 IP-change 监听

**Files:**
- Modify: `tools/js/common/polling.js:372-387`
- Modify: `tools/js/app.js:236-249`

- [ ] **Step 1: 确认重复**

读两边代码，确认同一 `#espIp` `change` 事件被绑定两次（一次 `app.js` 初始化时，一次 `polling.js` setTimeout 500ms 后）。

- [ ] **Step 2: 删除 polling.js 中的重复绑定**

保留 `app.js:236-249` 版本（更早绑定），删除 `polling.js:376-386` 的 IP change 绑定；保留 `polling.js:373-375` 的 upload button label 更新（若无重复）。

- [ ] **Step 3: 冒烟**

`tools/index.html` 修改 IP 输入，确认只触发一次保存 + 重置失败计数 + 重启轮询。

- [ ] **Step 4: 提交**

```bash
git add tools/js/common/polling.js
git commit -m "refactor(js): 删除 polling.js 重复的 IP change 监听"
```

---

### Task 25: JS 杂项死代码清理

**Files:**
- Modify: `tools/js/can/messages.js:27`（`el.importDbcBtn.hidden = false` 无操作）
- Modify: `tools/js/can_monitor.js:22-23`（重复的 toast ID 缓存，若与 `dbc.js` 的 `el` 共享）
- Modify: `tools/js/ble/state.js:99-105`（`hexToBytes` 单调用点评估内联）

- [ ] **Step 1: 删除 messages.js:27 无操作行**

- [ ] **Step 2: can_monitor.js toast ID 评估**

确认 `can_monitor.js` 是否与 `can/dbc.js` 共享 `el`。若共享，删除 `:22-23` 重复 toast ID。若独立保留。

- [ ] **Step 3: 评估 hexToBytes 内联**

`ble/state.js:99-105` 仅 `ble/transfer.js:408` 调用。若 `cfg.ctrlHex` 改为预编码常量字节数组（在 `ble/constants.js` 的 TARGETS 里），可删除 `hexToBytes`。本 step 可降级为 no-op。

- [ ] **Step 4: 冒烟**

`tools/can.html` 加载无报错；CAN 监控 toast 正常。

- [ ] **Step 5: 提交**

```bash
git add tools/js/can/messages.js tools/js/can_monitor.js tools/js/ble/state.js tools/js/ble/constants.js tools/js/ble/transfer.js
git commit -m "refactor(js): 清理无操作行与重复 toast 缓存"
```

---

## 自检清单（Self-Review）

### 1. Spec 覆盖

用户请求：「扫描每个函数，评估是否可以精简，主要关注复用关系」。覆盖维度：
- ✅ 复用：hex_utils、progress、stage-ui、sha、fetch-json、device-ip、gatt-helpers、verify-panel、buildOtaBody、triggerGd32RelayCommon、shouldSendThrottledAck、bytesToHex/formatTime 统一
- ✅ 精简：拆分 ble_ota_ctrl_access、ota_upload_handler、handleNotify、uploadFirmware、startUpload、pollStatus、applyGd32OtaSessionUi
- ✅ 死代码：F9 常量、重复 IP 监听、无操作 hidden、重复 toast 缓存

### 2. Placeholder 扫描

- Task 4 Step 2、Task 7 Step 1、Task 11 Step 1、Task 18 Step 1 含「以原文为准」骨架——已显式标注 ⚠️ 并要求先 Read 原文。这是必要的谨慎（避免凭记忆改写节流条件/DOM id），非占位。
- Task 23 Step 3、Task 25 Step 3 标注「可降级为 no-op」——这是显式的可选清理，非占位。

### 3. 类型/命名一致性

- `setProgress(percent, text)` 跨 BLE/OTA 一致。
- `getStageStatusBar`/`getStageStatusText` 在 ble/stages.js 与 common/stage-ui.js 一致。
- `compareShaHex(a, b)` 在 common/sha.js 与调用点一致。
- `buildOtaBody(arrayBuffer, headerObj)` 在 common/ota-body.js 与调用点一致。
- `triggerGd32RelayCommon` 在 C++ 侧命名与 `triggerGd32Relay` 区分清晰。

---

## Execution Handoff

**Plan complete and saved to `.zcode/plans/2026-07-18-function-simplify-reuse.md`. Two execution options:**

**1. Subagent-Driven (recommended)** - 每个 task 派一个新 subagent，task 间做两阶段 review，迭代快。

**2. Inline Execution** - 在当前 session 用 executing-plans 批量执行 + checkpoint review。

**Which approach?**