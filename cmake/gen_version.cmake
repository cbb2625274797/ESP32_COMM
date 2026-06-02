# gen_version.cmake - 自动生成版本信息头文件
# 包含版本号、编译时间、Git 信息

# 版本号（可手动修改）
set(FW_VERSION_MAJOR 0)
set(FW_VERSION_MINOR 3)
set(FW_VERSION_PATCH 10)

# 获取编译时间
string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S")

# 获取 Git 信息（如果可用）
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
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

# 检查文件是否需要更新
if(EXISTS ${VERSION_HEADER_PATH})
    file(READ ${VERSION_HEADER_PATH} EXISTING_CONTENT)
    if(NOT "${EXISTING_CONTENT}" STREQUAL "${VERSION_HEADER_CONTENT}")
        file(WRITE ${VERSION_HEADER_PATH} "${VERSION_HEADER_CONTENT}")
        message(STATUS "Version header updated: ${VERSION_HEADER_PATH}")
    endif()
else()
    file(WRITE ${VERSION_HEADER_PATH} "${VERSION_HEADER_CONTENT}")
    message(STATUS "Version header generated: ${VERSION_HEADER_PATH}")
endif()
