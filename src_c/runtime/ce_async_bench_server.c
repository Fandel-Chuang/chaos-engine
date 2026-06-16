/*
 * ChaosEngine io_uring 异步 I/O 性能基准测试服务器
 *
 * 与 ce_async_echo_main.c 类似，但支持：
 *   - 可配置最大客户端数（默认 2048，支持 1000+ 并发）
 *   - 可配置端口（环境变量 CHAOS_BENCH_PORT）
 *   - 静默模式（减少日志输出对性能的影响）
 *   - 统计计数器（总 echo 次数）
 *
 * 编译为独立可执行文件 chaos_async_bench。
 */

#define _POSIX_C_SOURCE 200112L
#include "public_api/chaos_engine.h"
#include "network/ce_network.h"
#include "network/ce_async_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

#define DEFAULT_PORT    7778
#define BUFFER_SIZE     4096
#define MAX_CLIENTS     2048

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/** 每个客户端连接的上下文 */
typedef struct ClientCtx {
    int   fd;
    char  buf[BUFFER_SIZE];
    int   connected;
} ClientCtx;

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 读取端口（环境变量或命令行参数） */
    int port = DEFAULT_PORT;
    const char* env_port = getenv("CHAOS_BENCH_PORT");
    if (env_port) {
        port = atoi(env_port);
        if (port <= 0 || port > 65535) port = DEFAULT_PORT;
    }
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) port = DEFAULT_PORT;
    }

    /* 静默模式 */
    int quiet = (getenv("CHAOS_BENCH_QUIET") != NULL);

    /* 初始化引擎 */
    CeEngineConfig config = {
        .app_name = "ChaosEngine-BenchServer",
        .window_width = 0, .window_height = 0,
        .fullscreen = CE_FALSE, .vsync = CE_FALSE,
        .log_level = quiet ? CE_LOG_ERROR : CE_LOG_INFO,
        .log_file_path = "logs/chaos_bench_server.log"
    };
    ce_init(&config);
    ce_net_init();

    /* 创建监听 socket */
    CeSocket* listen_sock = ce_socket_create_tcp();
    CeNetAddress addr;
    memset(&addr, 0, sizeof(addr));
    strncpy(addr.host, "0.0.0.0", sizeof(addr.host) - 1);
    addr.port = port;
    ce_socket_bind(listen_sock, &addr);
    ce_socket_listen(listen_sock, 128);  /* 更大的 backlog */
    ce_socket_set_nonblocking(listen_sock, CE_TRUE);

    int listen_fd = -1;
    {
        int* pfd = (int*)listen_sock;
        listen_fd = *pfd;
    }

    /* 初始化异步 I/O */
    CeAsyncContext* async = ce_async_init(512);  /* 更大的队列深度 */

    if (!quiet) {
        printf("========================================\n");
        printf("  ChaosEngine Benchmark Server\n");
        printf("  Backend: %s\n", ce_async_backend_name());
        printf("  ZCRX: %s\n", ce_async_has_zcrx() ? "YES" : "NO");
        printf("  Max Clients: %d\n", MAX_CLIENTS);
        printf("  Listening on port %d\n", port);
        printf("========================================\n\n");
    } else {
        /* 输出机器可读的启动信息 */
        printf("BENCH_SERVER_READY|backend=%s|port=%d|max_clients=%d\n",
               ce_async_backend_name(), port, MAX_CLIENTS);
        fflush(stdout);
    }

    /* 客户端管理 */
    ClientCtx* clients = (ClientCtx*)calloc(MAX_CLIENTS, sizeof(ClientCtx));
    if (!clients) {
        fprintf(stderr, "FATAL: failed to allocate client array\n");
        return 1;
    }

    /* 提交初始 accept */
    ce_async_accept(async, listen_fd, NULL);

    int total_echoes = 0;
    int peak_clients = 0;
    int current_clients = 0;

    /* 事件循环 */
    while (g_running) {
        ce_async_submit(async);

        int n = ce_async_wait(async, 1, 100);  /* 100ms 超时 */
        if (n < 0) break;

        for (int i = 0; i < n; i++) {
            const CeAsyncEvent* ev = ce_async_get_event(async, i);

            switch (ev->type) {
            case CE_ASYNC_ACCEPT: {
                if (ev->client_fd < 0) break;

                /* 找空闲槽位 */
                int slot = -1;
                for (int j = 0; j < MAX_CLIENTS; j++) {
                    if (!clients[j].connected) { slot = j; break; }
                }

                if (slot >= 0) {
                    clients[slot].fd        = ev->client_fd;
                    clients[slot].connected = 1;
                    current_clients++;
                    if (current_clients > peak_clients) {
                        peak_clients = current_clients;
                    }
                    ce_async_recv(async, ev->client_fd, clients[slot].buf,
                                  BUFFER_SIZE, (void*)(intptr_t)slot);
                } else {
                    /* 槽位已满，拒绝连接 */
                    close(ev->client_fd);
                }

                /* 继续 accept */
                ce_async_accept(async, listen_fd, NULL);
                break;
            }

            case CE_ASYNC_RECV: {
                int slot = (int)(intptr_t)ev->user_data;
                if (slot < 0 || slot >= MAX_CLIENTS) break;

                if (ev->nbytes > 0) {
                    /* echo 回去 */
                    ce_async_send(async, ev->fd, clients[slot].buf,
                                  ev->nbytes, (void*)(intptr_t)slot);
                    total_echoes++;
                } else {
                    /* 客户端断开 */
                    close(ev->fd);
                    clients[slot].connected = 0;
                    current_clients--;
                }
                break;
            }

            case CE_ASYNC_SEND: {
                int slot = (int)(intptr_t)ev->user_data;
                if (slot < 0 || slot >= MAX_CLIENTS) break;

                if (ev->nbytes > 0) {
                    /* 继续接收 */
                    ce_async_recv(async, ev->fd, clients[slot].buf,
                                  BUFFER_SIZE, (void*)(intptr_t)slot);
                }
                break;
            }

            default:
                break;
            }
        }

        ce_update();
    }

    if (!quiet) {
        printf("\nShutting down...\n");
        printf("  Total echoes: %d\n", total_echoes);
        printf("  Peak clients: %d\n", peak_clients);
    }

    /* 清理 */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].connected) close(clients[i].fd);
    }
    free(clients);
    ce_socket_close(listen_sock);
    ce_async_shutdown(async);
    ce_net_shutdown();
    ce_shutdown();

    if (!quiet) {
        printf("Benchmark server shut down cleanly.\n");
    }
    return 0;
}
