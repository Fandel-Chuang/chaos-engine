/*
 * ChaosEngine 插件系统
 * 状态机 + 注册表管理
 */

#include "plugin/ce_plugin_internal.h"
#include "core/ce_memory.h"
#include "log/ce_log_internal.h"
#include <string.h>
#include <stdio.h>

/* ---- 插件上下文 ---- */

#define CE_MAX_PLUGINS 64

static struct {
    CeAllocator* allocator;
    uint32_t     plugin_count;
    CePluginInstance plugins[CE_MAX_PLUGINS];
} g_plugin;

/* ---- 初始化 ---- */

CeResult ce_plugin_init(CeAllocator* allocator) {
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.allocator = allocator;
    return CE_OK;
}

void ce_plugin_shutdown(void) {
    /* 卸载所有插件 */
    for (uint32_t i = 0; i < g_plugin.plugin_count; i++) {
        CePluginInstance* inst = &g_plugin.plugins[i];
        if (inst->state == CE_PLUGIN_RUNNING || inst->state == CE_PLUGIN_LOADED) {
            if (inst->desc.shutdown) {
                inst->desc.shutdown();
            }
        }
    }
    memset(&g_plugin, 0, sizeof(g_plugin));
}

/* ---- 插件注册 ---- */

CeResult ce_plugin_register(const CePluginDesc* desc) {
    if (g_plugin.plugin_count >= CE_MAX_PLUGINS) return CE_ERR;
    if (!desc || !desc->name) return CE_ERR;

    /* 检查重复 */
    for (uint32_t i = 0; i < g_plugin.plugin_count; i++) {
        if (strcmp(g_plugin.plugins[i].desc.name, desc->name) == 0) {
            CE_LOG_WARN("PLUGIN", "Plugin '%s' already registered", desc->name);
            return CE_ERR;
        }
    }

    CePluginInstance* inst = &g_plugin.plugins[g_plugin.plugin_count++];
    memset(inst, 0, sizeof(*inst));
    inst->desc  = *desc;
    inst->state = CE_PLUGIN_UNLOADED;

    CE_LOG_INFO("PLUGIN", "Plugin '%s' v%s registered", desc->name, desc->version);
    return CE_OK;
}

/* ---- 插件加载/卸载 ---- */

static void set_state(CePluginInstance* inst, CePluginState new_state) {
    CePluginState old = inst->state;
    inst->state = new_state;
    CE_LOG_INFO("PLUGIN", "Plugin '%s': %d -> %d", inst->desc.name, old, new_state);
}

CeResult ce_plugin_load(const char* name) {
    for (uint32_t i = 0; i < g_plugin.plugin_count; i++) {
        CePluginInstance* inst = &g_plugin.plugins[i];
        if (strcmp(inst->desc.name, name) != 0) continue;

        if (inst->state != CE_PLUGIN_UNLOADED) {
            CE_LOG_WARN("PLUGIN", "Plugin '%s' is not unloaded (state=%d)", name, inst->state);
            return CE_ERR;
        }

        set_state(inst, CE_PLUGIN_LOADING);

        if (inst->desc.init) {
            CeResult r = inst->desc.init();
            if (r != CE_OK) {
                set_state(inst, CE_PLUGIN_ERROR);
                CE_LOG_ERROR("PLUGIN", "Plugin '%s' init failed", name);
                return CE_ERR;
            }
        }

        set_state(inst, CE_PLUGIN_LOADED);
        set_state(inst, CE_PLUGIN_RUNNING);
        return CE_OK;
    }

    CE_LOG_ERROR("PLUGIN", "Plugin '%s' not found", name);
    return CE_ERR;
}

CeResult ce_plugin_unload(const char* name) {
    for (uint32_t i = 0; i < g_plugin.plugin_count; i++) {
        CePluginInstance* inst = &g_plugin.plugins[i];
        if (strcmp(inst->desc.name, name) != 0) continue;

        if (inst->desc.is_core) {
            CE_LOG_WARN("PLUGIN", "Cannot unload core plugin '%s'", name);
            return CE_ERR;
        }

        set_state(inst, CE_PLUGIN_UNLOADING);

        if (inst->desc.shutdown) {
            inst->desc.shutdown();
        }

        set_state(inst, CE_PLUGIN_UNLOADED);
        return CE_OK;
    }

    return CE_ERR;
}

/* ---- 插件更新 ---- */

void ce_plugin_update_all(float delta_time) {
    for (uint32_t i = 0; i < g_plugin.plugin_count; i++) {
        CePluginInstance* inst = &g_plugin.plugins[i];
        if (inst->state == CE_PLUGIN_RUNNING && inst->desc.update) {
            inst->desc.update(delta_time);
        }
    }
}

/* ---- 插件查询 ---- */

uint32_t ce_plugin_get_count(void) {
    return g_plugin.plugin_count;
}

CeResult ce_plugin_get_info(uint32_t index, CePluginInfo* out_info) {
    if (index >= g_plugin.plugin_count) return CE_ERR;

    CePluginInstance* inst = &g_plugin.plugins[index];
    memset(out_info, 0, sizeof(*out_info));
    strncpy(out_info->name,        inst->desc.name,        sizeof(out_info->name) - 1);
    strncpy(out_info->version,     inst->desc.version,     sizeof(out_info->version) - 1);
    strncpy(out_info->author,      inst->desc.author ? inst->desc.author : "",
            sizeof(out_info->author) - 1);
    strncpy(out_info->description, inst->desc.description ? inst->desc.description : "",
            sizeof(out_info->description) - 1);
    out_info->state  = inst->state;
    out_info->is_core = inst->desc.is_core;

    return CE_OK;
}

CeResult ce_plugin_find(const char* name, CePluginInfo* out_info) {
    for (uint32_t i = 0; i < g_plugin.plugin_count; i++) {
        if (strcmp(g_plugin.plugins[i].desc.name, name) == 0) {
            return ce_plugin_get_info(i, out_info);
        }
    }
    return CE_ERR;
}

uint32_t ce_plugin_snapshot_all(CePluginInfo* out_buffer, uint32_t max_count) {
    uint32_t count = g_plugin.plugin_count;
    if (count > max_count) count = max_count;

    for (uint32_t i = 0; i < count; i++) {
        ce_plugin_get_info(i, &out_buffer[i]);
    }

    return count;
}
