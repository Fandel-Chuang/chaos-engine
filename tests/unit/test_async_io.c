/*
 * ChaosEngine io_uring 异步 I/O 单元测试 — Phase 7.1
 * 验证 ce_async_init / ce_async_accept / ce_async_recv / ce_async_send 基本功能
 *
 * 测试场景：
 *   1. ce_async_init 正常/异常参数
 *   2. ce_async_shutdown 正常/空指针
 *   3. 本地回环 accept → recv → send 完整流程
 *   4. ce_async_submit / ce_async_wait / ce_async_get_event
 *   5. ce_async_backend_name
 */

#include "public_api/ce_types.h"
#include "network/ce_async_io.h"
#include "network/ce_network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

/* ---- 测试辅助宏 ---- */
static int _test_failed = 0;
static int _test_count  = 0;

#define TEST(name) do { \
    _test_failed = 0; \
    _test_count++; \
    printf("  TEST %d: %s ... ", _test_count, name); \
    fflush(stdout); \
} while(0)

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    %s:%d: CHECK(%s) failed (errno=%d: %s)\n", \
               __FILE__, __LINE__, #cond, errno, strerror(errno)); \
        _test_failed = 1; \
    } \
} while(0)

#define CHECKEQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: expected %d, got %d\n", \
               __FILE__, __LINE__, (int)(b), (int)(a)); \
        _test_failed = 1; \
    } \
} while(0)

#define PASS() do { \
    if (!_test_failed) printf("PASS\n"); \
} while(0)

/* ---- 辅助函数：创建监听 socket ---- */
static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 5) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- 辅助函数：创建并连接客户端 socket ---- */
static int create_client_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port        = htons(port);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ================================================================
 * 测试 1: ce_async_init — 正常初始化
 * ================================================================ */
static int test_init_normal(void) {
    TEST("ce_async_init (queue_depth=256)");
    CeAsyncContext* ctx = ce_async_init(256);
    CHECK(ctx != NULL);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 2: ce_async_init — 边界参数
 * ================================================================ */
static int test_init_edge(void) {
    TEST("ce_async_init (queue_depth=0 → default 256)");
    CeAsyncContext* ctx = ce_async_init(0);
    CHECK(ctx != NULL);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

static int test_init_large(void) {
    TEST("ce_async_init (queue_depth=32768 → clamped)");
    CeAsyncContext* ctx = ce_async_init(32768);
    CHECK(ctx != NULL);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

static int test_init_oversized(void) {
    TEST("ce_async_init (queue_depth=65536 → clamped to 32768)");
    CeAsyncContext* ctx = ce_async_init(65536);
    CHECK(ctx != NULL);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 3: ce_async_shutdown — NULL 安全
 * ================================================================ */
static int test_shutdown_null(void) {
    TEST("ce_async_shutdown(NULL) — should not crash");
    ce_async_shutdown(NULL);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 4: ce_async_accept — 基本 accept 流程
 * ================================================================ */
static int test_accept_basic(void) {
    TEST("ce_async_accept — accept single connection");
    int listen_fd = create_listen_socket(15001);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* 提交 accept */
    ce_async_accept(ctx, listen_fd, (void*)0x1234);
    int submitted = ce_async_submit(ctx);
    CHECK(submitted == 1);

    /* 创建客户端连接 */
    int client_fd = create_client_socket(15001);
    CHECK(client_fd >= 0);

    /* 等待完成事件 */
    int n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);

    const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
    CHECK(ev != NULL);
    CHECK(ev->type == CE_ASYNC_ACCEPT);
    CHECK(ev->fd == listen_fd);
    CHECK(ev->user_data == (void*)0x1234);
    CHECK(ev->client_fd >= 0);
    CHECK(ev->error == 0);

    /* 清理 */
    close(ev->client_fd);
    close(client_fd);
    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 5: ce_async_recv — 基本 recv 流程
 * ================================================================ */
static int test_recv_basic(void) {
    TEST("ce_async_recv — receive data from client");
    int listen_fd = create_listen_socket(15002);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* 提交 accept */
    ce_async_accept(ctx, listen_fd, NULL);
    ce_async_submit(ctx);

    /* 客户端连接并发送数据 */
    int client_fd = create_client_socket(15002);
    CHECK(client_fd >= 0);
    const char* send_msg = "Hello Async IO!";
    int sent = send(client_fd, send_msg, strlen(send_msg) + 1, 0);
    CHECK(sent == (int)(strlen(send_msg) + 1));

    /* 等待 accept 完成 */
    int n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
    CHECK(ev != NULL);
    CHECK(ev->type == CE_ASYNC_ACCEPT);
    int accepted_fd = ev->client_fd;
    CHECK(accepted_fd >= 0);

    /* 提交 recv */
    char recv_buf[256];
    memset(recv_buf, 0, sizeof(recv_buf));
    ce_async_recv(ctx, accepted_fd, recv_buf, sizeof(recv_buf), (void*)0x5678);
    ce_async_submit(ctx);

    /* 等待 recv 完成 */
    n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    ev = ce_async_get_event(ctx, 0);
    CHECK(ev != NULL);
    CHECK(ev->type == CE_ASYNC_RECV);
    CHECK(ev->fd == accepted_fd);
    CHECK(ev->user_data == (void*)0x5678);
    CHECK(ev->nbytes == (int)(strlen(send_msg) + 1));
    CHECK(ev->error == 0);
    CHECK(strcmp(recv_buf, send_msg) == 0);

    /* 清理 */
    close(accepted_fd);
    close(client_fd);
    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 6: ce_async_send — 基本 send 流程
 * ================================================================ */
static int test_send_basic(void) {
    TEST("ce_async_send — send data to client");
    int listen_fd = create_listen_socket(15003);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* 提交 accept */
    ce_async_accept(ctx, listen_fd, NULL);
    ce_async_submit(ctx);

    /* 客户端连接 */
    int client_fd = create_client_socket(15003);
    CHECK(client_fd >= 0);

    /* 等待 accept 完成 */
    int n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
    CHECK(ev != NULL);
    int accepted_fd = ev->client_fd;
    CHECK(accepted_fd >= 0);

    /* 提交 send */
    const char* send_msg = "Echo from server!";
    ce_async_send(ctx, accepted_fd, send_msg, strlen(send_msg) + 1, (void*)0xABCD);
    ce_async_submit(ctx);

    /* 等待 send 完成 */
    n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    ev = ce_async_get_event(ctx, 0);
    CHECK(ev != NULL);
    CHECK(ev->type == CE_ASYNC_SEND);
    CHECK(ev->fd == accepted_fd);
    CHECK(ev->user_data == (void*)0xABCD);
    CHECK(ev->nbytes == (int)(strlen(send_msg) + 1));
    CHECK(ev->error == 0);

    /* 客户端接收验证 */
    char recv_buf[256];
    memset(recv_buf, 0, sizeof(recv_buf));
    int recvd = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
    CHECK(recvd == (int)(strlen(send_msg) + 1));
    CHECK(strcmp(recv_buf, send_msg) == 0);

    /* 清理 */
    close(accepted_fd);
    close(client_fd);
    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 7: 完整回环 — accept → recv → send → recv
 * ================================================================ */
static int test_full_loopback(void) {
    TEST("full loopback — accept → recv → send → client recv");
    int listen_fd = create_listen_socket(15004);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* Step 1: async accept */
    ce_async_accept(ctx, listen_fd, NULL);
    ce_async_submit(ctx);

    int client_fd = create_client_socket(15004);
    CHECK(client_fd >= 0);

    int n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    int accepted_fd = ce_async_get_event(ctx, 0)->client_fd;
    CHECK(accepted_fd >= 0);

    /* Step 2: client sends, server async recv */
    const char* client_msg = "Ping";
    send(client_fd, client_msg, strlen(client_msg) + 1, 0);

    char buf[256];
    memset(buf, 0, sizeof(buf));
    ce_async_recv(ctx, accepted_fd, buf, sizeof(buf), NULL);
    ce_async_submit(ctx);

    n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
    CHECK(ev->type == CE_ASYNC_RECV);
    CHECK(ev->nbytes == (int)(strlen(client_msg) + 1));
    CHECK(strcmp(buf, client_msg) == 0);

    /* Step 3: server async send, client recv */
    const char* server_msg = "Pong";
    ce_async_send(ctx, accepted_fd, server_msg, strlen(server_msg) + 1, NULL);
    ce_async_submit(ctx);

    n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    ev = ce_async_get_event(ctx, 0);
    CHECK(ev->type == CE_ASYNC_SEND);
    CHECK(ev->nbytes == (int)(strlen(server_msg) + 1));

    memset(buf, 0, sizeof(buf));
    int recvd = recv(client_fd, buf, sizeof(buf), 0);
    CHECK(recvd == (int)(strlen(server_msg) + 1));
    CHECK(strcmp(buf, server_msg) == 0);

    /* 清理 */
    close(accepted_fd);
    close(client_fd);
    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 8: 多次 accept
 * ================================================================ */
static int test_multi_accept(void) {
    TEST("multiple accepts — 3 connections");
    int listen_fd = create_listen_socket(15005);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* 提交 3 个 accept */
    ce_async_accept(ctx, listen_fd, (void*)1);
    ce_async_accept(ctx, listen_fd, (void*)2);
    ce_async_accept(ctx, listen_fd, (void*)3);
    ce_async_submit(ctx);

    /* 创建 3 个客户端连接 */
    int clients[3];
    for (int i = 0; i < 3; i++) {
        clients[i] = create_client_socket(15005);
        CHECK(clients[i] >= 0);
    }

    /* 等待所有 accept 完成 */
    int total = 0;
    int accepted_fds[3] = {-1, -1, -1};
    while (total < 3) {
        int n = ce_async_wait(ctx, 1, 2000);
        CHECK(n >= 1);
        for (int i = 0; i < n && total < 3; i++) {
            const CeAsyncEvent* ev = ce_async_get_event(ctx, i);
            CHECK(ev->type == CE_ASYNC_ACCEPT);
            intptr_t idx = (intptr_t)ev->user_data - 1;
            CHECK(idx >= 0 && idx < 3);
            accepted_fds[idx] = ev->client_fd;
            total++;
        }
    }

    /* 验证所有 accept 都成功 */
    for (int i = 0; i < 3; i++) {
        CHECK(accepted_fds[i] >= 0);
        close(accepted_fds[i]);
        close(clients[i]);
    }

    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 9: ce_async_backend_name
 * ================================================================ */
static int test_backend_name(void) {
    TEST("ce_async_backend_name — returns valid string");
    const char* name = ce_async_backend_name();
    CHECK(name != NULL);
    CHECK(strlen(name) > 0);
    printf("[%s] ", name);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 10: ce_async_submit — 无待处理操作
 * ================================================================ */
static int test_submit_empty(void) {
    TEST("ce_async_submit — no pending ops returns 0");
    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    int ret = ce_async_submit(ctx);
    CHECK(ret == 0);

    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 11: ce_async_wait — 超时
 * ================================================================ */
static int test_wait_timeout(void) {
    TEST("ce_async_wait — timeout returns 0");
    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* 没有提交任何操作，等待应超时 */
    int n = ce_async_wait(ctx, 1, 100);
    CHECK(n == 0);

    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 12: ce_async_get_event — 越界访问
 * ================================================================ */
static int test_get_event_oob(void) {
    TEST("ce_async_get_event — out-of-bounds returns NULL");
    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* 未提交任何操作，索引 0 应返回 NULL */
    const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
    CHECK(ev == NULL);

    ev = ce_async_get_event(ctx, -1);
    CHECK(ev == NULL);

    ev = ce_async_get_event(ctx, 999);
    CHECK(ev == NULL);

    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * main
 * ================================================================ */
int main(void) {
    printf("\n=== Async I/O Unit Tests (Phase 7.1) ===\n");
    printf("Backend: %s\n\n", ce_async_backend_name());

    int failures = 0;

    /* 初始化网络层 */
    ce_net_init();

    failures += test_init_normal();
    failures += test_init_edge();
    failures += test_init_large();
    failures += test_init_oversized();
    failures += test_shutdown_null();
    failures += test_accept_basic();
    failures += test_recv_basic();
    failures += test_send_basic();
    failures += test_full_loopback();
    failures += test_multi_accept();
    failures += test_backend_name();
    failures += test_submit_empty();
    failures += test_wait_timeout();
    failures += test_get_event_oob();

    ce_net_shutdown();

    printf("\n=== Results: %d tests, %d failures ===\n", _test_count, failures);
    return failures > 0 ? 1 : 0;
}
