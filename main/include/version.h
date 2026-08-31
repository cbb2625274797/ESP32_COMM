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
#define FW_VERSION_MINOR 3
#define FW_VERSION_PATCH 82
#define FW_VERSION_STRING "0.3.82"   // DebugLog 新增 UART0 RX 泵任务(GPIO20, 3M 8N1)：收到的串口行按 \n 切行、剥 \r，以 tag=UART 推入 ring → HTTP /logs 出现在网页日志面板；接线=任意串口源 TX→H1/H2 的 ESP_RX。0.3.81 GD32 OTA 包级防呆：中继触发前校验 gd32_ota 镜像向量表身份(Gd32FlashStore::validateStoredImageIdentity)，BOOT 包/非 APP 包在 WiFi /ota/relay 与 BLE 0xF7 均拒绝，GD32 APP 区不再被错误包擦除；BLE 新增 0x87 完成码

// Git 信息（手动填写）
#define GIT_HASH "770ae14"