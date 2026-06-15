/*
 * ChaosEngine 编辑器 UI 面板实现
 * 纯终端 UI：printf + ANSI 转义码
 * 仅通过 public_api 头文件调用引擎内核
 */

#include "editor_ui.h"
#include "chaos_engine.h"
#include "ce_log.h"
#include "ce_plugin.h"
#include "ce_ecs.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>

namespace ChaosEditor {

// ============================================================
// 内部辅助
// ============================================================

static const char* state_to_string(CeEngineState state) {
    switch (state) {
        case CE_STATE_UNINITIALIZED:  return "UNINITIALIZED";
        case CE_STATE_INITIALIZING:   return "INITIALIZING";
        case CE_STATE_RUNNING:        return "RUNNING";
        case CE_STATE_PAUSED:         return "PAUSED";
        case CE_STATE_SHUTTING_DOWN:  return "SHUTTING DOWN";
        case CE_STATE_ERROR:          return "ERROR";
        default:                      return "UNKNOWN";
    }
}

static const char* plugin_state_to_string(CePluginState state) {
    switch (state) {
        case CE_PLUGIN_UNLOADED:  return "UNLOADED";
        case CE_PLUGIN_LOADING:   return "LOADING";
        case CE_PLUGIN_LOADED:    return "LOADED";
        case CE_PLUGIN_RUNNING:   return "RUNNING";
        case CE_PLUGIN_PAUSED:    return "PAUSED";
        case CE_PLUGIN_ERROR:     return "ERROR";
        case CE_PLUGIN_UNLOADING: return "UNLOADING";
        default:                  return "???";
    }
}

static const char* log_level_to_string(CeLogLevel level) {
    switch (level) {
        case CE_LOG_TRACE: return "TRACE";
        case CE_LOG_DEBUG: return "DEBUG";
        case CE_LOG_INFO:  return "INFO";
        case CE_LOG_WARN:  return "WARN";
        case CE_LOG_ERROR: return "ERROR";
        case CE_LOG_FATAL: return "FATAL";
        default:           return "????";
    }
}

/** 打印分隔线 */
static void print_separator(const char* title) {
    printf("+");
    for (int i = 0; i < 38; i++) printf("-");
    printf("+");
    if (title && title[0]) {
        printf(" %s ", title);
    }
    printf("\n");
}

/** 打印带标签的键值对 */
static void print_kv(const char* key, const char* fmt, ...) {
    printf("  %-20s ", key);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

// ============================================================
// 生命周期
// ============================================================

void Init() {
    printf("[EditorUI] Initialized terminal UI subsystem.\n");
}

void Shutdown() {
    printf("[EditorUI] Shutdown terminal UI subsystem.\n");
}

void BeginFrame() {
    // ANSI 清屏 + 光标归位
    printf("\033[2J\033[H");
}

void EndFrame() {
    fflush(stdout);
}

// ============================================================
// 面板：层次结构 (Hierarchy)
// ============================================================

void ShowHierarchy() {
    print_separator("HIERARCHY");

    uint32_t entity_count = ce_ecs_get_entity_count();
    uint32_t component_count = ce_ecs_get_component_count();

    print_kv("Total Entities", "%u", entity_count);
    print_kv("Component Types", "%u", component_count);

    if (entity_count == 0) {
        printf("  (no entities in scene)\n");
    } else {
        printf("  Scene contains %u active entities.\n", entity_count);
    }

    printf("\n");
}

// ============================================================
// 面板：Inspector
// ============================================================

void ShowInspector() {
    print_separator("INSPECTOR");

    CeEngineState state = ce_get_state();
    print_kv("Engine State", "%s", state_to_string(state));

    // 渲染统计
    CeRenderStats render_stats = ce_render_get_stats();
    print_kv("Draw Calls", "%u", render_stats.draw_calls);
    print_kv("Triangles", "%u", render_stats.triangles);
    print_kv("Vertices", "%u", render_stats.vertices);
    print_kv("Frame Time (ms)", "%.2f", render_stats.frame_time_ms);
    print_kv("GPU Time (ms)", "%.2f", render_stats.gpu_time_ms);

    // 资源统计
    for (int i = 0; i < CE_RESOURCE_COUNT; i++) {
        CeResourceType type = static_cast<CeResourceType>(i);
        uint32_t count = ce_resource_get_count(type);
        const char* type_names[] = {
            "Mesh", "Texture", "Shader", "Material", "Audio", "Script"
        };
        char key[32];
        snprintf(key, sizeof(key), "  Resources/%s", type_names[i]);
        printf("  %-20s %u\n", key, count);
    }

    // 场景信息
    const char* scene_name = ce_scene_get_name();
    print_kv("Scene", "%s", scene_name ? scene_name : "(none)");

    printf("\n");
}

// ============================================================
// 面板：控制台 (Console)
// ============================================================

void ShowConsole() {
    print_separator("CONSOLE");

    static const uint32_t MAX_ENTRIES = 20;
    CeLogEntry entries[MAX_ENTRIES];
    uint32_t count = ce_log_get_recent(entries, MAX_ENTRIES);

    if (count == 0) {
        printf("  (no log entries)\n");
    } else {
        for (uint32_t i = 0; i < count; i++) {
            const CeLogEntry& e = entries[i];
            const char* level_str = log_level_to_string(e.level);
            const char* cat = e.category ? e.category : "?";
            const char* msg = e.message ? e.message : "";
            printf("  [%s][%s] %s\n", level_str, cat, msg);
        }
    }

    printf("\n");
}

// ============================================================
// 面板：插件监控 (Plugin Monitor)
// ============================================================

void ShowPluginMonitor() {
    print_separator("PLUGIN MONITOR");

    uint32_t plugin_count = ce_plugin_get_count();
    print_kv("Loaded Plugins", "%u", plugin_count);

    if (plugin_count == 0) {
        printf("  (no plugins loaded)\n");
    } else {
        static const uint32_t MAX_PLUGINS = 64;
        CePluginInfo plugins[MAX_PLUGINS];
        uint32_t count = ce_plugin_snapshot_all(plugins, MAX_PLUGINS);

        printf("  %-24s %-10s %-8s %-20s\n",
               "Name", "Version", "State", "Author");
        printf("  ----------------------------------------------------------------\n");

        for (uint32_t i = 0; i < count; i++) {
            const CePluginInfo& p = plugins[i];
            const char* state_str = plugin_state_to_string(p.state);
            const char* core_mark = p.is_core ? " [CORE]" : "";
            printf("  %-24s %-10s %-8s %-20s%s\n",
                   p.name, p.version, state_str, p.author, core_mark);
        }
    }

    printf("\n");
}

// ============================================================
// 面板：统计 (Stats)
// ============================================================

void ShowStats() {
    print_separator("STATS");

    double total_time = ce_time_get_total();
    double delta_time = ce_time_get_delta();

    // 计算 FPS
    double fps = (delta_time > 0.0) ? (1.0 / delta_time) : 0.0;

    print_kv("Total Time", "%.2f s", total_time);
    print_kv("Delta Time", "%.4f s (%.2f ms)", delta_time, delta_time * 1000.0);
    print_kv("FPS", "%.1f", fps);

    // 引擎状态摘要
    CeEngineState state = ce_get_state();
    print_kv("Engine State", "%s", state_to_string(state));

    printf("\n");
}

} // namespace ChaosEditor
