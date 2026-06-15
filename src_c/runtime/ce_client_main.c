/*
 * ChaosEngine Vulkan 客户端入口
 * 使用 Vulkan RHI 渲染三角形
 */

#define _POSIX_C_SOURCE 199309L
#include "public_api/chaos_engine.h"
#include "core/ce_platform.h"
#include "core/ce_time.h"
#include "core/ce_math.h"
#include "render/ce_rhi.h"
#include "log/ce_log_internal.h"
#include <stdio.h>
#include <stdlib.h>
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

    /* 初始化引擎 */
    CeEngineConfig config = {
        .app_name      = "ChaosEngine Vulkan",
        .window_width  = 1280,
        .window_height = 720,
        .fullscreen    = CE_FALSE,
        .vsync         = CE_TRUE,
        .log_level     = CE_LOG_INFO,
        .log_file_path = "logs/chaos_client.log"
    };

    if (ce_init(&config) != CE_OK) {
        fprintf(stderr, "Failed to initialize engine\n");
        return 1;
    }

    printf("========================================\n");
    printf("  ChaosEngine Vulkan Client v0.1.0\n");
    printf("  Render: Vulkan RHI\n");
    printf("========================================\n");

    /* 创建 Vulkan 设备 */
    CeRhiConfig rhi_config = {
        .backend       = CE_RHI_VULKAN,
        .native_window = NULL,  /* 无头模式暂不创建窗口 */
        .width         = 1280,
        .height        = 720,
        .title         = "ChaosEngine Vulkan",
        .vsync         = CE_TRUE
    };

    CeRhiDevice* rhi = rhi_create_device(&rhi_config);
    if (!rhi) {
        fprintf(stderr, "Failed to create Vulkan device\n");
        ce_shutdown();
        return 1;
    }

    printf("Vulkan device created successfully\n");

    /* 创建帧计时器 */
    CeFrameTimer* timer = ce_frame_timer_create(60.0);

    printf("Rendering... Press Ctrl+C to exit.\n\n");

    /* 主循环 */
    while (g_running && !rhi_should_close(rhi)) {
        rhi_poll_events(rhi);

        double dt = ce_frame_timer_tick(timer);

        /* 引擎更新 */
        ce_update();

        /* 渲染 */
        rhi_begin_frame(rhi, ce_color_rgba(0.1f, 0.1f, 0.15f, 1.0f));
        rhi_draw(rhi, 3, 0);  /* 画三角形（3个顶点，硬编码在着色器中） */
        rhi_end_frame(rhi);
        rhi_present(rhi);

        /* 每 60 帧打印 FPS */
        if (ce_frame_timer_frame_count(timer) % 60 == 0) {
            printf("\rFPS: %.1f, Frame: %lu, dt: %.3f ms",
                   ce_frame_timer_fps(timer),
                   (unsigned long)ce_frame_timer_frame_count(timer),
                   dt * 1000.0);
            fflush(stdout);
        }
    }

    printf("\n\nShutting down...\n");

    rhi_destroy_device(rhi);
    ce_frame_timer_destroy(timer);
    ce_shutdown();

    printf("Client shut down cleanly.\n");
    return 0;
}
