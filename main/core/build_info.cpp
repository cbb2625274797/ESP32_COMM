/*
 * build_info.cpp - 唯一包含自动生成时间头的编译单元。
 * build_time_gen.h 每次构建都被 gen_build_time 目标重写 → 仅本文件重编 + 重链接，
 * 其余增量编译不受影响；其他模块经 fwBuildTime() 取值。
 */
#include "build_info.h"
#include "build_time_gen.h"

const char *fwBuildTime(void)
{
    return FW_BUILD_TIME_AUTO;
}
