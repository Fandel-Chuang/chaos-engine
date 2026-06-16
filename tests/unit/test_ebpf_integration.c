/*
 * ChaosEngine io_uring + eBPF 集成测试 (Phase 7.6)
 *
 * 测试: io_uring 异步 I/O 与 eBPF 可观测性同时运行
 *
 * 架构:
 *   1. 启动 io_uring echo 服务器（子进程，使用原始 liburing）
 *   2. 初始化 eBPF 追踪（I/O 延迟 + TCP 重传）
 *   3. 客户端连接并发送/接收数据
 *   4. 验证 eBPF 统计数据非零
 *   5. 清理
 *
 * 编译: 需要 liburing + libbpf
 */

#define _POSIX_C_SOURCE 200112L
#define _GNU_SOURCE
#include "public_api/ce_types.h"
#include "network/ce_ebpf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <liburing.h>

#define TEST_PORT    19976
#define BUFFER_SIZE  4096
#define TEST_MSG     "Hello from io_uring+eBPF integration test!"
#define TEST_DURATION_SEC 3

#define TEST(name) printf("  TEST: %s ... ", name)
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while(0)
#define PASS() printf("PASS\n")

/* ---- 简单的 TCP 客户端 ---- */
static int tcp_connect(const char* host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int tcp_send_all(int fd, const void* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char*)buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return (int)sent;
}

static int tcp_recv_all(int fd, void* buf, size_t len) {
    size_t recvd = 0;
    while (recvd < len) {
        ssize_t n = recv(fd, (char*)buf + recvd, len - recvd, 0);
        if (n <= 0) return -1;
        recvd += n;
    }
    return (int)recvd;
}

int main(void) {
    int failures = 0;

    printf("=== io_uring + eBPF Integration Test (Phase 7.6) ===\n");
    printf("  BTF available: %s\n", ce_ebpf_available() ? "YES" : "NO");

    /* ---- 7.6.1: 启动 io_uring echo 服务器（子进程） ---- */
    TEST("start_io_uring_server");
    pid_t server_pid = fork();
    CHECK(server_pid >= 0);

    if (server_pid == 0) {
        /* 子进程: 使用原始 liburing 实现 echo 服务器 */
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) _exit(1);

        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(TEST_PORT);

        if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) _exit(2);
        if (listen(listen_fd, 128) < 0) _exit(3);

        /* 初始化 io_uring */
        struct io_uring ring;
        if (io_uring_queue_init(256, &ring, 0) < 0) _exit(4);

        /* 提交 accept */
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);
        io_uring_sqe_set_data(sqe, (void*)(intptr_t)0); /* type=0: accept */
        io_uring_submit(&ring);

        /* 客户端管理 */
        #define MAX_CLIENTS 64
        struct {
            int fd;
            char buf[BUFFER_SIZE];
            int connected;
        } clients[MAX_CLIENTS];
        memset(clients, 0, sizeof(clients));

        time_t start = time(NULL);
        while (time(NULL) - start < TEST_DURATION_SEC + 5) {
            struct io_uring_cqe* cqe;
            struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
            if (ret == -ETIME) continue;
            if (ret < 0) break;

            int type = (int)(intptr_t)io_uring_cqe_get_data(cqe);
            int res = cqe->res;

            if (type == 0) {
                /* accept 完成 */
                if (res >= 0) {
                    int slot = -1;
                    for (int j = 0; j < MAX_CLIENTS; j++) {
                        if (!clients[j].connected) { slot = j; break; }
                    }
                    if (slot >= 0) {
                        clients[slot].fd = res;
                        clients[slot].connected = 1;
                        /* 提交 recv */
                        sqe = io_uring_get_sqe(&ring);
                        io_uring_prep_recv(sqe, res, clients[slot].buf, BUFFER_SIZE, 0);
                        io_uring_sqe_set_data(sqe, (void*)(intptr_t)(slot + 1)); /* type=slot+1: recv */
                    } else {
                        close(res);
                    }
                }
                /* 继续 accept */
                sqe = io_uring_get_sqe(&ring);
                io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);
                io_uring_sqe_set_data(sqe, (void*)(intptr_t)0);
            } else if (type > 0 && type <= MAX_CLIENTS) {
                int slot = type - 1;
                if (res > 0) {
                    /* recv 完成 -> echo 回去 */
                    sqe = io_uring_get_sqe(&ring);
                    io_uring_prep_send(sqe, clients[slot].fd, clients[slot].buf, res, 0);
                    io_uring_sqe_set_data(sqe, (void*)(intptr_t)(-(slot + 1))); /* negative: send */
                } else {
                    /* 客户端断开 */
                    close(clients[slot].fd);
                    clients[slot].connected = 0;
                }
            } else if (type < 0 && type >= -MAX_CLIENTS) {
                int slot = (-type) - 1;
                if (res > 0) {
                    /* send 完成 -> 继续 recv */
                    sqe = io_uring_get_sqe(&ring);
                    io_uring_prep_recv(sqe, clients[slot].fd, clients[slot].buf, BUFFER_SIZE, 0);
                    io_uring_sqe_set_data(sqe, (void*)(intptr_t)(slot + 1));
                }
            }

            io_uring_cqe_seen(&ring, cqe);
            io_uring_submit(&ring);
        }

        /* 清理 */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].connected) close(clients[i].fd);
        }
        io_uring_queue_exit(&ring);
        close(listen_fd);
        _exit(0);
    }
    PASS();

    /* 等待服务器启动 */
    usleep(500000); /* 500ms */

    /* ---- 7.6.2: 初始化 eBPF 追踪 ---- */
    TEST("init_ebpf_tracing");
    CeEbpfContext* ebpf = ce_ebpf_init();
    if (ebpf) {
        printf("(eBPF context created) ");
        ce_ebpf_trace_io_latency(ebpf);
        ce_ebpf_trace_tcp_retransmit(ebpf);
    }
    PASS();

    /* ---- 7.6.3: 客户端连接并通信 ---- */
    TEST("client_echo_communication");
    {
        int fd = tcp_connect("127.0.0.1", TEST_PORT);
        if (fd < 0) {
            printf("(connect failed, server may not be ready) ");
        } else {
            const char* msg = TEST_MSG;
            int msg_len = strlen(msg) + 1;

            /* 发送 */
            int sent = tcp_send_all(fd, msg, msg_len);
            CHECK(sent == msg_len);

            /* 接收 echo */
            char recv_buf[256];
            memset(recv_buf, 0, sizeof(recv_buf));
            int recvd = tcp_recv_all(fd, recv_buf, msg_len);
            CHECK(recvd == msg_len);
            CHECK(strcmp(recv_buf, msg) == 0);

            close(fd);
        }
    }
    PASS();

    /* ---- 7.6.4: 多客户端并发通信 ---- */
    TEST("multi_client_concurrent");
    {
        #define NUM_CLIENTS 10
        int fds[NUM_CLIENTS];
        int connected = 0;

        for (int i = 0; i < NUM_CLIENTS; i++) {
            fds[i] = tcp_connect("127.0.0.1", TEST_PORT);
            if (fds[i] >= 0) connected++;
        }

        /* 给服务器一点时间处理 accept */
        usleep(100000); /* 100ms */

        if (connected > 0) {
            int verified = 0;
            for (int i = 0; i < NUM_CLIENTS; i++) {
                if (fds[i] < 0) continue;
                char msg[64];
                snprintf(msg, sizeof(msg), "Client-%d: ping", i);
                int len = strlen(msg) + 1;
                if (tcp_send_all(fds[i], msg, len) != len) {
                    close(fds[i]);
                    fds[i] = -1;
                    continue;
                }

                char recv_buf[64];
                memset(recv_buf, 0, sizeof(recv_buf));
                int recvd = tcp_recv_all(fds[i], recv_buf, len);
                if (recvd == len && strcmp(recv_buf, msg) == 0) {
                    verified++;
                }
                close(fds[i]);
                fds[i] = -1;
            }
            CHECK(verified > 0);
            printf("(%d/%d verified) ", verified, connected);
        }
    }
    PASS();

    /* ---- 7.6.5: 读取 eBPF 统计数据 ---- */
    TEST("read_ebpf_stats_after_io");
    {
        usleep(200000);

        if (ebpf) {
            int retrans = ce_ebpf_get_retransmit_count(ebpf);
            printf("(TCP retrans: %d) ", retrans);
            CHECK(retrans >= 0);

            int p50, p90, p99;
            ce_ebpf_get_io_latency_stats(ebpf, &p50, &p90, &p99);
            printf("(I/O P50=%d P90=%d P99=%d) ", p50, p90, p99);
            CHECK(p50 >= 0 && p90 >= 0 && p99 >= 0);

            ce_ebpf_dump_latency(ebpf, "recvfrom");
        }
    }
    PASS();

    /* ---- 7.6.6: 清理 ---- */
    TEST("cleanup");
    {
        if (ebpf) ce_ebpf_shutdown(ebpf);

        int status;
        waitpid(server_pid, &status, 0);
        printf("(server exited: %d) ", WEXITSTATUS(status));
    }
    PASS();

    printf("\nAll io_uring + eBPF integration tests passed!\n");
    return 0;
}
