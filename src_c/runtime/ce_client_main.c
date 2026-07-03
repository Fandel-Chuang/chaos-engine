/*
 * ChaosEngine Vulkan 客户端入口
 * 使用 Vulkan RHI 渲染三角形，集成网络连接
 */

#define _POSIX_C_SOURCE 199309L
#include "public_api/chaos_engine.h"
#include "core/ce_platform.h"
#include "core/ce_time.h"
#include "core/ce_math.h"
#include "render/ce_rhi.h"
#include "log/ce_log_internal.h"
#include "runtime/ce_client_network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <time.h>

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* 默认 Game Server 地址 */
static const char* g_gateway_host = "127.0.0.1";
static int         g_gateway_port = 7777;

static void parse_args(int argc, char** argv, const char** host, int* port) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            /* 解析 host:port */
            char* colon = strchr(argv[i + 1], ':');
            if (colon) {
                *colon = '\0';
                *host = argv[i + 1];
                *port = atoi(colon + 1);
                /* 不恢复 colon，host 字符串在冒号处截断 */
            } else {
                *host = argv[i + 1];
            }
            i++;
        }
    }
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    srand((unsigned)time(NULL));

    /* 解析命令行参数 */
    parse_args(argc, argv, &g_gateway_host, &g_gateway_port);

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
    printf("  Gateway: %s:%d\n", g_gateway_host, g_gateway_port);
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

    /* ---- 初始化网络连接 ---- */
    CeClientNet* net = ce_client_net_connect(g_gateway_host, g_gateway_port);
    if (net && ce_client_net_is_connected(net)) {
        ce_client_net_set_binary_mode(net, 1);
        /* 发送加入请求 */
        ce_client_net_send_join(net, net->entity_id);
    } else {
        printf("[Client] No network connection (Gateway may be offline)\n");
    }

    /* 创建帧计时器 */
    CeFrameTimer* timer = ce_frame_timer_create(60.0);

    printf("Rendering... Press Ctrl+C to exit.\n\n");

    /* 主循环 */
    unsigned long frame_count = 0;
    while (g_running && !rhi_should_close(rhi)) {
        rhi_poll_events(rhi);

        double dt = ce_frame_timer_tick(timer);
        frame_count = (unsigned long)ce_frame_timer_frame_count(timer);

        /* 双通道输入：方向键 / WASD 都能驱动本地球体 */
        static CeBool spawn_initialized = CE_FALSE;
        static float local_x = 0.0f;
        static float local_z = 0.0f;
        const float move_speed = 6.0f * (float)dt;
        const float spawn_radius = 10.0f * 0.25f;
        if (!spawn_initialized) {
            const float two_pi = 6.28318530717958647692f;
            float angle = ((float)rand() / (float)RAND_MAX) * two_pi;
            float dist = spawn_radius * sqrtf((float)rand() / (float)RAND_MAX);
            local_x = cosf(angle) * dist;
            local_z = sinf(angle) * dist;
            printf("[Client] Spawn pos: (%.2f, %.2f, %.2f) within r=%.2f\n",
                   local_x, 0.0f, local_z, spawn_radius);
            spawn_initialized = CE_TRUE;
        }
        CeBool move_left  = ce_input_is_key_down(CE_KEY_LEFT) || ce_input_is_key_down(CE_KEY_A);
        CeBool move_right = ce_input_is_key_down(CE_KEY_RIGHT) || ce_input_is_key_down(CE_KEY_D);
        CeBool move_up    = ce_input_is_key_down(CE_KEY_UP) || ce_input_is_key_down(CE_KEY_W);
        CeBool move_down  = ce_input_is_key_down(CE_KEY_DOWN) || ce_input_is_key_down(CE_KEY_S);
        if (move_left)  local_x -= move_speed;
        if (move_right) local_x += move_speed;
        if (move_up)    local_z -= move_speed;
        if (move_down)  local_z += move_speed;

        char key_hint[16];
        snprintf(key_hint, sizeof(key_hint), "%c%c%c%c",
                 move_up ? 'W' : '-',
                 move_down ? 'S' : '-',
                 move_left ? 'A' : '-',
                 move_right ? 'D' : '-');

        char title[256];
        if (net && ce_client_net_is_connected(net)) {
            int ec = ce_client_net_entity_count(net);
            snprintf(title, sizeof(title),
                     "ChaosEngine Vulkan | pos=(%.2f, %.2f, %.2f) | visible=%d | keys=%s",
                     local_x, 0.0f, local_z, ec, key_hint);
        } else {
            snprintf(title, sizeof(title),
                     "ChaosEngine Vulkan | pos=(%.2f, %.2f, %.2f) | disconnected | keys=%s",
                     local_x, 0.0f, local_z, key_hint);
        }
        rhi_set_window_title(rhi, title);

        /* ---- 网络更新 ---- */
        if (net && ce_client_net_is_connected(net)) {
            /* 轮询网络消息 */
            int msg_count = ce_client_net_poll(net);
            if (msg_count < 0) {
                printf("[Client] Network error, continuing without network\n");
            }

            /* 每 10 帧发送一次位置更新 */
            if (frame_count % CE_CLIENT_POSITION_INTERVAL == 0) {
                ce_client_net_send_position(net, local_x, 0.0f, local_z);
            }

            /* 打印可见实体信息（每 60 帧） */
            if (frame_count % 60 == 0) {
                int ec = ce_client_net_entity_count(net);
                printf("\n[Client] Local pos: (%.2f, %.2f, %.2f), visible=%d\n",
                       local_x, 0.0f, local_z, ec);
                for (int i = 0; i < ec && i < 5; i++) {
                    const CeClientEntity* e = ce_client_net_get_entity(net, i);
                    if (e) {
                        printf("  Entity %u: (%.2f, %.2f, %.2f)\n",
                               e->entity_id, e->x, e->y, e->z);
                    }
                }
                if (ec > 5) {
                    printf("  ... and %d more\n", ec - 5);
                }
            }
        } else if (frame_count % 60 == 0) {
            printf("\n[Client] Local pos: (%.2f, %.2f, %.2f), network disconnected\n",
                   local_x, 0.0f, local_z);
        }

        /* 引擎更新 */
        ce_update();

        /* 渲染：按网络收到的实体位置绘制球体 */
        rhi_begin_frame(rhi, ce_color_rgba(0.1f, 0.1f, 0.15f, 1.0f));
        if (net && ce_client_net_is_connected(net)) {
            int ec = ce_client_net_entity_count(net);
            int rendered_self = 0;
            for (int i = 0; i < ec; i++) {
                const CeClientEntity* e = ce_client_net_get_entity(net, i);
                if (!e) continue;

                if (e->entity_id == net->entity_id) {
                    rendered_self = 1;
                }

                /* 将世界坐标压缩到相机前方的可见范围 */
                CeVec3 pos = {
                    e->x * 0.05f,
                    e->y * 0.05f,
                    e->z * 0.05f
                };
                CeMat4 proj = ce_mat4_perspective(ce_radians(45.0f),
                                                  1280.0f / 720.0f,
                                                  0.1f, 100.0f);
                CeMat4 view = ce_mat4_look_at(
                    (CeVec3){0.0f, 0.0f, 3.0f},
                    (CeVec3){0.0f, 0.0f, 0.0f},
                    (CeVec3){0.0f, 1.0f, 0.0f}
                );
                CeMat4 model = ce_mat4_translation(pos);
                CeMat4 mvp = ce_mat4_mul(proj, ce_mat4_mul(view, model));
                rhi_set_uniform_mat4(rhi, "mvp", &mvp);
                rhi_draw(rhi, 0, 0);
            }

            /* 如果服务端还没回传自己的实体，就先画本地预测球 */
            if (ec == 0 || !rendered_self) {
                CeVec3 pos = {local_x * 0.05f, 0.0f, local_z * 0.05f};
                CeMat4 proj = ce_mat4_perspective(ce_radians(45.0f),
                                                  1280.0f / 720.0f,
                                                  0.1f, 100.0f);
                CeMat4 view = ce_mat4_look_at(
                    (CeVec3){0.0f, 0.0f, 3.0f},
                    (CeVec3){0.0f, 0.0f, 0.0f},
                    (CeVec3){0.0f, 1.0f, 0.0f}
                );
                CeMat4 model = ce_mat4_translation(pos);
                CeMat4 mvp = ce_mat4_mul(proj, ce_mat4_mul(view, model));
                rhi_set_uniform_mat4(rhi, "mvp", &mvp);
                rhi_draw(rhi, 0, 0);
            }
        } else {
            CeVec3 pos = {local_x * 0.05f, 0.0f, local_z * 0.05f};
            CeMat4 proj = ce_mat4_perspective(ce_radians(45.0f),
                                              1280.0f / 720.0f,
                                              0.1f, 100.0f);
            CeMat4 view = ce_mat4_look_at(
                (CeVec3){0.0f, 0.0f, 3.0f},
                (CeVec3){0.0f, 0.0f, 0.0f},
                (CeVec3){0.0f, 1.0f, 0.0f}
            );
            CeMat4 model = ce_mat4_translation(pos);
            CeMat4 mvp = ce_mat4_mul(proj, ce_mat4_mul(view, model));
            rhi_set_uniform_mat4(rhi, "mvp", &mvp);
            rhi_draw(rhi, 0, 0);
        }
        rhi_end_frame(rhi);
        rhi_present(rhi);

        /* 每 60 帧打印 FPS */
        if (frame_count % 60 == 0) {
            printf("\rFPS: %.1f, Frame: %lu, dt: %.3f ms%s",
                   ce_frame_timer_fps(timer),
                   frame_count,
                   dt * 1000.0,
                   (net && ce_client_net_is_connected(net)) ? " [NET]" : "");
            fflush(stdout);
        }
    }

    printf("\n\nShutting down...\n");

    /* ---- 断开网络 ---- */
    if (net) {
        ce_client_net_disconnect(net);
        net = NULL;
    }

    rhi_destroy_device(rhi);
    ce_frame_timer_destroy(timer);
    ce_shutdown();

    printf("Client shut down cleanly.\n");
    return 0;
}
