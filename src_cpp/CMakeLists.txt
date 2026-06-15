# ============================================================
# ChaosEngine 编辑器 — C++17
# 仅链接 engine_core，通过 extern "C" 调用
# ============================================================

file(GLOB_RECURSE EDITOR_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/editor_logic/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/log_observer/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/importer/*.cpp"
)

add_executable(chaos_editor
    ${EDITOR_SOURCES}
    editor_main.cpp
)

target_include_directories(chaos_editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src_c/public_api
)

target_link_libraries(chaos_editor PRIVATE engine_core)

target_compile_options(chaos_editor PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)
