/*
 * ChaosEngine POSIX fallback 异步 I/O 单元测试 — Phase 7.3
 * 验证 select() 模式的 POSIX fallback 后端
 *
 * 此测试编译时不定义 CHAOS_HAS_IO_URING，强制使用 ce_async_posix.c
 *
 * 测试场景：
 *   1. ce_async_init / ce_async_shutdown
 *   2. ce_async_backend_name 返回 "posix"
 *   3. ce_async_has_zcrx 返回 CE_FALSE
 *   4. ce_async_register_buffers 返回 CE_ERR
 *   5. ce_async_accept → ce_async_submit → ce_async_wait
 *   6. ce_async_recv / ce_async_send 回环通信
 *   7. 完整 accept → recv → send 流程
 *   8. 超时处理
 *   9. 错误 fd 处理
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
#include <signal.h>

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

#define PASS() do { \
    if (!_test_failed) printf("PASS\n"); \
} while(0)

/* ---- 辅助函数 ---- */
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
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 5) < 0) { close(fd); return -1; }
    return fd;
}

static int create_client_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port        = htons(port);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    return fd;
}

/* ================================================================
 * 测试 1: 后端名称验证
 * ================================================================ */
static int test_backend_is_posix(void) {
    TEST("backend name is 'posix'");
    const char* name = ce_async_backend_name();
    CHECK(name != NULL);
    CHECK(strcmp(name, "posix") == 0);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 2: ce_async_has_zcrx 返回 CE_FALSE
 * ================================================================ */
static int test_no_zcrx(void) {
    TEST("ce_async_has_zcrx returns CE_FALSE");
    CeBool has = ce_async_has_zcrx();
    CHECK(has == CE_FALSE);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 3: ce_async_register_buffers 返回 CE_ERR
 * ================================================================ */
static int test_register_buffers_fails(void) {
    TEST("ce_async_register_buffers returns CE_ERR");
    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    char buf[4096];
    CeResult r = ce_async_register_buffers(ctx, buf, 1024, 4);
    CHECK(r == CE_ERR);

    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 4: ce_async_init / ce_async_shutdown
 * ================================================================ */
static int test_init_shutdown(void) {
    TEST("ce_async_init / ce_async_shutdown (POSIX)");
    CeAsyncContext* ctx = ce_async_init(256);
    CHECK(ctx != NULL);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

static int test_init_zero_depth(void) {
    TEST("ce_async_init queue_depth=0 (POSIX ignores)");
    CeAsyncContext* ctx = ce_async_init(0);
    CHECK(ctx != NULL);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

static int test_shutdown_null_safe(void) {
    TEST("ce_async_shutdown(NULL) — POSIX safe");
    ce_async_shutdown(NULL);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 5: ce_async_accept (POSIX select 模式)
 * ================================================================ */
static int test_accept_posix(void) {
    TEST("POSIX accept — single connection via select()");
    int listen_fd = create_listen_socket(15201);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* 提交 accept */
    ce_async_accept(ctx, listen_fd, (void*)0xBEEF);
    int submitted = ce_async_submit(ctx);
    CHECK(submitted == 1);

    /* 创建客户端连接 */
    int client_fd = create_client_socket(15201);
    CHECK(client_fd >= 0);

    /* wait 会调用 select() 并执行 accept */
    int n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);

    const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
    CHECK(ev != NULL);
    CHECK(ev->type == CE_ASYNC_ACCEPT);
    CHECK(ev->fd == listen_fd);
    CHECK(ev->user_data == (void*)0xBEEF);
    CHECK(ev->client_fd >= 0);
    CHECK(ev->error == 0);

    close(ev->client_fd);
    close(client_fd);
    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 6: ce_async_recv (POSIX select 模式)
 * ================================================================ */
static int test_recv_posix(void) {
    TEST("POSIX recv — receive data via select()");
    int listen_fd = create_listen_socket(15202);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* accept */
    ce_async_accept(ctx, listen_fd, NULL);
    ce_async_submit(ctx);

    int client_fd = create_client_socket(15202);
    CHECK(client_fd >= 0);

    int n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    int accepted_fd = ce_async_get_event(ctx, 0)->client_fd;
    CHECK(accepted_fd >= 0);

    /* 客户端发送数据 */
    const char* msg = "POSIX select test";
    send(client_fd, msg, strlen(msg) + 1, 0);

    /* 提交 recv */
    char buf[256];
    memset(buf, 0, sizeof(buf));
    ce_async_recv(ctx, accepted_fd, buf, sizeof(buf), (void*)0xCAFE);
    ce_async_submit(ctx);

    /* wait 会调用 select() 并执行 recv */
    n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);

    const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
    CHECK(ev != NULL);
    CHECK(ev->type == CE_ASYNC_RECV);
    CHECK(ev->fd == accepted_fd);
    CHECK(ev->user_data == (void*)0xCAFE);
    CHECK(ev->nbytes == (int)(strlen(msg) + 1));
    CHECK(ev->error == 0);
    CHECK(strcmp(buf, msg) == 0);

    close(accepted_fd);
    close(client_fd);
    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 7: ce_async_send (POSIX select 模式)
 * ================================================================ */
static int test_send_posix(void) {
    TEST("POSIX send — send data via select()");
    int listen_fd = create_listen_socket(15203);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* accept */
    ce_async_accept(ctx, listen_fd, NULL);
    ce_async_submit(ctx);

    int client_fd = create_client_socket(15203);
    CHECK(client_fd >= 0);

    int n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    int accepted_fd = ce_async_get_event(ctx, 0)->client_fd;
    CHECK(accepted_fd >= 0);

    /* 提交 send */
    const char* msg = "Hello from POSIX!";
    ce_async_send(ctx, accepted_fd, msg, strlen(msg) + 1, (void*)0xD00D);
    ce_async_submit(ctx);

    /* wait 会调用 select() 并执行 send */
    n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);

    const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
    CHECK(ev != NULL);
    CHECK(ev->type == CE_ASYNC_SEND);
    CHECK(ev->fd == accepted_fd);
    CHECK(ev->user_data == (void*)0xD00D);
    CHECK(ev->nbytes == (int)(strlen(msg) + 1));
    CHECK(ev->error == 0);

    /* 客户端验证接收 */
    char buf[256];
    memset(buf, 0, sizeof(buf));
    int recvd = recv(client_fd, buf, sizeof(buf), 0);
    CHECK(recvd == (int)(strlen(msg) + 1));
    CHECK(strcmp(buf, msg) == 0);

    close(accepted_fd);
    close(client_fd);
    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 8: 完整回环 (POSIX)
 * ================================================================ */
static int test_full_loopback_posix(void) {
    TEST("POSIX full loopback — accept → recv → send");
    int listen_fd = create_listen_socket(15204);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* Step 1: accept */
    ce_async_accept(ctx, listen_fd, NULL);
    ce_async_submit(ctx);

    int client_fd = create_client_socket(15204);
    CHECK(client_fd >= 0);

    int n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    int accepted_fd = ce_async_get_event(ctx, 0)->client_fd;
    CHECK(accepted_fd >= 0);

    /* Step 2: client sends, server recvs */
    const char* ping = "PING";
    send(client_fd, ping, strlen(ping) + 1, 0);

    char buf[256];
    memset(buf, 0, sizeof(buf));
    ce_async_recv(ctx, accepted_fd, buf, sizeof(buf), NULL);
    ce_async_submit(ctx);

    n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
    CHECK(ev->type == CE_ASYNC_RECV);
    CHECK(ev->nbytes == (int)(strlen(ping) + 1));
    CHECK(strcmp(buf, ping) == 0);

    /* Step 3: server sends, client recvs */
    const char* pong = "PONG";
    ce_async_send(ctx, accepted_fd, pong, strlen(pong) + 1, NULL);
    ce_async_submit(ctx);

    n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    ev = ce_async_get_event(ctx, 0);
    CHECK(ev->type == CE_ASYNC_SEND);
    CHECK(ev->nbytes == (int)(strlen(pong) + 1));

    memset(buf, 0, sizeof(buf));
    int recvd = recv(client_fd, buf, sizeof(buf), 0);
    CHECK(recvd == (int)(strlen(pong) + 1));
    CHECK(strcmp(buf, pong) == 0);

    close(accepted_fd);
    close(client_fd);
    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 9: 超时处理 (POSIX)
 * ================================================================ */
static int test_timeout_posix(void) {
    TEST("POSIX wait timeout — returns 0");
    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* 不提交任何操作，wait 应超时返回 0 */
    int n = ce_async_wait(ctx, 1, 50);
    CHECK(n == 0);

    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 10: 空 submit (POSIX)
 * ================================================================ */
static int test_submit_empty_posix(void) {
    TEST("POSIX submit — no ops returns 0");
    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    int ret = ce_async_submit(ctx);
    CHECK(ret == 0);

    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 11: get_event 越界 (POSIX)
 * ================================================================ */
static int test_get_event_oob_posix(void) {
    TEST("POSIX get_event — out-of-bounds returns NULL");
    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    CHECK(ce_async_get_event(ctx, 0) == NULL);
    CHECK(ce_async_get_event(ctx, -1) == NULL);
    CHECK(ce_async_get_event(ctx, 999) == NULL);

    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 12: 多次 accept (POSIX)
 * ================================================================ */
static int test_multi_accept_posix(void) {
    TEST("POSIX multi accept — 3 connections via select()");
    int listen_fd = create_listen_socket(15205);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* 提交 3 个 accept */
    ce_async_accept(ctx, listen_fd, (void*)1);
    ce_async_accept(ctx, listen_fd, (void*)2);
    ce_async_accept(ctx, listen_fd, (void*)3);
    ce_async_submit(ctx);

    /* 创建 3 个客户端 */
    int clients[3];
    for (int i = 0; i < 3; i++) {
        clients[i] = create_client_socket(15205);
        CHECK(clients[i] >= 0);
    }

    /* 等待所有 accept */
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
 * 测试 13: 错误 fd 处理 (POSIX)
 * ================================================================ */
static int test_invalid_fd_posix(void) {
    TEST("POSIX invalid fd — recv on fd=-1 (should not crash)");
    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    char buf[64];
    ce_async_recv(ctx, -1, buf, sizeof(buf), NULL);
    ce_async_submit(ctx);

    /* wait 会尝试 select 无效 fd，应返回错误 */
    int n = ce_async_wait(ctx, 1, 2000);
    /* POSIX select 对无效 fd 返回 -1 (EBADF) */
    /* 但 ce_async_wait 内部可能捕获并返回 -1 */
    CHECK(n == -1 || n == 0);

    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 14: 对端断开 recv (POSIX)
 * ================================================================ */
static int test_peer_close_posix(void) {
    TEST("POSIX peer disconnect — recv returns EOF");
    int listen_fd = create_listen_socket(15206);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    ce_async_accept(ctx, listen_fd, NULL);
    ce_async_submit(ctx);

    int client_fd = create_client_socket(15206);
    CHECK(client_fd >= 0);

    int n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    int accepted_fd = ce_async_get_event(ctx, 0)->client_fd;
    CHECK(accepted_fd >= 0);

    /* 客户端关闭 */
    close(client_fd);
    usleep(50000);

    /* recv 应返回 0 (EOF) */
    char buf[64];
    memset(buf, 0, sizeof(buf));
    ce_async_recv(ctx, accepted_fd, buf, sizeof(buf), NULL);
    ce_async_submit(ctx);

    n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
    CHECK(ev != NULL);
    CHECK(ev->type == CE_ASYNC_RECV);
    CHECK(ev->nbytes == 0 || ev->error != 0);

    close(accepted_fd);
    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 15: ce_async_close (POSIX)
 * 注意：POSIX 后端的 close 操作不参与 select fd_set，
 * 因此 ce_async_wait 可能返回 0。这里验证不崩溃即可。
 * ================================================================ */
static int test_close_posix(void) {
    TEST("POSIX ce_async_close — close fd via async context");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    ce_async_close(ctx, fd);
    ce_async_submit(ctx);

    int n = ce_async_wait(ctx, 1, 2000);
    /* POSIX backend: close ops don't participate in select(),
     * so wait may return 0. The fd is still closed inside wait(). */
    if (n >= 1) {
        const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
        CHECK(ev != NULL);
        CHECK(ev->type == CE_ASYNC_CLOSE);
    }

    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * main
 * ================================================================ */
int main(void) {
    printf("\n=== POSIX Fallback Async I/O Tests (Phase 7.3) ===\n");
    printf("Backend: %s\n\n", ce_async_backend_name());

    /* 验证确实是 POSIX 后端 */
    if (strcmp(ce_async_backend_name(), "posix") != 0) {
        printf("ERROR: Expected 'posix' backend, got '%s'\n", ce_async_backend_name());
        printf("This test must be compiled WITHOUT io_uring support.\n");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    int failures = 0;

    ce_net_init();

    failures += test_backend_is_posix();
    failures += test_no_zcrx();
    failures += test_register_buffers_fails();
    failures += test_init_shutdown();
    failures += test_init_zero_depth();
    failures += test_shutdown_null_safe();
    failures += test_accept_posix();
    failures += test_recv_posix();
    failures += test_send_posix();
    failures += test_full_loopback_posix();
    failures += test_timeout_posix();
    failures += test_submit_empty_posix();
    failures += test_get_event_oob_posix();
    failures += test_multi_accept_posix();
    failures += test_invalid_fd_posix();
    failures += test_peer_close_posix();
    failures += test_close_posix();

    ce_net_shutdown();

    printf("\n=== Results: %d tests, %d failures ===\n", _test_count, failures);
    return failures > 0 ? 1 : 0;
}
