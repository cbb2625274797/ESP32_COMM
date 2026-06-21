# ESP32-C3 OTA 中继项目 - 根构建文件
# 要求 ESP-IDF v6.0+，使用 idf.py build 编译
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP32_COMM)  # 项目名：ESP32 通信模块
