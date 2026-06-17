# gen_version_script.cmake - 构建时执行的独立脚本
# 通过 add_custom_target 在每次 build 时调用，确保版本信息始终最新
#
# 需要传入的参数:
#   SOURCE_DIR   - 项目根目录 (ESP32_COMM)
#   OUTPUT_FILE  - 输出的 version.h 路径

# 主/次版本号（手动修改，大版本时改）
set(FW_VERSION_MAJOR 0)
set(FW_VERSION_MINOR 3)

# PATCH 自动递增：git 总提交次数，每次新提交 +1
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
        WORKING_DIRECTORY ${SOURCE_DIR}
        OUTPUT_VARIABLE FW_VERSION_PATCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
else()
    set(FW_VERSION_PATCH 0)
endif()

# 编译时间
string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S")

# Git 短 hash
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${SOURCE_DIR}
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
else()
    set(GIT_HASH "unknown")
endif()

# 生成版本头文件
set(VERSION_HEADER_CONTENT
"// Auto-generated version info - DO NOT EDIT
#pragma once

// 固件版本
#define FW_VERSION_MAJOR ${FW_VERSION_MAJOR}
#define FW_VERSION_MINOR ${FW_VERSION_MINOR}
#define FW_VERSION_PATCH ${FW_VERSION_PATCH}
#define FW_VERSION_STRING \"${FW_VERSION_MAJOR}.${FW_VERSION_MINOR}.${FW_VERSION_PATCH}\"

// 编译时间
#define BUILD_TIME \"${BUILD_TIME}\"

// Git 信息
#define GIT_HASH \"${GIT_HASH}\"
")

# 仅在内容变化时写入，避免不必要的重编译
if(EXISTS ${OUTPUT_FILE})
    file(READ ${OUTPUT_FILE} EXISTING_CONTENT)
    if(NOT "${EXISTING_CONTENT}" STREQUAL "${VERSION_HEADER_CONTENT}")
        file(WRITE ${OUTPUT_FILE} "${VERSION_HEADER_CONTENT}")
        message(STATUS "Version header updated: v${FW_VERSION_MAJOR}.${FW_VERSION_MINOR}.${FW_VERSION_PATCH} (${GIT_HASH})")
    endif()
else()
    file(WRITE ${OUTPUT_FILE} "${VERSION_HEADER_CONTENT}")
    message(STATUS "Version header generated: v${FW_VERSION_MAJOR}.${FW_VERSION_MINOR}.${FW_VERSION_PATCH} (${GIT_HASH})")
endif()
