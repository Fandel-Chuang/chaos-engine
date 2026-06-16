/*
 * ChaosEngine TCP 回显服务器示例
 *
 * 启动 TCP 服务器，监听 7777 端口，
 * 接受连接后，将收到的数据原样返回（echo），
 * 按 Ctrl+C 退出。
 *
 * 支持两种 I/O 模式：
 *   CHAOS_HAS_IO_URING  → io_uring 事件驱动（高性能，零拷贝）
 *   未定义              → POSIX 非阻塞轮询（fallback）
 */

#define _POSIX_C_SOURCE 200112L
#include "public_api/chaos_engine.h"
#include "network/ce_network.h"
#include "server/ce_cell.h"
#include "server/ce_aoi.h"
#include "admin_ipc/ce_admin_ipc.h"
#include "sync/ce_sync.h"
#include "save/ce_save.h"
#include "dbproxy/ce_dbproxy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#ifdef CHAOS_HAS_IO_URING
#include "network/ce_async_io.h"
#endif

#define ECHO_PORT    7777
#define BUFFER_SIZE  4096
#define MAX_CLIENTS  32
#define TICK_RATE    60   /* 固定 60Hz 主循环 */

#define DEFAULT_ADMIN_SOCK  "/tmp/chaos_admin.sock"

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ================================================================
 * io_uring 事件驱动模式
 * ================================================================ */
#ifdef CHAOS_HAS_IO_URING

/** 每个客户端连接的上下文 */
typedef struct ClientCtx {
    int   fd;
    char  buf[BUFFER_SIZE];
    int   connected;
} ClientCtx;

int main(int argc, char** argv) {
    int admin_enabled = 0;
    const char* admin_sock_path = DEFAULT_ADMIN_SOCK;
    CeAdminIpc* admin_ipc = NULL;

    /* DBProxy / Sync / Save 配置（可通过命令行覆盖） */
    const char* dbproxy_host = "192.168.1.100";
    int         dbproxy_port = 9003;
    const char* backup_host  = "127.0.0.1";
    int         backup_port  = 9003;
    int         save_interval = 300;

    /* 解析命令行参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--admin") == 0) {
            admin_enabled = 1;
        } else if (strcmp(argv[i], "--admin-sock") == 0 && i + 1 < argc) {
            admin_sock_path = argv[++i];
        } else if (strcmp(argv[i], "--dbproxy-host") == 0 && i + 1 < argc) {
            dbproxy_host = argv[++i];
        } else if (strcmp(argv[i], "--dbproxy-port") == 0 && i + 1 < argc) {
            dbproxy_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--backup-host") == 0 && i + 1 < argc) {
            backup_host = argv[++i];
        } else if (strcmp(argv[i], "--backup-port") == 0 && i + 1 < argc) {
            backup_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--save-interval") == 0 && i + 1 < argc) {
            save_interval = atoi(argv[++i]);
        }
    }

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

    /* 初始化服务端空间子系统（AOI + Cell） */
    if (ce_cell_init(10000.0f, 10000.0f, 500.0f, 500.0f, 150, 20) != CE_OK) {
        fprintf(stderr, "Failed to initialize Cell manager\n");
        ce_net_shutdown();
        ce_shutdown();
        return 1;
    }
    printf("[Server] Cell manager initialized: 10000x10000 world, 500x500 cells\n");

    /* ---- DBProxy 连接初始化 ---- */
    CeDbproxyConfig dbproxy_cfg = {
        .primary_host = dbproxy_host,
        .primary_port = dbproxy_port,
        .backup_host  = backup_host,
        .backup_port  = backup_port
    };
    CeDbproxyContext* dbproxy = ce_dbproxy_connect(&dbproxy_cfg);
    if (!dbproxy) {
        fprintf(stderr, "[WARN] DBProxy connection failed, continuing without DBProxy\n");
    } else {
        printf("[Server] DBProxy connected: %s\n", ce_dbproxy_current_endpoint(dbproxy));
    }

    /* ---- Sync 初始化 ---- */
    CeSyncConfig sync_cfg = {
        .dbproxy_host = dbproxy_host,
        .dbproxy_port = 9001,
        .backup_host  = backup_host,
        .backup_port  = 9001
    };
    CeSyncContext* sync = ce_sync_init(&sync_cfg);
    if (!sync) {
        fprintf(stderr, "[WARN] Sync init failed, continuing without sync\n");
    } else {
        printf("[Server] Sync initialized: %s:%d\n", dbproxy_host, 9001);
    }

    /* ---- Save 初始化 ---- */
    CeSaveConfig save_cfg = {
        .save_interval_sec = save_interval,
        .full_save_every_n = 6
    };
    CeSaveContext* save = ce_save_init(dbproxy, &save_cfg);
    if (!save) {
        fprintf(stderr, "[WARN] Save init failed, continuing without save\n");
    } else {
        printf("[Server] Save initialized: interval=%ds, full_every=%d\n",
               save_interval, 6);
    }

    /* 启动 Admin IPC（如果 --admin 传入） */
    if (admin_enabled) {
        admin_ipc = ce_admin_ipc_start(admin_sock_path);
        if (admin_ipc) {
            printf("Admin IPC: enabled at %s\n", admin_sock_path);
        } else {
            fprintf(stderr, "Admin IPC: failed to start at %s\n", admin_sock_path);
        }
    } else {
        printf("Admin IPC: disabled\n");
    }

    /* 创建监听 socket */
    CeSocket* listen_sock = ce_socket_create_tcp();
    if (!listen_sock) {
        fprintf(stderr, "Failed to create TCP socket\n");
        if (admin_ipc) ce_admin_ipc_stop(admin_ipc);
        ce_net_shutdown();
        ce_cell_shutdown();
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
        if (admin_ipc) ce_admin_ipc_stop(admin_ipc);
        ce_net_shutdown();
        ce_cell_shutdown();
        ce_shutdown();
        return 1;
    }

    /* 开始监听 */
    if (ce_socket_listen(listen_sock, 5) != CE_OK) {
        fprintf(stderr, "Failed to listen on port %d\n", ECHO_PORT);
        ce_socket_close(listen_sock);
        if (admin_ipc) ce_admin_ipc_stop(admin_ipc);
        ce_net_shutdown();
        ce_cell_shutdown();
        ce_shutdown();
        return 1;
    }

    /* 设置非阻塞模式 */
    ce_socket_set_nonblocking(listen_sock, CE_TRUE);

    /* 从 CeSocket 提取 fd（CeSocket 首成员为 int fd） */
    int listen_fd = *(int*)listen_sock;

    /* 初始化 io_uring 异步上下文 */
    CeAsyncContext* async = ce_async_init(256);
    if (!async) {
        fprintf(stderr, "Failed to initialize io_uring\n");
        ce_socket_close(listen_sock);
        if (admin_ipc) ce_admin_ipc_stop(admin_ipc);
        ce_net_shutdown();
        ce_cell_shutdown();
        ce_shutdown();
        return 1;
    }

    printf("========================================\n");
    printf("  ChaosEngine TCP Echo Server v0.2.0\n");
    printf("  Backend: %s\n", ce_async_backend_name());
    printf("  Listening on port %d\n", ECHO_PORT);
    printf("  Press Ctrl+C to exit.\n");
    printf("========================================\n\n");

    /* 客户端管理 */
    ClientCtx clients[MAX_CLIENTS];
    memset(clients, 0, sizeof(clients));

    /* 提交初始 accept */
    ce_async_accept(async, listen_fd, NULL);

    /* 主循环 — io_uring 事件驱动 + 60Hz tick */
    const struct timespec tick_interval = {0, 1000000000L / TICK_RATE}; /* ~16.67ms */
    struct timespec last_tick;
    clock_gettime(CLOCK_MONOTONIC, &last_tick);

    while (g_running) {
        /* 提交所有待处理的 I/O 请求到内核 */
        ce_async_submit(async);

        /* 等待完成事件（1ms 超时，确保空闲时也能执行 ECS/Cell update） */
        int n = ce_async_wait(async, 1, 1);
        if (n < 0) break;

        /* 处理完成事件 */
        for (int i = 0; i < n; i++) {
            const CeAsyncEvent* ev = ce_async_get_event(async, i);

            switch (ev->type) {
            case CE_ASYNC_ACCEPT: {
                if (ev->client_fd < 0) {
                    /* accept 失败，继续尝试 */
                    ce_async_accept(async, listen_fd, NULL);
                    break;
                }

                /* 找空闲槽位 */
                int slot = -1;
                for (int j = 0; j < MAX_CLIENTS; j++) {
                    if (!clients[j].connected) { slot = j; break; }
                }

                if (slot >= 0) {
                    clients[slot].fd        = ev->client_fd;
                    clients[slot].connected = 1;
                    printf("[+] Client connected (fd=%d, slot=%d)\n",
                           ev->client_fd, slot);
                    /* 提交 recv 请求，user_data 携带 slot 索引 */
                    ce_async_recv(async, ev->client_fd,
                                  clients[slot].buf, BUFFER_SIZE,
                                  (void*)(intptr_t)slot);
                } else {
                    printf("[-] Client rejected (max %d clients)\n", MAX_CLIENTS);
                    close(ev->client_fd);
                }

                /* 继续 accept 下一个连接 */
                ce_async_accept(async, listen_fd, NULL);
                break;
            }

            case CE_ASYNC_RECV: {
                int slot = (int)(intptr_t)ev->user_data;
                if (slot < 0 || slot >= MAX_CLIENTS || !clients[slot].connected) break;

                if (ev->nbytes > 0) {
                    printf("[<] Received %d bytes from slot %d\n", ev->nbytes, slot);
                    /* 回显数据 */
                    ce_async_send(async, ev->fd,
                                  clients[slot].buf, ev->nbytes,
                                  (void*)(intptr_t)slot);
                } else {
                    /* 客户端断开（nbytes == 0 即 EOF，或 error） */
                    printf("[-] Client disconnected (fd=%d, slot=%d)\n", ev->fd, slot);
                    ce_async_close(async, ev->fd);
                    clients[slot].connected = 0;
                }
                break;
            }

            case CE_ASYNC_SEND: {
                int slot = (int)(intptr_t)ev->user_data;
                if (slot < 0 || slot >= MAX_CLIENTS || !clients[slot].connected) break;

                if (ev->nbytes > 0 && ev->error == 0) {
                    printf("[>] Echoed %d bytes to slot %d\n", ev->nbytes, slot);
                    /* 继续接收 */
                    ce_async_recv(async, ev->fd,
                                  clients[slot].buf, BUFFER_SIZE,
                                  (void*)(intptr_t)slot);
                } else {
                    /* send 失败 */
                    printf("[-] Send error on slot %d (errno=%d)\n", slot, ev->error);
                    ce_async_close(async, ev->fd);
                    clients[slot].connected = 0;
                }
                break;
            }

            case CE_ASYNC_CLOSE:
                /* close 完成，无需额外处理 */
                break;

            default:
                break;
            }
        }

        /* 60Hz tick：在 I/O 等待间隙执行 ECS 和 Cell 更新 */
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            long elapsed_ns = (now.tv_sec - last_tick.tv_sec) * 1000000000L
                            + (now.tv_nsec - last_tick.tv_nsec);
            long interval_ns = tick_interval.tv_sec * 1000000000L
                             + tick_interval.tv_nsec;

            if (elapsed_ns >= interval_ns) {
                ce_update();
                ce_cell_update();

                /* Sync / Save / DBProxy 每 tick 处理 */
                if (sync) {
                    ce_sync_heartbeat(sync);
                    CeSyncResponse sync_resp;
                    ce_sync_poll(sync, &sync_resp);
                }
                if (save) {
                    ce_save_tick(save);
                }
                if (dbproxy) {
                    CeDbproxyResponse db_resp;
                    ce_dbproxy_recv(dbproxy, &db_resp);
                }

                last_tick = now;
            }
        }
    }

    printf("\nShutting down...\n");

    /* 关闭所有客户端连接 */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].connected) {
            close(clients[i].fd);
            clients[i].connected = 0;
        }
    }

    /* 关闭监听 socket */
    ce_socket_close(listen_sock);

    /* 关闭异步 I/O 上下文 */
    ce_async_shutdown(async);

    /* 清理网络层和引擎 */
    ce_net_shutdown();
    ce_cell_shutdown();

    /* 停止 Admin IPC */
    if (admin_ipc) {
        ce_admin_ipc_stop(admin_ipc);
    }

    /* 关闭 Sync / Save / DBProxy */
    if (save) ce_save_shutdown(save);
    if (sync) ce_sync_shutdown(sync);
    if (dbproxy) ce_dbproxy_disconnect(dbproxy);

    ce_shutdown();

    printf("Echo server shut down cleanly.\n");
    return 0;
}

/* ================================================================
 * POSIX 非阻塞轮询 fallback
 * ================================================================ */
#else /* !CHAOS_HAS_IO_URING */

int main(int argc, char** argv) {
    int admin_enabled = 0;
    const char* admin_sock_path = DEFAULT_ADMIN_SOCK;
    CeAdminIpc* admin_ipc = NULL;

    /* DBProxy / Sync / Save 配置（可通过命令行覆盖） */
    const char* dbproxy_host = "192.168.1.100";
    int         dbproxy_port = 9003;
    const char* backup_host  = "127.0.0.1";
    int         backup_port  = 9003;
    int         save_interval = 300;

    /* 解析命令行参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--admin") == 0) {
            admin_enabled = 1;
        } else if (strcmp(argv[i], "--admin-sock") == 0 && i + 1 < argc) {
            admin_sock_path = argv[++i];
        } else if (strcmp(argv[i], "--dbproxy-host") == 0 && i + 1 < argc) {
            dbproxy_host = argv[++i];
        } else if (strcmp(argv[i], "--dbproxy-port") == 0 && i + 1 < argc) {
            dbproxy_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--backup-host") == 0 && i + 1 < argc) {
            backup_host = argv[++i];
        } else if (strcmp(argv[i], "--backup-port") == 0 && i + 1 < argc) {
            backup_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--save-interval") == 0 && i + 1 < argc) {
            save_interval = atoi(argv[++i]);
        }
    }

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

    /* 初始化服务端空间子系统（AOI + Cell） */
    if (ce_cell_init(10000.0f, 10000.0f, 500.0f, 500.0f, 150, 20) != CE_OK) {
        fprintf(stderr, "Failed to initialize Cell manager\n");
        ce_net_shutdown();
        ce_shutdown();
        return 1;
    }
    printf("[Server] Cell manager initialized: 10000x10000 world, 500x500 cells\n");

    /* ---- DBProxy 连接初始化 ---- */
    CeDbproxyConfig dbproxy_cfg = {
        .primary_host = dbproxy_host,
        .primary_port = dbproxy_port,
        .backup_host  = backup_host,
        .backup_port  = backup_port
    };
    CeDbproxyContext* dbproxy = ce_dbproxy_connect(&dbproxy_cfg);
    if (!dbproxy) {
        fprintf(stderr, "[WARN] DBProxy connection failed, continuing without DBProxy\n");
    } else {
        printf("[Server] DBProxy connected: %s\n", ce_dbproxy_current_endpoint(dbproxy));
    }

    /* ---- Sync 初始化 ---- */
    CeSyncConfig sync_cfg = {
        .dbproxy_host = dbproxy_host,
        .dbproxy_port = 9001,
        .backup_host  = backup_host,
        .backup_port  = 9001
    };
    CeSyncContext* sync = ce_sync_init(&sync_cfg);
    if (!sync) {
        fprintf(stderr, "[WARN] Sync init failed, continuing without sync\n");
    } else {
        printf("[Server] Sync initialized: %s:%d\n", dbproxy_host, 9001);
    }

    /* ---- Save 初始化 ---- */
    CeSaveConfig save_cfg = {
        .save_interval_sec = save_interval,
        .full_save_every_n = 6
    };
    CeSaveContext* save = ce_save_init(dbproxy, &save_cfg);
    if (!save) {
        fprintf(stderr, "[WARN] Save init failed, continuing without save\n");
    } else {
        printf("[Server] Save initialized: interval=%ds, full_every=%d\n",
               save_interval, 6);
    }

    /* 启动 Admin IPC（如果 --admin 传入） */
    if (admin_enabled) {
        admin_ipc = ce_admin_ipc_start(admin_sock_path);
        if (admin_ipc) {
            printf("Admin IPC: enabled at %s\n", admin_sock_path);
        } else {
            fprintf(stderr, "Admin IPC: failed to start at %s\n", admin_sock_path);
        }
    } else {
        printf("Admin IPC: disabled\n");
    }

    /* 创建监听 socket */
    CeSocket* listen_sock = ce_socket_create_tcp();
    if (!listen_sock) {
        fprintf(stderr, "Failed to create TCP socket\n");
        if (admin_ipc) ce_admin_ipc_stop(admin_ipc);
        ce_net_shutdown();
        ce_cell_shutdown();
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
        if (admin_ipc) ce_admin_ipc_stop(admin_ipc);
        ce_net_shutdown();
        ce_cell_shutdown();
        ce_shutdown();
        return 1;
    }

    /* 开始监听 */
    if (ce_socket_listen(listen_sock, 5) != CE_OK) {
        fprintf(stderr, "Failed to listen on port %d\n", ECHO_PORT);
        ce_socket_close(listen_sock);
        if (admin_ipc) ce_admin_ipc_stop(admin_ipc);
        ce_net_shutdown();
        ce_cell_shutdown();
        ce_shutdown();
        return 1;
    }

    /* 设置非阻塞模式，以便在主循环中检查 Ctrl+C */
    ce_socket_set_nonblocking(listen_sock, CE_TRUE);

    printf("========================================\n");
    printf("  ChaosEngine TCP Echo Server v0.1.0\n");
    printf("  Backend: POSIX (non-blocking poll)\n");
    printf("  Listening on port %d\n", ECHO_PORT);
    printf("  Press Ctrl+C to exit.\n");
    printf("========================================\n\n");

    /* 客户端连接数组 */
    CeSocket* clients[MAX_CLIENTS];
    int client_count = 0;
    memset(clients, 0, sizeof(clients));

    char buffer[BUFFER_SIZE];

    /* 主循环 — 固定 60Hz tick rate */
    const struct timespec tick_interval = {0, 1000000000L / TICK_RATE}; /* ~16.67ms */
    struct timespec last_tick, now;
    clock_gettime(CLOCK_MONOTONIC, &last_tick);

    while (g_running) {
        clock_gettime(CLOCK_MONOTONIC, &now);

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

        /* 更新引擎和 Cell */
        ce_update();
        ce_cell_update();

        /* Sync / Save / DBProxy 每 tick 处理 */
        if (sync) {
            ce_sync_heartbeat(sync);
            CeSyncResponse sync_resp;
            ce_sync_poll(sync, &sync_resp);
        }
        if (save) {
            ce_save_tick(save);
        }
        if (dbproxy) {
            CeDbproxyResponse db_resp;
            ce_dbproxy_recv(dbproxy, &db_resp);
        }

        /* 限速：睡眠到下一个 tick */
        {
            struct timespec target;
            target.tv_sec  = last_tick.tv_sec  + tick_interval.tv_sec;
            target.tv_nsec = last_tick.tv_nsec + tick_interval.tv_nsec;
            if (target.tv_nsec >= 1000000000L) {
                target.tv_sec++;
                target.tv_nsec -= 1000000000L;
            }
            long sleep_ns = (target.tv_sec - now.tv_sec) * 1000000000L
                          + (target.tv_nsec - now.tv_nsec);
            if (sleep_ns > 0) {
                struct timespec ts = {0, sleep_ns};
                nanosleep(&ts, NULL);
            }
            last_tick = target;
        }
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
    ce_cell_shutdown();

    /* 停止 Admin IPC */
    if (admin_ipc) {
        ce_admin_ipc_stop(admin_ipc);
    }

    /* 关闭 Sync / Save / DBProxy */
    if (save) ce_save_shutdown(save);
    if (sync) ce_sync_shutdown(sync);
    if (dbproxy) ce_dbproxy_disconnect(dbproxy);

    ce_shutdown();

    printf("Echo server shut down cleanly.\n");
    return 0;
}

#endif /* CHAOS_HAS_IO_URING */
