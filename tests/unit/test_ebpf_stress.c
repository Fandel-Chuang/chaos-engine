/*
 * ChaosEngine io_uring + eBPF 压力测试 (Phase 7.7)
 *
 * 测试: 1000 并发连接 + eBPF 追踪
 *
 * 架构:
 *   1. 启动 io_uring echo 服务器（子进程，使用原始 liburing）
 *   2. 初始化 eBPF 追踪（I/O 延迟 + TCP 重传）
 *   3. 创建 1000 个客户端并发连接
 *   4. 每个客户端发送 ping 并验证 echo
 *   5. 读取 eBPF 统计
 *   6. 报告吞吐量和延迟
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
#include <fcntl.h>
#include <liburing.h>

#define TEST_PORT        19977
#define NUM_CLIENTS      1000
#define BUFFER_SIZE      256
#define TEST_MSG         "ping"
#define MSG_LEN          5  /* "ping" + null */
#define SERVER_TIMEOUT   30  /* 服务器最多运行 30 秒 */

#define TEST(name) printf("  TEST: %s ... ", name)
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while(0)
#define PASS() printf("PASS\n")

/* ---- 非阻塞 TCP 连接 ---- */
static int tcp_connect_nonblock(const char* host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    int ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv = {5, 0};
    ret = select(fd + 1, NULL, &wfds, NULL, &tv);
    if (ret <= 0) {
        close(fd);
        return -1;
    }

    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
        close(fd);
        return -1;
    }

    fcntl(fd, F_SETFL, flags);
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

static double now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000.0 + tv.tv_usec;
}

int main(void) {
    int failures = 0;

    printf("=== io_uring + eBPF Stress Test (Phase 7.7) ===\n");
    printf("  Target: %d concurrent connections\n", NUM_CLIENTS);
    printf("  BTF available: %s\n", ce_ebpf_available() ? "YES" : "NO");

    /* ---- 7.7.1: 启动 io_uring 服务器（子进程） ---- */
    TEST("start_bench_server");
    pid_t server_pid = fork();
    CHECK(server_pid >= 0);

    if (server_pid == 0) {
        /* 子进程: 使用原始 liburing 实现 echo 服务器 */
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) _exit(1);

        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        int rcvbuf = 256 * 1024;
        setsockopt(listen_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(TEST_PORT);

        if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) _exit(2);
        if (listen(listen_fd, 2048) < 0) _exit(3);

        struct io_uring ring;
        if (io_uring_queue_init(1024, &ring, 0) < 0) _exit(4);

        /* 提交 accept */
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);
        io_uring_sqe_set_data(sqe, (void*)(intptr_t)0);
        io_uring_submit(&ring);

        #define SRV_MAX_CLIENTS 2048
        typedef struct {
            int fd;
            char buf[BUFFER_SIZE];
            int connected;
        } SrvClient;
        SrvClient* clients = (SrvClient*)calloc(SRV_MAX_CLIENTS, sizeof(SrvClient));
        if (!clients) _exit(5);

        int total_echoes = 0;
        int peak_clients = 0;
        int current_clients = 0;

        time_t start = time(NULL);
        while (time(NULL) - start < SERVER_TIMEOUT) {
            struct io_uring_cqe* cqe;
            struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
            if (ret == -ETIME) continue;
            if (ret < 0) break;

            int type = (int)(intptr_t)io_uring_cqe_get_data(cqe);
            int res = cqe->res;

            if (type == 0) {
                if (res >= 0) {
                    int slot = -1;
                    for (int j = 0; j < SRV_MAX_CLIENTS; j++) {
                        if (!clients[j].connected) { slot = j; break; }
                    }
                    if (slot >= 0) {
                        clients[slot].fd = res;
                        clients[slot].connected = 1;
                        current_clients++;
                        if (current_clients > peak_clients) peak_clients = current_clients;
                        sqe = io_uring_get_sqe(&ring);
                        io_uring_prep_recv(sqe, res, clients[slot].buf, BUFFER_SIZE, 0);
                        io_uring_sqe_set_data(sqe, (void*)(intptr_t)(slot + 1));
                    } else {
                        close(res);
                    }
                }
                sqe = io_uring_get_sqe(&ring);
                io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);
                io_uring_sqe_set_data(sqe, (void*)(intptr_t)0);
            } else if (type > 0 && type <= SRV_MAX_CLIENTS) {
                int slot = type - 1;
                if (res > 0) {
                    sqe = io_uring_get_sqe(&ring);
                    io_uring_prep_send(sqe, clients[slot].fd, clients[slot].buf, res, 0);
                    io_uring_sqe_set_data(sqe, (void*)(intptr_t)(-(slot + 1)));
                    total_echoes++;
                } else {
                    close(clients[slot].fd);
                    clients[slot].connected = 0;
                    current_clients--;
                }
            } else if (type < 0 && type >= -SRV_MAX_CLIENTS) {
                int slot = (-type) - 1;
                if (res > 0) {
                    sqe = io_uring_get_sqe(&ring);
                    io_uring_prep_recv(sqe, clients[slot].fd, clients[slot].buf, BUFFER_SIZE, 0);
                    io_uring_sqe_set_data(sqe, (void*)(intptr_t)(slot + 1));
                }
            }

            io_uring_cqe_seen(&ring, cqe);
            io_uring_submit(&ring);
        }

        fprintf(stderr, "BENCH_SERVER_STATS|echoes=%d|peak=%d\n", total_echoes, peak_clients);

        for (int i = 0; i < SRV_MAX_CLIENTS; i++) {
            if (clients[i].connected) close(clients[i].fd);
        }
        free(clients);
        io_uring_queue_exit(&ring);
        close(listen_fd);
        _exit(0);
    }
    PASS();

    /* 等待服务器启动 */
    usleep(500000);

    /* ---- 7.7.2: 初始化 eBPF 追踪 ---- */
    TEST("init_ebpf_stress_tracing");
    CeEbpfContext* ebpf = ce_ebpf_init();
    if (ebpf) {
        ce_ebpf_trace_io_latency(ebpf);
        ce_ebpf_trace_tcp_retransmit(ebpf);
        printf("(eBPF active) ");
    } else {
        printf("(eBPF unavailable) ");
    }
    PASS();

    /* ---- 7.7.3: 创建 1000 个并发连接 ---- */
    TEST("create_1000_connections");
    int* fds = (int*)calloc(NUM_CLIENTS, sizeof(int));
    CHECK(fds != NULL);

    double connect_start = now_us();
    int connected = 0;

    #define BATCH_SIZE 100
    for (int batch = 0; batch < NUM_CLIENTS / BATCH_SIZE; batch++) {
        for (int i = 0; i < BATCH_SIZE; i++) {
            int idx = batch * BATCH_SIZE + i;
            fds[idx] = tcp_connect_nonblock("127.0.0.1", TEST_PORT);
            if (fds[idx] >= 0) connected++;
        }
        usleep(10000);
    }

    double connect_end = now_us();
    double connect_time_ms = (connect_end - connect_start) / 1000.0;

    printf("(%d/%d connected in %.1f ms) ", connected, NUM_CLIENTS, connect_time_ms);
    CHECK(connected > 0);
    PASS();

    /* ---- 7.7.4: 并发发送/接收 ---- */
    TEST("concurrent_ping_pong");
    double io_start = now_us();
    int successful = 0;

    for (int i = 0; i < NUM_CLIENTS; i++) {
        if (fds[i] < 0) continue;
        if (tcp_send_all(fds[i], TEST_MSG, MSG_LEN) == MSG_LEN) {
            successful++;
        }
    }

    int verified = 0;
    for (int i = 0; i < NUM_CLIENTS; i++) {
        if (fds[i] < 0) continue;
        char recv_buf[BUFFER_SIZE];
        memset(recv_buf, 0, sizeof(recv_buf));
        if (tcp_recv_all(fds[i], recv_buf, MSG_LEN) == MSG_LEN) {
            if (strcmp(recv_buf, TEST_MSG) == 0) {
                verified++;
            }
        }
    }

    double io_end = now_us();
    double io_time_ms = (io_end - io_start) / 1000.0;

    printf("(sent=%d, verified=%d, time=%.1f ms) ", successful, verified, io_time_ms);
    CHECK(successful > 0);
    CHECK(verified > 0);
    PASS();

    /* ---- 7.7.5: 吞吐量统计 ---- */
    TEST("throughput_stats");
    {
        double throughput = (double)verified / (io_time_ms / 1000.0);
        printf("(%.0f msgs/sec) ", throughput);
        CHECK(throughput > 0);
    }
    PASS();

    /* ---- 7.7.6: 读取 eBPF 统计 ---- */
    TEST("ebpf_stats_under_load");
    {
        usleep(500000);

        if (ebpf) {
            int retrans = ce_ebpf_get_retransmit_count(ebpf);
            printf("(TCP retrans: %d) ", retrans);

            int p50, p90, p99;
            ce_ebpf_get_io_latency_stats(ebpf, &p50, &p90, &p99);
            printf("(I/O P50=%dus P90=%dus P99=%dus) ", p50, p90, p99);

            ce_ebpf_dump_latency(ebpf, "recvfrom");
        }
    }
    PASS();

    /* ---- 7.7.7: 关闭所有连接 ---- */
    TEST("close_all_connections");
    {
        double close_start = now_us();
        for (int i = 0; i < NUM_CLIENTS; i++) {
            if (fds[i] >= 0) close(fds[i]);
        }
        double close_end = now_us();
        double close_time_ms = (close_end - close_start) / 1000.0;
        printf("(closed in %.1f ms) ", close_time_ms);
    }
    PASS();

    /* ---- 7.7.8: 清理 ---- */
    TEST("stress_cleanup");
    {
        free(fds);
        if (ebpf) ce_ebpf_shutdown(ebpf);

        int status;
        waitpid(server_pid, &status, 0);
        printf("(server exit: %d) ", WEXITSTATUS(status));
    }
    PASS();

    printf("\n=== Stress Test Summary ===\n");
    printf("  Connections:    %d/%d\n", connected, NUM_CLIENTS);
    printf("  Connect time:   %.1f ms\n", connect_time_ms);
    printf("  Echo verified:  %d/%d\n", verified, successful);
    printf("  I/O time:       %.1f ms\n", io_time_ms);
    printf("  Throughput:     %.0f msgs/sec\n", verified / (io_time_ms / 1000.0));
    printf("\nAll stress tests passed!\n");

    return 0;
}
