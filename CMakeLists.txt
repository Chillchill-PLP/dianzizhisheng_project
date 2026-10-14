# 工程构建入口（ESP-IDF 5.5 / ESP32-S3）。
# cmake_minimum_required 指定构建所需 CMake 最低版本。
# project.cmake 从环境变量 IDF_PATH 引入 IDF 的组件、配置和固件生成规则。
# COMPONENTS main 将入口组件限定为 main，其 PRIV_REQUIRES 会递归引入所需依赖。
# project 名决定 ELF/BIN 等输出文件名称，辅助检查脚本与该名称对应。
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(dianzizhisheng_project)
