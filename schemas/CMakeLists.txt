# ChaosEngine FlatBuffers Schema 编译
# 使用 flatc 编译 .fbs → C 头文件

find_program(FLATC_EXECUTABLE flatc REQUIRED)

set(FBS_SCHEMAS
    ${CMAKE_CURRENT_SOURCE_DIR}/chaos_common.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/chaos_replication.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/chaos_rpc.fbs
)

set(FBS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/generated/flatbuffers")

# 确保输出目录存在
file(MAKE_DIRECTORY ${FBS_OUTPUT_DIR})

# 编译 FlatBuffers schema → C 头文件
add_custom_command(
    OUTPUT
        ${FBS_OUTPUT_DIR}/chaos_common_generated.h
        ${FBS_OUTPUT_DIR}/chaos_replication_generated.h
        ${FBS_OUTPUT_DIR}/chaos_rpc_generated.h
    COMMAND ${FLATC_EXECUTABLE}
        --cpp --gen-object-api --scoped-enums
        -o ${FBS_OUTPUT_DIR}
        ${FBS_SCHEMAS}
    DEPENDS ${FBS_SCHEMAS}
    COMMENT "Compiling FlatBuffers schemas → C headers"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# 聚合目标
add_custom_target(chaos_fbs DEPENDS
    ${FBS_OUTPUT_DIR}/chaos_common_generated.h
    ${FBS_OUTPUT_DIR}/chaos_replication_generated.h
    ${FBS_OUTPUT_DIR}/chaos_rpc_generated.h
)

# 提供 include 路径给依赖方
set(FLATBUFFERS_INCLUDE_DIR ${FBS_OUTPUT_DIR} PARENT_SCOPE)
