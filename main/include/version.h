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
#define FW_VERSION_PATCH 76
#define FW_VERSION_STRING "0.3.76"   // 重启恢复提速2：httpd就绪后再打 MAIN「启动」banner（防环缩容裁掉）；前端用 uptime<120s 判重启完成，不再干等10s

// Git 信息（手动填写）
#define GIT_HASH "770ae14"