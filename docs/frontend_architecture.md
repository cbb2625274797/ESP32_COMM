# 前端架构约定与回归清单（tools/ Web 界面）

> 2026-07 网页框架化重构产物。本文档固化 tools/ 目录的模块化约定，
> 所有新增页面/模块必须遵循；同时提供改动后的回归验证清单。

## 1. 架构总览

- **托管方式**：Web 资源不嵌入固件，由 `tools/serve.bat`（`python -m http.server 8080`）本地托管；
  浏览器跨源调用 ESP32 REST API（路由表见 `main/server/wifi_ota.cpp` kHttpRoutes）。
  改动 web 文件无需重编译固件。
- **零构建**：原生 ES Modules，无打包器/无 CDN 依赖；页面必须经 http 访问（ES module 有 file:// CORS 限制）。

### 页面薄入口范式

每个页面 = HTML（纯结构，无内联 script/style/事件）+ 单个 `type="module"` 入口：

| 页面 | 入口 | 说明 |
|---|---|---|
| index.html | js/app.js + js/ota/console-mode.js + js/ble_ota.js | OTA 控制台（WiFi/BLE 双通道，三入口历史原因保留） |
| motor.html | js/motor/main.js | 电机控制台（独立 seed 主题，见 §4） |
| can.html | js/can.js | CAN 工具 |
| can_monitor.html | js/can_monitor.js | CAN 接收监控 |

### JS 分层

```
js/
├── common/   跨页面公共层：base-url(IP存取) / toast / log / polling / connection / utils
├── ota/      OTA 状态机与上传流程（高风险区，见 §5）
├── ble/      BLE OTA 传输时序（高风险区，见 §5）
├── can/      CAN 子系统（dbc 为基础层，导出共享 state/el）
├── motor/    电机控制台
└── *.js      页面入口（薄装配层）
```

## 2. 缓存戳约定（强制）

- **JS 间 import 一律不带 `?v=`**——同一模块带不同 query 会被浏览器实例化两次，
  导致模块级单例（state/toast 去抖等）分裂。
- 版本戳只出现在 HTML 的 `<script>/<link>` 标签上；改动任何 JS/CSS 后，
  **必须更新对应页面入口标签的 `?v=`**（格式 `?v=YYYYMMDD<字母>`），否则浏览器可能混载新旧模块。

## 3. 公共层 API 契约

- `common/base-url.js`：设备 IP 唯一存取点（localStorage `dcu_esp_ip`，OTA/CAN/电机页共用）。
  页面特有回退（CAN 页 `el.toolIp`、OTA 页 `#espIp` 输入框）由各自入口在此基础上包装，
  不得绕过它直接读写 localStorage。
- `common/toast.js`：单例 toast（`#toast/#toastIco/#toastMsg` + components.css 样式），
  内置 5s 去重。适用于 index/can/can_monitor；motor 页因独立主题使用自有堆叠式 toast（§4）。
- `common/polling.js`：递归 setTimeout 轮询 + in-flight 守卫 + Page Visibility 补偿。
  依赖 app.js 先挂载 window.OtaSessionState 等（见文件头注释的 window.* 清单），加载顺序不可变。
- index.html 按钮事件：统一 `data-action`/`data-arg` 属性 + app.js 末尾的事件委托，
  **禁止新增内联 onclick**；window.* 函数挂载保留（polling/ota 模块间仍经 window 总线）。

## 4. motor 页特殊性

- 独立视觉主题（seed tokens，`css/motor.css`），**不引入** base.css/components.css
  （两者与 motor.css 存在 `.btn/.toast/.toolbar` 等同名类，混用会破坏视觉）。
- 自有堆叠式 toast（`#toastContainer`，多条并存），与 common/toast.js 的单例 DOM 契约不同，
  不做统一。
- 三条轮询链路（/motor/state、/can/feedback、/system/status）必须互不阻塞
  （服务端约束，见 wifi_ota.cpp 注释）。

## 5. 红线（不可动区域）

- `js/ble/*`：Web Bluetooth 包发送时序、重试退避、FINISH 中止——大量真机实测调优。
- `js/ota/{session,stages,progress,upload-esp32,upload-gd32}.js`：OTA 状态机与
  window.* 状态总线（activePhase/otaSessionState/currentOtaTarget 等）。
- index.html 视觉：shimmer / logoPulse 动画（css/responsive.css，仅 transform/opacity）不得削弱。
- 性能规范：高频更新走增量渲染 + write-before-compare + rAF 批量；CSS 动画仅 transform/opacity。

## 6. 回归清单

验证方式：`tools/serve.bat` 启动 → 浏览器打开 `http://localhost:8080/<页面>` → 真机 IP 连接。

### index.html（OTA 控制台）
- [ ] WiFi/BLE 模式切换、GD32/ESP32 目标切换（顶部按钮）
- [ ] 固件选择：点击/拖拽上传区、清除按钮、SHA-256 显示、FSAPI 热重载
- [ ] ESP32 上传 → 阶段流程条驱动 → 重启 → 版本核对
- [ ] GD32 OTA relay → 进度/阶段 → 完成 toast
- [ ] 日志面板：过滤、自动滚动、清空；JSON 状态面板；任务排序
- [ ] 断线/重连：连接胶囊状态、"测试"按钮、重启 ESP32 按钮
- [ ] 视觉：shimmer/logoPulse 动画正常，无掉帧
- [ ] BLE 模式：连接、一键刷写、中止、清除文件、拖拽选文件

### motor.html（电机控制台）
- [ ] 三条轮询链路存活（节点列表 1Hz 刷新、状态栏 RX/TX、健康检查）
- [ ] 控制指令发送、周期重发、模式切换、PID 配置
- [ ] 版本查询、NodeID 设置、电机 OTA relay
- [ ] 输入持久化（刷新后回填）、波形显示、console 无报错
- [ ] `window.phaseDebug` 调试句柄可用

### can.html / can_monitor.html
- [ ] DBC 导入/解析/合并、报文列表/信号编辑、发送（单发/批量/手动）
- [ ] 发送历史、会话持久化、接收监控（独立页 + 抽屉）
- [ ] toast 仅弹一次（无双实例重复弹）
