# ============================================================
# ChaosEngine I/O 性能基准测试
# ============================================================

# ---- bench_client: 高性能基准测试客户端 ----
# 使用 epoll + 非阻塞 socket，支持 10000+ 并发连接
add_executable(bench_client bench_client.c)
target_include_directories(bench_client PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(bench_client PRIVATE m pthread)
set_target_properties(bench_client PROPERTIES LINKER_LANGUAGE C)

# ---- bench_runner: 自动化对比测试脚本 ----
# 自动启动 io_uring 和 POSIX 服务器，运行 bench_client 并收集结果
add_executable(bench_runner bench_runner.c)
target_include_directories(bench_runner PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(bench_runner PRIVATE m pthread)
set_target_properties(bench_runner PROPERTIES LINKER_LANGUAGE C)

# ---- bench_xdp: XDP 性能基准测试 (Phase 4.6) ----
# 对比 XDP 路径 vs 传统内核网络栈的吞吐量和延迟
add_executable(bench_xdp bench_xdp.c)
target_include_directories(bench_xdp PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(bench_xdp PRIVATE m pthread)
set_target_properties(bench_xdp PROPERTIES LINKER_LANGUAGE C)
