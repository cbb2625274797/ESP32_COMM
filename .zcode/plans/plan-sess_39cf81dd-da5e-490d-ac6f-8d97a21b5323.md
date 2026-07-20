## 目标
重新生成 `G:/Robot_v2/DCU/SPEC/code_optimization_audit_2026-07-19.html`，让它反映当前工作树的代码状态（已修改的 `health_monitor.*`、`spi_master.*`、`ble_ota_data.cpp`、`version.h`、`tools/js/common/connection.js` 等）。

## 步骤
1. 在 `G:/Robot_v2/DCU` 下运行生成器（不带 `--verify`，按你刚才的选择）：
   ```bash
   python tools/gen_code_optimization_audit.py
   ```
   - 默认输出到 `SPEC/code_optimization_audit_<今天日期>.html`，即 `code_optimization_audit_2026-07-20.html`（今天 2026-07-20）。
   - 注意：现有文件名是 `..._2026-07-19.html`，新生成的会是 `..._2026-07-20.html`，不会覆盖旧文件。

2. 检查脚本输出：
   - 看 `[audit] files=... lines=...`、findings 数量、是否出现 `重复 finding ID` 或锚点漂移错误。
   - 脚本内置 HTML 结构与内部链接自校验，失败会返回非 0。

3. 如有锚点漂移（某个 finding 引用的行号在改动后对不上原证据），脚本会报错并指出哪个 finding；此时需要我先核对对应源码再决定是调整脚本里的锚点还是接受新行号——会先汇报给你，不会私自改 finding 逻辑。

4. 生成成功后，简要汇报：
   - 新报告路径
   - findings 数量与 P0/P1/P2 分布是否变化
   - 与旧报告的关键差异（例如新增/消失的 finding、行号变化）
   - 旧报告 `2026-07-19.html` 是否保留（默认保留）

## 不做的事
- 不带 `--verify`（不跑 ESP32/GD32 构建、SPI 自测、前端语法检查）
- 不删除旧报告 `2026-07-19.html`
- 不改生成器脚本本身
- 不做 git 提交