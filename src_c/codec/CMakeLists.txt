# ============================================================
# ChaosEngine 编解码模块
# 统一压缩/加密接口，条件编译检测库
# ============================================================

# 检测可选库
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LZ4 QUIET liblz4)
    pkg_check_modules(ZSTD QUIET libzstd)
    pkg_check_modules(ZLIB QUIET zlib)
    pkg_check_modules(OPENSSL QUIET openssl)
endif()

# __has_include 检测（C 预处理器层面）
include(CheckIncludeFile)
check_include_file(lz4.h HAVE_LZ4_H)
check_include_file(zstd.h HAVE_ZSTD_H)
check_include_file(zlib.h HAVE_ZLIB_H)

file(GLOB CODEC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
add_library(engine_codec STATIC ${CODEC_SOURCES})

target_include_directories(engine_codec PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c
    ${CMAKE_SOURCE_DIR}/src_c/public_api
)

# 链接检测到的库
if(LZ4_FOUND)
    target_compile_definitions(engine_codec PRIVATE HAVE_LZ4=1)
    target_include_directories(engine_codec PRIVATE ${LZ4_INCLUDE_DIRS})
    target_link_libraries(engine_codec PRIVATE ${LZ4_LIBRARIES})
elseif(HAVE_LZ4_H)
    target_compile_definitions(engine_codec PRIVATE HAVE_LZ4=1)
    target_link_libraries(engine_codec PRIVATE lz4)
endif()

if(ZSTD_FOUND)
    target_compile_definitions(engine_codec PRIVATE HAVE_ZSTD=1)
    target_include_directories(engine_codec PRIVATE ${ZSTD_INCLUDE_DIRS})
    target_link_libraries(engine_codec PRIVATE ${ZSTD_LIBRARIES})
elseif(HAVE_ZSTD_H)
    target_compile_definitions(engine_codec PRIVATE HAVE_ZSTD=1)
    target_link_libraries(engine_codec PRIVATE zstd)
endif()

if(ZLIB_FOUND)
    target_compile_definitions(engine_codec PRIVATE HAVE_ZLIB=1)
    target_include_directories(engine_codec PRIVATE ${ZLIB_INCLUDE_DIRS})
    target_link_libraries(engine_codec PRIVATE ${ZLIB_LIBRARIES})
elseif(HAVE_ZLIB_H)
    target_compile_definitions(engine_codec PRIVATE HAVE_ZLIB=1)
    target_link_libraries(engine_codec PRIVATE z)
endif()

if(OPENSSL_FOUND)
    target_compile_definitions(engine_codec PRIVATE HAVE_OPENSSL=1)
    target_include_directories(engine_codec PRIVATE ${OPENSSL_INCLUDE_DIRS})
    target_link_libraries(engine_codec PRIVATE ${OPENSSL_LIBRARIES})
endif()

target_link_libraries(engine_codec PRIVATE engine_core pthread)

target_compile_options(engine_codec PRIVATE
    $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -std=c99>
    $<$<C_COMPILER_ID:Clang>:-Wall -Wextra -std=c99>
)

set_target_properties(engine_codec PROPERTIES LINKER_LANGUAGE C)
