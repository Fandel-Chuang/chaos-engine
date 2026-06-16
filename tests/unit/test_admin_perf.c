/*
 * test_admin_perf.c — Phase 5.4: Admin IPC 性能基准测试
 *
 * 测试 JSON-RPC 请求/响应的吞吐量、延迟和并发性能。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

/* ---- 测试框架 ---- */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  TEST: %s ... ", name); \
    fflush(stdout); \
} while(0)

#define PASS() do { \
    printf("PASS\n"); \
    tests_passed++; \
} while(0)

#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { FAIL(msg); return; } \
} while(0)

/* ---- 计时辅助 ---- */

static double get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000000.0 + (double)tv.tv_usec;
}

/* ---- Socket 辅助 ---- */

static int connect_to_socket(const char* path, int timeout_sec) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_request(int fd, const char* request, char* response, int max_len) {
    size_t req_len = strlen(request);
    char* send_buf = (char*)malloc(req_len + 2);
    if (!send_buf) return -1;
    memcpy(send_buf, request, req_len);
    send_buf[req_len] = '\n';
    send_buf[req_len + 1] = '\0';

    size_t sent = 0;
    while (sent < req_len + 1) {
        ssize_t n = send(fd, send_buf + sent, req_len + 1 - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(send_buf);
            return -1;
        }
        sent += (size_t)n;
    }
    free(send_buf);

    int total = 0;
    while (total < max_len - 1) {
        ssize_t n = recv(fd, response + total, 1, 0);
        if (n <= 0) {
            if (n == 0 && total > 0) break;
            return -1;
        }
        if (response[total] == '\n') {
            response[total] = '\0';
            if (total > 0 && response[total - 1] == '\r') {
                response[total - 1] = '\0';
            }
            return total;
        }
        total++;
    }
    response[total] = '\0';
    return total;
}

/* ---- Echo 服务器线程 ---- */

typedef struct {
    const char* path;
    volatile int ready;
    volatile int stop;
} EchoServerCtx;

static void* echo_server_thread(void* arg) {
    EchoServerCtx* ctx = (EchoServerCtx*)arg;

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) return NULL;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ctx->path, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(listen_fd);
        return NULL;
    }
    if (listen(listen_fd, 5) < 0) {
        close(listen_fd);
        return NULL;
    }

    ctx->ready = 1;

    while (!ctx->stop) {
        struct timeval tv = {0, 100000}; /* 100ms timeout */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);

        int ret = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret <= 0) continue;

        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) continue;

        /* 处理客户端请求 */
        char buf[4096];
        while (1) {
            int total = 0;
            while (total < 4095) {
                ssize_t n = recv(client_fd, buf + total, 1, 0);
                if (n <= 0) goto client_done;
                if (buf[total] == '\n') { buf[total] = '\0'; break; }
                total++;
            }

            /* 构造 echo 响应 */
            char resp[8192];
            snprintf(resp, sizeof(resp),
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"echo\":true,\"len\":%d}}\n",
                     total);
            send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
        }
client_done:
        close(client_fd);
    }

    close(listen_fd);
    return NULL;
}

/* ---- 5.4.1: 单请求延迟基准 ---- */

static void test_perf_single_request_latency(void) {
    TEST("single request latency benchmark");

    const char* path = "/tmp/test_admin_perf_latency.sock";
    unlink(path);

    EchoServerCtx ctx = {path, 0, 0};
    pthread_t server_thread;
    pthread_create(&server_thread, NULL, echo_server_thread, &ctx);

    /* 等待服务器就绪 */
    while (!ctx.ready) usleep(10000);

    int fd = connect_to_socket(path, 2);
    ASSERT(fd >= 0, "connect");

    const char* req = "{\"jsonrpc\":\"2.0\",\"method\":\"stats\",\"id\":1}";
    char resp[8192];

    /* 预热 */
    for (int i = 0; i < 10; i++) {
        send_request(fd, req, resp, sizeof(resp));
    }

    /* 测量 100 次请求的延迟 */
    int iterations = 100;
    double total_time_us = 0.0;
    double min_time_us = 1e9;
    double max_time_us = 0.0;

    for (int i = 0; i < iterations; i++) {
        double start = get_time_us();
        int ret = send_request(fd, req, resp, sizeof(resp));
        double elapsed = get_time_us() - start;

        ASSERT(ret > 0, "request should succeed");
        total_time_us += elapsed;
        if (elapsed < min_time_us) min_time_us = elapsed;
        if (elapsed > max_time_us) max_time_us = elapsed;
    }

    double avg_us = total_time_us / iterations;
    double throughput = 1000000.0 / avg_us; /* requests per second */

    printf("\n    Latency stats (100 requests):\n");
    printf("      Avg:   %.1f us\n", avg_us);
    printf("      Min:   %.1f us\n", min_time_us);
    printf("      Max:   %.1f us\n", max_time_us);
    printf("      Thrpt: %.0f req/s\n", throughput);

    /* 性能断言：平均延迟应 < 10ms */
    ASSERT(avg_us < 10000.0, "average latency should be under 10ms");

    close(fd);
    ctx.stop = 1;
    pthread_join(server_thread, NULL);
    unlink(path);

    PASS();
}

/* ---- 5.4.2: 吞吐量基准 ---- */

static void test_perf_throughput(void) {
    TEST("throughput benchmark (1000 requests)");

    const char* path = "/tmp/test_admin_perf_throughput.sock";
    unlink(path);

    EchoServerCtx ctx = {path, 0, 0};
    pthread_t server_thread;
    pthread_create(&server_thread, NULL, echo_server_thread, &ctx);

    while (!ctx.ready) usleep(10000);

    int fd = connect_to_socket(path, 2);
    ASSERT(fd >= 0, "connect");

    const char* req = "{\"jsonrpc\":\"2.0\",\"method\":\"stats\",\"id\":1}";
    char resp[8192];

    int total_requests = 1000;
    int success = 0;
    double start = get_time_us();

    for (int i = 0; i < total_requests; i++) {
        int ret = send_request(fd, req, resp, sizeof(resp));
        if (ret > 0) success++;
    }

    double elapsed_us = get_time_us() - start;
    double elapsed_s = elapsed_us / 1000000.0;
    double throughput = (double)success / elapsed_s;

    printf("\n    Throughput stats (%d requests):\n", total_requests);
    printf("      Success:  %d/%d\n", success, total_requests);
    printf("      Time:     %.3f s\n", elapsed_s);
    printf("      Thrpt:    %.0f req/s\n", throughput);

    ASSERT(success == total_requests, "all requests should succeed");
    ASSERT(throughput > 100.0, "throughput should be > 100 req/s");

    close(fd);
    ctx.stop = 1;
    pthread_join(server_thread, NULL);
    unlink(path);

    PASS();
}

/* ---- 5.4.3: JSON 编码性能 ---- */

static void test_perf_json_encoding(void) {
    TEST("JSON encoding performance");

    /* 测试 JSON 键值对编码速度 */
    int iterations = 10000;
    double start = get_time_us();

    for (int i = 0; i < iterations; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{"
                 "\"entity_count\":%d,\"fps\":%.1f,\"uptime\":%.1f}}",
                 i, i * 10, 60.0, (double)i);
    }

    double elapsed_us = get_time_us() - start;
    double avg_us = elapsed_us / iterations;

    printf("\n    JSON encoding stats (%d iterations):\n", iterations);
    printf("      Total: %.1f us\n", elapsed_us);
    printf("      Avg:   %.1f us/encode\n", avg_us);

    ASSERT(avg_us < 100.0, "JSON encoding should be fast (< 100us per encode)");

    PASS();
}

/* ---- 5.4.4: JSON 解析性能 ---- */

static int json_extract_string(const char* json, const char* key, char* out, int max_len) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ':' || *pos == ' ' || *pos == '\t') pos++;
    if (*pos != '"') return -1;
    pos++;
    int i = 0;
    while (*pos && *pos != '"' && i < max_len - 1) {
        if (*pos == '\\' && *(pos + 1)) {
            pos++;
            switch (*pos) {
            case '"':  out[i++] = '"';  break;
            case '\\': out[i++] = '\\'; break;
            case 'n':  out[i++] = '\n'; break;
            default:   out[i++] = *pos; break;
            }
        } else {
            out[i++] = *pos;
        }
        pos++;
    }
    out[i] = '\0';
    return i;
}

static void test_perf_json_parsing(void) {
    TEST("JSON parsing performance");

    const char* test_json = "{\"jsonrpc\":\"2.0\",\"method\":\"stats\",\"id\":12345,"
                            "\"params\":{\"lines\":100,\"since_us\":0}}";

    int iterations = 10000;
    double start = get_time_us();

    for (int i = 0; i < iterations; i++) {
        char method[64];
        json_extract_string(test_json, "method", method, sizeof(method));
    }

    double elapsed_us = get_time_us() - start;
    double avg_us = elapsed_us / iterations;

    printf("\n    JSON parsing stats (%d iterations):\n", iterations);
    printf("      Total: %.1f us\n", elapsed_us);
    printf("      Avg:   %.1f us/parse\n", avg_us);

    ASSERT(avg_us < 50.0, "JSON parsing should be fast (< 50us per parse)");

    PASS();
}

/* ---- 5.4.5: 大响应传输性能 ---- */

static void test_perf_large_response(void) {
    TEST("large response transfer performance");

    const char* path = "/tmp/test_admin_perf_large.sock";
    unlink(path);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT(listen_fd >= 0, "create socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    ASSERT(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0, "bind");
    ASSERT(listen(listen_fd, 1) == 0, "listen");

    int client_fd = connect_to_socket(path, 2);
    ASSERT(client_fd >= 0, "connect");

    int server_fd = accept(listen_fd, NULL, NULL);
    ASSERT(server_fd >= 0, "accept");

    /* 构造大响应（~64KB） */
    char* large_resp = (char*)malloc(70000);
    ASSERT(large_resp != NULL, "allocate large buffer");

    /* 构造包含大量数据的 JSON 响应 */
    int offset = snprintf(large_resp, 70000,
                          "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"data\":\"");
    for (int i = 0; i < 60000 && offset < 69000; i++) {
        large_resp[offset++] = 'A' + (i % 26);
    }
    snprintf(large_resp + offset, 70000 - offset, "\"}}\n");

    /* 发送请求 */
    const char* req = "{\"jsonrpc\":\"2.0\",\"method\":\"large\",\"id\":1}\n";
    send(client_fd, req, strlen(req), MSG_NOSIGNAL);

    /* 服务器读取请求 */
    char buf[4096];
    recv(server_fd, buf, sizeof(buf) - 1, 0);

    /* 测量大响应发送时间 */
    double start = get_time_us();
    size_t resp_len = strlen(large_resp);
    send(server_fd, large_resp, resp_len, MSG_NOSIGNAL);

    /* 客户端接收 */
    char* client_buf = (char*)malloc(70000);
    int total = 0;
    while (total < 69999) {
        ssize_t n = recv(client_fd, client_buf + total, 4096, 0);
        if (n <= 0) break;
        total += n;
        if (total > 0 && client_buf[total - 1] == '\n') break;
    }
    double elapsed_us = get_time_us() - start;

    printf("\n    Large response stats:\n");
    printf("      Response size: %zu bytes\n", resp_len);
    printf("      Received:      %d bytes\n", total);
    printf("      Time:          %.1f us\n", elapsed_us);
    printf("      Speed:         %.1f MB/s\n",
           (double)total / elapsed_us); /* MB/s = bytes/us */

    ASSERT(total > 1000, "should receive large response");

    free(large_resp);
    free(client_buf);
    close(client_fd);
    close(server_fd);
    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 5.4.6: 连接建立性能 ---- */

static void test_perf_connection_setup(void) {
    TEST("connection setup performance");

    const char* path = "/tmp/test_admin_perf_connect.sock";
    unlink(path);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT(listen_fd >= 0, "create socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    ASSERT(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0, "bind");
    ASSERT(listen(listen_fd, 5) == 0, "listen");

    int iterations = 100;
    double total_time_us = 0.0;

    for (int i = 0; i < iterations; i++) {
        double start = get_time_us();

        int fd = connect_to_socket(path, 1);
        ASSERT(fd >= 0, "connect");

        int server_fd = accept(listen_fd, NULL, NULL);
        ASSERT(server_fd >= 0, "accept");

        double elapsed = get_time_us() - start;
        total_time_us += elapsed;

        close(fd);
        close(server_fd);
    }

    double avg_us = total_time_us / iterations;

    printf("\n    Connection setup stats (%d iterations):\n", iterations);
    printf("      Avg: %.1f us/connection\n", avg_us);

    ASSERT(avg_us < 5000.0, "connection setup should be < 5ms");

    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 主函数 ---- */

int main(void) {
    printf("\n=== Phase 5.4: Admin IPC Performance Benchmarks ===\n\n");

    test_perf_single_request_latency();
    test_perf_throughput();
    test_perf_json_encoding();
    test_perf_json_parsing();
    test_perf_large_response();
    test_perf_connection_setup();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
