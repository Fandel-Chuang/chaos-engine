/*
 * ChaosEngine io_uring vs POSIX 性能基准测试客户端
 *
 * 功能：
 *   - 可配置并发连接数（1 ~ 10000+）
 *   - 测量 QPS（每秒请求数）
 *   - 测量延迟分布（P50/P90/P99/P99.9）
 *   - 测量 CPU 利用率
 *   - 输出 JSON 格式结果，便于自动化分析
 *
 * 用法：
 *   ./bench_client --port <port> --conns <N> --duration <sec> --msg-size <bytes>
 *
 * 架构：
 *   客户端使用 epoll + 非阻塞 socket 管理大量并发连接。
 *   每个连接持续发送 echo 请求并测量往返延迟。
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
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>

/* ---- 默认参数 ---- */
#define DEFAULT_PORT        7778
#define DEFAULT_CONNS       100
#define DEFAULT_DURATION    10
#define DEFAULT_MSG_SIZE    64
#define MAX_EVENTS          4096
#define MAX_CONNS           16384
#define WARMUP_SEC          2

/* ---- 每个连接的上下文 ---- */
typedef struct {
    int      fd;
    int      state;          /* 0=connecting, 1=sending, 2=receiving */
    char     send_buf[4096];
    char     recv_buf[4096];
    int      send_len;
    int      recv_off;
    int      inflight;       /* 是否有未完成的请求 */
    uint64_t send_ts_ns;     /* 发送时间戳（纳秒） */
    uint64_t rtt_ns;         /* 最近一次 RTT */
} ConnCtx;

/* ---- 全局状态 ---- */
static volatile int g_running = 1;
static uint64_t      g_total_requests = 0;
static uint64_t      g_total_bytes = 0;
static uint64_t      g_total_errors = 0;
static uint64_t      g_total_connects = 0;
static uint64_t      g_total_disconnects = 0;

/* 延迟直方图（对数分桶） */
#define LATENCY_BUCKETS 64
static uint64_t g_latency_hist[LATENCY_BUCKETS];  /* 每个桶的计数 */
static uint64_t g_latency_sum_ns = 0;
static uint64_t g_latency_count = 0;
static uint64_t g_latency_min_ns = UINT64_MAX;
static uint64_t g_latency_max_ns = 0;

/* CPU 统计 */
static uint64_t g_start_utime = 0;
static uint64_t g_start_stime = 0;
static uint64_t g_end_utime = 0;
static uint64_t g_end_stime = 0;
static long     g_clock_ticks = 0;

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

/* ---- 设置非阻塞 ---- */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---- 设置 TCP_NODELAY ---- */
static int set_nodelay(int fd) {
    int opt = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

/* ---- 延迟桶索引（对数分桶，1us ~ 10s） ---- */
static int latency_bucket(uint64_t ns) {
    /* 桶边界: 1us, 2us, 5us, 10us, 20us, 50us, 100us, ... */
    /* 使用 log2 近似 */
    if (ns < 1000) return 0;           /* < 1us */
    uint64_t us = ns / 1000;
    if (us < 2)  return 1;
    if (us < 5)  return 2;
    if (us < 10) return 3;
    if (us < 20) return 4;
    if (us < 50) return 5;

    /* 100us 以上用 log10 */
    int bucket = 6;
    uint64_t boundary = 100;
    while (us >= boundary * 10 && bucket < LATENCY_BUCKETS - 1) {
        bucket += 3;
        boundary *= 10;
    }
    if (us >= boundary * 2 && bucket + 1 < LATENCY_BUCKETS) bucket++;
    if (us >= boundary * 5 && bucket + 1 < LATENCY_BUCKETS) bucket++;
    return bucket;
}

/* ---- 记录延迟 ---- */
static void record_latency(uint64_t rtt_ns) {
    g_latency_sum_ns += rtt_ns;
    g_latency_count++;
    if (rtt_ns < g_latency_min_ns) g_latency_min_ns = rtt_ns;
    if (rtt_ns > g_latency_max_ns) g_latency_max_ns = rtt_ns;

    int b = latency_bucket(rtt_ns);
    if (b >= 0 && b < LATENCY_BUCKETS) {
        __atomic_fetch_add(&g_latency_hist[b], 1, __ATOMIC_RELAXED);
    }
}

/* ---- 计算百分位 ---- */
static uint64_t percentile_p50(void) {
    uint64_t target = g_latency_count / 2;
    uint64_t cum = 0;
    for (int i = 0; i < LATENCY_BUCKETS; i++) {
        cum += g_latency_hist[i];
        if (cum >= target) {
            /* 返回该桶的中点（近似） */
            return (uint64_t)(1000 * pow(10, (double)i / 3.0));
        }
    }
    return g_latency_max_ns;
}

static uint64_t percentile_p90(void) {
    uint64_t target = g_latency_count * 90 / 100;
    uint64_t cum = 0;
    for (int i = 0; i < LATENCY_BUCKETS; i++) {
        cum += g_latency_hist[i];
        if (cum >= target) {
            return (uint64_t)(1000 * pow(10, (double)i / 3.0));
        }
    }
    return g_latency_max_ns;
}

static uint64_t percentile_p99(void) {
    uint64_t target = g_latency_count * 99 / 100;
    uint64_t cum = 0;
    for (int i = 0; i < LATENCY_BUCKETS; i++) {
        cum += g_latency_hist[i];
        if (cum >= target) {
            return (uint64_t)(1000 * pow(10, (double)i / 3.0));
        }
    }
    return g_latency_max_ns;
}

static uint64_t percentile_p999(void) {
    uint64_t target = g_latency_count * 999 / 1000;
    uint64_t cum = 0;
    for (int i = 0; i < LATENCY_BUCKETS; i++) {
        cum += g_latency_hist[i];
        if (cum >= target) {
            return (uint64_t)(1000 * pow(10, (double)i / 3.0));
        }
    }
    return g_latency_max_ns;
}

/* ---- 获取进程 CPU 时间 ---- */
static void get_cpu_time(uint64_t *utime, uint64_t *stime) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    *utime = (uint64_t)ru.ru_utime.tv_sec * 1000000 + ru.ru_utime.tv_usec;
    *stime = (uint64_t)ru.ru_stime.tv_sec * 1000000 + ru.ru_stime.tv_usec;
}

/* ---- 创建非阻塞 TCP 连接 ---- */
static int create_connection(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    set_nonblocking(fd);
    set_nodelay(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    return fd;
}

/* ---- 打印 JSON 结果 ---- */
static void print_json_results(double duration_sec, int target_conns,
                                int peak_conns, const char *backend) {
    double qps = (duration_sec > 0) ? g_total_requests / duration_sec : 0;
    double avg_lat_us = (g_latency_count > 0)
        ? (double)g_latency_sum_ns / g_latency_count / 1000.0 : 0;
    double p50_us = (g_latency_count > 0) ? percentile_p50() / 1000.0 : 0;
    double p90_us = (g_latency_count > 0) ? percentile_p90() / 1000.0 : 0;
    double p99_us = (g_latency_count > 0) ? percentile_p99() / 1000.0 : 0;
    double p999_us = (g_latency_count > 0) ? percentile_p999() / 1000.0 : 0;

    /* CPU 利用率 */
    uint64_t cpu_us = (g_end_utime - g_start_utime) + (g_end_stime - g_start_stime);
    double cpu_pct = (duration_sec > 0) ? (cpu_us / 1000000.0) / duration_sec * 100.0 : 0;

    printf("{\n");
    printf("  \"backend\": \"%s\",\n", backend);
    printf("  \"target_connections\": %d,\n", target_conns);
    printf("  \"peak_connections\": %d,\n", peak_conns);
    printf("  \"duration_sec\": %.2f,\n", duration_sec);
    printf("  \"total_requests\": %lu,\n", (unsigned long)g_total_requests);
    printf("  \"total_errors\": %lu,\n", (unsigned long)g_total_errors);
    printf("  \"total_bytes\": %lu,\n", (unsigned long)g_total_bytes);
    printf("  \"qps\": %.2f,\n", qps);
    printf("  \"bandwidth_mbps\": %.2f,\n", (g_total_bytes * 8.0 / duration_sec / 1000000.0));
    printf("  \"latency_us\": {\n");
    printf("    \"avg\": %.2f,\n", avg_lat_us);
    printf("    \"min\": %.2f,\n", (g_latency_count > 0) ? g_latency_min_ns / 1000.0 : 0);
    printf("    \"max\": %.2f,\n", (g_latency_count > 0) ? g_latency_max_ns / 1000.0 : 0);
    printf("    \"p50\": %.2f,\n", p50_us);
    printf("    \"p90\": %.2f,\n", p90_us);
    printf("    \"p99\": %.2f,\n", p99_us);
    printf("    \"p99.9\": %.2f\n", p999_us);
    printf("  },\n");
    printf("  \"cpu_percent\": %.2f\n", cpu_pct);
    printf("}\n");
}

/* ---- 打印人类可读结果 ---- */
static void print_human_results(double duration_sec, int target_conns,
                                 int peak_conns, const char *backend) {
    double qps = (duration_sec > 0) ? g_total_requests / duration_sec : 0;
    double avg_lat_us = (g_latency_count > 0)
        ? (double)g_latency_sum_ns / g_latency_count / 1000.0 : 0;
    double p50_us = (g_latency_count > 0) ? percentile_p50() / 1000.0 : 0;
    double p90_us = (g_latency_count > 0) ? percentile_p90() / 1000.0 : 0;
    double p99_us = (g_latency_count > 0) ? percentile_p99() / 1000.0 : 0;
    double p999_us = (g_latency_count > 0) ? percentile_p999() / 1000.0 : 0;

    uint64_t cpu_us = (g_end_utime - g_start_utime) + (g_end_stime - g_start_stime);
    double cpu_pct = (duration_sec > 0) ? (cpu_us / 1000000.0) / duration_sec * 100.0 : 0;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║         ChaosEngine I/O Benchmark Results           ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Backend:         %-33s ║\n", backend);
    printf("║  Target Conns:    %-33d ║\n", target_conns);
    printf("║  Peak Conns:      %-33d ║\n", peak_conns);
    printf("║  Duration:        %-30.2f s ║\n", duration_sec);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Total Requests:  %-33lu ║\n", (unsigned long)g_total_requests);
    printf("║  Total Errors:    %-33lu ║\n", (unsigned long)g_total_errors);
    printf("║  QPS:             %-30.2f req/s ║\n", qps);
    printf("║  Bandwidth:       %-27.2f Mbps ║\n",
           (g_total_bytes * 8.0 / duration_sec / 1000000.0));
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Latency (us):                                      ║\n");
    printf("║    Avg:   %-40.2f ║\n", avg_lat_us);
    printf("║    Min:   %-40.2f ║\n",
           (g_latency_count > 0) ? g_latency_min_ns / 1000.0 : 0);
    printf("║    Max:   %-40.2f ║\n",
           (g_latency_count > 0) ? g_latency_max_ns / 1000.0 : 0);
    printf("║    P50:   %-40.2f ║\n", p50_us);
    printf("║    P90:   %-40.2f ║\n", p90_us);
    printf("║    P99:   %-40.2f ║\n", p99_us);
    printf("║    P99.9: %-40.2f ║\n", p999_us);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  CPU Usage:       %-30.2f %% ║\n", cpu_pct);
    printf("╚══════════════════════════════════════════════════════╝\n\n");
}

/* ---- 打印延迟分布直方图 ---- */
static void print_latency_histogram(void) {
    if (g_latency_count == 0) return;

    printf("\n--- Latency Distribution Histogram ---\n");
    printf("%-12s %10s %8s %s\n", "Range", "Count", "Pct", "Bar");
    printf("---------------------------------------------\n");

    uint64_t cum = 0;
    for (int i = 0; i < LATENCY_BUCKETS; i++) {
        if (g_latency_hist[i] == 0) continue;
        cum += g_latency_hist[i];
        double pct = (double)g_latency_hist[i] / g_latency_count * 100.0;
        double cum_pct = (double)cum / g_latency_count * 100.0;

        /* 桶标签 */
        char label[32];
        uint64_t lo_ns = (uint64_t)(1000 * pow(10, (double)i / 3.0));
        if (lo_ns < 1000) snprintf(label, sizeof(label), "<1us");
        else if (lo_ns < 1000000) snprintf(label, sizeof(label), "%.0fus", lo_ns / 1000.0);
        else snprintf(label, sizeof(label), "%.1fms", lo_ns / 1000000.0);

        /* 条形图 */
        int bar_len = (int)(pct * 2);
        if (bar_len > 60) bar_len = 60;

        printf("%-12s %10lu %6.1f%% %5.1f%% ",
               label, (unsigned long)g_latency_hist[i], pct, cum_pct);
        for (int j = 0; j < bar_len; j++) printf("#");
        printf("\n");
    }
    printf("---------------------------------------------\n");
    printf("Total samples: %lu\n\n", (unsigned long)g_latency_count);
}

/* ---- 使用说明 ---- */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "ChaosEngine I/O Performance Benchmark Client\n"
        "\n"
        "Options:\n"
        "  -p, --port PORT       Server port (default: %d)\n"
        "  -c, --conns N         Number of concurrent connections (default: %d)\n"
        "  -d, --duration SEC    Test duration in seconds (default: %d)\n"
        "  -s, --msg-size BYTES  Echo message size (default: %d)\n"
        "  -h, --host HOST       Server host (default: 127.0.0.1)\n"
        "  -j, --json            Output results as JSON\n"
        "  -H, --histogram       Print latency distribution histogram\n"
        "  -b, --backend NAME    Backend label for output (default: auto)\n"
        "  --help                Show this help\n"
        "\n"
        "Examples:\n"
        "  %s -c 100 -d 10              # 100 conns, 10 seconds\n"
        "  %s -c 1000 -d 30 -j          # 1000 conns, JSON output\n"
        "  %s -c 100 -d 10 -H           # With latency histogram\n"
        "\n",
        prog, DEFAULT_PORT, DEFAULT_CONNS, DEFAULT_DURATION, DEFAULT_MSG_SIZE,
        prog, prog, prog);
}

int main(int argc, char *argv[]) {
    int    port       = DEFAULT_PORT;
    int    target_conns = DEFAULT_CONNS;
    int    duration   = DEFAULT_DURATION;
    int    msg_size   = DEFAULT_MSG_SIZE;
    const char *host  = "127.0.0.1";
    const char *backend = "auto";
    int    json_output = 0;
    int    show_hist   = 0;

    /* 解析命令行参数 */
    static struct option long_opts[] = {
        {"port",     required_argument, 0, 'p'},
        {"conns",    required_argument, 0, 'c'},
        {"duration", required_argument, 0, 'd'},
        {"msg-size", required_argument, 0, 's'},
        {"host",     required_argument, 0, 'h'},
        {"json",     no_argument,       0, 'j'},
        {"histogram",no_argument,       0, 'H'},
        {"backend",  required_argument, 0, 'b'},
        {"help",     no_argument,       0, 0},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:c:d:s:h:jHb:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'c': target_conns = atoi(optarg); break;
        case 'd': duration = atoi(optarg); break;
        case 's': msg_size = atoi(optarg); break;
        case 'h': host = optarg; break;
        case 'j': json_output = 1; break;
        case 'H': show_hist = 1; break;
        case 'b': backend = optarg; break;
        default:  usage(argv[0]); return 1;
        }
    }

    if (target_conns < 1) target_conns = 1;
    if (target_conns > MAX_CONNS) target_conns = MAX_CONNS;
    if (msg_size < 1) msg_size = 1;
    if (msg_size > 4000) msg_size = 4000;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* 分配连接上下文 */
    ConnCtx *conns = (ConnCtx *)calloc(target_conns, sizeof(ConnCtx));
    if (!conns) {
        fprintf(stderr, "FATAL: failed to allocate %d connections\n", target_conns);
        return 1;
    }

    /* 生成消息内容 */
    char *message = (char *)malloc(msg_size);
    if (!message) { free(conns); return 1; }
    memset(message, 'A', msg_size);
    message[msg_size - 1] = '\n';

    /* 创建 epoll */
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        free(message); free(conns);
        return 1;
    }

    if (!json_output) {
        printf("=== ChaosEngine I/O Benchmark Client ===\n");
        printf("  Target: %s:%d\n", host, port);
        printf("  Connections: %d\n", target_conns);
        printf("  Duration: %d s\n", duration);
        printf("  Message size: %d bytes\n", msg_size);
        printf("  Backend label: %s\n", backend);
        printf("========================================\n\n");
        printf("Connecting to server...\n");
        fflush(stdout);
    }

    /* ---- Phase 1: 建立连接 ---- */
    uint64_t connect_start = now_ns();
    int connected = 0;
    int connecting = 0;

    /* 分批连接，避免 SYN flood */
    int batch_size = 100;
    for (int i = 0; i < target_conns && g_running; i++) {
        conns[i].fd = create_connection(host, port);
        if (conns[i].fd < 0) {
            g_total_errors++;
            continue;
        }
        conns[i].state = 0; /* connecting */
        conns[i].send_len = msg_size;
        memcpy(conns[i].send_buf, message, msg_size);

        struct epoll_event ev;
        ev.events = EPOLLOUT; /* 等待连接完成 */
        ev.data.ptr = &conns[i];
        epoll_ctl(epfd, EPOLL_CTL_ADD, conns[i].fd, &ev);
        connecting++;

        /* 每 batch_size 个连接暂停一下 */
        if ((i + 1) % batch_size == 0) {
            /* 等待这批连接完成 */
            struct epoll_event events[64];
            int nfds = epoll_wait(epfd, events, 64, 5000);
            for (int j = 0; j < nfds; j++) {
                ConnCtx *c = (ConnCtx *)events[j].data.ptr;
                if (c->state == 0) {
                    /* 检查连接是否成功 */
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err == 0) {
                        c->state = 1; /* 已连接，准备发送 */
                        connected++;
                        g_total_connects++;
                        /* 切换到 EPOLLOUT | EPOLLIN */
                        struct epoll_event ev2;
                        ev2.events = EPOLLOUT | EPOLLIN;  /* level-triggered for reliability */
                        ev2.data.ptr = c;
                        epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev2);
                    } else {
                        close(c->fd);
                        c->fd = -1;
                        g_total_errors++;
                    }
                }
            }
        }
    }

    /* 等待剩余连接完成 */
    {
        uint64_t deadline = now_ns() + 10000000000ULL; /* 10s timeout */
        while (connected < target_conns && now_ns() < deadline && g_running) {
            struct epoll_event events[64];
            int nfds = epoll_wait(epfd, events, 64, 1000);
            for (int j = 0; j < nfds; j++) {
                ConnCtx *c = (ConnCtx *)events[j].data.ptr;
                if (c->state == 0) {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err == 0) {
                        c->state = 1;
                        connected++;
                        g_total_connects++;
                        struct epoll_event ev2;
                        ev2.events = EPOLLOUT | EPOLLIN;  /* level-triggered for reliability */
                        ev2.data.ptr = c;
                        epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev2);
                    } else {
                        close(c->fd);
                        c->fd = -1;
                        g_total_errors++;
                    }
                }
            }
            if (nfds == 0) break;
        }
    }

    uint64_t connect_end = now_ns();
    double connect_time = (connect_end - connect_start) / 1000000000.0;

    if (!json_output) {
        printf("  Connected: %d/%d in %.2f s\n", connected, target_conns, connect_time);
        fflush(stdout);
    }

    if (connected == 0) {
        fprintf(stderr, "ERROR: No connections established\n");
        free(message); free(conns); close(epfd);
        return 1;
    }

    /* ---- Phase 2: 预热 ---- */
    if (!json_output) {
        printf("Warming up (%d s)...\n", WARMUP_SEC);
        fflush(stdout);
    }

    uint64_t warmup_end = now_ns() + (uint64_t)WARMUP_SEC * 1000000000ULL;
    while (now_ns() < warmup_end && g_running) {
        struct epoll_event events[MAX_EVENTS];
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 100);
        for (int i = 0; i < nfds; i++) {
            ConnCtx *c = (ConnCtx *)events[i].data.ptr;
            uint32_t evs = events[i].events;

            if (evs & (EPOLLERR | EPOLLHUP)) {
                close(c->fd);
                c->fd = -1;
                c->state = 0;
                g_total_disconnects++;
                continue;
            }

            if (evs & EPOLLOUT && c->state == 1 && !c->inflight) {
                /* 发送请求 */
                ssize_t n = send(c->fd, c->send_buf, c->send_len, MSG_NOSIGNAL);
                if (n > 0) {
                    c->inflight = 1;
                    c->send_ts_ns = now_ns();
                    c->recv_off = 0;
                } else if (n < 0 && errno != EAGAIN) {
                    close(c->fd); c->fd = -1; c->state = 0;
                    g_total_disconnects++;
                }
            }

            if (evs & EPOLLIN && c->inflight) {
                ssize_t n = recv(c->fd, c->recv_buf + c->recv_off,
                                 c->send_len - c->recv_off, 0);
                if (n > 0) {
                    c->recv_off += n;
                    if (c->recv_off >= c->send_len) {
                        c->inflight = 0;
                    }
                } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
                    close(c->fd); c->fd = -1; c->state = 0;
                    g_total_disconnects++;
                }
            }
        }
    }

    /* ---- Phase 3: 正式测试 ---- */
    if (!json_output) {
        printf("Running benchmark (%d s)...\n", duration);
        fflush(stdout);
    }

    /* 记录起始 CPU 时间 */
    get_cpu_time(&g_start_utime, &g_start_stime);

    uint64_t test_start = now_ns();
    uint64_t test_end = test_start + (uint64_t)duration * 1000000000ULL;
    int peak_conns = connected;

    while (now_ns() < test_end && g_running) {
        struct epoll_event events[MAX_EVENTS];
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 100);
        for (int i = 0; i < nfds; i++) {
            ConnCtx *c = (ConnCtx *)events[i].data.ptr;
            uint32_t evs = events[i].events;

            if (evs & (EPOLLERR | EPOLLHUP)) {
                close(c->fd);
                c->fd = -1;
                c->state = 0;
                c->inflight = 0;
                g_total_disconnects++;
                connected--;
                continue;
            }

            if (evs & EPOLLOUT && c->state == 1 && !c->inflight) {
                ssize_t n = send(c->fd, c->send_buf, c->send_len, MSG_NOSIGNAL);
                if (n > 0) {
                    c->inflight = 1;
                    c->send_ts_ns = now_ns();
                    c->recv_off = 0;
                } else if (n < 0 && errno != EAGAIN) {
                    close(c->fd); c->fd = -1; c->state = 0;
                    g_total_disconnects++; connected--;
                    g_total_errors++;
                }
            }

            if (evs & EPOLLIN && c->inflight) {
                ssize_t n = recv(c->fd, c->recv_buf + c->recv_off,
                                 c->send_len - c->recv_off, 0);
                if (n > 0) {
                    c->recv_off += n;
                    if (c->recv_off >= c->send_len) {
                        /* 完整响应收到 */
                        uint64_t rtt = now_ns() - c->send_ts_ns;
                        record_latency(rtt);
                        c->inflight = 0;
                        g_total_requests++;
                        g_total_bytes += c->send_len;
                    }
                } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
                    close(c->fd); c->fd = -1; c->state = 0;
                    c->inflight = 0;
                    g_total_disconnects++; connected--;
                    g_total_errors++;
                }
            }
        }

        if (connected > peak_conns) peak_conns = connected;
    }

    uint64_t test_actual_end = now_ns();
    double actual_duration = (test_actual_end - test_start) / 1000000000.0;

    /* 记录结束 CPU 时间 */
    get_cpu_time(&g_end_utime, &g_end_stime);

    /* ---- 输出结果 ---- */
    if (json_output) {
        print_json_results(actual_duration, target_conns, peak_conns, backend);
    } else {
        print_human_results(actual_duration, target_conns, peak_conns, backend);
    }

    if (show_hist) {
        print_latency_histogram();
    }

    /* ---- 清理 ---- */
    for (int i = 0; i < target_conns; i++) {
        if (conns[i].fd >= 0) close(conns[i].fd);
    }
    free(conns);
    free(message);
    close(epfd);

    return 0;
}
