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

# Vulkan 头文件路径
set(VULKAN_HEADERS_DIR "${CMAKE_SOURCE_DIR}/third_party/vulkan/include")
set(X11_HEADERS_DIR "${CMAKE_SOURCE_DIR}/third_party/x11/include")

# Lua 5.4 路径
set(LUA_HEADERS_DIR "${CMAKE_SOURCE_DIR}/third_party/lua/include")
set(LUA_LIB_DIR "${CMAKE_SOURCE_DIR}/third_party/lua/lib")

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
        ${VULKAN_HEADERS_DIR}
        ${X11_HEADERS_DIR}
        ${LUA_HEADERS_DIR}
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
    target_link_libraries(engine_core PRIVATE m pthread dl ${LUA_LIB_DIR}/liblua5.4.so)

    # io_uring 检测
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LIBURING liburing)
        if(LIBURING_FOUND)
            target_compile_definitions(engine_core PRIVATE CHAOS_HAS_IO_URING)
            target_include_directories(engine_core PRIVATE ${LIBURING_INCLUDE_DIRS})
            target_link_libraries(engine_core PRIVATE ${LIBURING_LIBRARIES})
            message(STATUS "io_uring: enabled (liburing ${LIBURING_VERSION})")
        else()
            message(STATUS "io_uring: disabled (liburing not found)")
        endif()
    endif()
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

# ============================================================
# Vulkan 客户端可执行文件
# ============================================================
add_executable(chaos_client
    runtime/ce_client_main.c
)
target_include_directories(chaos_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VULKAN_HEADERS_DIR}
    ${X11_HEADERS_DIR}
)
target_link_libraries(chaos_client PRIVATE engine_core)

# 链接 Vulkan 和 X11
if(UNIX AND NOT APPLE)
    target_link_libraries(chaos_client PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/vulkan/lib/libvulkan.so.1
        ${CMAKE_SOURCE_DIR}/third_party/x11/lib/libX11.so
    )
elseif(WIN32)
    target_link_libraries(chaos_client PRIVATE vulkan-1)
else()
    target_link_libraries(chaos_client PRIVATE vulkan)
endif()
set_target_properties(chaos_client PROPERTIES LINKER_LANGUAGE C)

# ============================================================
# 网络客户端测试
# ============================================================
add_executable(chaos_net_client
    runtime/ce_net_client_main.c
)
target_include_directories(chaos_net_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(chaos_net_client PRIVATE engine_core)
set_target_properties(chaos_net_client PROPERTIES LINKER_LANGUAGE C)

# ============================================================
# io_uring 异步 Echo 服务器
# ============================================================
add_executable(chaos_async_echo
    runtime/ce_async_echo_main.c
)
target_include_directories(chaos_async_echo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(chaos_async_echo PRIVATE engine_core)
set_target_properties(chaos_async_echo PROPERTIES LINKER_LANGUAGE C)

# ============================================================
# 服务端模块 (AOI + Cell)
# ============================================================
add_subdirectory(server)

# ============================================================
# TCP 回显服务器可执行文件
# ============================================================
add_executable(chaos_server
    runtime/ce_server_main.c
)
target_include_directories(chaos_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(chaos_server PRIVATE engine_core engine_server)
set_target_properties(chaos_server PROPERTIES LINKER_LANGUAGE C)
