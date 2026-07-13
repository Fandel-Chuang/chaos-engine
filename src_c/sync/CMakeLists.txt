# ============================================================
# ChaosEngine 同步模块 — Game↔DBProxy
# 纯 C99，产出静态库 libengine_sync.a
# ============================================================

file(GLOB SYNC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
add_library(engine_sync STATIC ${SYNC_SOURCES})
set_source_files_properties(${SYNC_SOURCES} PROPERTIES LANGUAGE C)

target_include_directories(engine_sync PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c
    ${CMAKE_SOURCE_DIR}/src_c/public_api
)

if(WIN32)
    set(_CE_COMPAT "${CMAKE_SOURCE_DIR}/src_c/core/ce_win_compat.h")
    if(MSVC)
        target_compile_options(engine_sync PRIVATE
            /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS "/FI${_CE_COMPAT}"
        )
    endif()
    target_link_libraries(engine_sync PRIVATE engine_core)
else()
    target_link_libraries(engine_sync PRIVATE engine_core pthread)
    target_compile_options(engine_sync PRIVATE
        $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -std=c99>
        $<$<C_COMPILER_ID:Clang>:-Wall -Wextra -std=c99>
    )
endif()

set_target_properties(engine_sync PROPERTIES LINKER_LANGUAGE C)
