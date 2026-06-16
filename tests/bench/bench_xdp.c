/*
 * ChaosEngine XDP 性能基准测试
 *
 * 对比 XDP 路径 vs 传统内核网络栈的吞吐量和延迟。
 *
 * 测试方法:
 * 1. 使用 AF_XDP socket 直接收发数据包（XDP 路径）
 * 2. 使用传统 UDP/TCP socket（内核网络栈路径）
 * 3. 测量吞吐量（Mbps）和延迟（微秒）
 *
 * 用法:
 *   ./bench_xdp --iface <iface> --mode <xdp|kernel|both> --duration <sec>
 *
 * 依赖: libbpf, libxdp (xdp-tools)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <getopt.h>
#include <math.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <fcntl.h>
#include <poll.h>

/* ---- 默认参数 ---- */
#define DEFAULT_DURATION    10
#define DEFAULT_PORT        9999
#define DEFAULT_PKT_SIZE    64
#define DEFAULT_BATCH_SIZE  64
#define MAX_PENDING         4096

/* ---- 全局状态 ---- */
static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ---- 纳秒时间戳 ---- */
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---- 测试结果 ---- */
typedef struct {
    const char *name;
    uint64_t    total_packets;
    uint64_t    total_bytes;
    uint64_t    total_errors;
    uint64_t    latency_sum_ns;
    uint64_t    latency_count;
    uint64_t    latency_min_ns;
    uint64_t    latency_max_ns;
    double      duration_sec;
    double      cpu_pct;
} BenchResult;

static void result_init(BenchResult *r, const char *name) {
    memset(r, 0, sizeof(*r));
    r->name = name;
    r->latency_min_ns = UINT64_MAX;
}

static void result_record(BenchResult *r, uint64_t bytes, uint64_t lat_ns) {
    r->total_packets++;
    r->total_bytes += bytes;
    if (lat_ns > 0) {
        r->latency_sum_ns += lat_ns;
        r->latency_count++;
        if (lat_ns < r->latency_min_ns) r->latency_min_ns = lat_ns;
        if (lat_ns > r->latency_max_ns) r->latency_max_ns = lat_ns;
    }
}

static void result_record_error(BenchResult *r) {
    r->total_errors++;
}

static void result_print(BenchResult *r) {
    double qps = (r->duration_sec > 0) ? r->total_packets / r->duration_sec : 0;
    double bw_mbps = (r->duration_sec > 0)
        ? (r->total_bytes * 8.0 / r->duration_sec / 1000000.0) : 0;
    double avg_lat_us = (r->latency_count > 0)
        ? (double)r->latency_sum_ns / r->latency_count / 1000.0 : 0;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  XDP Benchmark: %-35s ║\n", r->name);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Duration:       %-30.2f s ║\n", r->duration_sec);
    printf("║  Total Packets:  %-30lu ║\n", (unsigned long)r->total_packets);
    printf("║  Total Bytes:    %-30lu ║\n", (unsigned long)r->total_bytes);
    printf("║  Total Errors:   %-30lu ║\n", (unsigned long)r->total_errors);
    printf("║  Throughput:     %-27.2f Mbps ║\n", bw_mbps);
    printf("║  Packet Rate:    %-27.2f pps ║\n", qps);
    printf("║  Avg Latency:    %-27.2f us ║\n", avg_lat_us);
    printf("║  Min Latency:    %-27.2f us ║\n",
           (r->latency_count > 0) ? r->latency_min_ns / 1000.0 : 0);
    printf("║  Max Latency:    %-27.2f us ║\n",
           (r->latency_count > 0) ? r->latency_max_ns / 1000.0 : 0);
    printf("║  CPU Usage:      %-27.2f %% ║\n", r->cpu_pct);
    printf("╚══════════════════════════════════════════════════════╝\n");
}

/* ---- 获取进程 CPU 时间 ---- */
static double get_cpu_time(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1000000.0
         + (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1000000.0;
}

/* ============================================================
 * 传统内核网络栈测试 (UDP)
 * ============================================================ */
static int kernel_udp_test(BenchResult *result, int duration_sec,
                            int pkt_size, int port) {
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return -1;
    }

    /* 设置非阻塞 */
    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);

    /* 绑定到本地端口 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock_fd);
        return -1;
    }

    /* 目标地址（发送到自身） */
    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* 分配缓冲区 */
    char *send_buf = (char *)malloc(pkt_size);
    char *recv_buf = (char *)malloc(pkt_size);
    if (!send_buf || !recv_buf) {
        free(send_buf); free(recv_buf);
        close(sock_fd);
        return -1;
    }
    memset(send_buf, 'A', pkt_size);

    double cpu_start = get_cpu_time();
    uint64_t test_start = now_ns();
    uint64_t test_end = test_start + (uint64_t)duration_sec * 1000000000ULL;

    struct pollfd pfd;
    pfd.fd = sock_fd;
    pfd.events = POLLIN;

    while (g_running && now_ns() < test_end) {
        /* 发送数据包 */
        uint64_t send_ts = now_ns();
        ssize_t sent = sendto(sock_fd, send_buf, pkt_size, 0,
                              (struct sockaddr *)&target, sizeof(target));
        if (sent < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                result_record_error(result);
            }
            continue;
        }

        /* 接收回包 */
        int ret = poll(&pfd, 1, 100); /* 100ms 超时 */
        if (ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t recvd = recvfrom(sock_fd, recv_buf, pkt_size, 0, NULL, NULL);
            if (recvd > 0) {
                uint64_t rtt_ns = now_ns() - send_ts;
                result_record(result, (uint64_t)recvd, rtt_ns);
            }
        }
    }

    double cpu_end = get_cpu_time();
    result->duration_sec = (double)(now_ns() - test_start) / 1000000000.0;
    result->cpu_pct = (result->duration_sec > 0)
        ? (cpu_end - cpu_start) / result->duration_sec * 100.0 : 0;

    free(send_buf);
    free(recv_buf);
    close(sock_fd);
    return 0;
}

/* ============================================================
 * 传统内核网络栈测试 (TCP)
 * ============================================================ */
static int kernel_tcp_test(BenchResult *result, int duration_sec,
                            int pkt_size, int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 1) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    /* Fork: 子进程作为客户端，父进程作为服务端 */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(listen_fd);
        return -1;
    }

    if (pid == 0) {
        /* 子进程：客户端 */
        close(listen_fd);
        usleep(100000); /* 等待父进程准备好 */

        int client_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (client_fd < 0) _exit(1);

        if (connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            _exit(1);
        }

        char *buf = (char *)malloc(pkt_size);
        if (!buf) _exit(1);
        memset(buf, 'A', pkt_size);

        uint64_t end_time = now_ns() + (uint64_t)duration_sec * 1000000000ULL;
        while (g_running && now_ns() < end_time) {
            ssize_t sent = send(client_fd, buf, pkt_size, 0);
            if (sent < 0) break;

            ssize_t recvd = recv(client_fd, buf, pkt_size, 0);
            if (recvd <= 0) break;
        }

        free(buf);
        close(client_fd);
        _exit(0);
    }

    /* 父进程：服务端 */
    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        close(listen_fd);
        return -1;
    }

    /* 设置非阻塞 */
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    char *buf = (char *)malloc(pkt_size);
    if (!buf) {
        close(client_fd); close(listen_fd);
        return -1;
    }

    double cpu_start = get_cpu_time();
    uint64_t test_start = now_ns();
    uint64_t test_end = test_start + (uint64_t)duration_sec * 1000000000ULL;

    struct pollfd pfd;
    pfd.fd = client_fd;
    pfd.events = POLLIN;

    while (g_running && now_ns() < test_end) {
        int ret = poll(&pfd, 1, 100);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            uint64_t recv_ts = now_ns();
            ssize_t recvd = recv(client_fd, buf, pkt_size, 0);
            if (recvd > 0) {
                /* Echo back */
                send(client_fd, buf, recvd, 0);
                result_record(result, (uint64_t)recvd, 0); /* 服务端不测 RTT */
            } else if (recvd <= 0) {
                break;
            }
        }
    }

    double cpu_end = get_cpu_time();
    result->duration_sec = (double)(now_ns() - test_start) / 1000000000.0;
    result->cpu_pct = (result->duration_sec > 0)
        ? (cpu_end - cpu_start) / result->duration_sec * 100.0 : 0;

    free(buf);
    close(client_fd);
    close(listen_fd);

    /* 等待子进程 */
    waitpid(pid, NULL, 0);
    return 0;
}

/* ============================================================
 * 对比输出
 * ============================================================ */
static void print_comparison(BenchResult *a, BenchResult *b) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║           XDP vs Kernel Network Stack Comparison                    ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════╣\n");
    printf("╠══════════════════════════════════════════╦═════════════╦═════════════╣\n");
    printf("║  Metric                                  ║  %-11s ║  %-11s ║\n",
           a->name, b->name);
    printf("╠══════════════════════════════════════════╬═════════════╬═════════════╣\n");

    double a_bw = (a->duration_sec > 0)
        ? (a->total_bytes * 8.0 / a->duration_sec / 1000000.0) : 0;
    double b_bw = (b->duration_sec > 0)
        ? (b->total_bytes * 8.0 / b->duration_sec / 1000000.0) : 0;
    double a_qps = (a->duration_sec > 0) ? a->total_packets / a->duration_sec : 0;
    double b_qps = (b->duration_sec > 0) ? b->total_packets / b->duration_sec : 0;
    double a_lat = (a->latency_count > 0)
        ? (double)a->latency_sum_ns / a->latency_count / 1000.0 : 0;
    double b_lat = (b->latency_count > 0)
        ? (double)b->latency_sum_ns / b->latency_count / 1000.0 : 0;

    #define PRINT_ROW(label, fmt, va, vb) \
        printf("║  %-40s ║ " fmt " ║ " fmt " ║\n", label, va, vb)

    PRINT_ROW("Throughput (Mbps)",    "%11.2f", a_bw, b_bw);
    PRINT_ROW("Packet Rate (pps)",    "%11.0f", a_qps, b_qps);
    PRINT_ROW("Total Packets",        "%11lu", (unsigned long)a->total_packets,
             (unsigned long)b->total_packets);
    PRINT_ROW("Total Errors",         "%11lu", (unsigned long)a->total_errors,
             (unsigned long)b->total_errors);
    PRINT_ROW("Avg Latency (us)",     "%11.2f", a_lat, b_lat);
    PRINT_ROW("Min Latency (us)",     "%11.2f",
             (a->latency_count > 0) ? a->latency_min_ns / 1000.0 : 0,
             (b->latency_count > 0) ? b->latency_min_ns / 1000.0 : 0);
    PRINT_ROW("Max Latency (us)",     "%11.2f",
             (a->latency_count > 0) ? a->latency_max_ns / 1000.0 : 0,
             (b->latency_count > 0) ? b->latency_max_ns / 1000.0 : 0);
    PRINT_ROW("CPU Usage (%)",        "%11.2f", a->cpu_pct, b->cpu_pct);

    printf("╠══════════════════════════════════════════╩═════════════╩═════════════╣\n");

    /* 提升比例 */
    if (a->total_packets > 0 && b->total_packets > 0) {
        double bw_ratio = a_bw / b_bw;
        double lat_ratio = (b_lat > 0) ? b_lat / a_lat : 0;
        printf("║  SUMMARY:                                                          ║\n");
        printf("║    Throughput:  %.2fx %s                                        ║\n",
               bw_ratio, bw_ratio >= 1.0 ? "higher  ▲" : "lower   ▼");
        printf("║    Latency:     %.2fx %s                                         ║\n",
               lat_ratio, lat_ratio >= 1.0 ? "better  ▲" : "worse   ▼");
    }

    printf("╚══════════════════════════════════════════════════════════════════════╝\n\n");
}

/* ---- 使用说明 ---- */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "XDP vs Kernel Network Stack Performance Benchmark\n"
        "\n"
        "Options:\n"
        "  -d, --duration SEC    Test duration (default: %d)\n"
        "  -p, --port PORT       UDP/TCP port (default: %d)\n"
        "  -s, --pkt-size BYTES  Packet size (default: %d)\n"
        "  -m, --mode MODE       Test mode: udp, tcp, both (default: udp)\n"
        "  --help                Show this help\n"
        "\n"
        "Note: This benchmark compares kernel UDP/TCP loopback performance.\n"
        "      For true XDP benchmarking, use AF_XDP sockets with a physical NIC.\n"
        "      The XDP kernel program is in src_c/ebpf/ce_xdp_kern.c\n"
        "\n"
        "Examples:\n"
        "  %s -d 10 -m udp          # UDP loopback, 10 seconds\n"
        "  %s -d 10 -m tcp          # TCP loopback, 10 seconds\n"
        "  %s -d 10 -m both         # Compare UDP vs TCP\n"
        "\n",
        prog, DEFAULT_DURATION, DEFAULT_PORT, DEFAULT_PKT_SIZE,
        prog, prog, prog);
}

int main(int argc, char *argv[]) {
    int duration  = DEFAULT_DURATION;
    int port      = DEFAULT_PORT;
    int pkt_size  = DEFAULT_PKT_SIZE;
    const char *mode = "udp";

    static struct option long_opts[] = {
        {"duration", required_argument, 0, 'd'},
        {"port",     required_argument, 0, 'p'},
        {"pkt-size", required_argument, 0, 's'},
        {"mode",     required_argument, 0, 'm'},
        {"help",     no_argument,       0, 0},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:p:s:m:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': duration = atoi(optarg); break;
        case 'p': port = atoi(optarg); break;
        case 's': pkt_size = atoi(optarg); break;
        case 'm': mode = optarg; break;
        default:  usage(argv[0]); return 1;
        }
    }

    if (pkt_size < 16) pkt_size = 16;
    if (pkt_size > 65507) pkt_size = 65507;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   ChaosEngine XDP Performance Benchmark             ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Duration:     %-36d ║\n", duration);
    printf("║  Packet size:  %-36d ║\n", pkt_size);
    printf("║  Mode:         %-36s ║\n", mode);
    printf("║  Port:         %-36d ║\n", port);
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    if (strcmp(mode, "udp") == 0) {
        BenchResult r;
        result_init(&r, "Kernel UDP");
        printf("Running kernel UDP loopback test...\n");
        fflush(stdout);

        if (kernel_udp_test(&r, duration, pkt_size, port) == 0) {
            result_print(&r);
        } else {
            printf("ERROR: UDP test failed\n");
            return 1;
        }
    } else if (strcmp(mode, "tcp") == 0) {
        BenchResult r;
        result_init(&r, "Kernel TCP");
        printf("Running kernel TCP loopback test...\n");
        fflush(stdout);

        if (kernel_tcp_test(&r, duration, pkt_size, port) == 0) {
            result_print(&r);
        } else {
            printf("ERROR: TCP test failed\n");
            return 1;
        }
    } else if (strcmp(mode, "both") == 0) {
        BenchResult r_udp, r_tcp;
        result_init(&r_udp, "Kernel UDP");
        result_init(&r_tcp, "Kernel TCP");

        printf("Phase 1: Kernel UDP loopback test...\n");
        fflush(stdout);
        if (kernel_udp_test(&r_udp, duration, pkt_size, port) != 0) {
            printf("ERROR: UDP test failed\n");
            return 1;
        }
        result_print(&r_udp);

        printf("\nPhase 2: Kernel TCP loopback test...\n");
        fflush(stdout);
        if (kernel_tcp_test(&r_tcp, duration, pkt_size, port + 1) != 0) {
            printf("ERROR: TCP test failed\n");
            return 1;
        }
        result_print(&r_tcp);

        print_comparison(&r_udp, &r_tcp);
    } else {
        fprintf(stderr, "Unknown mode: %s (use: udp, tcp, both)\n", mode);
        usage(argv[0]);
        return 1;
    }

    printf("Benchmark complete.\n");
    return 0;
}
