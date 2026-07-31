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
#define FW_VERSION_PATCH 74
#define FW_VERSION_STRING "0.3.74"   // can batch: 帧间隔 5→1ms；前端 BATCH_CHUNK=64 + fetch 超时加固 + finally 兜底 renderHistory

// Git 信息（手动填写）
#define GIT_HASH "770ae14"