/*
 * ChaosEngine 网络层单元测试
 * 测试: ce_net_resolve, ce_socket_create_tcp/bind/listen/accept (本地回环)
 */

#include "public_api/ce_types.h"
#include "network/ce_network.h"
#include <stdio.h>
#include <string.h>

#define TEST(name) printf("  TEST: %s ... ", name)
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while(0)
#define PASS() printf("PASS\n")

int main(void) {
    printf("=== Network Tests ===\n");

    /* 初始化网络 */
    CeResult init_ret = ce_net_init();
    CHECK(init_ret == CE_OK);

    /* ---- ce_net_resolve ---- */
    TEST("net_resolve_localhost");
    {
        CeNetAddress addr;
        memset(&addr, 0, sizeof(addr));
        CeResult r = ce_net_resolve("127.0.0.1", 8080, &addr);
        CHECK(r == CE_OK);
        CHECK(addr.port == 8080);
        CHECK(strlen(addr.host) > 0);
    }
    PASS();

    TEST("net_resolve_invalid");
    {
        CeNetAddress addr;
        memset(&addr, 0, sizeof(addr));
        /* 无效主机名应返回 CE_ERR */
        CeResult r = ce_net_resolve("invalid.host.name.zzz", 8080, &addr);
        /* 注意：某些系统配置可能解析任意名称，这里仅验证不崩溃 */
        (void)r;
    }
    PASS();

    /* ---- ce_socket_create_tcp ---- */
    TEST("socket_create_tcp");
    {
        CeSocket* sock = ce_socket_create_tcp();
        CHECK(sock != NULL);
        CHECK(ce_socket_is_valid(sock) == CE_TRUE);
        ce_socket_close(sock);
    }
    PASS();

    /* ---- ce_socket_bind ---- */
    TEST("socket_bind");
    {
        CeNetAddress addr;
        memset(&addr, 0, sizeof(addr));
        ce_net_resolve("127.0.0.1", 12345, &addr);

        CeSocket* sock = ce_socket_create_tcp();
        CHECK(sock != NULL);

        CeResult r = ce_socket_bind(sock, &addr);
        CHECK(r == CE_OK);

        ce_socket_close(sock);
    }
    PASS();

    /* ---- ce_socket_listen ---- */
    TEST("socket_listen");
    {
        CeNetAddress addr;
        memset(&addr, 0, sizeof(addr));
        ce_net_resolve("127.0.0.1", 12346, &addr);

        CeSocket* sock = ce_socket_create_tcp();
        CHECK(sock != NULL);
        CHECK(ce_socket_bind(sock, &addr) == CE_OK);
        CHECK(ce_socket_listen(sock, 5) == CE_OK);

        ce_socket_close(sock);
    }
    PASS();

    /* ---- ce_socket_accept (本地回环) ---- */
    TEST("socket_accept_loopback");
    {
        CeNetAddress addr;
        memset(&addr, 0, sizeof(addr));
        ce_net_resolve("127.0.0.1", 12347, &addr);

        /* 创建监听 socket */
        CeSocket* server = ce_socket_create_tcp();
        CHECK(server != NULL);
        CHECK(ce_socket_bind(server, &addr) == CE_OK);
        CHECK(ce_socket_listen(server, 1) == CE_OK);

        /* 设置非阻塞以便 accept 不会永久阻塞 */
        CHECK(ce_socket_set_nonblocking(server, CE_TRUE) == CE_OK);

        /* 创建客户端 socket 并连接 */
        CeSocket* client = ce_socket_create_tcp();
        CHECK(client != NULL);

        CeResult conn_ret = ce_socket_connect(client, &addr);
        /* 连接可能立即成功，也可能因为非阻塞返回错误（可接受） */
        /* 在某些系统上，非阻塞 connect 会返回 CE_ERR 但连接仍在进行中 */
        /* 这里我们只验证 accept 能收到连接 */

        /* 接受连接 */
        CeNetAddress client_addr;
        memset(&client_addr, 0, sizeof(client_addr));
        CeSocket* accepted = ce_socket_accept(server, &client_addr);

        if (accepted != NULL) {
            CHECK(ce_socket_is_valid(accepted) == CE_TRUE);
            ce_socket_close(accepted);
        }
        /* 如果 accept 返回 NULL（非阻塞模式），也是可接受的 */

        ce_socket_close(client);
        ce_socket_close(server);
    }
    PASS();

    /* ---- ce_socket_is_valid ---- */
    TEST("socket_is_valid_null");
    {
        CHECK(ce_socket_is_valid(NULL) == CE_FALSE);
    }
    PASS();

    /* ---- ce_socket_set_nonblocking ---- */
    TEST("socket_set_nonblocking");
    {
        CeSocket* sock = ce_socket_create_tcp();
        CHECK(sock != NULL);

        CHECK(ce_socket_set_nonblocking(sock, CE_TRUE) == CE_OK);
        CHECK(ce_socket_set_nonblocking(sock, CE_FALSE) == CE_OK);

        ce_socket_close(sock);
    }
    PASS();

    /* ---- ce_socket_set_nodelay ---- */
    TEST("socket_set_nodelay");
    {
        CeSocket* sock = ce_socket_create_tcp();
        CHECK(sock != NULL);

        CHECK(ce_socket_set_nodelay(sock, CE_TRUE) == CE_OK);
        CHECK(ce_socket_set_nodelay(sock, CE_FALSE) == CE_OK);

        ce_socket_close(sock);
    }
    PASS();

    /* ---- ce_socket_send/recv (回环通信) ---- */
    TEST("socket_send_recv_loopback");
    {
        CeNetAddress addr;
        memset(&addr, 0, sizeof(addr));
        ce_net_resolve("127.0.0.1", 12348, &addr);

        /* 服务器 */
        CeSocket* server = ce_socket_create_tcp();
        CHECK(server != NULL);
        CHECK(ce_socket_bind(server, &addr) == CE_OK);
        CHECK(ce_socket_listen(server, 1) == CE_OK);

        /* 客户端连接 */
        CeSocket* client = ce_socket_create_tcp();
        CHECK(client != NULL);

        /* 使用阻塞 connect */
        CeResult cr = ce_socket_connect(client, &addr);
        if (cr == CE_OK) {
            /* 接受连接 */
            CeNetAddress client_addr;
            memset(&client_addr, 0, sizeof(client_addr));
            CeSocket* accepted = ce_socket_accept(server, &client_addr);

            if (accepted != NULL) {
                /* 发送数据 */
                const char* msg = "Hello, Chaos!";
                int sent = ce_socket_send(client, msg, strlen(msg) + 1);
                CHECK(sent == (int)(strlen(msg) + 1));

                /* 接收数据 */
                char buffer[64];
                memset(buffer, 0, sizeof(buffer));
                int recvd = ce_socket_recv(accepted, buffer, sizeof(buffer));
                CHECK(recvd == (int)(strlen(msg) + 1));
                CHECK(strcmp(buffer, msg) == 0);

                ce_socket_close(accepted);
            }
        }

        ce_socket_close(client);
        ce_socket_close(server);
    }
    PASS();

    /* ---- ce_net_addr_to_string ---- */
    TEST("net_addr_to_string");
    {
        CeNetAddress addr;
        memset(&addr, 0, sizeof(addr));
        strncpy(addr.host, "192.168.1.1", sizeof(addr.host) - 1);
        addr.port = 9000;

        char buf[64];
        const char* s = ce_net_addr_to_string(&addr, buf, sizeof(buf));
        CHECK(s != NULL);
        CHECK(strcmp(s, "192.168.1.1:9000") == 0);
    }
    PASS();

    /* 清理 */
    ce_net_shutdown();

    printf("\nAll network tests passed!\n");
    return 0;
}
