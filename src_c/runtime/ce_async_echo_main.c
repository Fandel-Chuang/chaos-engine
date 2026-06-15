/*
 * ChaosEngine io_uring 异步 Echo 服务器测试
 *
 * 使用 ce_async_io 层实现 TCP echo 服务器，
 * 验证 io_uring 后端的 accept/recv/send 功能。
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

#define TEST_PORT    7778
#define BUFFER_SIZE  4096
#define MAX_CLIENTS  32

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

int main(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 初始化引擎 */
    CeEngineConfig config = {
        .app_name = "ChaosEngine-AsyncEcho",
        .window_width = 0, .window_height = 0,
        .fullscreen = CE_FALSE, .vsync = CE_FALSE,
        .log_level = CE_LOG_INFO,
        .log_file_path = "logs/chaos_async_echo.log"
    };
    ce_init(&config);
    ce_net_init();

    /* 创建监听 socket */
    CeSocket* listen_sock = ce_socket_create_tcp();
    CeNetAddress addr;
    memset(&addr, 0, sizeof(addr));
    strncpy(addr.host, "0.0.0.0", sizeof(addr.host) - 1);
    addr.port = TEST_PORT;
    ce_socket_bind(listen_sock, &addr);
    ce_socket_listen(listen_sock, 5);
    ce_socket_set_nonblocking(listen_sock, CE_TRUE);

    int listen_fd = -1;
    /* 从 CeSocket 获取 fd（内部实现依赖） */
    {
        /* CeSocket 结构: int fd; CeSocketType type; CeBool valid; */
        int* pfd = (int*)listen_sock;
        listen_fd = *pfd;
    }

    /* 初始化异步 I/O */
    CeAsyncContext* async = ce_async_init(256);
    printf("========================================\n");
    printf("  ChaosEngine Async Echo Server\n");
    printf("  Backend: %s\n", ce_async_backend_name());
    printf("  ZCRX: %s\n", ce_async_has_zcrx() ? "YES" : "NO");
    printf("  Listening on port %d\n", TEST_PORT);
    printf("========================================\n\n");

    /* 客户端管理 */
    ClientCtx clients[MAX_CLIENTS];
    memset(clients, 0, sizeof(clients));

    /* 提交初始 accept */
    ce_async_accept(async, listen_fd, NULL);

    int total_echoes = 0;

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
                    printf("[+] Client accepted (fd=%d, slot=%d)\n", ev->client_fd, slot);
                    ce_async_recv(async, ev->client_fd, clients[slot].buf, BUFFER_SIZE, (void*)(intptr_t)slot);
                } else {
                    printf("[-] Client rejected (full)\n");
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
                    ce_async_send(async, ev->fd, clients[slot].buf, ev->nbytes, (void*)(intptr_t)slot);
                    total_echoes++;
                } else {
                    /* 客户端断开 */
                    printf("[-] Client disconnected (fd=%d, slot=%d)\n", ev->fd, slot);
                    close(ev->fd);
                    clients[slot].connected = 0;
                }
                break;
            }

            case CE_ASYNC_SEND: {
                int slot = (int)(intptr_t)ev->user_data;
                if (slot < 0 || slot >= MAX_CLIENTS) break;

                if (ev->nbytes > 0) {
                    /* 继续接收 */
                    ce_async_recv(async, ev->fd, clients[slot].buf, BUFFER_SIZE, (void*)(intptr_t)slot);
                }
                break;
            }

            default:
                break;
            }
        }

        ce_update();
    }

    printf("\nShutting down... (total echoes: %d)\n", total_echoes);

    /* 清理 */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].connected) close(clients[i].fd);
    }
    ce_socket_close(listen_sock);
    ce_async_shutdown(async);
    ce_net_shutdown();
    ce_shutdown();

    printf("Async echo server shut down cleanly.\n");
    return 0;
}
