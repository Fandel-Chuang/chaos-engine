/*
 * ChaosEngine 插件系统接口
 * 插件状态机 + 生命周期管理
 */

#ifndef CE_PLUGIN_H
#define CE_PLUGIN_H

#include "ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 插件状态机 ---- */

typedef enum CePluginState {
    CE_PLUGIN_UNLOADED  = 0,
    CE_PLUGIN_LOADING   = 1,
    CE_PLUGIN_LOADED    = 2,
    CE_PLUGIN_RUNNING   = 3,
    CE_PLUGIN_PAUSED    = 4,
    CE_PLUGIN_ERROR     = 5,
    CE_PLUGIN_UNLOADING = 6
} CePluginState;

/* ---- 插件信息 ---- */

typedef struct CePluginInfo {
    char          name[64];
    char          version[16];
    char          author[64];
    char          description[256];
    CePluginState state;
    CeBool        is_core;       /* 核心插件不可卸载 */
    uint64_t      load_time_us;  /* 加载耗时（微秒） */
} CePluginInfo;

/* ---- 插件接口 ---- */

/** 插件初始化回调 */
typedef CeResult (*CePluginInitFn)(void);

/** 插件帧更新回调 */
typedef void (*CePluginUpdateFn)(float delta_time);

/** 插件关闭回调 */
typedef void (*CePluginShutdownFn)(void);

typedef struct CePluginDesc {
    const char*        name;
    const char*        version;
    const char*        author;
    const char*        description;
    CePluginInitFn     init;
    CePluginUpdateFn   update;
    CePluginShutdownFn shutdown;
    CeBool             is_core;
} CePluginDesc;

/* ---- 插件管理 API ---- */

/** 注册插件 */
CeResult ce_plugin_register(const CePluginDesc* desc);

/** 加载插件（状态: UNLOADED -> LOADING -> LOADED） */
CeResult ce_plugin_load(const char* name);

/** 卸载插件 */
CeResult ce_plugin_unload(const char* name);

/** 获取插件数量 */
uint32_t ce_plugin_get_count(void);

/** 获取插件信息快照（只读） */
CeResult ce_plugin_get_info(uint32_t index, CePluginInfo* out_info);

/** 按名称查找插件信息 */
CeResult ce_plugin_find(const char* name, CePluginInfo* out_info);

/** 获取所有插件状态快照（批量，用于编辑器观测） */
uint32_t ce_plugin_snapshot_all(CePluginInfo* out_buffer, uint32_t max_count);

#ifdef __cplusplus
}
#endif

#endif /* CE_PLUGIN_H */
