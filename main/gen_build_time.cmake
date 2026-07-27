# 每次构建执行：生成真实编译时间头文件（由 main/CMakeLists.txt 的 gen_build_time 目标调用）。
# 手工维护的 version.h BUILD_TIME 曾停留在旧值，导致「刷了新固件仍显示旧时间」被误判为回退。
string(TIMESTAMP _build_time "%Y-%m-%d %H:%M:%S")
file(WRITE "${OUT}"
"/* 自动生成（每次构建刷新），勿手改、勿提交。仅 core/build_info.cpp 包含本文件。 */
#pragma once
#define FW_BUILD_TIME_AUTO \"${_build_time}\"
")
