# ============================================================
# ChaosEngine 微服务 RPC 模块
# 纯 C99，产出静态库 libengine_rpc.a
# ============================================================

file(GLOB RPC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
add_library(engine_rpc STATIC ${RPC_SOURCES})

target_include_directories(engine_rpc PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c
    ${CMAKE_SOURCE_DIR}/src_c/public_api
)

target_link_libraries(engine_rpc PRIVATE engine_core pthread)

target_compile_options(engine_rpc PRIVATE
    $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -std=c99>
    $<$<C_COMPILER_ID:Clang>:-Wall -Wextra -std=c99>
)

set_target_properties(engine_rpc PROPERTIES LINKER_LANGUAGE C)

# ---- 示例程序 ----
add_executable(echo_service example/echo_service.c)
target_link_libraries(echo_service PRIVATE engine_rpc engine_core)
set_target_properties(echo_service PROPERTIES LINKER_LANGUAGE C)

add_executable(rpc_client_test example/rpc_client_test.c)
target_link_libraries(rpc_client_test PRIVATE engine_rpc engine_core)
set_target_properties(rpc_client_test PROPERTIES LINKER_LANGUAGE C)
