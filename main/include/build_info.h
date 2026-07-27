/*
 * build_info.h - 真实编译时间（构建系统每次自动生成，替代 version.h 手工 BUILD_TIME）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 返回本固件的真实编译时间字符串 "YYYY-MM-DD HH:MM:SS"（每次构建自动刷新）。 */
const char *fwBuildTime(void);

#ifdef __cplusplus
}
#endif
