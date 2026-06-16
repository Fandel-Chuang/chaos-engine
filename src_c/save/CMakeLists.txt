# ============================================================
# ChaosEngine 存档模块 — 定时/手动/事件存档
# 纯 C99，产出静态库 libengine_save.a
# ============================================================

file(GLOB SAVE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
add_library(engine_save STATIC ${SAVE_SOURCES})
set_source_files_properties(${SAVE_SOURCES} PROPERTIES LANGUAGE C)

target_include_directories(engine_save PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c
    ${CMAKE_SOURCE_DIR}/src_c/public_api
)

target_link_libraries(engine_save PRIVATE engine_core engine_admin_ipc pthread)

target_compile_options(engine_save PRIVATE
    $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -std=c99>
    $<$<C_COMPILER_ID:Clang>:-Wall -Wextra -std=c99>
)

set_target_properties(engine_save PROPERTIES LINKER_LANGUAGE C)
