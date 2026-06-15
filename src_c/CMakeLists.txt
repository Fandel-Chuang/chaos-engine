# ============================================================
# ChaosEngine 内核 — 纯 C99/C11，产出静态库 libengine_core.a
# 规则：禁止 C++ 语法、禁止 .cpp/.hpp 文件
# ============================================================

# 收集 src_c 下所有 .c 源文件
file(GLOB_RECURSE ENGINE_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ecs/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/log/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/render/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/network/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/script/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ce_engine.c"
)

# 收集 public_api 头文件（仅用于 IDE 索引，不参与编译）
file(GLOB_RECURSE ENGINE_PUBLIC_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/public_api/*.h"
)

# 静态库目标
add_library(engine_core STATIC ${ENGINE_CORE_SOURCES})

# 强制 C 编译模式
set_source_files_properties(${ENGINE_CORE_SOURCES} PROPERTIES LANGUAGE C)

target_include_directories(engine_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/public_api
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/ecs
        ${CMAKE_CURRENT_SOURCE_DIR}/log
        ${CMAKE_CURRENT_SOURCE_DIR}/plugin
        ${CMAKE_CURRENT_SOURCE_DIR}/render
        ${CMAKE_CURRENT_SOURCE_DIR}/network
        ${CMAKE_CURRENT_SOURCE_DIR}/script
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime
)

target_compile_options(engine_core PRIVATE
    $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic -Werror=implicit-function-declaration>
    $<$<C_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

# 链接系统库（平台相关）
if(WIN32)
    target_link_libraries(engine_core PRIVATE winmm)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(engine_core PRIVATE m pthread dl)
elseif(APPLE)
    target_link_libraries(engine_core PRIVATE
        "-framework Foundation"
        "-framework Cocoa"
        "-framework IOKit"
    )
endif()

# 禁止链接 C++ 标准库
set_target_properties(engine_core PROPERTIES
    LINKER_LANGUAGE C
)

# ============================================================
# 无头模式可执行文件（服务器 / 测试 / CI）
# ============================================================
add_executable(chaos_headless
    runtime/ce_headless_main.c
)
target_include_directories(chaos_headless PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(chaos_headless PRIVATE engine_core)
set_target_properties(chaos_headless PROPERTIES LINKER_LANGUAGE C)
