/*
 * ChaosEngine 基础类型定义
 * 纯 C 兼容，无 C++ 特性
 */

#ifndef CE_TYPES_H
#define CE_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 基础类型别名 ---- */
typedef bool     CeBool;
typedef int32_t  CeResult;

#define CE_OK     0
#define CE_ERR   -1
#define CE_ERR_VALIDATION  -2   /* 值域校验失败 */
#define CE_TRUE   1
#define CE_FALSE  0

/* ---- 数学类型 ---- */

typedef struct CeVec2 { float x, y; } CeVec2;
typedef struct CeVec3 { float x, y, z; } CeVec3;
typedef struct CeVec4 { float x, y, z, w; } CeVec4;

typedef struct CeMat4 {
    float m[16];
} CeMat4;

typedef struct CeQuat {
    float x, y, z, w;
} CeQuat;

typedef struct CeColor {
    float r, g, b, a;
} CeColor;

typedef struct CeRect {
    float x, y, w, h;
} CeRect;

/* ---- 引擎配置 ---- */

typedef enum CeLogLevel {
    CE_LOG_TRACE = 0,
    CE_LOG_DEBUG,
    CE_LOG_INFO,
    CE_LOG_WARN,
    CE_LOG_ERROR,
    CE_LOG_FATAL
} CeLogLevel;

typedef struct CeEngineConfig {
    const char* app_name;
    int         window_width;
    int         window_height;
    CeBool      fullscreen;
    CeBool      vsync;
    CeLogLevel  log_level;
    const char* log_file_path;
} CeEngineConfig;

/* ---- 引擎状态 ---- */

typedef enum CeEngineState {
    CE_STATE_UNINITIALIZED = 0,
    CE_STATE_INITIALIZING,
    CE_STATE_RUNNING,
    CE_STATE_PAUSED,
    CE_STATE_SHUTTING_DOWN,
    CE_STATE_ERROR
} CeEngineState;

/* ---- 资源类型 ---- */

typedef enum CeResourceType {
    CE_RESOURCE_MESH     = 0,
    CE_RESOURCE_TEXTURE  = 1,
    CE_RESOURCE_SHADER   = 2,
    CE_RESOURCE_MATERIAL = 3,
    CE_RESOURCE_AUDIO    = 4,
    CE_RESOURCE_SCRIPT   = 5,
    CE_RESOURCE_COUNT
} CeResourceType;

typedef struct CeResourceInfo {
    uint32_t      id;
    CeResourceType type;
    char          name[64];
    char          path[256];
    size_t        memory_bytes;
    CeBool        loaded;
} CeResourceInfo;

/* ---- 渲染统计 ---- */

typedef struct CeRenderStats {
    uint32_t draw_calls;
    uint32_t triangles;
    uint32_t vertices;
    float    frame_time_ms;
    float    gpu_time_ms;
} CeRenderStats;

/* ---- 内存分配器句柄 ---- */

typedef struct CeAllocator CeAllocator;

/** 默认内存分配回调 */
typedef void* (*CeMallocFn)(size_t size, void* user_data);
typedef void  (*CeFreeFn)(void* ptr, void* user_data);
typedef void* (*CeReallocFn)(void* ptr, size_t new_size, void* user_data);

typedef struct CeAllocatorVTable {
    CeMallocFn  malloc;
    CeFreeFn    free;
    CeReallocFn realloc;
} CeAllocatorVTable;

#ifdef __cplusplus
}
#endif

#endif /* CE_TYPES_H */
