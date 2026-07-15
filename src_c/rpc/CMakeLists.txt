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

# ---- 检测 libcurl (etcd 服务发现) ----
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(CURL QUIET libcurl)
endif()
if(NOT CURL_FOUND)
    check_include_file_cxx(curl/curl.h HAVE_CURL_H)
    if(HAVE_CURL_H)
        set(CURL_LIBRARIES "curl")
        set(CURL_FOUND TRUE)
    endif()
endif()

if(CURL_FOUND)
    message(STATUS "RPC: libcurl found, enabling etcd service discovery")
    target_compile_definitions(engine_rpc PRIVATE HAVE_CURL=1)
    target_include_directories(engine_rpc PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(engine_rpc PRIVATE ${CURL_LIBRARIES})
else()
    message(STATUS "RPC: libcurl NOT found, etcd service discovery compiled out")
    # 排除 etcd 文件
    list(FILTER RPC_SOURCES EXCLUDE REGEX ".*ce_etcd.*\\.c$")
    # 重新设置源文件
    set_target_properties(engine_rpc PROPERTIES SOURCES "")
    target_sources(engine_rpc PRIVATE ${RPC_SOURCES})
endif()

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
