/*
 * ChaosEngine C Gateway - 进程入口
 *
 * 纯 C 实现的 Gateway 进程，io_uring 事件驱动。
 * 与 ce_gateway_main.c 功能一致，但链接 engine_gateway 静态库而非
 * 直接编译 ce_gateway.c。
 *
 * 用法:
 *   ./chaos_gateway_c [--port PORT] [--backend HOST:PORT] [--max-connections N]
 *
 * 默认:
 *   监听 0.0.0.0:9000
 *   后端 127.0.0.1:7777 (chaos_server)
 *   最大客户端 10000
 */

#define _POSIX_C_SOURCE 200112L

#include "gateway/ce_gateway.h"
#include "public_api/chaos_engine.h"
#include "public_api/ce_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* ================================================================
 * 全局 Gateway 句柄（用于信号处理）
 * ================================================================ */

static CeGateway* g_gateway = NULL;

static void signal_handler(int sig)
{
    (void)sig;
    if (g_gateway) {
        ce_gateway_stop(g_gateway);
    }
}

/* ================================================================
 * 命令行参数解析
 * ================================================================ */

static void usage(const char* prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n\n"
        "Options:\n"
        "  --port PORT               TCP+KCP listen port (default: %d)\n"
        "  --backend HOST:PORT       Backend Game service address\n"
        "                            (default: 127.0.0.1:7777, can repeat)\n"
        "  --max-connections N       Max client connections (default: %d)\n"
        "  --heartbeat-interval MS   Heartbeat interval in ms (default: %d)\n"
        "  --heartbeat-timeout MS    Heartbeat timeout in ms (default: %d)\n"
        "  --no-kcp                  Disable KCP protocol (TCP only)\n"
        "  --help, -h                Show this help\n",
        prog,
        CE_GW_DEFAULT_PORT,
        CE_GW_DEFAULT_MAX_CONNS,
        CE_GW_HEARTBEAT_INTERVAL_MS,
        CE_GW_HEARTBEAT_TIMEOUT_MS);
}

int main(int argc, char** argv)
{
    /* 默认配置 */
    CeGatewayConfig gw_cfg;
    memset(&gw_cfg, 0, sizeof(gw_cfg));
    gw_cfg.port = CE_GW_DEFAULT_PORT;
    gw_cfg.max_connections = CE_GW_DEFAULT_MAX_CONNS;
    gw_cfg.heartbeat_interval_ms = CE_GW_HEARTBEAT_INTERVAL_MS;
    gw_cfg.heartbeat_timeout_ms = CE_GW_HEARTBEAT_TIMEOUT_MS;
    gw_cfg.kcp_enabled = 1;  /* 默认启用 KCP */

    /* 后端地址列表 (最多 CE_GW_MAX_BACKENDS 个) */
    const char* backend_hosts[CE_GW_MAX_BACKENDS];
    int         backend_ports[CE_GW_MAX_BACKENDS];
    int         backend_count = 0;

    /* 默认后端 */
    backend_hosts[0] = "127.0.0.1";
    backend_ports[0] = 7777;
    backend_count = 1;

    /* 解析命令行参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            gw_cfg.port = atoi(argv[++i]);
            if (gw_cfg.port <= 0 || gw_cfg.port > 65535)
                gw_cfg.port = CE_GW_DEFAULT_PORT;
        } else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            char* arg = argv[++i];
            char* colon = strrchr(arg, ':');
            if (colon) {
                *colon = '\0';
                if (backend_count < CE_GW_MAX_BACKENDS) {
                    if (backend_count == 1 &&
                        strcmp(backend_hosts[0], "127.0.0.1") == 0 &&
                        backend_ports[0] == 7777) {
                        /* 替换默认后端 */
                        backend_hosts[0] = arg;
                        backend_ports[0] = atoi(colon + 1);
                        if (backend_ports[0] <= 0) backend_ports[0] = 7777;
                    } else {
                        backend_hosts[backend_count] = arg;
                        backend_ports[backend_count] = atoi(colon + 1);
                        if (backend_ports[backend_count] <= 0)
                            backend_ports[backend_count] = 7777;
                        backend_count++;
                    }
                }
            }
        } else if (strcmp(argv[i], "--max-connections") == 0 && i + 1 < argc) {
            gw_cfg.max_connections = atoi(argv[++i]);
            if (gw_cfg.max_connections <= 0)
                gw_cfg.max_connections = CE_GW_DEFAULT_MAX_CONNS;
        } else if (strcmp(argv[i], "--heartbeat-interval") == 0 && i + 1 < argc) {
            gw_cfg.heartbeat_interval_ms = atoi(argv[++i]);
            if (gw_cfg.heartbeat_interval_ms <= 0)
                gw_cfg.heartbeat_interval_ms = CE_GW_HEARTBEAT_INTERVAL_MS;
        } else if (strcmp(argv[i], "--heartbeat-timeout") == 0 && i + 1 < argc) {
            gw_cfg.heartbeat_timeout_ms = atoi(argv[++i]);
            if (gw_cfg.heartbeat_timeout_ms <= 0)
                gw_cfg.heartbeat_timeout_ms = CE_GW_HEARTBEAT_TIMEOUT_MS;
        } else if (strcmp(argv[i], "--no-kcp") == 0) {
            gw_cfg.kcp_enabled = 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* 初始化引擎 */
    CeEngineConfig engine_cfg = {
        .app_name      = "ChaosEngine-Gateway-C",
        .window_width  = 0,
        .window_height = 0,
        .fullscreen    = CE_FALSE,
        .vsync         = CE_FALSE,
        .log_level     = CE_LOG_INFO,
        .log_file_path = "logs/chaos_gateway_c.log"
    };

    if (ce_init(&engine_cfg) != CE_OK) {
        fprintf(stderr, "[Gateway] Engine init failed\n");
        return 1;
    }

    /* 安装信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* 创建 Gateway */
    g_gateway = ce_gateway_create(&gw_cfg);
    if (!g_gateway) {
        fprintf(stderr, "[Gateway] Failed to create gateway instance\n");
        ce_shutdown();
        return 1;
    }

    /* 添加后端服务 */
    for (int i = 0; i < backend_count; i++) {
        if (ce_gateway_add_backend(g_gateway, backend_hosts[i],
                                   backend_ports[i]) != CE_OK) {
            fprintf(stderr, "[Gateway] Failed to add backend %s:%d\n",
                    backend_hosts[i], backend_ports[i]);
            ce_gateway_destroy(g_gateway);
            ce_shutdown();
            return 1;
        }
    }

    printf("========================================\n");
    printf("  ChaosEngine C Gateway (io_uring)\n");
    printf("  Backend: %s\n", ce_async_backend_name());
    printf("  Listen:  0.0.0.0:%d (%s)\n", gw_cfg.port,
           gw_cfg.kcp_enabled ? "TCP+KCP" : "TCP only");
    for (int i = 0; i < backend_count; i++) {
        printf("  Backend[%d]: %s:%d\n", i, backend_hosts[i], backend_ports[i]);
    }
    printf("  MaxConns: %d\n", gw_cfg.max_connections);
    printf("  HB: interval=%dms timeout=%dms\n",
           gw_cfg.heartbeat_interval_ms, gw_cfg.heartbeat_timeout_ms);
    printf("  Press Ctrl+C to exit.\n");
    printf("========================================\n\n");
    fflush(stdout);

    /* 运行事件循环（阻塞） */
    ce_gateway_run(g_gateway);

    /* 清理 */
    printf("\n[Gateway] Shutting down...\n");
    ce_gateway_destroy(g_gateway);
    g_gateway = NULL;

    ce_shutdown();
    printf("[Gateway] Shutdown complete.\n");
    return 0;
}
