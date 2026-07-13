# ============================================================
# ChaosEngine 服务端模块 — AOI + Cell
# 仅在 CE_BUILD_SERVER 模式下编译
# ============================================================

add_library(engine_server STATIC
    ce_aoi.c
    ce_cell.c
    ce_game_session.c
)

target_include_directories(engine_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c
)

target_link_libraries(engine_server PRIVATE engine_core)

if(MSVC)
    set(_CE_COMPAT "${CMAKE_SOURCE_DIR}/src_c/core/ce_win_compat.h")
    target_compile_options(engine_server PRIVATE
        /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS "/FI${_CE_COMPAT}"
    )
else()
    target_compile_options(engine_server PRIVATE
        $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -std=c99>
        $<$<C_COMPILER_ID:Clang>:-Wall -Wextra -std=c99>
    )
endif()
