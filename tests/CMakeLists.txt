# ============================================================
# ChaosEngine 单元测试
# 启用方式: cmake .. -DCHAOS_BUILD_TESTS=ON
# ============================================================

# ---- test_math ----
add_executable(test_math unit/test_math.c)
target_include_directories(test_math PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_math PRIVATE engine_core)
set_target_properties(test_math PROPERTIES LINKER_LANGUAGE C)
add_test(NAME math COMMAND test_math)

# ---- test_memory ----
add_executable(test_memory unit/test_memory.c)
target_include_directories(test_memory PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_memory PRIVATE engine_core)
set_target_properties(test_memory PROPERTIES LINKER_LANGUAGE C)
add_test(NAME memory COMMAND test_memory)

# ---- test_ecs ----
add_executable(test_ecs unit/test_ecs.c)
target_include_directories(test_ecs PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_ecs PRIVATE engine_core engine_sync)
set_target_properties(test_ecs PROPERTIES LINKER_LANGUAGE C)
add_test(NAME ecs COMMAND test_ecs)

# ---- test_network ----
add_executable(test_network unit/test_network.c)
target_include_directories(test_network PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_network PRIVATE engine_core)
set_target_properties(test_network PROPERTIES LINKER_LANGUAGE C)
add_test(NAME network COMMAND test_network)

# ---- test_aoi ----
add_executable(test_aoi unit/test_aoi.c)
target_include_directories(test_aoi PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_aoi PRIVATE engine_core engine_server)
set_target_properties(test_aoi PROPERTIES LINKER_LANGUAGE C)
add_test(NAME aoi COMMAND test_aoi)

# ---- test_cell ----
add_executable(test_cell unit/test_cell.c)
target_include_directories(test_cell PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_cell PRIVATE engine_core engine_server)
set_target_properties(test_cell PROPERTIES LINKER_LANGUAGE C)
add_test(NAME cell COMMAND test_cell)

# ---- test_ebpf (Phase 7.4: eBPF 单元测试) ----
add_executable(test_ebpf unit/test_ebpf.c)
target_include_directories(test_ebpf PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_ebpf PRIVATE engine_core)
set_target_properties(test_ebpf PROPERTIES LINKER_LANGUAGE C)
# Copy BPF object file to build output directory so ce_ebpf_init can find it
add_custom_command(TARGET test_ebpf POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/src_c/network/ce_ebpf_kern.o
        $<TARGET_FILE_DIR:test_ebpf>/ce_ebpf_kern.o
    COMMENT "Copying ce_ebpf_kern.o to test output dir"
)
add_test(NAME ebpf COMMAND test_ebpf WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ---- test_ebpf_errors (Phase 7.5: eBPF 错误处理测试) ----
add_executable(test_ebpf_errors unit/test_ebpf_errors.c)
target_include_directories(test_ebpf_errors PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_ebpf_errors PRIVATE engine_core)
set_target_properties(test_ebpf_errors PROPERTIES LINKER_LANGUAGE C)
add_custom_command(TARGET test_ebpf_errors POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/src_c/network/ce_ebpf_kern.o
        $<TARGET_FILE_DIR:test_ebpf_errors>/ce_ebpf_kern.o
)
add_test(NAME ebpf_errors COMMAND test_ebpf_errors WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ---- test_ebpf_integration (Phase 7.6: io_uring + eBPF 集成测试) ----
add_executable(test_ebpf_integration unit/test_ebpf_integration.c)
target_include_directories(test_ebpf_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_ebpf_integration PRIVATE engine_core)
# Need direct liburing link for raw io_uring in child process
if(LIBURING_FOUND)
    target_include_directories(test_ebpf_integration PRIVATE ${LIBURING_INCLUDE_DIRS})
    target_link_libraries(test_ebpf_integration PRIVATE ${LIBURING_LIBRARIES})
endif()
set_target_properties(test_ebpf_integration PROPERTIES LINKER_LANGUAGE C)
add_custom_command(TARGET test_ebpf_integration POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/src_c/network/ce_ebpf_kern.o
        $<TARGET_FILE_DIR:test_ebpf_integration>/ce_ebpf_kern.o
)
add_test(NAME ebpf_integration COMMAND test_ebpf_integration WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ---- test_ebpf_stress (Phase 7.7: 1000 并发 + eBPF 压力测试) ----
add_executable(test_ebpf_stress unit/test_ebpf_stress.c)
target_include_directories(test_ebpf_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_ebpf_stress PRIVATE engine_core)
# Need direct liburing link for raw io_uring in child process
if(LIBURING_FOUND)
    target_include_directories(test_ebpf_stress PRIVATE ${LIBURING_INCLUDE_DIRS})
    target_link_libraries(test_ebpf_stress PRIVATE ${LIBURING_LIBRARIES})
endif()
set_target_properties(test_ebpf_stress PROPERTIES LINKER_LANGUAGE C)
add_custom_command(TARGET test_ebpf_stress POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/src_c/network/ce_ebpf_kern.o
        $<TARGET_FILE_DIR:test_ebpf_stress>/ce_ebpf_kern.o
)
add_test(NAME ebpf_stress COMMAND test_ebpf_stress WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ============================================================
# Phase 7.1: io_uring 异步 I/O 基本功能测试
# ============================================================
add_executable(test_async_io unit/test_async_io.c)
target_include_directories(test_async_io PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_async_io PRIVATE engine_core)
set_target_properties(test_async_io PROPERTIES LINKER_LANGUAGE C)
add_test(NAME async_io COMMAND test_async_io WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ============================================================
# Phase 7.2: io_uring 异步 I/O 错误处理测试
# ============================================================
add_executable(test_async_io_errors unit/test_async_io_errors.c)
target_include_directories(test_async_io_errors PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_async_io_errors PRIVATE engine_core)
set_target_properties(test_async_io_errors PROPERTIES LINKER_LANGUAGE C)
add_test(NAME async_io_errors COMMAND test_async_io_errors WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ============================================================
# Phase 7.3: POSIX fallback (select 模式) 单元测试
# 编译时不定义 CHAOS_HAS_IO_URING，强制使用 ce_async_posix.c
# ============================================================
file(GLOB_RECURSE ENGINE_CORE_POSIX_SOURCES
    "${CMAKE_SOURCE_DIR}/src_c/core/*.c"
    "${CMAKE_SOURCE_DIR}/src_c/ecs/*.c"
    "${CMAKE_SOURCE_DIR}/src_c/log/*.c"
    "${CMAKE_SOURCE_DIR}/src_c/plugin/*.c"
    "${CMAKE_SOURCE_DIR}/src_c/render/*.c"
    "${CMAKE_SOURCE_DIR}/src_c/network/ce_network.c"
    "${CMAKE_SOURCE_DIR}/src_c/network/ce_async_posix.c"
    "${CMAKE_SOURCE_DIR}/src_c/network/ce_kcp.c"
    "${CMAKE_SOURCE_DIR}/src_c/network/ce_kcp_lua.c"
    "${CMAKE_SOURCE_DIR}/src_c/network/ikcp.c"
    "${CMAKE_SOURCE_DIR}/src_c/script/*.c"
    "${CMAKE_SOURCE_DIR}/src_c/runtime/ce_engine.c"
)
add_library(engine_core_posix_test STATIC ${ENGINE_CORE_POSIX_SOURCES})
set_source_files_properties(${ENGINE_CORE_POSIX_SOURCES} PROPERTIES LANGUAGE C)
target_include_directories(engine_core_posix_test
    PUBLIC  ${CMAKE_SOURCE_DIR}/src_c/public_api
    PRIVATE ${CMAKE_SOURCE_DIR}/src_c
            ${CMAKE_SOURCE_DIR}/src_c/core
            ${CMAKE_SOURCE_DIR}/src_c/ecs
            ${CMAKE_SOURCE_DIR}/src_c/log
            ${CMAKE_SOURCE_DIR}/src_c/plugin
            ${CMAKE_SOURCE_DIR}/src_c/render
            ${CMAKE_SOURCE_DIR}/src_c/network
            ${CMAKE_SOURCE_DIR}/src_c/script
            ${CMAKE_SOURCE_DIR}/src_c/runtime
            ${CMAKE_SOURCE_DIR}/third_party/vulkan/include
            ${CMAKE_SOURCE_DIR}/third_party/x11/include
            ${CMAKE_SOURCE_DIR}/third_party/lua/include
)
target_compile_options(engine_core_posix_test PRIVATE
    $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic -Werror=implicit-function-declaration>
    $<$<C_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(engine_core_posix_test PRIVATE m pthread dl
    ${CMAKE_SOURCE_DIR}/third_party/lua/lib/liblua5.4.so)
# eBPF support (same as engine_core)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBBPF libbpf)
    if(LIBBPF_FOUND)
        target_compile_definitions(engine_core_posix_test PRIVATE CHAOS_HAS_EBPF)
        target_include_directories(engine_core_posix_test PRIVATE ${LIBBPF_INCLUDE_DIRS})
        target_link_libraries(engine_core_posix_test PRIVATE ${LIBBPF_LIBRARIES})
    endif()
endif()
set_target_properties(engine_core_posix_test PROPERTIES LINKER_LANGUAGE C)

add_executable(test_async_io_posix unit/test_async_io_posix.c)
target_include_directories(test_async_io_posix PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_async_io_posix PRIVATE engine_core_posix_test)
set_target_properties(test_async_io_posix PROPERTIES LINKER_LANGUAGE C)
add_test(NAME async_io_posix COMMAND test_async_io_posix WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ============================================================
# Phase 5.1: Admin IPC 单元测试
# ============================================================
add_executable(test_admin_ipc unit/test_admin_ipc.c)
target_include_directories(test_admin_ipc PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
set_target_properties(test_admin_ipc PROPERTIES LINKER_LANGUAGE C)
add_test(NAME admin_ipc COMMAND test_admin_ipc)

# ============================================================
# Phase 5.2: Admin IPC 端到端集成测试
# ============================================================
add_executable(test_admin_e2e unit/test_admin_e2e.c)
target_include_directories(test_admin_e2e PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_admin_e2e PRIVATE pthread)
set_target_properties(test_admin_e2e PROPERTIES LINKER_LANGUAGE C)
add_test(NAME admin_e2e COMMAND test_admin_e2e)

# ============================================================
# Phase 5.3: Admin IPC 异常场景测试
# ============================================================
add_executable(test_admin_errors unit/test_admin_errors.c)
target_include_directories(test_admin_errors PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_admin_errors PRIVATE pthread)
set_target_properties(test_admin_errors PROPERTIES LINKER_LANGUAGE C)
add_test(NAME admin_errors COMMAND test_admin_errors)

# ============================================================
# Phase 5.4: Admin IPC 性能基准测试
# ============================================================
add_executable(test_admin_perf unit/test_admin_perf.c)
target_include_directories(test_admin_perf PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_admin_perf PRIVATE pthread)
set_target_properties(test_admin_perf PROPERTIES LINKER_LANGUAGE C)
add_test(NAME admin_perf COMMAND test_admin_perf)

# ============================================================
# Phase 1: ce_net_base 共享网络库单元测试
# ============================================================
add_executable(test_net_base unit/test_net_base.c)
target_include_directories(test_net_base PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_net_base PRIVATE engine_core)
set_target_properties(test_net_base PROPERTIES LINKER_LANGUAGE C)
add_test(NAME net_base COMMAND test_net_base WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ============================================================
# Replication Manager 单元测试
# ============================================================
add_executable(test_replication unit/test_replication.c)
target_include_directories(test_replication PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_replication PRIVATE engine_core)
set_target_properties(test_replication PROPERTIES LINKER_LANGUAGE C)
add_test(NAME replication COMMAND test_replication WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ---- test_rpc_channel ----
add_executable(test_rpc_channel unit/test_rpc_channel.c)
target_include_directories(test_rpc_channel PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_rpc_channel PRIVATE engine_core)
set_target_properties(test_rpc_channel PROPERTIES LINKER_LANGUAGE C)
add_test(NAME rpc_channel COMMAND test_rpc_channel WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ---- test_mailbox (Phase 3: Mailbox 单元测试) ----
add_executable(test_mailbox unit/test_mailbox.c)
target_include_directories(test_mailbox PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_mailbox PRIVATE engine_core)
set_target_properties(test_mailbox PROPERTIES LINKER_LANGUAGE C)
add_test(NAME mailbox COMMAND test_mailbox WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# ---- test_game_session ----
add_executable(test_game_session unit/test_game_session.c)
target_include_directories(test_game_session PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_game_session PRIVATE engine_core engine_server)
set_target_properties(test_game_session PROPERTIES LINKER_LANGUAGE C)
add_test(NAME game_session COMMAND test_game_session)

# ============================================================
# 性能基准测试 (Phase 2.5/2.6)
# ============================================================
add_subdirectory(bench)
