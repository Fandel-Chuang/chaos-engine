/*
 * ChaosEngine TCP 回显服务器示例
 *
 * 启动 TCP 服务器，监听 7777 端口，
 * 接受连接后，将收到的数据原样返回（echo），
 * 按 Ctrl+C 退出。
 */

#define _POSIX_C_SOURCE 200112L
#include "public_api/chaos_engine.h"
#include "network/ce_network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define ECHO_PORT    7777
#define BUFFER_SIZE  4096
#define MAX_CLIENTS  32

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
        .app_name      = "ChaosEngine-EchoServer",
        .window_width  = 0,
        .window_height = 0,
        .fullscreen    = CE_FALSE,
        .vsync         = CE_FALSE,
        .log_level     = CE_LOG_INFO,
        .log_file_path = "logs/chaos_echo_server.log"
    };

    if (ce_init(&config) != CE_OK) {
        fprintf(stderr, "Failed to initialize ChaosEngine\n");
        return 1;
    }

    /* 初始化网络层 */
    if (ce_net_init() != CE_OK) {
        fprintf(stderr, "Failed to initialize network layer\n");
        ce_shutdown();
        return 1;
    }

    /* 创建监听 socket */
    CeSocket* listen_sock = ce_socket_create_tcp();
    if (!listen_sock) {
        fprintf(stderr, "Failed to create TCP socket\n");
        ce_net_shutdown();
        ce_shutdown();
        return 1;
    }

    /* 绑定地址 */
    CeNetAddress addr;
    memset(&addr, 0, sizeof(addr));
    strncpy(addr.host, "0.0.0.0", sizeof(addr.host) - 1);
    addr.port = ECHO_PORT;

    if (ce_socket_bind(listen_sock, &addr) != CE_OK) {
        fprintf(stderr, "Failed to bind to port %d\n", ECHO_PORT);
        ce_socket_close(listen_sock);
        ce_net_shutdown();
        ce_shutdown();
        return 1;
    }

    /* 开始监听 */
    if (ce_socket_listen(listen_sock, 5) != CE_OK) {
        fprintf(stderr, "Failed to listen on port %d\n", ECHO_PORT);
        ce_socket_close(listen_sock);
        ce_net_shutdown();
        ce_shutdown();
        return 1;
    }

    /* 设置非阻塞模式，以便在主循环中检查 Ctrl+C */
    ce_socket_set_nonblocking(listen_sock, CE_TRUE);

    printf("========================================\n");
    printf("  ChaosEngine TCP Echo Server v0.1.0\n");
    printf("  Listening on port %d\n", ECHO_PORT);
    printf("  Press Ctrl+C to exit.\n");
    printf("========================================\n\n");

    /* 客户端连接数组 */
    CeSocket* clients[MAX_CLIENTS];
    int client_count = 0;
    memset(clients, 0, sizeof(clients));

    char buffer[BUFFER_SIZE];

    /* 主循环 */
    while (g_running) {
        /* 接受新连接 */
        if (client_count < MAX_CLIENTS) {
            CeNetAddress client_addr;
            memset(&client_addr, 0, sizeof(client_addr));
            CeSocket* client = ce_socket_accept(listen_sock, &client_addr);
            if (client) {
                ce_socket_set_nonblocking(client, CE_TRUE);
                clients[client_count++] = client;

                char addr_str[64];
                ce_net_addr_to_string(&client_addr, addr_str, sizeof(addr_str));
                printf("[+] Client connected: %s (total: %d)\n",
                       addr_str, client_count);
            }
        }

        /* 处理每个客户端的数据 */
        for (int i = 0; i < client_count; i++) {
            if (!clients[i]) continue;

            int n = ce_socket_recv(clients[i], buffer, BUFFER_SIZE - 1);
            if (n > 0) {
                buffer[n] = '\0';
                printf("[<] Received %d bytes: %s", n, buffer);

                /* 回显数据 */
                int sent = ce_socket_send(clients[i], buffer, (size_t)n);
                if (sent > 0) {
                    printf("[>] Echoed %d bytes back\n", sent);
                }
            } else if (n == 0) {
                /* 客户端断开连接 */
                printf("[-] Client disconnected (slot %d, remaining: %d)\n",
                       i, client_count - 1);
                ce_socket_close(clients[i]);
                clients[i] = NULL;
            }
            /* n < 0 表示暂无数据（非阻塞模式），正常继续 */
        }

        /* 压缩客户端数组：移除已断开的连接 */
        int write_idx = 0;
        for (int i = 0; i < client_count; i++) {
            if (clients[i]) {
                clients[write_idx++] = clients[i];
            }
        }
        client_count = write_idx;

        /* 更新引擎（处理日志等） */
        ce_update();
    }

    printf("\nShutting down...\n");

    /* 关闭所有客户端连接 */
    for (int i = 0; i < client_count; i++) {
        if (clients[i]) {
            ce_socket_close(clients[i]);
        }
    }

    /* 关闭监听 socket */
    ce_socket_close(listen_sock);

    /* 清理网络层和引擎 */
    ce_net_shutdown();
    ce_shutdown();

    printf("Echo server shut down cleanly.\n");
    return 0;
}
