/*
 * ChaosEngine Gateway 进程入口
 *
 * 基于 io_uring (ce_async_io) 的 C 原生网络网关。
 * 不再使用 Lua 协程 + LuaSocket + select 的事件循环。
 *
 * 功能：
 *   - Port 9000: TCP 客户端接入 (io_uring 异步 accept/recv/send)
 *   - 二进制协议帧解析与消息路由
 *   - 心跳检测 (PING/PONG) 与超时清理
 *   - 后端 Game 服务连接管理
 *
 * 用法:
 *   ./chaos_gateway [--port PORT] [--backend HOST:PORT] [--max-connections N]
 */

#define _POSIX_C_SOURCE 200112L
#include "public_api/chaos_engine.h"
#include "gateway/ce_gateway.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n\n"
        "Options:\n"
        "  --port PORT               TCP+KCP listen port (default: 9000)\n"
        "  --backend HOST:PORT       Backend Game service address (default: 127.0.0.1:7777)\n"
        "  --max-connections N       Max client connections (default: 10000)\n"
        "  --heartbeat-interval MS   Heartbeat interval in ms (default: 30000)\n"
        "  --heartbeat-timeout MS    Heartbeat timeout in ms (default: 90000)\n"
        "  --no-kcp                  Disable KCP protocol (TCP only)\n"
        "  --no-ws                   Disable WebSocket protocol\n"
        "  --help, -h                Show this help\n",
        prog);
}

int main(int argc, char** argv) {
    /* 默认配置 */
    CeGatewayConfig gw_cfg;
    memset(&gw_cfg, 0, sizeof(gw_cfg));
    gw_cfg.port = CE_GW_DEFAULT_PORT;
    gw_cfg.max_connections = CE_GW_DEFAULT_MAX_CONNS;
    gw_cfg.heartbeat_interval_ms = CE_GW_HEARTBEAT_INTERVAL_MS;
    gw_cfg.heartbeat_timeout_ms = CE_GW_HEARTBEAT_TIMEOUT_MS;
    gw_cfg.kcp_enabled = 1;  /* 默认启用 KCP */
    gw_cfg.ws_enabled = 1;   /* 默认启用 WebSocket */

    /* 后端地址 (可被命令行覆盖) */
    const char* backend_host = "127.0.0.1";
    int         backend_port = 7777;

    /* 解析命令行参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            gw_cfg.port = atoi(argv[++i]);
            if (gw_cfg.port <= 0 || gw_cfg.port > 65535) gw_cfg.port = CE_GW_DEFAULT_PORT;
        } else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            char* arg = argv[++i];
            char* colon = strrchr(arg, ':');
            if (colon) {
                *colon = '\0';
                backend_host = arg;
                backend_port = atoi(colon + 1);
                if (backend_port <= 0) backend_port = 7777;
            }
        } else if (strcmp(argv[i], "--max-connections") == 0 && i + 1 < argc) {
            gw_cfg.max_connections = atoi(argv[++i]);
            if (gw_cfg.max_connections <= 0) gw_cfg.max_connections = CE_GW_DEFAULT_MAX_CONNS;
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
        } else if (strcmp(argv[i], "--no-ws") == 0) {
            gw_cfg.ws_enabled = 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* 初始化引擎 */
    CeEngineConfig cfg = {
        .app_name = "ChaosEngine-Gateway",
        .window_width = 0, .window_height = 0,
        .fullscreen = CE_FALSE, .vsync = CE_FALSE,
        .log_level = CE_LOG_INFO,
        .log_file_path = "logs/chaos_gateway.log"
    };
    if (ce_init(&cfg) != CE_OK) {
        fprintf(stderr, "[Gateway] Engine init failed\n");
        return 1;
    }

    /* 创建 Gateway 实例 */
    CeGateway* gw = ce_gateway_create(&gw_cfg);
    if (!gw) {
        fprintf(stderr, "[Gateway] Failed to create gateway instance\n");
        ce_shutdown();
        return 1;
    }

    /* 添加后端服务 */
    if (ce_gateway_add_backend(gw, backend_host, backend_port) != CE_OK) {
        fprintf(stderr, "[Gateway] Failed to add backend %s:%d\n", backend_host, backend_port);
        ce_gateway_destroy(gw);
        ce_shutdown();
        return 1;
    }

    /* 打印启动信息 */
    printf("========================================\n");
    printf("  ChaosEngine Gateway (io_uring)\n");
    printf("  Backend: %s\n", ce_async_backend_name());
    {
        /* 构造协议描述字符串 */
        char proto_desc[64];
        int pw = 0;
        /* TCP 始终启用 (WebSocket 复用 TCP socket) */
        pw += snprintf(proto_desc + pw, sizeof(proto_desc) - pw, "TCP");
        if (gw_cfg.kcp_enabled)
            pw += snprintf(proto_desc + pw, sizeof(proto_desc) - pw, "+KCP");
        if (gw_cfg.ws_enabled)
            pw += snprintf(proto_desc + pw, sizeof(proto_desc) - pw, "+WebSocket");
        printf("  Listen:  0.0.0.0:%d (%s)\n", gw_cfg.port, proto_desc);
    }
    printf("  Backend: %s:%d\n", backend_host, backend_port);
    printf("  MaxConns: %d\n", gw_cfg.max_connections);
    printf("  HB: interval=%dms timeout=%dms\n",
           gw_cfg.heartbeat_interval_ms, gw_cfg.heartbeat_timeout_ms);
    printf("========================================\n");
    fflush(stdout);

    /* 运行事件循环 (阻塞直到 SIGINT/SIGTERM) */
    ce_gateway_run(gw);

    /* 清理 */
    ce_gateway_destroy(gw);
    ce_shutdown();

    printf("[Gateway] Shutdown complete.\n");
    return 0;
}
