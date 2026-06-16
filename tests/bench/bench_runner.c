/*
 * ChaosEngine I/O 性能基准测试自动化运行器
 *
 * 自动启动 io_uring 和 POSIX 服务器，运行 bench_client 并收集对比结果。
 *
 * 用法：
 *   ./bench_runner [--conns N] [--duration SEC] [--msg-size BYTES]
 *
 * 工作流程：
 *   1. 启动 io_uring 服务器 (chaos_async_bench)
 *   2. 等待服务器就绪
 *   3. 运行 bench_client 进行压力测试
 *   4. 停止服务器
 *   5. 启动 POSIX 服务器 (chaos_async_bench_posix)
 *   6. 重复步骤 2-4
 *   7. 输出对比报告
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

/* ---- 默认参数 ---- */
#define DEFAULT_CONNS      100
#define DEFAULT_DURATION   10
#define DEFAULT_MSG_SIZE   64
#define DEFAULT_URING_PORT 17778
#define DEFAULT_POSIX_PORT 17779
#define SERVER_STARTUP_WAIT 3   /* 等待服务器启动的秒数 */
#define SERVER_READY_TIMEOUT 30 /* 等待服务器就绪的超时秒数 */

/* ---- 测试结果 ---- */
typedef struct {
    char     backend[32];
    int      target_conns;
    int      peak_conns;
    double   duration_sec;
    uint64_t total_requests;
    uint64_t total_errors;
    double   qps;
    double   avg_lat_us;
    double   p50_lat_us;
    double   p90_lat_us;
    double   p99_lat_us;
    double   p999_lat_us;
    double   cpu_pct;
    double   bandwidth_mbps;
    int      success;  /* 测试是否成功 */
} BenchResult;

/* ---- 全局状态 ---- */
static pid_t g_server_pid = 0;

static void kill_server(void) {
    if (g_server_pid > 0) {
        kill(g_server_pid, SIGTERM);
        /* 等待进程退出 */
        int status;
        int waited = 0;
        while (waited < 30) {
            pid_t w = waitpid(g_server_pid, &status, WNOHANG);
            if (w > 0) break;
            usleep(100000);
            waited++;
        }
        if (waited >= 30) {
            kill(g_server_pid, SIGKILL);
            waitpid(g_server_pid, &status, 0);
        }
        g_server_pid = 0;
    }
}

/* ---- 检查端口是否可连接 ---- */
static int check_port(const char *host, int port, int timeout_sec) {
    time_t deadline = time(NULL) + timeout_sec;
    while (time(NULL) < deadline) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { usleep(500000); continue; }

        /* 设置非阻塞 */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host, &addr.sin_addr);

        int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (ret == 0) {
            close(fd);
            return 1; /* 立即连接成功 */
        }
        if (errno == EINPROGRESS) {
            /* 等待连接完成 */
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv = {1, 0};
            ret = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (ret > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                close(fd);
                if (err == 0) return 1;
            }
        }
        close(fd);
        usleep(500000); /* 500ms 后重试 */
    }
    return 0;
}

/* ---- 启动服务器 ---- */
static int start_server(const char *server_bin, int port, int quiet) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        /* 子进程：启动服务器 */
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);

        /* 设置环境变量 */
        setenv("CHAOS_BENCH_PORT", port_str, 1);
        if (quiet) setenv("CHAOS_BENCH_QUIET", "1", 1);

        /* 重定向 stdout/stderr 到 /dev/null（静默模式） */
        if (quiet) {
            freopen("/dev/null", "w", stdout);
            freopen("/dev/null", "w", stderr);
        }

        execl(server_bin, server_bin, port_str, NULL);
        /* execl 失败 */
        fprintf(stderr, "FATAL: failed to exec %s: %s\n", server_bin, strerror(errno));
        _exit(1);
    }

    g_server_pid = pid;
    return 0;
}

/* ---- 解析 bench_client JSON 输出 ---- */
static int parse_bench_output(const char *json_str, BenchResult *result) {
    /* 简单的 JSON 解析（不依赖第三方库） */
    char *s = (char *)json_str;

    /* 提取各字段 */
    #define EXTRACT_FIELD(name, fmt, field) do { \
        char *p = strstr(s, "\"" name "\":"); \
        if (p) { \
            p += strlen("\"" name "\":"); \
            while (*p == ' ' || *p == '\t') p++; \
            sscanf(p, fmt, &result->field); \
        } \
    } while(0)

    EXTRACT_FIELD("qps", "%lf", qps);
    EXTRACT_FIELD("total_requests", "%lu", total_requests);
    EXTRACT_FIELD("total_errors", "%lu", total_errors);
    EXTRACT_FIELD("duration_sec", "%lf", duration_sec);
    EXTRACT_FIELD("peak_connections", "%d", peak_conns);
    EXTRACT_FIELD("target_connections", "%d", target_conns);
    EXTRACT_FIELD("cpu_percent", "%lf", cpu_pct);
    EXTRACT_FIELD("bandwidth_mbps", "%lf", bandwidth_mbps);

    /* 解析嵌套的 latency_us */
    char *lat = strstr(s, "\"latency_us\":");
    if (lat) {
        char *p;
        #define EXTRACT_LAT(name, field) do { \
            p = strstr(lat, "\"" name "\":"); \
            if (p) { \
                p += strlen("\"" name "\":"); \
                while (*p == ' ' || *p == '\t') p++; \
                sscanf(p, "%lf", &result->field); \
            } \
        } while(0)

        EXTRACT_LAT("avg", avg_lat_us);
        EXTRACT_LAT("p50", p50_lat_us);
        EXTRACT_LAT("p90", p90_lat_us);
        EXTRACT_LAT("p99", p99_lat_us);
        EXTRACT_LAT("p99.9", p999_lat_us);
    }

    result->success = (result->total_requests > 0);
    return result->success ? 0 : -1;
}

/* ---- 运行 bench_client 并捕获输出 ---- */
static int run_bench_client(const char *client_bin, int port, int conns,
                             int duration, int msg_size, const char *backend,
                             BenchResult *result) {
    memset(result, 0, sizeof(*result));
    strncpy(result->backend, backend, sizeof(result->backend) - 1);

    /* 创建管道捕获输出 */
    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("pipe"); return -1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        /* 子进程：运行 bench_client */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        char port_str[16], conns_str[16], dur_str[16], size_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);
        snprintf(conns_str, sizeof(conns_str), "%d", conns);
        snprintf(dur_str, sizeof(dur_str), "%d", duration);
        snprintf(size_str, sizeof(size_str), "%d", msg_size);

        execl(client_bin, client_bin,
              "-p", port_str,
              "-c", conns_str,
              "-d", dur_str,
              "-s", size_str,
              "-j",
              "-b", backend,
              NULL);
        _exit(1);
    }

    /* 父进程：读取输出 */
    close(pipefd[1]);

    char output[65536];
    memset(output, 0, sizeof(output));
    int total = 0;
    ssize_t n;
    while ((n = read(pipefd[0], output + total, sizeof(output) - total - 1)) > 0) {
        total += n;
        if (total >= (int)sizeof(output) - 1) break;
    }
    close(pipefd[0]);

    /* 等待子进程 */
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return parse_bench_output(output, result);
    }

    return -1;
}

/* ---- 打印对比表格 ---- */
static void print_comparison(BenchResult *uring, BenchResult *posix) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║           ChaosEngine I/O Backend Performance Comparison            ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════╣\n");
    printf("║  Test: %d conns, %d s duration, %d byte messages                      ║\n",
           uring->target_conns, (int)uring->duration_sec,
           DEFAULT_MSG_SIZE);
    printf("╠══════════════════════════════════════════╦═════════════╦═════════════╣\n");
    printf("║  Metric                                  ║  io_uring   ║  POSIX      ║\n");
    printf("╠══════════════════════════════════════════╬═════════════╬═════════════╣\n");

    #define PRINT_ROW(label, fmt, a, b) \
        printf("║  %-40s ║ " fmt " ║ " fmt " ║\n", label, a, b)

    PRINT_ROW("QPS (requests/sec)", "%11.2f", uring->qps, posix->qps);
    PRINT_ROW("Total Requests",     "%11lu", (unsigned long)uring->total_requests,
             (unsigned long)posix->total_requests);
    PRINT_ROW("Total Errors",       "%11lu", (unsigned long)uring->total_errors,
             (unsigned long)posix->total_errors);
    PRINT_ROW("Bandwidth (Mbps)",   "%11.2f", uring->bandwidth_mbps, posix->bandwidth_mbps);
    PRINT_ROW("CPU Usage (%)",      "%11.2f", uring->cpu_pct, posix->cpu_pct);

    printf("╠══════════════════════════════════════════╬═════════════╬═════════════╣\n");
    printf("║  Latency (microseconds)                  ║             ║             ║\n");
    PRINT_ROW("  Average",           "%11.2f", uring->avg_lat_us, posix->avg_lat_us);
    PRINT_ROW("  P50 (median)",      "%11.2f", uring->p50_lat_us, posix->p50_lat_us);
    PRINT_ROW("  P90",               "%11.2f", uring->p90_lat_us, posix->p90_lat_us);
    PRINT_ROW("  P99",               "%11.2f", uring->p99_lat_us, posix->p99_lat_us);
    PRINT_ROW("  P99.9",             "%11.2f", uring->p999_lat_us, posix->p999_lat_us);

    printf("╠══════════════════════════════════════════╩═════════════╩═════════════╣\n");

    /* 计算提升比例 */
    if (uring->success && posix->success && posix->qps > 0) {
        double qps_ratio = uring->qps / posix->qps;
        double lat_ratio = posix->avg_lat_us > 0 ? posix->avg_lat_us / uring->avg_lat_us : 0;
        double cpu_ratio = posix->cpu_pct > 0 ? posix->cpu_pct / uring->cpu_pct : 0;

        printf("║  SUMMARY: io_uring vs POSIX                                         ║\n");
        printf("║    QPS:       %.2fx %s                                            ║\n",
               qps_ratio, qps_ratio >= 1.0 ? "higher  ▲" : "lower   ▼");
        printf("║    Latency:   %.2fx %s                                         ║\n",
               lat_ratio, lat_ratio >= 1.0 ? "better  ▲" : "worse   ▼");
        printf("║    CPU Eff:   %.2fx %s                                      ║\n",
               cpu_ratio, cpu_ratio >= 1.0 ? "more efficient ▲" : "less efficient ▼");
    }

    printf("╚══════════════════════════════════════════════════════════════════════╝\n\n");
}

/* ---- 使用说明 ---- */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "Automated io_uring vs POSIX benchmark runner\n"
        "\n"
        "Options:\n"
        "  -c, --conns N         Concurrent connections (default: %d)\n"
        "  -d, --duration SEC    Test duration per backend (default: %d)\n"
        "  -s, --msg-size BYTES  Echo message size (default: %d)\n"
        "  --uring-bin PATH      Path to io_uring server binary\n"
        "  --posix-bin PATH      Path to POSIX server binary\n"
        "  --client-bin PATH     Path to bench_client binary\n"
        "  --uring-port PORT     io_uring server port (default: %d)\n"
        "  --posix-port PORT     POSIX server port (default: %d)\n"
        "  --help                Show this help\n"
        "\n",
        prog, DEFAULT_CONNS, DEFAULT_DURATION, DEFAULT_MSG_SIZE,
        DEFAULT_URING_PORT, DEFAULT_POSIX_PORT);
}

int main(int argc, char *argv[]) {
    int    conns       = DEFAULT_CONNS;
    int    duration    = DEFAULT_DURATION;
    int    msg_size    = DEFAULT_MSG_SIZE;
    int    uring_port  = DEFAULT_URING_PORT;
    int    posix_port  = DEFAULT_POSIX_PORT;
    const char *uring_bin  = "./chaos_async_bench";
    const char *posix_bin  = "./chaos_async_bench_posix";
    const char *client_bin = "./bench_client";

    /* 解析命令行参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
        else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--conns") == 0)
            conns = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--duration") == 0)
            duration = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--msg-size") == 0)
            msg_size = atoi(argv[++i]);
        else if (strcmp(argv[i], "--uring-bin") == 0)
            uring_bin = argv[++i];
        else if (strcmp(argv[i], "--posix-bin") == 0)
            posix_bin = argv[++i];
        else if (strcmp(argv[i], "--client-bin") == 0)
            client_bin = argv[++i];
        else if (strcmp(argv[i], "--uring-port") == 0)
            uring_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--posix-port") == 0)
            posix_port = atoi(argv[++i]);
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   ChaosEngine I/O Backend Benchmark Runner          ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Connections:  %-36d ║\n", conns);
    printf("║  Duration:     %-36d ║\n", duration);
    printf("║  Message size: %-36d ║\n", msg_size);
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    BenchResult uring_result, posix_result;
    memset(&uring_result, 0, sizeof(uring_result));
    memset(&posix_result, 0, sizeof(posix_result));

    /* ================================================================
     * 测试 1: io_uring 后端
     * ================================================================ */
    printf("═══ Phase 1: Testing io_uring backend ═══\n");
    printf("  Starting io_uring server on port %d...\n", uring_port);

    if (start_server(uring_bin, uring_port, 1) < 0) {
        fprintf(stderr, "ERROR: Failed to start io_uring server\n");
        goto posix_test;
    }

    printf("  Waiting for server to be ready...\n");
    fflush(stdout);

    if (!check_port("127.0.0.1", uring_port, SERVER_READY_TIMEOUT)) {
        fprintf(stderr, "ERROR: io_uring server did not become ready\n");
        kill_server();
        goto posix_test;
    }

    printf("  Server ready. Running benchmark...\n");
    fflush(stdout);

    if (run_bench_client(client_bin, uring_port, conns, duration, msg_size,
                          "io_uring", &uring_result) < 0) {
        fprintf(stderr, "ERROR: bench_client failed for io_uring\n");
    } else {
        printf("  io_uring: QPS=%.2f, AvgLat=%.2fus, P99=%.2fus, CPU=%.1f%%\n",
               uring_result.qps, uring_result.avg_lat_us,
               uring_result.p99_lat_us, uring_result.cpu_pct);
    }

    printf("  Stopping io_uring server...\n");
    kill_server();
    printf("  Done.\n\n");

    /* ================================================================
     * 测试 2: POSIX 后端
     * ================================================================ */
posix_test:
    printf("═══ Phase 2: Testing POSIX backend ═══\n");
    printf("  Starting POSIX server on port %d...\n", posix_port);

    if (start_server(posix_bin, posix_port, 1) < 0) {
        fprintf(stderr, "ERROR: Failed to start POSIX server\n");
        goto done;
    }

    printf("  Waiting for server to be ready...\n");
    fflush(stdout);

    if (!check_port("127.0.0.1", posix_port, SERVER_READY_TIMEOUT)) {
        fprintf(stderr, "ERROR: POSIX server did not become ready\n");
        kill_server();
        goto done;
    }

    printf("  Server ready. Running benchmark...\n");
    fflush(stdout);

    if (run_bench_client(client_bin, posix_port, conns, duration, msg_size,
                          "posix", &posix_result) < 0) {
        fprintf(stderr, "ERROR: bench_client failed for POSIX\n");
    } else {
        printf("  POSIX:   QPS=%.2f, AvgLat=%.2fus, P99=%.2fus, CPU=%.1f%%\n",
               posix_result.qps, posix_result.avg_lat_us,
               posix_result.p99_lat_us, posix_result.cpu_pct);
    }

    printf("  Stopping POSIX server...\n");
    kill_server();
    printf("  Done.\n\n");

done:
    /* ================================================================
     * 输出对比报告
     * ================================================================ */
    if (uring_result.success || posix_result.success) {
        print_comparison(&uring_result, &posix_result);
    } else {
        printf("\nERROR: Both benchmarks failed. No comparison available.\n");
    }

    return 0;
}
