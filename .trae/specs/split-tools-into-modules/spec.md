# tools 网页工程化拆分 Spec

## Why
`tools/` 目录下 4 个核心文件（`app.js` 2371 行、`ble_ota.js` 1658 行、`can.js` 1526 行、`style.css` 2146 行）体积过大，单文件承载多重职责，难以维护和导航。`index.html` 还内联了 ~250 行胶水脚本。需要按职责拆分为小文件工程，提升可维护性。

## What Changes
- 将 `style.css` 拆分为 5 个按职责划分的 CSS 文件（base / layout / components / can / responsive）
- 将 `app.js` 拆分为 8 个 ES Module（ota/session、progress、stages、file-handle、upload-esp32、upload-gd32、spi、console-mode）+ 公共模块
- 将 `ble_ota.js` 拆分为 8 个 ES Module（ble/constants、state、stages、gatt、file、transfer、verify、session）
- 将 `can.js` 拆分为 5 个 ES Module（can/dbc、messages、signals、send、feedback）
- 抽取 `index.html` 内联 `<script>` 到 `js/ota/console-mode.js`
- 抽取跨页面共享的工具函数到 `js/common/`（utils、toast、log、connection、polling）
- 三个 HTML 页面的 `<script>` 改为 `type="module"`，`<link>` 拆分为多文件引用
- 删除原始大文件，用薄入口文件（`js/app.js`、`js/ble_ota.js`、`js/can.js`）替代

## Impact
- Affected code: `tools/` 目录全部文件（`app.js`、`ble_ota.js`、`can.js`、`can_monitor.js`、`style.css`、`index.html`、`can.html`、`can_monitor.html`）
- 不影响 ESP32 固件代码（`main/` 目录不改动）
- `serve.bat` 无需修改（Python http.server 天然支持 ESM 的 MIME 类型）
- **BREAKING**：旧的单文件引用路径（`app.js?v=...`）将不存在，HTML 中所有 `<script src>` 和 `<link href>` 路径需更新

## 技术方案

### 模块组织：原生 ES Modules
- 使用 `<script type="module">` + `import`/`export`，零构建、零工具链依赖
- `serve.bat`（Python http.server）无需修改，浏览器原生解析 ESM
- 所有 `import` 路径必须带 `.js` 后缀（浏览器 ESM 规范要求）

### HTML 内联事件处理器兼容策略
当前 HTML 大量使用 `onclick="funcName()"` 内联处理器。ES Module 作用域非全局，采用以下策略：
1. **内部函数**：模块内使用，不暴露到全局
2. **HTML 引用函数**：在入口模块中显式挂载到 `window`（如 `window.setConsoleMode = setConsoleMode`）
3. 模块脚本默认 `defer`，DOM 解析完成后执行，用户点击前 `window` 上的函数已就绪

### CSS 拆分策略
- 多 `<link>` 标签并行加载（优于 `@import` 串行）
- 每个页面引入相同的 5 个 CSS 文件（base + layout + components + responsive 为全站共用，can 为 CAN 页面专用但 OTA 页面也引用了 CAN 入口卡片样式）

### 目标目录结构
```
tools/
├── index.html                  # 结构 only，<script type="module" src="js/app.js"> + js/ble_ota.js
├── can.html                    # <script type="module" src="js/can.js">
├── can_monitor.html            # <script type="module" src="js/can_monitor.js">
├── serve.bat                   # 不变
├── css/
│   ├── base.css                # :root 变量、reset、container、背景纹理
│   ├── layout.css              # header、main-layout、cards、forms、buttons、conn-indicator
│   ├── components.css          # upload-zone、file-info、progress、stage-flow、badges、metrics、log-panel、toast、heap
│   ├── can.css                 # CAN workspace + monitor 专用样式
│   └── responsive.css          # @media 响应式 + @keyframes 动画
└── js/
    ├── app.js                  # 薄入口：import ota/* + common/*，挂载 window 函数
    ├── ble_ota.js              # 薄入口：import ble/*，导出 window.bleOta
    ├── can.js                  # 薄入口：import can/*，初始化 CAN 工具
    ├── can_monitor.js          # CAN 监控入口（小文件，引用 common 模块）
    ├── common/
    │   ├── utils.js            # formatBytes / formatTime / formatUptime / escapeHtml / sanitizeHtml
    │   ├── toast.js            # showToast
    │   ├── log.js              # addLog + 环形缓冲 + 去重 + 自动滚动
    │   ├── connection.js       # testConnection + 连接状态指示器更新
    │   └── polling.js          # 状态/日志轮询调度 + Page Visibility 补偿
    ├── ota/
    │   ├── session.js          # OTA 会话状态机（OtaSessionState + 转换 + 复位）
    │   ├── progress.js         # 进度条 + 阶段节点 UI（setProgress / setActivePhase / markStageDone）
    │   ├── stages.js           # ESP32/GD32 阶段驱动（updateEsp32StagesFromPhase / GD32 阶段更新）
    │   ├── file-handle.js      # 文件选择 / FSAPI / SHA-256 / HEX→BIN 转换 / 文件信息 UI
    │   ├── upload-esp32.js     # ESP32 上传流程 + SHA 校验 + 完成升级
    │   ├── upload-gd32.js      # GD32 中继 OTA 触发 + relay 态跟踪
    │   ├── spi.js              # SPI 帧构建与发送
    │   └── console-mode.js     # 从 index.html 抽出的内联脚本（setConsoleMode / selectFlashTarget / 共享固件胶水）
    ├── ble/
    │   ├── constants.js        # UUIDS + BLE_OTA_* 协议常量 + TARGETS 配置
    │   ├── state.js            # state 对象 + 元素缓存（els）
    │   ├── stages.js           # BLE 阶段流管理（setBleStage / resetBleStages / setBleStageError）
    │   ├── gatt.js             # BLE 连接 / 断开 / 服务发现 / 通知使能
    │   ├── file.js             # 文件准备 + 分包 + notifyFileChange
    │   ├── transfer.js         # 数据写入循环 + ACK 窗口节流 + 重传
    │   ├── verify.js           # SHA-256 校验面板（showBleVerifyInfo / hideBleVerifyInfo）
    │   └── session.js          # waitForOtaResult + resolveOtaResult + abort 任务
    └── can/
        ├── dbc.js              # DBC 解析器（parseDbc / findSignal / buildSummary）
        ├── messages.js         # 报文列表渲染 + 过滤 + 节点过滤
        ├── signals.js          # 信号值编辑器 + 编解码 + 批量添加
        ├── send.js             # 帧构建 + 发送 + 错误码描述
        └── feedback.js         # 1Hz 反馈轮询 + ID 快照表
```

### 拆分原则
1. **保持功能完全不变**：纯结构重构，不修改任何业务逻辑、算法、阈值
2. **按现有注释分节对齐**：`app.js` 已有 17 个 `// ==== SECTION ====` 注释，按此天然边界拆分
3. **共享函数提取到 common/**：`formatBytes`、`showToast`、`addLog`、`testConnection` 等在多个文件中重复的函数统一提取
4. **循环依赖处理**：模块间若存在循环引用，通过回调注册或共享 state 模块解耦
5. **全局状态最小化**：原 `window.*` 全局变量保留必要的（`window.connectionMode`、`window.flashTarget`、`window.bleOta`），内部状态用模块级变量

## ADDED Requirements

### Requirement: ES Module 工程结构
系统 SHALL 将 `tools/` 目录组织为原生 ES Module 工程，所有 JS 文件使用 `import`/`export` 语法，HTML 通过 `<script type="module">` 加载。

#### Scenario: 浏览器加载页面
- **WHEN** 用户通过 `http://localhost:8080/index.html` 访问 OTA 控制台
- **THEN** 浏览器原生解析 ESM import 链，无构建步骤，所有模块按依赖顺序加载执行

#### Scenario: Python http.server 提供 ESM 文件
- **WHON** `serve.bat` 启动 Python http.server
- **THEN** `.js` 文件以 `application/javascript` MIME 类型返回，`type="module"` 脚本正常加载

### Requirement: CSS 按职责拆分
系统 SHALL 将 `style.css` 拆分为 5 个独立 CSS 文件，每个文件负责一个明确的样式职责域。

#### Scenario: 页面引用 CSS
- **WHEN** 任意 HTML 页面加载
- **THEN** 通过 5 个 `<link>` 标签并行加载 base + layout + components + can + responsive，视觉效果与原单文件 `style.css` 完全一致

## MODIFIED Requirements

### Requirement: HTML 脚本引用方式
三个 HTML 页面 SHALL 使用 `<script type="module">` 引用入口 JS 文件，不再使用普通 `<script src>`。`index.html` SHALL 移除所有内联 `<script>` 块，改为模块导入。

#### Scenario: index.html 加载脚本
- **WHEN** `index.html` 解析到末尾
- **THEN** 按顺序加载 `js/app.js`（WiFi OTA 入口）和 `js/ble_ota.js`（BLE 入口），两者均为 `type="module"`，原内联胶水代码由 `js/ota/console-mode.js` 模块提供

### Requirement: HTML 内联事件处理器兼容
系统 SHALL 保留现有 HTML 中的 `onclick="funcName()"` 内联处理器，并在入口模块中将这些函数显式挂载到 `window` 对象。

#### Scenario: 用户点击内联 onclick 按钮
- **WHEN** 用户点击 `onclick="setConsoleMode('wifi')"` 按钮
- **THEN** `window.setConsoleMode` 函数已被 `js/ota/console-mode.js` 模块挂载，正常执行模式切换逻辑

## REMOVED Requirements

### Requirement: 原始大文件
**Reason**: `app.js`（2371 行）、`ble_ota.js`（1658 行）、`can.js`（1526 行）、`style.css`（2146 行）单文件过大，已拆分为模块化小文件
**Migration**: 原文件删除，功能由 `js/` 和 `css/` 子目录下的模块文件承接，HTML 引用路径已更新
