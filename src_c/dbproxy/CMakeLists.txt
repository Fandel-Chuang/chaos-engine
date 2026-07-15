# ============================================================
# ChaosEngine DBProxy 客户端模块
# 纯 C99，产出静态库 libengine_dbproxy.a
# ============================================================

# ---- libmongoc 检测（条件编译原生驱动） ----
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(MONGOC QUIET mongoc)
endif()

file(GLOB DBPROXY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
add_library(engine_dbproxy STATIC ${DBPROXY_SOURCES})

target_include_directories(engine_dbproxy PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c
    ${CMAKE_SOURCE_DIR}/src_c/public_api
)

# 如果找到 libmongoc，启用原生驱动
if(MONGOC_FOUND)
    message(STATUS "DBProxy: libmongoc found, enabling native driver")
    target_compile_definitions(engine_dbproxy PRIVATE HAVE_MONGOC=1)
    target_include_directories(engine_dbproxy PRIVATE ${MONGOC_INCLUDE_DIRS})
    target_link_directories(engine_dbproxy PRIVATE ${MONGOC_LIBRARY_DIRS})
    target_link_libraries(engine_dbproxy PRIVATE ${MONGOC_LIBRARIES})
else()
    message(STATUS "DBProxy: libmongoc NOT found, native driver compiled out")
endif()

target_link_libraries(engine_dbproxy PRIVATE engine_core pthread)

target_compile_options(engine_dbproxy PRIVATE
    $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -std=c99>
    $<$<C_COMPILER_ID:Clang>:-Wall -Wextra -std=c99>
)

set_target_properties(engine_dbproxy PROPERTIES LINKER_LANGUAGE C)
