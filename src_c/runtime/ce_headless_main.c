/*
 * ChaosEngine 无头模式入口
 * 用于服务器 / 单元测试 / CI
 */

#include "public_api/chaos_engine.h"
#include <stdio.h>
#include <signal.h>

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    CeEngineConfig config = {
        .app_name      = "ChaosEngine-Headless",
        .window_width  = 0,
        .window_height = 0,
        .fullscreen    = CE_FALSE,
        .vsync         = CE_FALSE,
        .log_level     = CE_LOG_INFO,
        .log_file_path = "logs/chaos_headless.log"
    };

    if (ce_init(&config) != CE_OK) {
        fprintf(stderr, "Failed to initialize ChaosEngine\n");
        return 1;
    }

    printf("ChaosEngine Headless v0.1.0\n");
    printf("Running... Press Ctrl+C to exit.\n");

    while (g_running && ce_update()) {
        /* 主循环 */
    }

    ce_shutdown();
    printf("ChaosEngine shut down cleanly.\n");
    return 0;
}
