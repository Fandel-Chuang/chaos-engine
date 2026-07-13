cmake_minimum_required(VERSION 3.20)
project(ChaosEngine VERSION 0.1.0 LANGUAGES C CXX)

# ============================================================
# ChaosEngine - 分层隔离游戏引擎
# 架构：纯C内核 + C++编辑器 + Lua适配层
# ============================================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 全局编译选项
option(CHAOS_BUILD_EDITOR   "Build the C++ editor application" ON)
option(CHAOS_BUILD_TESTS    "Build unit tests"                 OFF)
option(CHAOS_BUILD_SAMPLES  "Build sample applications"        OFF)
option(CHAOS_USE_DOUBLE_PROCESS "Use dual-process IPC architecture" OFF)

# 输出目录统一
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ============================================================
# FlatBuffers Schema 编译 (先于 src_c，engine_core 依赖 chaos_fbs)
# ============================================================
add_subdirectory(schemas)

# ============================================================
# 引擎内核 (纯 C99/C11) — 独立编译目标
# ============================================================
add_subdirectory(src_c)

# ============================================================
# 编辑器 (C++17) — 仅链接 engine_core，通过 extern "C" 调用
# ============================================================
if(CHAOS_BUILD_EDITOR)
    add_subdirectory(src_cpp)
endif()

# ============================================================
# Lua 适配层
# ============================================================
# add_subdirectory(src_lua)  # 后续启用

# ============================================================
# 测试
# ============================================================
if(CHAOS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
