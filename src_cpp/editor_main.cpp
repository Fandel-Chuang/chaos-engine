/*
 * ChaosEngine 编辑器主入口
 * C++17，仅通过 extern "C" 调用 engine_core
 */

#include "chaos_engine.h"
#include <cstdio>
#include <csignal>

static volatile bool g_running = true;

static void signal_handler(int) {
    g_running = false;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    CeEngineConfig config = {};
    config.app_name      = "ChaosEngine Editor";
    config.window_width  = 1280;
    config.window_height = 720;
    config.fullscreen    = CE_FALSE;
    config.vsync         = CE_TRUE;
    config.log_level     = CE_LOG_INFO;
    config.log_file_path = "logs/chaos_editor.log";

    if (ce_init(&config) != CE_OK) {
        fprintf(stderr, "Failed to initialize ChaosEngine\n");
        return 1;
    }

    printf("========================================\n");
    printf("  ChaosEngine Editor v0.1.0\n");
    printf("  Architecture: C kernel + C++ editor\n");
    printf("========================================\n");

    // 注册编辑器日志回调
    ce_log_add_callback([](const CeLogEntry* entry, void*) {
        const char* level_str = "????";
        switch (entry->level) {
            case CE_LOG_TRACE: level_str = "TRACE"; break;
            case CE_LOG_DEBUG: level_str = "DEBUG"; break;
            case CE_LOG_INFO:  level_str = "INFO";  break;
            case CE_LOG_WARN:  level_str = "WARN";  break;
            case CE_LOG_ERROR: level_str = "ERROR"; break;
            case CE_LOG_FATAL: level_str = "FATAL"; break;
        }
        printf("[%s][%s] %s\n", level_str, entry->category, entry->message);
    }, nullptr);

    printf("Editor initialized. Press Ctrl+C to exit.\n\n");

    // 主循环
    while (g_running && ce_update()) {
        // TODO: 渲染 Dear ImGui 界面
        // TODO: 处理编辑器输入
    }

    printf("\nShutting down...\n");
    ce_shutdown();
    printf("Editor shut down cleanly.\n");
    return 0;
}
