/*
 * ChaosEngine 主入口 — 引擎生命周期管理
 */

#define _POSIX_C_SOURCE 199309L
#include <time.h>
#include "public_api/chaos_engine.h"
#include "core/ce_memory.h"
#include "core/ce_math.h"
#include "ecs/ce_ecs_internal.h"
#include "log/ce_log_internal.h"
#include "plugin/ce_plugin_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- 引擎全局状态 ---- */

static struct {
    CeEngineConfig config;
    CeEngineState  state;
    CeAllocator*   allocator;
    double         total_time;
    double         delta_time;
    uint64_t       last_tick_us;
    CeRenderStats  render_stats;
} g_engine;

/* ---- 生命周期 ---- */

CeResult ce_init(const CeEngineConfig* config) {
    if (g_engine.state != CE_STATE_UNINITIALIZED) return CE_ERR;

    memset(&g_engine, 0, sizeof(g_engine));
    g_engine.config = *config;
    g_engine.state  = CE_STATE_INITIALIZING;

    /* 创建默认分配器 */
    g_engine.allocator = ce_allocator_create(NULL, NULL);
    if (!g_engine.allocator) {
        g_engine.state = CE_STATE_ERROR;
        return CE_ERR;
    }

    /* 初始化日志 */
    ce_log_init(g_engine.allocator, config->log_level, config->log_file_path);
    CE_LOG_INFO("ENGINE", "ChaosEngine v0.1.0 initializing...");
    CE_LOG_INFO("ENGINE", "App: %s, Window: %dx%d",
                config->app_name, config->window_width, config->window_height);

    /* 初始化 ECS */
    if (ce_ecs_init(g_engine.allocator) != CE_OK) {
        CE_LOG_FATAL("ENGINE", "Failed to initialize ECS");
        g_engine.state = CE_STATE_ERROR;
        return CE_ERR;
    }

    /* 初始化插件系统 */
    if (ce_plugin_init(g_engine.allocator) != CE_OK) {
        CE_LOG_FATAL("ENGINE", "Failed to initialize plugin system");
        g_engine.state = CE_STATE_ERROR;
        return CE_ERR;
    }

    g_engine.state = CE_STATE_RUNNING;
    CE_LOG_INFO("ENGINE", "ChaosEngine initialized successfully");
    return CE_OK;
}

void ce_shutdown(void) {
    CE_LOG_INFO("ENGINE", "ChaosEngine shutting down...");
    g_engine.state = CE_STATE_SHUTTING_DOWN;

    ce_plugin_shutdown();
    ce_ecs_shutdown();
    ce_log_shutdown();

    if (g_engine.allocator) {
        ce_allocator_destroy(g_engine.allocator);
        g_engine.allocator = NULL;
    }

    g_engine.state = CE_STATE_UNINITIALIZED;
}

CeBool ce_update(void) {
    if (g_engine.state != CE_STATE_RUNNING) return CE_FALSE;

    /* 计算 delta time */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_us = (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;

    if (g_engine.last_tick_us == 0) {
        g_engine.last_tick_us = now_us;
        g_engine.delta_time = 0.016f; /* 默认 60fps */
    } else {
        g_engine.delta_time = (double)(now_us - g_engine.last_tick_us) / 1000000.0;
        g_engine.last_tick_us = now_us;
    }

    /* 防止帧时间过大 */
    if (g_engine.delta_time > 0.1) {
        g_engine.delta_time = 0.1;
    }

    g_engine.total_time += g_engine.delta_time;

    /* 更新插件 */
    ce_plugin_update_all((float)g_engine.delta_time);

    /* 更新 ECS 系统 */
    ce_ecs_update_systems((float)g_engine.delta_time);

    return CE_TRUE;
}

/* ---- 查询接口 ---- */

CeEngineState ce_get_state(void) {
    return g_engine.state;
}

double ce_time_get_total(void) {
    return g_engine.total_time;
}

double ce_time_get_delta(void) {
    return g_engine.delta_time;
}

CeRenderStats ce_render_get_stats(void) {
    return g_engine.render_stats;
}

/* ---- 场景管理（占位） ---- */

CeResult ce_scene_load(const char* scene_path) {
    CE_LOG_INFO("ENGINE", "Loading scene: %s", scene_path);
    return CE_OK;
}

void ce_scene_unload(void) {
    CE_LOG_INFO("ENGINE", "Scene unloaded");
}

const char* ce_scene_get_name(void) {
    return "Untitled";
}

/* ---- 资源管理（占位） ---- */

uint32_t ce_resource_get_count(CeResourceType type) {
    (void)type;
    return 0;
}

CeResult ce_resource_query_info(uint32_t index, CeResourceInfo* out_info) {
    (void)index;
    (void)out_info;
    return CE_ERR;
}
