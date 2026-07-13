# ============================================================
# ChaosEngine C Gateway 模块 - io_uring 事件驱动网络网关
# 纯 C99，单线程 io_uring，Linux only
# ============================================================

# engine_gateway: 仅编译 ce_gateway.c (自包含，定义了所有内部逻辑)
# ce_gateway_conn.c / ce_gateway_backend.c / ce_gateway_protocol.c
# 是早期拆分版本，其结构体定义与 ce_gateway.h 中的公开定义冲突，
# 暂不编译（死代码）。待后续统一接口后再启用。
add_library(engine_gateway STATIC
    ce_gateway.c
)

target_include_directories(engine_gateway
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src_c
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(engine_gateway PRIVATE engine_core)

# 传递 io_uring 宏和链接库给 engine_gateway
if(CHAOS_USE_IO_URING AND LIBURING_FOUND)
    target_compile_definitions(engine_gateway PRIVATE CHAOS_HAS_IO_URING)
    target_include_directories(engine_gateway PRIVATE ${LIBURING_INCLUDE_DIRS})
    target_link_libraries(engine_gateway PRIVATE ${LIBURING_LIBRARIES})
endif()

target_compile_options(engine_gateway PRIVATE
    $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -std=c99>
    $<$<C_COMPILER_ID:Clang>:-Wall -Wextra -std=c99>
)

set_target_properties(engine_gateway PROPERTIES LINKER_LANGUAGE C)

# ============================================================
# chaos_gateway_c 可执行文件（C 版入口，使用 engine_gateway 库）
# ============================================================
add_executable(chaos_gateway_c
    ${CMAKE_SOURCE_DIR}/src_c/runtime/ce_gateway_c_main.c
)

target_include_directories(chaos_gateway_c PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c
)

target_link_libraries(chaos_gateway_c PRIVATE engine_gateway engine_core)

set_target_properties(chaos_gateway_c PROPERTIES LINKER_LANGUAGE C)
