# ============================================================
# ChaosEngine Admin IPC — Unix Socket JSON-RPC 2.0 Server
# 纯 C99，产出静态库 libengine_admin_ipc.a
# ============================================================

file(GLOB ADMIN_IPC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
add_library(engine_admin_ipc STATIC ${ADMIN_IPC_SOURCES})
set_source_files_properties(${ADMIN_IPC_SOURCES} PROPERTIES LANGUAGE C)
target_include_directories(engine_admin_ipc PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c
    ${CMAKE_SOURCE_DIR}/src_c/public_api
)
target_link_libraries(engine_admin_ipc PRIVATE engine_core engine_server pthread)
set_target_properties(engine_admin_ipc PROPERTIES LINKER_LANGUAGE C)
