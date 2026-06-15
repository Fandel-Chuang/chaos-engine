/*
 * ChaosEngine 编辑器主入口
 * C++17，仅通过 extern "C" 调用 engine_core
 */

#include "chaos_engine.h"
#include "ui/editor_ui.h"
#include "log_observer/log_observer.h"

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

    // 初始化编辑器 UI
    ChaosEditor::Init();

    // 初始化日志观测器（第八模式）
    ChaosEditor::LogObserver log_observer;
    log_observer.Init();

    printf("========================================\n");
    printf("  ChaosEngine Editor v0.1.0\n");
    printf("  Architecture: C kernel + C++ editor\n");
    printf("========================================\n");

    // 注册编辑器日志回调（终端输出）
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
        // ---- 编辑器 UI 面板渲染 ----
        ChaosEditor::BeginFrame();

        ChaosEditor::ShowHierarchy();
        ChaosEditor::ShowInspector();
        ChaosEditor::ShowConsole();
        ChaosEditor::ShowPluginMonitor();
        ChaosEditor::ShowStats();

        // 第八模式：日志观测面板
        log_observer.Update();
        log_observer.Render();

        ChaosEditor::EndFrame();
    }

    printf("\nShutting down...\n");

    // 关闭编辑器 UI
    ChaosEditor::Shutdown();

    ce_shutdown();
    printf("Editor shut down cleanly.\n");
    return 0;
}
