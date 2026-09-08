/*
 * version.h - 固件版本信息（手动维护）
 *
 * 注意：本文件不再由构建脚本自动生成。如需修改版本号或 Git Hash，
 * 请直接编辑本文件。
 *   - FW_VERSION_MAJOR / MINOR / PATCH：发版时手动递增
 *   - GIT_HASH：手动填写对应提交的 git short hash
 *   - 编译时间：已改为构建系统自动生成（build_info.h 的 fwBuildTime()，
 *     每次构建刷新），手写 BUILD_TIME 已删——旧值停在 2026-07-26 20:25
 *     曾被误判为 OTA 回退。
 */
#pragma once

// 固件版本
#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 4
#define FW_VERSION_PATCH 1
#define FW_VERSION_STRING "0.4.1"   // GD32 OTA 中继全程 ring 抑制修复：handleIdle 起点调 reset() 曾把 taskEntry 刚置位的 setSuppressRingService(true) 立即解除，导致中继全程 worker 偷读 OTA ACK/WAIT（"serviceRing 读到 RESPONSE"告警，log#449），poll 空等→50ms 重发→包失败；reset() 增加 release_ring_suppress 参数，状态机起点 reset(false) 只清状态不清抑制，结束/中止路径仍统一释放。0.3.85 INIT_STATUS 日志补打 fault_codes hex 一行（13B 模块故障码，编码见 GD32 sys/initial.h）——排查 GD32 init 失败无需解码位图；配套 GD32 1.5.38 看门狗 LSI 修复验证。0.3.84 GD32 模块级故障码（配套 GD32 1.5.36）：0xA3 INIT_STATUS 响应 25B 时解析 [12..24] fault_codes（每模块 1B，canfd 高 4 位为通道号），gd32_init_status_t 增 fault_codes/has_fault_codes；/system/status gd32_init 新增 fault_codes 数组，前端「GD32 自检」悬停/值直接显示中文失败原因。0.3.83 GD32 APP 初始化状态可视（配套 GD32 1.5.34）：身份块[15]解析 init_state(0=OK/1=degraded/2=critical_fail,HELLO/PING 自动刷新,异常打 WARN)；新增 SpiMaster::appInitStatus() 经 SPI 0xA3 查询模块位图/critical 失败位图/复位源(仅 APP 角色,成功即缓存,session 重建自清)；/system/status 新增 gd32_init 字段(state/state_known/detail_known/module_mask/critical_fail_mask/reset_cause,惰性查询+10s 失败退避)；0xA3 加入短响应/ISR 快速响应类型表。0.3.82 DebugLog 新增 UART0 RX 泵任务(GPIO20, 3M 8N1)：收到的串口行按 \n 切行、剥 \r，以 tag=UART 推入 ring → HTTP /logs 出现在网页日志面板；接线=任意串口源 TX→H1/H2 的 ESP_RX。0.3.81 GD32 OTA 包级防呆：中继触发前校验 gd32_ota 镜像向量表身份(Gd32FlashStore::validateStoredImageIdentity)，BOOT 包/非 APP 包在 WiFi /ota/relay 与 BLE 0xF7 均拒绝，GD32 APP 区不再被错误包擦除；BLE 新增 0x87 完成码

// Git 信息（手动填写）
#define GIT_HASH "770ae14"