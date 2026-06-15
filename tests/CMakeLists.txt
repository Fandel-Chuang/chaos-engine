# ============================================================
# ChaosEngine 单元测试
# 启用方式: cmake .. -DCHAOS_BUILD_TESTS=ON
# ============================================================

# ---- test_math ----
add_executable(test_math unit/test_math.c)
target_include_directories(test_math PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_math PRIVATE engine_core)
set_target_properties(test_math PROPERTIES LINKER_LANGUAGE C)
add_test(NAME math COMMAND test_math)

# ---- test_memory ----
add_executable(test_memory unit/test_memory.c)
target_include_directories(test_memory PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_memory PRIVATE engine_core)
set_target_properties(test_memory PROPERTIES LINKER_LANGUAGE C)
add_test(NAME memory COMMAND test_memory)

# ---- test_ecs ----
add_executable(test_ecs unit/test_ecs.c)
target_include_directories(test_ecs PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_ecs PRIVATE engine_core)
set_target_properties(test_ecs PROPERTIES LINKER_LANGUAGE C)
add_test(NAME ecs COMMAND test_ecs)

# ---- test_network ----
add_executable(test_network unit/test_network.c)
target_include_directories(test_network PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_network PRIVATE engine_core)
set_target_properties(test_network PROPERTIES LINKER_LANGUAGE C)
add_test(NAME network COMMAND test_network)

# ---- test_aoi ----
add_executable(test_aoi unit/test_aoi.c)
target_include_directories(test_aoi PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_aoi PRIVATE engine_core engine_server)
set_target_properties(test_aoi PROPERTIES LINKER_LANGUAGE C)
add_test(NAME aoi COMMAND test_aoi)

# ---- test_cell ----
add_executable(test_cell unit/test_cell.c)
target_include_directories(test_cell PRIVATE
    ${CMAKE_SOURCE_DIR}/src_c/public_api
    ${CMAKE_SOURCE_DIR}/src_c
)
target_link_libraries(test_cell PRIVATE engine_core engine_server)
set_target_properties(test_cell PROPERTIES LINKER_LANGUAGE C)
add_test(NAME cell COMMAND test_cell)
