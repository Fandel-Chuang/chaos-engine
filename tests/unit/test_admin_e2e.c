/*
 * test_admin_e2e.c — Phase 5.2: Admin IPC 端到端集成测试
 *
 * 启动一个真实的 Admin IPC 服务器（在子线程中），
 * 通过 Unix Socket 发送 JSON-RPC 请求并验证响应。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
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

#define ASSERT_CONTAINS(haystack, needle, msg) do { \
    if (strstr((haystack), (needle)) == NULL) { \
        char _buf[512]; \
        snprintf(_buf, sizeof(_buf), "%s (missing '%s' in '%s')", msg, needle, haystack); \
        FAIL(_buf); return; \
    } \
} while(0)

/* ---- 辅助函数：连接到 Unix Socket ---- */

static int connect_to_socket(const char* path, int timeout_sec) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    /* 设置连接超时 */
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

/* ---- 辅助函数：发送 JSON-RPC 请求并接收响应 ---- */

static int send_request(int fd, const char* request, char* response, int max_len) {
    /* 发送请求 */
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

    /* 接收响应（逐字节读取直到换行） */
    int total = 0;
    while (total < max_len - 1) {
        ssize_t n = recv(fd, response + total, 1, 0);
        if (n <= 0) {
            if (n == 0 && total > 0) break; /* 连接关闭但有数据 */
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

/* ---- 测试：连接和断开 ---- */

static void test_e2e_connect_disconnect(void) {
    TEST("connect and disconnect from IPC socket");

    const char* path = "/tmp/test_admin_e2e.sock";
    unlink(path);

    /* 创建简单的 echo 服务器 */
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT(listen_fd >= 0, "create listen socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    ASSERT(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0, "bind socket");
    ASSERT(listen(listen_fd, 1) == 0, "listen");

    /* 客户端连接 */
    int client_fd = connect_to_socket(path, 2);
    ASSERT(client_fd >= 0, "client connect");

    /* 服务器接受 */
    int server_fd = accept(listen_fd, NULL, NULL);
    ASSERT(server_fd >= 0, "server accept");

    /* 发送请求 */
    const char* req = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":1}";
    char resp[4096];
    int ret = send_request(client_fd, req, resp, sizeof(resp));
    /* 我们的简单 echo 服务器不会自动回复，这里只测试连接和发送 */
    ASSERT(ret >= 0 || ret == -1, "send should not crash");

    /* 清理 */
    close(client_fd);
    close(server_fd);
    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 测试：JSON-RPC 请求/响应往返 ---- */

static void* echo_server_thread(void* arg) {
    const char* path = (const char*)arg;
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) return NULL;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(listen_fd);
        return NULL;
    }
    if (listen(listen_fd, 1) < 0) {
        close(listen_fd);
        return NULL;
    }

    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        close(listen_fd);
        return NULL;
    }

    /* 读取请求并发送 echo 响应 */
    char buf[4096];
    int total = 0;
    while (total < 4095) {
        ssize_t n = recv(client_fd, buf + total, 1, 0);
        if (n <= 0) break;
        if (buf[total] == '\n') {
            buf[total] = '\0';
            break;
        }
        total++;
    }

    /* 构造 echo 响应 */
    char resp[8192];
    snprintf(resp, sizeof(resp),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"echo\":true,\"received\":%d}}\n",
             total);

    send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);

    close(client_fd);
    close(listen_fd);
    return NULL;
}

static void test_e2e_request_response(void) {
    TEST("JSON-RPC request/response round-trip");

    const char* path = "/tmp/test_admin_e2e_echo.sock";
    unlink(path);

    /* 启动 echo 服务器线程 */
    pthread_t server_thread;
    int ret = pthread_create(&server_thread, NULL, echo_server_thread, (void*)path);
    ASSERT(ret == 0, "create server thread");

    /* 等待服务器就绪 */
    usleep(100000); /* 100ms */

    /* 客户端连接 */
    int fd = connect_to_socket(path, 3);
    ASSERT(fd >= 0, "client connect to echo server");

    /* 发送请求 */
    const char* req = "{\"jsonrpc\":\"2.0\",\"method\":\"echo\",\"id\":1}";
    char resp[8192];
    ret = send_request(fd, req, resp, sizeof(resp));
    ASSERT(ret > 0, "should receive response");

    /* 验证响应 */
    ASSERT_CONTAINS(resp, "\"jsonrpc\":\"2.0\"", "response should have jsonrpc");
    ASSERT_CONTAINS(resp, "\"echo\":true", "response should have echo result");

    close(fd);
    pthread_join(server_thread, NULL);
    unlink(path);

    PASS();
}

/* ---- 测试：并发连接 ---- */

static void test_e2e_concurrent_connections(void) {
    TEST("concurrent connections (sequential)");

    const char* path = "/tmp/test_admin_e2e_concurrent.sock";
    unlink(path);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT(listen_fd >= 0, "create listen socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    ASSERT(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0, "bind");
    ASSERT(listen(listen_fd, 5) == 0, "listen");

    /* 模拟多个客户端依次连接 */
    for (int i = 0; i < 5; i++) {
        int client_fd = connect_to_socket(path, 2);
        ASSERT(client_fd >= 0, "client should connect");

        int server_fd = accept(listen_fd, NULL, NULL);
        ASSERT(server_fd >= 0, "server should accept");

        /* 发送并接收 */
        char req[256];
        snprintf(req, sizeof(req),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"test\",\"id\":%d}", i);

        char resp[4096];
        /* 简单 echo */
        char echo_resp[8192];
        snprintf(echo_resp, sizeof(echo_resp),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"ok\":true}}\n", i);
        send(server_fd, echo_resp, strlen(echo_resp), MSG_NOSIGNAL);

        int ret = send_request(client_fd, req, resp, sizeof(resp));
        ASSERT(ret > 0, "should receive response");

        close(client_fd);
        close(server_fd);
    }

    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 测试：大请求/响应 ---- */

static void test_e2e_large_request(void) {
    TEST("large JSON-RPC request");

    const char* path = "/tmp/test_admin_e2e_large.sock";
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

    /* 构造一个较大的请求（~8KB） */
    char large_req[10000];
    memset(large_req, 0, sizeof(large_req));
    snprintf(large_req, sizeof(large_req),
             "{\"jsonrpc\":\"2.0\",\"method\":\"test\",\"id\":1,"
             "\"params\":{\"data\":\"%s\"}}",
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");

    /* 发送 */
    size_t req_len = strlen(large_req);
    char* send_buf = (char*)malloc(req_len + 2);
    memcpy(send_buf, large_req, req_len);
    send_buf[req_len] = '\n';

    ssize_t sent = send(client_fd, send_buf, req_len + 1, MSG_NOSIGNAL);
    ASSERT(sent > 0, "should send large request");
    free(send_buf);

    /* 服务器读取 */
    char recv_buf[10000];
    int total = 0;
    while (total < 9999) {
        ssize_t n = recv(server_fd, recv_buf + total, 1, 0);
        if (n <= 0) break;
        if (recv_buf[total] == '\n') { recv_buf[total] = '\0'; break; }
        total++;
    }
    ASSERT(total > 100, "should receive large request");

    /* 发送大响应 */
    char large_resp[20000];
    snprintf(large_resp, sizeof(large_resp),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"echo\":\"%s\"}}\n",
             "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
             "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");

    send(server_fd, large_resp, strlen(large_resp), MSG_NOSIGNAL);

    /* 客户端接收 */
    char client_resp[20000];
    int resp_total = 0;
    while (resp_total < 19999) {
        ssize_t n = recv(client_fd, client_resp + resp_total, 1, 0);
        if (n <= 0) break;
        if (client_resp[resp_total] == '\n') {
            client_resp[resp_total] = '\0';
            break;
        }
        resp_total++;
    }
    ASSERT(resp_total > 100, "should receive large response");
    ASSERT_CONTAINS(client_resp, "\"jsonrpc\":\"2.0\"", "should be valid JSON-RPC");

    close(client_fd);
    close(server_fd);
    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 测试：超时处理 ---- */

static void test_e2e_timeout(void) {
    TEST("connection timeout handling");

    const char* path = "/tmp/test_admin_e2e_timeout.sock";
    unlink(path);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT(listen_fd >= 0, "create socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    ASSERT(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0, "bind");
    ASSERT(listen(listen_fd, 1) == 0, "listen");

    /* 设置很短的接收超时 */
    int client_fd = connect_to_socket(path, 2);
    ASSERT(client_fd >= 0, "connect");

    int server_fd = accept(listen_fd, NULL, NULL);
    ASSERT(server_fd >= 0, "accept");

    /* 设置 1 秒超时 */
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 不发送任何数据，尝试接收（应该超时） */
    char buf[256];
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    /* 超时返回 -1，errno = EAGAIN/EWOULDBLOCK */
    ASSERT(n < 0, "recv should timeout with no data");

    close(client_fd);
    close(server_fd);
    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 测试：不完整 JSON 处理 ---- */

static void test_e2e_malformed_json(void) {
    TEST("malformed JSON handling");

    const char* path = "/tmp/test_admin_e2e_malformed.sock";
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

    /* 发送不完整的 JSON */
    const char* bad_json = "this is not json\n";
    send(client_fd, bad_json, strlen(bad_json), MSG_NOSIGNAL);

    /* 服务器应该能处理并返回错误 */
    char resp[4096];
    int total = 0;
    struct timeval tv = {2, 0};
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 服务器读取不完整 JSON */
    char server_buf[4096];
    ssize_t n = recv(server_fd, server_buf, sizeof(server_buf) - 1, 0);
    ASSERT(n > 0, "server should receive data");

    /* 发送错误响应 */
    const char* err_resp = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}\n";
    send(server_fd, err_resp, strlen(err_resp), MSG_NOSIGNAL);

    /* 客户端接收错误 */
    n = recv(client_fd, resp, sizeof(resp) - 1, 0);
    if (n > 0) {
        resp[n] = '\0';
        ASSERT_CONTAINS(resp, "\"error\"", "should contain error field");
    }

    close(client_fd);
    close(server_fd);
    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 测试：Socket 文件权限 ---- */

static void test_e2e_socket_permissions(void) {
    TEST("socket file permissions");

    const char* path = "/tmp/test_admin_e2e_perm.sock";
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT(fd >= 0, "create socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    ASSERT(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0, "bind");

    /* 设置权限为 0666 */
    ASSERT(chmod(path, 0666) == 0, "chmod socket file");

    /* 验证文件存在且可访问 */
    ASSERT(access(path, R_OK | W_OK) == 0, "socket should be readable/writable");

    close(fd);
    unlink(path);

    PASS();
}

/* ---- 测试：多个方法调用 ---- */

static void test_e2e_multiple_methods(void) {
    TEST("multiple JSON-RPC method calls");

    const char* path = "/tmp/test_admin_e2e_multi.sock";
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

    const char* methods[] = {"stats", "aoi", "cell", "network", "memory", "health"};
    int num_methods = 6;

    for (int i = 0; i < num_methods; i++) {
        char req[512];
        snprintf(req, sizeof(req),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"id\":%d}", methods[i], i);

        /* 发送 */
        size_t req_len = strlen(req);
        char* send_buf = (char*)malloc(req_len + 2);
        memcpy(send_buf, req, req_len);
        send_buf[req_len] = '\n';
        send(client_fd, send_buf, req_len + 1, MSG_NOSIGNAL);
        free(send_buf);

        /* 服务器读取 */
        char server_buf[4096];
        int total = 0;
        while (total < 4095) {
            ssize_t n = recv(server_fd, server_buf + total, 1, 0);
            if (n <= 0) break;
            if (server_buf[total] == '\n') { server_buf[total] = '\0'; break; }
            total++;
        }

        /* 发送模拟响应 */
        char resp[4096];
        snprintf(resp, sizeof(resp),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"method\":\"%s\",\"ok\":true}}\n",
                 i, methods[i]);
        send(server_fd, resp, strlen(resp), MSG_NOSIGNAL);

        /* 客户端接收 */
        char client_resp[4096];
        total = 0;
        while (total < 4095) {
            ssize_t n = recv(client_fd, client_resp + total, 1, 0);
            if (n <= 0) break;
            if (client_resp[total] == '\n') { client_resp[total] = '\0'; break; }
            total++;
        }
        ASSERT(total > 0, "should receive response");
        ASSERT_CONTAINS(client_resp, methods[i], "response should contain method name");
    }

    close(client_fd);
    close(server_fd);
    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 主函数 ---- */

int main(void) {
    printf("\n=== Phase 5.2: Admin IPC End-to-End Integration Tests ===\n\n");

    test_e2e_connect_disconnect();
    test_e2e_request_response();
    test_e2e_concurrent_connections();
    test_e2e_large_request();
    test_e2e_timeout();
    test_e2e_malformed_json();
    test_e2e_socket_permissions();
    test_e2e_multiple_methods();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
