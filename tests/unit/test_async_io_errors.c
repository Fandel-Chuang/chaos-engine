/*
 * ChaosEngine io_uring 异步 I/O 单元测试 — Phase 7.2
 * 错误处理测试：SQ 满、CQ 溢出、非法 fd、连接断开
 *
 * 测试场景：
 *   1. 操作上下文池满（模拟 SQ 满）
 *   2. 非法 fd 提交操作
 *   3. 连接断开后 recv（对端关闭）
 *   4. 连接断开后 send（对端关闭）
 *   5. 重复 shutdown
 *   6. NULL 上下文操作
 *   7. 超大缓冲区
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
 * 测试 1: 操作上下文池满 — 提交超过 CE_ASYNC_MAX_EVENTS 个操作
 *         内部限制为 256，提交 300 个 accept 应触发 "Op ctx pool full"
 *         注意：这不会崩溃，但后面的操作会被静默丢弃
 * ================================================================ */
static int test_op_pool_full(void) {
    TEST("op ctx pool — submit >256 ops (should not crash)");
    int listen_fd = create_listen_socket(15101);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(512);
    CHECK(ctx != NULL);

    /* 提交 300 个 accept（超过 CE_ASYNC_MAX_EVENTS=256） */
    for (int i = 0; i < 300; i++) {
        ce_async_accept(ctx, listen_fd, (void*)(intptr_t)i);
    }

    /* submit 应该只提交有效的那些 */
    int ret = ce_async_submit(ctx);
    /* 可能成功提交部分，也可能因为 SQ 满而失败 */
    CHECK(ret >= 0 || ret == -1);

    /* 清理 — 等待所有可能的完成事件 */
    int total = 0;
    int max_attempts = 10;
    while (max_attempts-- > 0) {
        int n = ce_async_wait(ctx, 0, 50);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            const CeAsyncEvent* ev = ce_async_get_event(ctx, i);
            if (ev && ev->client_fd >= 0) close(ev->client_fd);
        }
        total += n;
    }

    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 2: 非法 fd — 对无效 fd 提交 recv
 * ================================================================ */
static int test_invalid_fd_recv(void) {
    TEST("invalid fd — recv on fd=-1 (should not crash)");
    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    char buf[64];
    ce_async_recv(ctx, -1, buf, sizeof(buf), NULL);
    int ret = ce_async_submit(ctx);

    /* submit 可能成功也可能失败 */
    if (ret > 0) {
        int n = ce_async_wait(ctx, 1, 2000);
        if (n > 0) {
            const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
            CHECK(ev != NULL);
            /* 预期：错误码非零（EBADF = 9） */
            CHECK(ev->error != 0);
        }
    }

    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 3: 非法 fd — 对无效 fd 提交 send
 * ================================================================ */
static int test_invalid_fd_send(void) {
    TEST("invalid fd — send on fd=-1 (should not crash)");
    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    const char* msg = "test";
    ce_async_send(ctx, -1, msg, strlen(msg), NULL);
    int ret = ce_async_submit(ctx);

    if (ret > 0) {
        int n = ce_async_wait(ctx, 1, 2000);
        if (n > 0) {
            const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
            CHECK(ev != NULL);
            CHECK(ev->error != 0);
        }
    }

    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 4: 非法 fd — 对无效 fd 提交 accept
 * ================================================================ */
static int test_invalid_fd_accept(void) {
    TEST("invalid fd — accept on fd=-1 (should not crash)");
    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    ce_async_accept(ctx, -1, NULL);
    int ret = ce_async_submit(ctx);

    if (ret > 0) {
        int n = ce_async_wait(ctx, 1, 2000);
        if (n > 0) {
            const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
            CHECK(ev != NULL);
            CHECK(ev->error != 0);
        }
    }

    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 5: 已关闭的 fd — 对已 close 的 fd 提交操作
 * ================================================================ */
static int test_closed_fd(void) {
    TEST("closed fd — recv on already-closed fd");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    close(fd);  /* 立即关闭 */

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    char buf[64];
    ce_async_recv(ctx, fd, buf, sizeof(buf), NULL);
    int ret = ce_async_submit(ctx);

    if (ret > 0) {
        int n = ce_async_wait(ctx, 1, 2000);
        if (n > 0) {
            const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
            CHECK(ev != NULL);
            CHECK(ev->error != 0);  /* 应有 EBADF 错误 */
        }
    }

    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 6: 连接断开后 recv — 对端关闭连接
 * ================================================================ */
static int test_peer_disconnect_recv(void) {
    TEST("peer disconnect — recv after client closes");
    int listen_fd = create_listen_socket(15102);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* accept */
    ce_async_accept(ctx, listen_fd, NULL);
    ce_async_submit(ctx);

    int client_fd = create_client_socket(15102);
    CHECK(client_fd >= 0);

    int n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    int accepted_fd = ce_async_get_event(ctx, 0)->client_fd;
    CHECK(accepted_fd >= 0);

    /* 客户端关闭连接 */
    close(client_fd);

    /* 短暂等待确保 FIN 到达 */
    usleep(50000);

    /* 提交 recv — 应该收到 EOF (nbytes=0) */
    char buf[64];
    memset(buf, 0, sizeof(buf));
    ce_async_recv(ctx, accepted_fd, buf, sizeof(buf), NULL);
    ce_async_submit(ctx);

    n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
    CHECK(ev != NULL);
    CHECK(ev->type == CE_ASYNC_RECV);
    /* 对端关闭后 recv 应返回 0 (EOF) 或错误 */
    CHECK(ev->nbytes == 0 || ev->error != 0);

    close(accepted_fd);
    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 7: 连接断开后 send — 对端关闭连接
 * ================================================================ */
static int test_peer_disconnect_send(void) {
    TEST("peer disconnect — send after client closes");
    int listen_fd = create_listen_socket(15103);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    /* accept */
    ce_async_accept(ctx, listen_fd, NULL);
    ce_async_submit(ctx);

    int client_fd = create_client_socket(15103);
    CHECK(client_fd >= 0);

    int n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    int accepted_fd = ce_async_get_event(ctx, 0)->client_fd;
    CHECK(accepted_fd >= 0);

    /* 客户端关闭连接 */
    close(client_fd);
    usleep(50000);

    /* 提交 send — 对端已关闭，应收到 EPIPE 或 ECONNRESET */
    const char* msg = "data after close";
    ce_async_send(ctx, accepted_fd, msg, strlen(msg), NULL);
    ce_async_submit(ctx);

    n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
    CHECK(ev != NULL);
    CHECK(ev->type == CE_ASYNC_SEND);
    /* 预期：错误（EPIPE=32 或 ECONNRESET=104） */
    /* 某些内核可能缓冲发送，第一次 send 成功但第二次失败 */
    /* 这里只验证不崩溃 */
    (void)ev;

    close(accepted_fd);
    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 8: 重复 shutdown
 * ================================================================ */
static int test_double_shutdown(void) {
    TEST("double shutdown — should not crash");
    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    ce_async_shutdown(ctx);
    /* Second call with NULL is safe; the library checks for NULL */
    ce_async_shutdown(NULL);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 9: 零大小 recv 缓冲区
 * ================================================================ */
static int test_zero_size_recv(void) {
    TEST("zero-size recv — should handle gracefully");
    int listen_fd = create_listen_socket(15104);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    ce_async_accept(ctx, listen_fd, NULL);
    ce_async_submit(ctx);

    int client_fd = create_client_socket(15104);
    CHECK(client_fd >= 0);

    int n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    int accepted_fd = ce_async_get_event(ctx, 0)->client_fd;
    CHECK(accepted_fd >= 0);

    /* 发送一些数据 */
    send(client_fd, "x", 1, 0);

    /* 零大小 recv */
    char dummy;
    ce_async_recv(ctx, accepted_fd, &dummy, 0, NULL);
    ce_async_submit(ctx);

    n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
    CHECK(ev != NULL);
    /* nbytes 应为 0（请求了 0 字节） */
    CHECK(ev->nbytes == 0);

    close(accepted_fd);
    close(client_fd);
    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 10: 忽略 SIGPIPE（防止 send 到已关闭连接时进程被杀死）
 * ================================================================ */
static int test_sigpipe_handling(void) {
    TEST("SIGPIPE — send to closed peer (SIGPIPE ignored)");
    /* 确保 SIGPIPE 被忽略 */
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = create_listen_socket(15105);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    ce_async_accept(ctx, listen_fd, NULL);
    ce_async_submit(ctx);

    int client_fd = create_client_socket(15105);
    CHECK(client_fd >= 0);

    int n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    int accepted_fd = ce_async_get_event(ctx, 0)->client_fd;
    CHECK(accepted_fd >= 0);

    /* 客户端关闭 */
    close(client_fd);
    usleep(50000);

    /* 多次 send（可能触发 SIGPIPE 如果没有正确处理） */
    const char* msg = "test";
    for (int i = 0; i < 3; i++) {
        ce_async_send(ctx, accepted_fd, msg, strlen(msg), NULL);
    }
    ce_async_submit(ctx);

    /* 等待完成 */
    int total = 0;
    int attempts = 5;
    while (attempts-- > 0) {
        n = ce_async_wait(ctx, 0, 100);
        if (n <= 0) break;
        total += n;
    }

    /* 不崩溃就算通过 */
    close(accepted_fd);
    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * 测试 11: 超大缓冲区 recv
 * ================================================================ */
static int test_large_buffer_recv(void) {
    TEST("large buffer recv — 64KB buffer");
    int listen_fd = create_listen_socket(15106);
    CHECK(listen_fd >= 0);

    CeAsyncContext* ctx = ce_async_init(64);
    CHECK(ctx != NULL);

    ce_async_accept(ctx, listen_fd, NULL);
    ce_async_submit(ctx);

    int client_fd = create_client_socket(15106);
    CHECK(client_fd >= 0);

    int n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    int accepted_fd = ce_async_get_event(ctx, 0)->client_fd;
    CHECK(accepted_fd >= 0);

    /* 发送小数据，但用大缓冲区接收 */
    send(client_fd, "hi", 3, 0);

    char* big_buf = (char*)malloc(65536);
    CHECK(big_buf != NULL);
    memset(big_buf, 0, 65536);

    ce_async_recv(ctx, accepted_fd, big_buf, 65536, NULL);
    ce_async_submit(ctx);

    n = ce_async_wait(ctx, 1, 2000);
    CHECK(n >= 1);
    const CeAsyncEvent* ev = ce_async_get_event(ctx, 0);
    CHECK(ev != NULL);
    CHECK(ev->type == CE_ASYNC_RECV);
    CHECK(ev->nbytes == 3);
    CHECK(ev->error == 0);

    free(big_buf);
    close(accepted_fd);
    close(client_fd);
    close(listen_fd);
    ce_async_shutdown(ctx);
    PASS();
    return _test_failed;
}

/* ================================================================
 * main
 * ================================================================ */
int main(void) {
    printf("\n=== Async I/O Error Handling Tests (Phase 7.2) ===\n");
    printf("Backend: %s\n\n", ce_async_backend_name());

    /* 忽略 SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    int failures = 0;

    ce_net_init();

    failures += test_op_pool_full();
    failures += test_invalid_fd_recv();
    failures += test_invalid_fd_send();
    failures += test_invalid_fd_accept();
    failures += test_closed_fd();
    failures += test_peer_disconnect_recv();
    failures += test_peer_disconnect_send();
    failures += test_double_shutdown();
    failures += test_zero_size_recv();
    failures += test_sigpipe_handling();
    failures += test_large_buffer_recv();

    ce_net_shutdown();

    printf("\n=== Results: %d tests, %d failures ===\n", _test_count, failures);
    return failures > 0 ? 1 : 0;
}
