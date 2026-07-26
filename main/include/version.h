/*
 * version.h - 固件版本信息（手动维护）
 *
 * 注意：本文件不再由构建脚本自动生成。如需修改版本号、编译时间或
 * Git Hash，请直接编辑本文件。
 *   - FW_VERSION_MAJOR / MINOR / PATCH：发版时手动递增
 *   - BUILD_TIME：每次发版编译时手动填写
 *   - GIT_HASH：手动填写对应提交的 git short hash
 */
#pragma once

// 固件版本
#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 3
#define FW_VERSION_PATCH 71
#define FW_VERSION_STRING "0.3.71"

// 编译时间（手动填写）
#define BUILD_TIME "2026-07-26 20:25:00"

// Git 信息（手动填写）
#define GIT_HASH "770ae14"