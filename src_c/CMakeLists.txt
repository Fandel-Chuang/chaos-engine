# ============================================================
# ChaosEngine 内核 — 纯 C99/C11，产出静态库 libengine_core.a
# 规则：禁止 C++ 语法、禁止 .cpp/.hpp 文件
# ============================================================

# 收集 src_c 下所有 .c 源文件 (排除 BPF 内核程序 *_kern.c 和 *.bpf.c)
file(GLOB_RECURSE ENGINE_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ecs/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/log/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/render/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/network/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ebpf/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/script/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/replication/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ce_engine.c"
)

# 排除 BPF 内核程序 (这些需要 clang -target bpf 编译，不能通过标准 C 编译器)
list(FILTER ENGINE_CORE_SOURCES EXCLUDE REGEX ".*_kern\\.c$")
list(FILTER ENGINE_CORE_SOURCES EXCLUDE REGEX ".*\\.bpf\\.c$")

# 收集 public_api 头文件（仅用于 IDE 索引，不参与编译）
file(GLOB_RECURSE ENGINE_PUBLIC_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/public_api/*.h"
)

# 辅助函数：检查路径是否真实可用（排除断链符号链接）
function(ce_check_path var path)
    if(EXISTS "${path}")
        set(${var} TRUE PARENT_SCOPE)
    else()
        set(${var} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Vulkan / X11 头文件路径：优先 third_party，回退系统包
set(VULKAN_HEADERS_DIR "${CMAKE_SOURCE_DIR}/third_party/vulkan/include")
set(X11_HEADERS_DIR "${CMAKE_SOURCE_DIR}/third_party/x11/include")
ce_check_path(VK_HEADERS_OK "${VULKAN_HEADERS_DIR}")
if(NOT VK_HEADERS_OK)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(VULKAN_HEADERS_DIR "")
    endif()
endif()
ce_check_path(X11_HEADERS_OK "${X11_HEADERS_DIR}")
if(NOT X11_HEADERS_OK)
    find_package(X11 QUIET)
    if(X11_FOUND)
        set(X11_HEADERS_DIR "")
    endif()
endif()

# Lua 5.4 路径：优先 third_party，回退系统包
set(LUA_HEADERS_DIR "${CMAKE_SOURCE_DIR}/third_party/lua/include")
set(LUA_LIB_DIR "${CMAKE_SOURCE_DIR}/third_party/lua/lib")
set(LUA_LIB_FILE "${LUA_LIB_DIR}/liblua5.4.so")
ce_check_path(LUA_HEADERS_OK "${LUA_HEADERS_DIR}")
ce_check_path(LUA_LIB_OK "${LUA_LIB_FILE}")
if(NOT LUA_HEADERS_OK OR NOT LUA_LIB_OK)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LUA lua5.4)
    endif()
    if(LUA_FOUND)
        set(LUA_HEADERS_DIR "${LUA_INCLUDE_DIRS}")
        set(LUA_LIB_FILE "${LUA_LIBRARIES}")
    else()
        # 最后回退到系统默认路径
        find_path(LUA_SYS_HEADER lua.h PATHS /usr/include/lua5.4)
        if(LUA_SYS_HEADER)
            set(LUA_HEADERS_DIR "${LUA_SYS_HEADER}")
            set(LUA_LIB_FILE "lua5.4")
        endif()
    endif()
endif()
message(STATUS "Lua: headers=${LUA_HEADERS_DIR} lib=${LUA_LIB_FILE}")

# Windows POSIX compat header — define early so all targets can reference it
set(CE_WIN_COMPAT_H "${CMAKE_CURRENT_SOURCE_DIR}/core/ce_win_compat.h")

# Add compat_win stub headers to ALL targets on Windows
# (provides stub unistd.h, libgen.h, sys/socket.h etc.)
if(WIN32)
    include_directories("${CMAKE_CURRENT_SOURCE_DIR}/compat_win")
endif()

# Find Lua library
if(WIN32)
    find_library(LUA_LIBRARY NAMES lua54 lua5.4 lua
        HINTS
            "${LUA_LIB_DIR}"
            "$ENV{LUA_DIR}/lib"
            "C:/Lua/lib"
            "C:/Program Files/Lua/lib"
        PATH_SUFFIXES lib
    )
    if(NOT LUA_LIBRARY)
        find_package(Lua 5.4 QUIET)
        if(Lua_FOUND)
            set(LUA_LIBRARY ${LUA_LIBRARIES})
            set(LUA_HEADERS_DIR ${LUA_INCLUDE_DIR})
        else()
            message(WARNING "Lua 5.4 library not found. Run install_deps.bat first.")
            set(LUA_LIBRARY "")
        endif()
    endif()
else()
    set(LUA_LIBRARY "${LUA_LIB_DIR}/liblua5.4.so")
endif()
message(STATUS "Lua library: ${LUA_LIBRARY}")

# Windows: 排除 Linux 专属文件，加入 Windows 平台实现
if(WIN32)
    list(FILTER ENGINE_CORE_SOURCES EXCLUDE REGEX ".*/core/ce_platform\\.c$")
    list(FILTER ENGINE_CORE_SOURCES EXCLUDE REGEX ".*/core/ce_time\\.c$")
    list(FILTER ENGINE_CORE_SOURCES EXCLUDE REGEX ".*/network/ce_async_posix\\.c$")
    list(FILTER ENGINE_CORE_SOURCES EXCLUDE REGEX ".*/network/ce_async_uring\\.c$")
    list(FILTER ENGINE_CORE_SOURCES EXCLUDE REGEX ".*/network/ce_ebpf\\.c$")
    list(FILTER ENGINE_CORE_SOURCES EXCLUDE REGEX ".*/network/ce_network\\.c$")
    list(FILTER ENGINE_CORE_SOURCES EXCLUDE REGEX ".*/network/ce_rpc_call\\.c$")
    list(FILTER ENGINE_CORE_SOURCES EXCLUDE REGEX ".*/ebpf/.*\\.c$")
    list(APPEND ENGINE_CORE_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/core/ce_platform_win.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/ce_time_win.c"
    )
endif()

# 静态库目标
add_library(engine_core STATIC ${ENGINE_CORE_SOURCES})

# 强制 C 编译模式
set_source_files_properties(${ENGINE_CORE_SOURCES} PROPERTIES LANGUAGE C)

target_include_directories(engine_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/public_api
        ${FLATBUFFERS_INCLUDE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/ecs
        ${CMAKE_CURRENT_SOURCE_DIR}/log
        ${CMAKE_CURRENT_SOURCE_DIR}/plugin
        ${CMAKE_CURRENT_SOURCE_DIR}/render
        ${CMAKE_CURRENT_SOURCE_DIR}/network
        ${CMAKE_CURRENT_SOURCE_DIR}/script
        ${CMAKE_CURRENT_SOURCE_DIR}/ebpf
        ${CMAKE_CURRENT_SOURCE_DIR}/replication
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime
        ${VULKAN_HEADERS_DIR}
        ${X11_HEADERS_DIR}
        ${LUA_HEADERS_DIR}
)

target_compile_options(engine_core PRIVATE
    $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic -Werror=implicit-function-declaration -Wno-gnu-zero-variadic-macro-arguments>
    $<$<C_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic -Wno-gnu-zero-variadic-macro-arguments>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

# MSVC-specific flags (must use if(MSVC) — generator expressions don't propagate /FI reliably)
if(MSVC)
    target_compile_options(engine_core PRIVATE
        /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS
    )
    target_compile_options(engine_core PRIVATE
        "/FI${CE_WIN_COMPAT_H}"
    )
endif()

# 链接系统库（平台相关）
if(WIN32)
    target_link_libraries(engine_core PRIVATE winmm ws2_32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(engine_core PRIVATE m pthread dl ${LUA_LIB_FILE})

    if(LINUX)
        # eBPF: Linux 5.x+ 自动启用
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(LIBBPF libbpf)
            if(LIBBPF_FOUND)
                target_compile_definitions(engine_core PRIVATE CHAOS_HAS_EBPF)
                target_include_directories(engine_core PRIVATE ${LIBBPF_INCLUDE_DIRS})
                target_link_libraries(engine_core PRIVATE ${LIBBPF_LIBRARIES})
                message(STATUS "eBPF: enabled (libbpf ${LIBBPF_VERSION})")

                # ---- BPF 程序编译 (clang -target bpf) ----
                set(BPF_C_FLAGS -O2 -target bpf -I${CMAKE_CURRENT_SOURCE_DIR}/ebpf
                    -I/usr/include/x86_64-linux-gnu)

                add_custom_command(
                    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ce_xdp_filter.bpf.o
                    COMMAND clang ${BPF_C_FLAGS}
                        -c ${CMAKE_CURRENT_SOURCE_DIR}/ebpf/ce_xdp_filter.bpf.c
                        -o ${CMAKE_CURRENT_BINARY_DIR}/ce_xdp_filter.bpf.o
                    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ebpf/ce_xdp_filter.bpf.c
                            ${CMAKE_CURRENT_SOURCE_DIR}/ebpf/ce_xdp_common.h
                    COMMENT "Compiling XDP filter BPF program"
                )

                add_custom_command(
                    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ce_xdp_dispatch.bpf.o
                    COMMAND clang ${BPF_C_FLAGS}
                        -c ${CMAKE_CURRENT_SOURCE_DIR}/ebpf/ce_xdp_dispatch.bpf.c
                        -o ${CMAKE_CURRENT_BINARY_DIR}/ce_xdp_dispatch.bpf.o
                    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ebpf/ce_xdp_dispatch.bpf.c
                            ${CMAKE_CURRENT_SOURCE_DIR}/ebpf/ce_xdp_common.h
                    COMMENT "Compiling XDP dispatch BPF program"
                )

                add_custom_command(
                    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ce_ebpf_kern.bpf.o
                    COMMAND clang ${BPF_C_FLAGS}
                        -c ${CMAKE_CURRENT_SOURCE_DIR}/network/ce_ebpf_kern.c
                        -o ${CMAKE_CURRENT_BINARY_DIR}/ce_ebpf_kern.bpf.o
                    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/network/ce_ebpf_kern.c
                    COMMENT "Compiling eBPF observability program"
                )

                add_custom_target(bpf_programs ALL
                    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ce_xdp_filter.bpf.o
                            ${CMAKE_CURRENT_BINARY_DIR}/ce_xdp_dispatch.bpf.o
                            ${CMAKE_CURRENT_BINARY_DIR}/ce_ebpf_kern.bpf.o
                )
                add_dependencies(engine_core bpf_programs)
            else()
                message(STATUS "eBPF: disabled (libbpf not found)")
            endif()
        endif()

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

# 确保 FlatBuffers schema 在 engine_core 之前编译
add_dependencies(engine_core chaos_fbs)

# Windows: CE_WIN_FI — force-include flag for exe targets
if(WIN32 AND MSVC)
    set(CE_WIN_FI "/FI${CE_WIN_COMPAT_H}")
else()
    set(CE_WIN_FI "")
endif()

# ============================================================
# 无头模式可执行文件（服务器 / 测试 / CI）
# ============================================================
add_executable(chaos_headless
    runtime/ce_headless_main.c
)
target_include_directories(chaos_headless PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_options(chaos_headless PRIVATE ${CE_WIN_FI})
target_link_libraries(chaos_headless PRIVATE engine_core)
set_target_properties(chaos_headless PROPERTIES LINKER_LANGUAGE C)

# ============================================================
# Vulkan 客户端可执行文件 (skipped on Windows if Vulkan SDK absent)
# ============================================================
if(WIN32)
    find_library(VULKAN_LIB NAMES vulkan-1
        HINTS "$ENV{VULKAN_SDK}/Lib" "$ENV{VULKAN_SDK}/lib" "C:/VulkanSDK/Lib"
    )
    if(NOT VULKAN_LIB AND DEFINED ENV{VULKAN_SDK})
        set(VULKAN_LIB "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib")
    endif()
else()
    set(VULKAN_LIB "")
endif()

if(WIN32 AND NOT VULKAN_LIB)
    message(STATUS "Vulkan SDK not found - skipping chaos_client/chaos_editor")
else()
add_executable(chaos_client
    runtime/ce_client_main.c
    runtime/ce_client_network.c
)
target_include_directories(chaos_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VULKAN_HEADERS_DIR}
    ${X11_HEADERS_DIR}
)
target_compile_options(chaos_client PRIVATE ${CE_WIN_FI})
target_link_libraries(chaos_client PRIVATE engine_core)

# 链接 Vulkan 和 X11：优先 third_party，回退系统包
if(UNIX AND NOT APPLE)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/vulkan/lib/libvulkan.so.1")
        target_link_libraries(chaos_client PRIVATE
            ${CMAKE_SOURCE_DIR}/third_party/vulkan/lib/libvulkan.so.1
            ${CMAKE_SOURCE_DIR}/third_party/x11/lib/libX11.so
        )
    else()
        find_package(Vulkan QUIET)
        find_package(X11 QUIET)
        if(Vulkan_FOUND AND X11_FOUND)
            target_link_libraries(chaos_client PRIVATE Vulkan::Vulkan X11::X11)
        else()
            message(WARNING "Vulkan/X11 not found - chaos_client will not be built")
            set_target_properties(chaos_client PROPERTIES EXCLUDE_FROM_ALL TRUE)
        endif()
    endif()
elseif(WIN32)
    target_link_libraries(chaos_client PRIVATE "${VULKAN_LIB}")
else()
    target_link_libraries(chaos_client PRIVATE vulkan)
endif()
set_target_properties(chaos_client PROPERTIES LINKER_LANGUAGE C)
endif() # Vulkan found

# ============================================================
# 网络客户端测试 (Linux/macOS only — uses POSIX sockets)
# ============================================================
if(NOT WIN32)
add_executable(chaos_net_client
    runtime/ce_net_client_main.c
)
target_include_directories(chaos_net_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(chaos_net_client PRIVATE engine_core)
set_target_properties(chaos_net_client PROPERTIES LINKER_LANGUAGE C)
endif()

# ============================================================
# io_uring 异步 Echo 服务器 (Linux only)
# ============================================================
if(NOT WIN32)
add_executable(chaos_async_echo
    runtime/ce_async_echo_main.c
)
target_include_directories(chaos_async_echo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(chaos_async_echo PRIVATE engine_core)
set_target_properties(chaos_async_echo PROPERTIES LINKER_LANGUAGE C)
endif()

# ============================================================
# io_uring 异步 I/O 性能基准测试服务器 (Linux only)
# ============================================================
if(NOT WIN32)
# 支持 2048 并发连接，可配置端口，静默模式
add_executable(chaos_async_bench
    runtime/ce_async_bench_server.c
)
target_include_directories(chaos_async_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(chaos_async_bench PRIVATE engine_core)
set_target_properties(chaos_async_bench PROPERTIES LINKER_LANGUAGE C)
endif()

# POSIX-only 版本（强制禁用 io_uring，用于对比基准测试）
# 用法: cmake -DBUILD_POSIX_BENCH=ON ..
option(BUILD_POSIX_BENCH "Build POSIX-only benchmark server (no io_uring)" OFF)
if(BUILD_POSIX_BENCH AND NOT WIN32)
    # 收集除 ce_async_uring.c 之外的所有源文件
    file(GLOB_RECURSE ENGINE_CORE_POSIX_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/core/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/ecs/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/log/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/render/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/network/ce_network.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/network/ce_async_posix.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/network/ce_ebpf.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/script/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ce_engine.c"
    )
    add_library(engine_core_posix STATIC ${ENGINE_CORE_POSIX_SOURCES})
    set_source_files_properties(${ENGINE_CORE_POSIX_SOURCES} PROPERTIES LANGUAGE C)
    target_include_directories(engine_core_posix
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/public_api
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
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
    target_compile_options(engine_core_posix PRIVATE
        $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic -Werror=implicit-function-declaration>
        $<$<C_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
    )
    target_link_libraries(engine_core_posix PRIVATE m pthread dl ${LUA_LIB_FILE})
    # eBPF support (same as engine_core)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LIBBPF libbpf)
        if(LIBBPF_FOUND)
            target_compile_definitions(engine_core_posix PRIVATE CHAOS_HAS_EBPF)
            target_include_directories(engine_core_posix PRIVATE ${LIBBPF_INCLUDE_DIRS})
            target_link_libraries(engine_core_posix PRIVATE ${LIBBPF_LIBRARIES})
        endif()
    endif()
    set_target_properties(engine_core_posix PROPERTIES LINKER_LANGUAGE C)

    add_executable(chaos_async_bench_posix
        runtime/ce_async_bench_server.c
    )
    target_include_directories(chaos_async_bench_posix PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(chaos_async_bench_posix PRIVATE engine_core_posix)
    set_target_properties(chaos_async_bench_posix PROPERTIES LINKER_LANGUAGE C)
endif()

# ============================================================
# 服务端模块 (AOI + Cell)
# ============================================================
add_subdirectory(server)

# ============================================================
# 同步模块 (Game↔DBProxy)
# ============================================================
add_subdirectory(sync)

# ============================================================
# DBProxy 客户端模块
# ============================================================
add_subdirectory(dbproxy)

# ============================================================
# TCP 回显服务器可执行文件 (Linux only — uses io_uring/epoll)
# ============================================================
if(NOT WIN32)
add_executable(chaos_server
    runtime/ce_server_main.c
)
target_include_directories(chaos_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(chaos_server PRIVATE engine_core engine_server engine_admin_ipc engine_sync engine_dbproxy engine_save)
set_target_properties(chaos_server PROPERTIES LINKER_LANGUAGE C)

# 传递 io_uring 宏给 chaos_server（ce_server_main.c 需要）
if(CHAOS_USE_IO_URING AND LIBURING_FOUND)
    target_compile_definitions(chaos_server PRIVATE CHAOS_HAS_IO_URING)
    target_include_directories(chaos_server PRIVATE ${LIBURING_INCLUDE_DIRS})
    target_link_libraries(chaos_server PRIVATE ${LIBURING_LIBRARIES})
endif()
endif() # NOT WIN32

# ============================================================
# Router 进程可执行文件 (Lua 驱动)
# ============================================================
add_executable(chaos_router
    runtime/ce_router_main.c
)
target_include_directories(chaos_router PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LUA_HEADERS_DIR}
)
target_compile_options(chaos_router PRIVATE ${CE_WIN_FI})
target_link_libraries(chaos_router PRIVATE engine_core ${LUA_LIBRARY})
set_target_properties(chaos_router PROPERTIES LINKER_LANGUAGE C)

# ============================================================
# Gateway 进程可执行文件 (io_uring 事件驱动)
# ============================================================
add_executable(chaos_gateway
    runtime/ce_gateway_main.c
    gateway/ce_gateway.c
)
target_include_directories(chaos_gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/gateway
    ${LUA_HEADERS_DIR}
)
target_compile_options(chaos_gateway PRIVATE ${CE_WIN_FI})
target_link_libraries(chaos_gateway PRIVATE engine_core ${LUA_LIBRARY})
set_target_properties(chaos_gateway PROPERTIES LINKER_LANGUAGE C)

# 传递 io_uring 宏给 chaos_gateway (ce_gateway.c 需要)
if(CHAOS_USE_IO_URING AND LIBURING_FOUND)
    target_compile_definitions(chaos_gateway PRIVATE CHAOS_HAS_IO_URING)
    target_include_directories(chaos_gateway PRIVATE ${LIBURING_INCLUDE_DIRS})
    target_link_libraries(chaos_gateway PRIVATE ${LIBURING_LIBRARIES})
endif()

# ============================================================
# DBProxy 进程可执行文件 (Lua 驱动)
# ============================================================
add_executable(chaos_dbproxy
    runtime/ce_dbproxy_main.c
)
target_include_directories(chaos_dbproxy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LUA_HEADERS_DIR}
)
target_compile_options(chaos_dbproxy PRIVATE ${CE_WIN_FI})
target_link_libraries(chaos_dbproxy PRIVATE engine_core ${LUA_LIBRARY})
set_target_properties(chaos_dbproxy PROPERTIES LINKER_LANGUAGE C)

# ============================================================
# Admin IPC 模块
# ============================================================
add_subdirectory(admin_ipc)

# ============================================================
# C Gateway 模块 (TCP epoll 接入 + 消息路由 + 后端转发)
# ============================================================
add_subdirectory(gateway)

# ============================================================
# 存档模块
# ============================================================
add_subdirectory(save)
