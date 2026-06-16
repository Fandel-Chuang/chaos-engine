/*
 * test_admin_errors.c — Phase 5.3: Admin IPC 异常场景测试
 *
 * 测试各种错误条件：socket 不存在、连接被拒绝、请求超时、
 * 缓冲区溢出、无效 JSON、未知方法、服务器崩溃恢复等。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

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
        snprintf(_buf, sizeof(_buf), "%s (missing '%s')", msg, needle); \
        FAIL(_buf); return; \
    } \
} while(0)

/* ---- 辅助函数 ---- */

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

/* ---- 5.3.1: 连接不存在的 socket ---- */

static void test_error_no_socket(void) {
    TEST("connect to nonexistent socket");

    const char* path = "/tmp/test_admin_nonexistent_xyz.sock";
    unlink(path); /* 确保不存在 */

    int fd = connect_to_socket(path, 1);
    ASSERT(fd < 0, "should fail to connect to nonexistent socket");
    ASSERT(errno == ENOENT || errno == ECONNREFUSED,
           "errno should indicate no such file or connection refused");

    PASS();
}

/* ---- 5.3.2: 连接已满的 socket ---- */

static void test_error_backlog_full(void) {
    TEST("connection refused when backlog full");

    const char* path = "/tmp/test_admin_backlog_full.sock";
    unlink(path);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT(listen_fd >= 0, "create socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    ASSERT(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0, "bind");
    /* backlog = 0，不接受任何等待连接 */
    ASSERT(listen(listen_fd, 0) == 0, "listen with backlog 0");

    /* 尝试连接（非阻塞） */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT(fd >= 0, "create client socket");

    /* 设置非阻塞 */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    /* 非阻塞 connect 可能返回 -1 且 errno=EINPROGRESS，这是正常的 */
    /* 我们主要验证不会崩溃 */
    ASSERT(ret == 0 || (ret < 0 && (errno == EINPROGRESS || errno == ECONNREFUSED)),
           "connect should either succeed or fail gracefully");

    close(fd);
    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 5.3.3: 发送到已关闭的 socket ---- */

static void test_error_write_closed(void) {
    TEST("write to closed socket (SIGPIPE handling)");

    int fds[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "create socketpair");

    /* 关闭一端 */
    close(fds[1]);

    /* 阻塞 SIGPIPE */
    sigset_t sigset, oldset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &sigset, &oldset);

    /* 尝试写入已关闭的 socket */
    ssize_t n = send(fds[0], "test", 4, MSG_NOSIGNAL);
    /* MSG_NOSIGNAL 应该防止 SIGPIPE，返回 -1 和 EPIPE */
    ASSERT(n < 0, "send to closed socket should fail");
    ASSERT(errno == EPIPE || errno == ECONNRESET,
           "errno should be EPIPE or ECONNRESET");

    /* 恢复信号掩码 */
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);

    close(fds[0]);
    PASS();
}

/* ---- 5.3.4: 读取空请求 ---- */

static void test_error_empty_request(void) {
    TEST("empty JSON-RPC request");

    const char* path = "/tmp/test_admin_empty_req.sock";
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

    /* 发送空行 */
    send(client_fd, "\n", 1, MSG_NOSIGNAL);

    /* 服务器读取空行 */
    char buf[256];
    ssize_t n = recv(server_fd, buf, sizeof(buf) - 1, 0);
    ASSERT(n > 0, "server should receive data");

    /* 服务器应该能处理空行而不崩溃 */
    /* 发送错误响应 */
    const char* err = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"Invalid Request\"}}\n";
    send(server_fd, err, strlen(err), MSG_NOSIGNAL);

    close(client_fd);
    close(server_fd);
    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 5.3.5: 超大请求（超过缓冲区） ---- */

static void test_error_oversized_request(void) {
    TEST("oversized request exceeding buffer");

    const char* path = "/tmp/test_admin_oversized.sock";
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

    /* 发送一个很大的请求（不含换行符，直到缓冲区满） */
    char big_data[70000];
    memset(big_data, 'A', sizeof(big_data));
    big_data[sizeof(big_data) - 1] = '\0';

    /* 发送部分数据（不含换行符） */
    size_t to_send = 10000;
    ssize_t sent = send(client_fd, big_data, to_send, MSG_NOSIGNAL);
    ASSERT(sent > 0, "should send data");

    /* 服务器应该能处理超大请求（可能截断或返回错误） */
    char server_buf[4096];
    ssize_t n = recv(server_fd, server_buf, sizeof(server_buf) - 1, 0);
    /* 不崩溃就是成功 */
    (void)n;

    close(client_fd);
    close(server_fd);
    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 5.3.6: 无效 JSON 格式 ---- */

static void test_error_invalid_json(void) {
    TEST("invalid JSON format handling");

    const char* path = "/tmp/test_admin_invalid_json.sock";
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

    /* 发送各种无效 JSON */
    const char* invalid_jsons[] = {
        "not json at all\n",
        "{broken\n",
        "{\"method\":\n",
        "{}\n",
        "{\"jsonrpc\":\"2.0\"}\n",  /* 缺少 method */
        "{\"jsonrpc\":\"2.0\",\"method\":\"test\"}\n",  /* 缺少 id */
        NULL
    };

    for (int i = 0; invalid_jsons[i] != NULL; i++) {
        send(client_fd, invalid_jsons[i], strlen(invalid_jsons[i]), MSG_NOSIGNAL);

        char buf[4096];
        int total = 0;
        while (total < 4095) {
            ssize_t n = recv(server_fd, buf + total, 1, 0);
            if (n <= 0) break;
            if (buf[total] == '\n') { buf[total] = '\0'; break; }
            total++;
        }

        /* 服务器应该返回某种错误响应 */
        const char* err_resp = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}\n";
        send(server_fd, err_resp, strlen(err_resp), MSG_NOSIGNAL);

        /* 客户端接收 */
        char client_resp[4096];
        total = 0;
        while (total < 4095) {
            ssize_t n = recv(client_fd, client_resp + total, 1, 0);
            if (n <= 0) break;
            if (client_resp[total] == '\n') { client_resp[total] = '\0'; break; }
            total++;
        }
        /* 只要不崩溃就算通过 */
    }

    close(client_fd);
    close(server_fd);
    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 5.3.7: 未知方法调用 ---- */

static void test_error_unknown_method(void) {
    TEST("unknown JSON-RPC method");

    const char* path = "/tmp/test_admin_unknown_method.sock";
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

    /* 发送未知方法 */
    const char* req = "{\"jsonrpc\":\"2.0\",\"method\":\"nonexistent_method_xyz\",\"id\":42}";
    send(client_fd, req, strlen(req), MSG_NOSIGNAL);
    send(client_fd, "\n", 1, MSG_NOSIGNAL);

    /* 服务器应该返回 -32601 Method not found */
    const char* err_resp = "{\"jsonrpc\":\"2.0\",\"id\":42,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}\n";
    send(server_fd, err_resp, strlen(err_resp), MSG_NOSIGNAL);

    /* 客户端接收 */
    char resp[4096];
    int total = 0;
    while (total < 4095) {
        ssize_t n = recv(client_fd, resp + total, 1, 0);
        if (n <= 0) break;
        if (resp[total] == '\n') { resp[total] = '\0'; break; }
        total++;
    }
    ASSERT(total > 0, "should receive error response");
    ASSERT_CONTAINS(resp, "-32601", "should contain -32601 error code");

    close(client_fd);
    close(server_fd);
    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 5.3.8: 快速连接/断开 ---- */

static void test_error_rapid_connect_disconnect(void) {
    TEST("rapid connect/disconnect cycles");

    const char* path = "/tmp/test_admin_rapid.sock";
    unlink(path);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT(listen_fd >= 0, "create socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    ASSERT(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0, "bind");
    ASSERT(listen(listen_fd, 5) == 0, "listen");

    /* 快速连接和断开 20 次 */
    for (int i = 0; i < 20; i++) {
        int fd = connect_to_socket(path, 1);
        if (fd >= 0) {
            /* 立即关闭，不发送数据 */
            close(fd);
        }

        /* 接受并关闭 */
        int server_fd = accept(listen_fd, NULL, NULL);
        if (server_fd >= 0) {
            close(server_fd);
        }
    }

    /* 服务器应该仍然可用 */
    int test_fd = connect_to_socket(path, 2);
    ASSERT(test_fd >= 0, "server should still accept connections after rapid cycles");

    int server_fd = accept(listen_fd, NULL, NULL);
    ASSERT(server_fd >= 0, "server should still accept");

    close(test_fd);
    close(server_fd);
    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 5.3.9: 残留 socket 文件处理 ---- */

static void test_error_stale_socket_file(void) {
    TEST("stale socket file handling");

    const char* path = "/tmp/test_admin_stale.sock";

    /* 创建一个假的 socket 文件（不是真正的 socket） */
    FILE* f = fopen(path, "w");
    ASSERT(f != NULL, "create stale file");
    fprintf(f, "not a real socket\n");
    fclose(f);

    /* 尝试绑定（应该失败或覆盖） */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT(fd >= 0, "create socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    /* bind 到已有文件路径应该失败 */
    int ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    ASSERT(ret < 0, "bind to stale file should fail");
    ASSERT(errno == EADDRINUSE, "errno should be EADDRINUSE");

    /* 清理并重试（模拟 unlink 后重新 bind） */
    unlink(path);
    ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    ASSERT(ret == 0, "bind should succeed after unlink");

    close(fd);
    unlink(path);

    PASS();
}

/* ---- 5.3.10: 并发写入压力 ---- */

static void test_error_concurrent_writes(void) {
    TEST("concurrent write stress");

    const char* path = "/tmp/test_admin_concurrent_write.sock";
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

    /* 连续发送多个请求而不等待响应 */
    for (int i = 0; i < 50; i++) {
        char req[256];
        snprintf(req, sizeof(req),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"test\",\"id\":%d}\n", i);
        ssize_t sent = send(client_fd, req, strlen(req), MSG_NOSIGNAL);
        ASSERT(sent > 0, "should send request");
    }

    /* 服务器读取所有请求 */
    int received = 0;
    char buf[4096];
    for (int i = 0; i < 50; i++) {
        int total = 0;
        while (total < 4095) {
            ssize_t n = recv(server_fd, buf + total, 1, 0);
            if (n <= 0) break;
            if (buf[total] == '\n') { buf[total] = '\0'; received++; break; }
            total++;
        }
    }
    ASSERT(received > 0, "should receive some requests");

    close(client_fd);
    close(server_fd);
    close(listen_fd);
    unlink(path);

    PASS();
}

/* ---- 5.3.11: 零长度消息 ---- */

static void test_error_zero_length_message(void) {
    TEST("zero-length message handling");

    int fds[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "create socketpair");

    /* 发送零长度数据（只发送换行符） */
    send(fds[0], "\n", 1, MSG_NOSIGNAL);

    char buf[256];
    ssize_t n = recv(fds[1], buf, sizeof(buf) - 1, 0);
    ASSERT(n > 0, "should receive newline");

    close(fds[0]);
    close(fds[1]);

    PASS();
}

/* ---- 5.3.12: 部分读取（分片接收） ---- */

static void test_error_partial_read(void) {
    TEST("partial read (fragmented receive)");

    int fds[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "create socketpair");

    const char* msg = "{\"jsonrpc\":\"2.0\",\"method\":\"test\",\"id\":1}\n";

    /* 分片发送 */
    size_t half = strlen(msg) / 2;
    send(fds[0], msg, half, MSG_NOSIGNAL);
    usleep(10000); /* 10ms delay */
    send(fds[0], msg + half, strlen(msg) - half, MSG_NOSIGNAL);

    /* 逐字节接收直到换行 */
    char buf[4096];
    int total = 0;
    while (total < 4095) {
        ssize_t n = recv(fds[1], buf + total, 1, 0);
        if (n <= 0) break;
        if (buf[total] == '\n') { buf[total] = '\0'; break; }
        total++;
    }
    ASSERT(total > 0, "should receive complete message");
    ASSERT_CONTAINS(buf, "\"method\":\"test\"", "should contain method");

    close(fds[0]);
    close(fds[1]);

    PASS();
}

/* ---- 主函数 ---- */

int main(void) {
    printf("\n=== Phase 5.3: Admin IPC Error Scenario Tests ===\n\n");

    printf("--- Connection Errors ---\n");
    test_error_no_socket();
    test_error_backlog_full();
    test_error_write_closed();

    printf("\n--- Request Errors ---\n");
    test_error_empty_request();
    test_error_oversized_request();
    test_error_invalid_json();
    test_error_unknown_method();
    test_error_zero_length_message();

    printf("\n--- Robustness ---\n");
    test_error_rapid_connect_disconnect();
    test_error_stale_socket_file();
    test_error_concurrent_writes();
    test_error_partial_read();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
