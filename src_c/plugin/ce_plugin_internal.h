/*
 * ChaosEngine 插件系统内部头文件
 */

#ifndef CE_PLUGIN_INTERNAL_H
#define CE_PLUGIN_INTERNAL_H

#include "public_api/ce_plugin.h"
#include "public_api/ce_types.h"
#include "core/ce_memory.h"

/* ---- 插件实例 ---- */

typedef struct CePluginInstance {
    CePluginDesc  desc;
    CePluginState state;
} CePluginInstance;

/* ---- 内部 API ---- */

CeResult ce_plugin_init(CeAllocator* allocator);
void     ce_plugin_shutdown(void);
void     ce_plugin_update_all(float delta_time);

#endif /* CE_PLUGIN_INTERNAL_H */
