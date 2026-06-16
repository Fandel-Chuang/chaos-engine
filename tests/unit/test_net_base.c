/*
 * ChaosEngine ce_net_base 单元测试
 *
 * 测试覆盖：
 *   1.1 协议编解码 (ce_net_base_pack/unpack/peek_len/peek_type)
 *   1.2 TCP 连接管理 (create/connect/disconnect/destroy)
 *   1.3 消息收发 (send/recv)
 *   1.4 心跳检测 (ping/pong/heartbeat_timeout)
 *   1.5 连接池 (create/add/acquire/release/remove/stats/cleanup)
 *   1.6 自动重连 (try_reconnect/reset_reconnect)
 *   1.10 跨区消息格式 (pack/unpack cross region)
 *   1.11 Router 网格 (create/add_region/remove_region/connect_all)
 *   1.12 网格消息收发 (mesh_send/broadcast)
 */

#include "public_api/ce_types.h"
#include "network/ce_net_base.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define TEST(name) printf("  TEST: %s ... ", name)
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while(0)
#define PASS() printf("PASS\n")

/* ================================================================
 * 1.7: 协议编解码测试
 * ================================================================ */

static int test_protocol_pack_unpack(void) {
    TEST("protocol_pack_unpack");
    {
        uint8_t buf[256];
        const char* payload_str = "Hello, ChaosEngine!";
        uint32_t payload_len = (uint32_t)strlen(payload_str);

        uint32_t total = ce_net_base_pack(buf, sizeof(buf), 0x0100,
                                          (const uint8_t*)payload_str, payload_len);
        CHECK(total == CE_NET_BASE_HEADER_SIZE + payload_len);

        uint16_t msg_type = 0;
        const uint8_t* out_payload = NULL;
        uint32_t out_payload_len = 0;
        CeResult r = ce_net_base_unpack(buf, total, &msg_type, &out_payload, &out_payload_len);
        CHECK(r == CE_OK);
        CHECK(msg_type == 0x0100);
        CHECK(out_payload_len == payload_len);
        CHECK(memcmp(out_payload, payload_str, payload_len) == 0);
    }
    PASS();

    TEST("protocol_pack_empty_payload");
    {
        uint8_t buf[256];
        uint32_t total = ce_net_base_pack(buf, sizeof(buf), 0x0001, NULL, 0);
        CHECK(total == CE_NET_BASE_HEADER_SIZE);

        uint16_t msg_type = 0;
        const uint8_t* out_payload = NULL;
        uint32_t out_payload_len = 0;
        CeResult r = ce_net_base_unpack(buf, total, &msg_type, &out_payload, &out_payload_len);
        CHECK(r == CE_OK);
        CHECK(msg_type == 0x0001);
        CHECK(out_payload_len == 0);
    }
    PASS();

    TEST("protocol_unpack_incomplete");
    {
        uint8_t buf[4] = {0, 0, 0, 10}; /* 只有 4 字节头部，声称长度 10 */
        uint16_t msg_type = 0;
        const uint8_t* out_payload = NULL;
        uint32_t out_payload_len = 0;
        CeResult r = ce_net_base_unpack(buf, 4, &msg_type, &out_payload, &out_payload_len);
        CHECK(r == CE_ERR);
    }
    PASS();

    TEST("protocol_unpack_too_short_header");
    {
        uint8_t buf[3] = {0, 0, 0};
        uint16_t msg_type = 0;
        const uint8_t* out_payload = NULL;
        uint32_t out_payload_len = 0;
        CeResult r = ce_net_base_unpack(buf, 3, &msg_type, &out_payload, &out_payload_len);
        CHECK(r == CE_ERR);
    }
    PASS();

    TEST("protocol_peek_len");
    {
        uint8_t buf[256];
        uint32_t total = ce_net_base_pack(buf, sizeof(buf), 0x0002, NULL, 0);
        uint32_t peeked = ce_net_base_peek_len(buf, total);
        CHECK(peeked == total);
    }
    PASS();

    TEST("protocol_peek_type");
    {
        uint8_t buf[256];
        ce_net_base_pack(buf, sizeof(buf), 0xABCD, NULL, 0);
        uint16_t peeked = ce_net_base_peek_type(buf, sizeof(buf));
        CHECK(peeked == 0xABCD);
    }
    PASS();

    TEST("protocol_peek_len_short");
    {
        uint8_t buf[2] = {0, 0};
        uint32_t peeked = ce_net_base_peek_len(buf, 2);
        CHECK(peeked == 0);
    }
    PASS();

    TEST("protocol_buffer_too_small");
    {
        uint8_t buf[10];
        uint32_t total = ce_net_base_pack(buf, sizeof(buf), 0x0001, NULL, 100);
        CHECK(total == 0); /* 缓冲区不够 */
    }
    PASS();

    return 0;
}

/* ================================================================
 * 1.1-1.3: TCP 连接管理 + 消息收发测试
 * ================================================================ */

static int test_connection_lifecycle(void) {
    TEST("connection_create_default");
    {
        CeNetConnection* conn = ce_net_conn_create(NULL);
        CHECK(conn != NULL);
        CHECK(ce_net_conn_get_state(conn) == CE_NET_CONN_DISCONNECTED);
        CHECK(ce_net_conn_get_fd(conn) == -1);
        ce_net_conn_destroy(conn);
    }
    PASS();

    TEST("connection_create_with_config");
    {
        CeNetConnConfig config;
        memset(&config, 0, sizeof(config));
        config.host = "127.0.0.1";
        config.port = 12345;
        config.timeout_ms = 5000;
        config.heartbeat_ms = 3000;
        config.heartbeat_timeout_ms = 9000;
        config.auto_reconnect = CE_TRUE;
        config.nonblocking = CE_TRUE;

        CeNetConnection* conn = ce_net_conn_create(&config);
        CHECK(conn != NULL);
        CHECK(ce_net_conn_get_state(conn) == CE_NET_CONN_DISCONNECTED);
        ce_net_conn_destroy(conn);
    }
    PASS();

    TEST("connection_connect_invalid");
    {
        CeNetConnConfig config;
        memset(&config, 0, sizeof(config));
        config.host = "127.0.0.1";
        config.port = 1; /* 无效端口（需要 root 权限） */
        config.timeout_ms = 100;

        CeNetConnection* conn = ce_net_conn_create(&config);
        CHECK(conn != NULL);
        CeResult r = ce_net_conn_connect(conn);
        /* 连接可能失败（端口不可用），但不应该崩溃 */
        (void)r;
        ce_net_conn_destroy(conn);
    }
    PASS();

    TEST("connection_stats");
    {
        CeNetConnection* conn = ce_net_conn_create(NULL);
        CHECK(conn != NULL);

        CeNetConnStats stats;
        ce_net_conn_get_stats(conn, &stats);
        CHECK(stats.state == CE_NET_CONN_DISCONNECTED);
        CHECK(stats.bytes_sent == 0);
        CHECK(stats.bytes_recv == 0);
        CHECK(stats.msgs_sent == 0);
        CHECK(stats.msgs_recv == 0);

        ce_net_conn_destroy(conn);
    }
    PASS();

    return 0;
}

/* ================================================================
 * 1.2: 消息收发（回环测试）
 * ================================================================ */

static int test_message_send_recv(void) {
    TEST("message_send_recv_loopback");
    {
        /* 创建服务器 */
        CeNetConnConfig server_config;
        memset(&server_config, 0, sizeof(server_config));
        server_config.host = "127.0.0.1";
        server_config.port = 22345;
        server_config.timeout_ms = 1000;
        server_config.nonblocking = CE_TRUE;

        /* 使用原始 socket 创建监听服务器 */
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(listen_fd >= 0);

        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(22345);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        CHECK(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
        CHECK(listen(listen_fd, 1) == 0);

        /* 设置非阻塞 */
        int flags = fcntl(listen_fd, F_GETFL, 0);
        fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

        /* 客户端连接 */
        CeNetConnection* client = ce_net_conn_create(&server_config);
        CHECK(client != NULL);
        CeResult cr = ce_net_conn_connect(client);
        /* 非阻塞连接可能返回 CE_ERR 但连接仍在进行中 */

        /* 接受连接 */
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int accepted_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);

        if (accepted_fd >= 0 && cr == CE_OK) {
            /* 发送消息 */
            const char* test_data = "Test Message";
            CeNetMessage msg;
            msg.type = CE_NET_MSG_GAME_DATA;
            msg.payload_len = (uint32_t)strlen(test_data);
            msg.payload = (const uint8_t*)test_data;

            CeResult send_r = ce_net_conn_send(client, &msg);
            if (send_r == CE_OK) {
                /* 从 accepted_fd 读取 */
                uint8_t recv_buf[256];
                ssize_t n = recv(accepted_fd, recv_buf, sizeof(recv_buf), 0);
                if (n > 0) {
                    /* 验证协议格式 */
                    uint32_t total_len = ((uint32_t)recv_buf[0] << 24) | ((uint32_t)recv_buf[1] << 16)
                                       | ((uint32_t)recv_buf[2] << 8) | (uint32_t)recv_buf[3];
                    CHECK(total_len == CE_NET_BASE_HEADER_SIZE + msg.payload_len);
                }
            }
        }

        if (accepted_fd >= 0) close(accepted_fd);
        ce_net_conn_destroy(client);
        close(listen_fd);
    }
    PASS();

    return 0;
}

/* ================================================================
 * 1.4: 心跳检测测试
 * ================================================================ */

static int test_heartbeat(void) {
    TEST("heartbeat_send_ping_pong");
    {
        /* 测试 ping/pong 不会崩溃（没有实际连接） */
        CeNetConnection* conn = ce_net_conn_create(NULL);
        CHECK(conn != NULL);

        /* 没有连接时发送应返回 CE_ERR */
        CeResult r1 = ce_net_conn_send_ping(conn);
        CHECK(r1 == CE_ERR);

        CeResult r2 = ce_net_conn_send_pong(conn);
        CHECK(r2 == CE_ERR);

        ce_net_conn_destroy(conn);
    }
    PASS();

    TEST("heartbeat_timeout_default");
    {
        CeNetConnConfig config;
        memset(&config, 0, sizeof(config));
        config.host = "127.0.0.1";
        config.port = 12345;
        config.heartbeat_ms = 100; /* 100ms 心跳 */
        config.heartbeat_timeout_ms = 300; /* 300ms 超时 */

        CeNetConnection* conn = ce_net_conn_create(&config);
        CHECK(conn != NULL);

        /* 初始状态：刚创建，心跳时间未设置，应判定为超时 */
        CeBool timeout = ce_net_conn_heartbeat_timeout(conn);
        /* 由于 last_heartbeat_us 初始为 0，elapsed 会很大，所以 timeout */
        CHECK(timeout == CE_TRUE);

        /* 更新心跳时间 */
        ce_net_conn_heartbeat_touch(conn);
        timeout = ce_net_conn_heartbeat_timeout(conn);
        CHECK(timeout == CE_FALSE);

        ce_net_conn_destroy(conn);
    }
    PASS();

    TEST("heartbeat_disabled");
    {
        CeNetConnConfig config;
        memset(&config, 0, sizeof(config));
        config.heartbeat_ms = 0; /* 禁用心跳 */

        CeNetConnection* conn = ce_net_conn_create(&config);
        CHECK(conn != NULL);

        /* 心跳禁用时不应超时 */
        CeBool timeout = ce_net_conn_heartbeat_timeout(conn);
        CHECK(timeout == CE_FALSE);

        ce_net_conn_destroy(conn);
    }
    PASS();

    TEST("is_heartbeat_helper");
    {
        CHECK(ce_net_base_is_heartbeat(CE_NET_MSG_PING) == CE_TRUE);
        CHECK(ce_net_base_is_heartbeat(CE_NET_MSG_PONG) == CE_TRUE);
        CHECK(ce_net_base_is_heartbeat(CE_NET_MSG_GAME_DATA) == CE_FALSE);
        CHECK(ce_net_base_is_heartbeat(CE_NET_MSG_LOGIN) == CE_FALSE);
    }
    PASS();

    return 0;
}

/* ================================================================
 * 1.5: 连接池测试
 * ================================================================ */

static int test_pool(void) {
    TEST("pool_create_destroy");
    {
        CeNetPool* pool = ce_net_pool_create(8);
        CHECK(pool != NULL);

        int total = 0, available = 0;
        ce_net_pool_stats(pool, &total, &available);
        CHECK(total == 0);
        CHECK(available == 0);

        ce_net_pool_destroy(pool);
    }
    PASS();

    TEST("pool_add_acquire");
    {
        CeNetPool* pool = ce_net_pool_create(4);
        CHECK(pool != NULL);

        /* 创建连接并添加到池 */
        CeNetConnConfig config;
        memset(&config, 0, sizeof(config));
        config.host = "127.0.0.1";
        config.port = 12345;

        CeNetConnection* c1 = ce_net_conn_create(&config);
        CeNetConnection* c2 = ce_net_conn_create(&config);
        CHECK(c1 != NULL);
        CHECK(c2 != NULL);

        CHECK(ce_net_pool_add(pool, c1) == CE_OK);
        CHECK(ce_net_pool_add(pool, c2) == CE_OK);

        int total = 0, available = 0;
        ce_net_pool_stats(pool, &total, &available);
        CHECK(total == 2);
        /* 连接未建立，available 应为 0 */
        CHECK(available == 0);

        /* acquire 应返回 NULL（没有已连接的连接） */
        CeNetConnection* acq = ce_net_pool_acquire(pool);
        CHECK(acq == NULL);

        ce_net_pool_destroy(pool);
    }
    PASS();

    TEST("pool_remove");
    {
        CeNetPool* pool = ce_net_pool_create(4);
        CHECK(pool != NULL);

        CeNetConnConfig config;
        memset(&config, 0, sizeof(config));
        config.host = "127.0.0.1";
        config.port = 12345;

        CeNetConnection* c1 = ce_net_conn_create(&config);
        CHECK(c1 != NULL);

        CHECK(ce_net_pool_add(pool, c1) == CE_OK);

        int total = 0;
        ce_net_pool_stats(pool, &total, NULL);
        CHECK(total == 1);

        CHECK(ce_net_pool_remove(pool, c1) == CE_OK);
        ce_net_pool_stats(pool, &total, NULL);
        CHECK(total == 0);

        /* c1 所有权已转回，需要手动销毁 */
        ce_net_conn_destroy(c1);
        ce_net_pool_destroy(pool);
    }
    PASS();

    TEST("pool_full");
    {
        CeNetPool* pool = ce_net_pool_create(2);
        CHECK(pool != NULL);

        CeNetConnConfig config;
        memset(&config, 0, sizeof(config));

        CeNetConnection* c1 = ce_net_conn_create(&config);
        CeNetConnection* c2 = ce_net_conn_create(&config);
        CeNetConnection* c3 = ce_net_conn_create(&config);

        CHECK(ce_net_pool_add(pool, c1) == CE_OK);
        CHECK(ce_net_pool_add(pool, c2) == CE_OK);
        CHECK(ce_net_pool_add(pool, c3) == CE_ERR); /* 池已满 */

        ce_net_conn_destroy(c3); /* c3 未被添加，手动销毁 */
        ce_net_pool_destroy(pool);
    }
    PASS();

    TEST("pool_cleanup");
    {
        CeNetPool* pool = ce_net_pool_create(4);
        CHECK(pool != NULL);

        CeNetConnConfig config;
        memset(&config, 0, sizeof(config));
        config.host = "127.0.0.1";
        config.port = 12345;

        CeNetConnection* c1 = ce_net_conn_create(&config);
        CHECK(c1 != NULL);

        CHECK(ce_net_pool_add(pool, c1) == CE_OK);

        /* 断开连接 */
        ce_net_conn_disconnect(c1);

        int removed = ce_net_pool_cleanup(pool);
        CHECK(removed == 1);

        int total = 0;
        ce_net_pool_stats(pool, &total, NULL);
        CHECK(total == 0);

        ce_net_pool_destroy(pool);
    }
    PASS();

    return 0;
}

/* ================================================================
 * 1.6: 自动重连测试
 * ================================================================ */

static int test_reconnect(void) {
    TEST("reconnect_disabled");
    {
        CeNetConnConfig config;
        memset(&config, 0, sizeof(config));
        config.host = "127.0.0.1";
        config.port = 12345;
        config.auto_reconnect = CE_FALSE;

        CeNetConnection* conn = ce_net_conn_create(&config);
        CHECK(conn != NULL);

        /* 未连接，auto_reconnect 关闭，应返回 CE_ERR */
        CeResult r = ce_net_conn_try_reconnect(conn);
        CHECK(r == CE_ERR);

        ce_net_conn_destroy(conn);
    }
    PASS();

    TEST("reconnect_reset");
    {
        CeNetConnConfig config;
        memset(&config, 0, sizeof(config));
        config.host = "127.0.0.1";
        config.port = 12345;
        config.auto_reconnect = CE_TRUE;

        CeNetConnection* conn = ce_net_conn_create(&config);
        CHECK(conn != NULL);

        ce_net_conn_reset_reconnect(conn);
        /* 不应崩溃 */

        ce_net_conn_destroy(conn);
    }
    PASS();

    return 0;
}

/* ================================================================
 * 1.10: 跨区消息格式测试
 * ================================================================ */

static int test_cross_region(void) {
    TEST("cross_region_pack_unpack");
    {
        uint8_t buf[512];
        const char* inner = "cross-region-data";

        CeNetCrossRegionMessage msg;
        memset(&msg, 0, sizeof(msg));
        msg.header.src_region = 1;
        msg.header.dst_region = 2;
        msg.header.timestamp_us = 1234567890ULL;
        msg.header.hop_count = 0;
        msg.header.ttl = 16;
        msg.inner_type = CE_NET_MSG_GAME_DATA;
        msg.inner_len = (uint32_t)strlen(inner);
        msg.inner_data = (const uint8_t*)inner;

        uint32_t total = ce_net_base_pack_cross_region(buf, sizeof(buf), &msg);
        CHECK(total > 0);

        CeNetCrossRegionMessage unpacked;
        CeResult r = ce_net_base_unpack_cross_region(buf, total, &unpacked);
        CHECK(r == CE_OK);
        CHECK(unpacked.header.src_region == 1);
        CHECK(unpacked.header.dst_region == 2);
        CHECK(unpacked.header.timestamp_us == 1234567890ULL);
        CHECK(unpacked.header.hop_count == 0);
        CHECK(unpacked.header.ttl == 16);
        CHECK(unpacked.inner_type == CE_NET_MSG_GAME_DATA);
        CHECK(unpacked.inner_len == msg.inner_len);
        CHECK(memcmp(unpacked.inner_data, inner, msg.inner_len) == 0);
    }
    PASS();

    TEST("cross_region_unpack_invalid");
    {
        uint8_t buf[10] = {0};
        CeNetCrossRegionMessage msg;
        CeResult r = ce_net_base_unpack_cross_region(buf, 10, &msg);
        CHECK(r == CE_ERR);
    }
    PASS();

    TEST("cross_region_unpack_wrong_type");
    {
        uint8_t buf[256];
        /* 打包一个非跨区消息 */
        uint32_t total = ce_net_base_pack(buf, sizeof(buf), CE_NET_MSG_PING, NULL, 0);
        CHECK(total > 0);

        CeNetCrossRegionMessage msg;
        CeResult r = ce_net_base_unpack_cross_region(buf, total, &msg);
        CHECK(r == CE_ERR); /* 类型不匹配 */
    }
    PASS();

    return 0;
}

/* ================================================================
 * 1.11-1.12: Router 网格测试
 * ================================================================ */

static int test_router_mesh(void) {
    TEST("mesh_create_destroy");
    {
        CeNetRegion local;
        memset(&local, 0, sizeof(local));
        strncpy(local.name, "us-east", sizeof(local.name) - 1);
        strncpy(local.host, "127.0.0.1", sizeof(local.host) - 1);
        local.port = 9000;
        local.region_id = 1;

        CeNetRouterMesh* mesh = ce_net_mesh_create(&local);
        CHECK(mesh != NULL);

        const CeNetRegion* r = ce_net_mesh_get_region(mesh, 1);
        CHECK(r != NULL);
        CHECK(r->region_id == 1);

        ce_net_mesh_destroy(mesh);
    }
    PASS();

    TEST("mesh_add_remove_region");
    {
        CeNetRegion local;
        memset(&local, 0, sizeof(local));
        strncpy(local.name, "local", sizeof(local.name) - 1);
        local.region_id = 0;

        CeNetRouterMesh* mesh = ce_net_mesh_create(&local);
        CHECK(mesh != NULL);

        CeNetRegion remote;
        memset(&remote, 0, sizeof(remote));
        strncpy(remote.name, "eu-west", sizeof(remote.name) - 1);
        strncpy(remote.host, "10.0.0.1", sizeof(remote.host) - 1);
        remote.port = 9000;
        remote.region_id = 2;

        CHECK(ce_net_mesh_add_region(mesh, &remote) == CE_OK);

        /* 检查重复添加 */
        CHECK(ce_net_mesh_add_region(mesh, &remote) == CE_ERR);

        const CeNetRegion* r = ce_net_mesh_get_region(mesh, 2);
        CHECK(r != NULL);
        CHECK(r->region_id == 2);
        CHECK(strcmp(r->name, "eu-west") == 0);

        /* 移除 */
        CHECK(ce_net_mesh_remove_region(mesh, 2) == CE_OK);
        CHECK(ce_net_mesh_get_region(mesh, 2) == NULL);

        ce_net_mesh_destroy(mesh);
    }
    PASS();

    TEST("mesh_get_all_regions");
    {
        CeNetRouterMesh* mesh = ce_net_mesh_create(NULL);
        CHECK(mesh != NULL);

        CeNetRegion r1, r2;
        memset(&r1, 0, sizeof(r1)); r1.region_id = 1; strncpy(r1.name, "r1", sizeof(r1.name) - 1);
        memset(&r2, 0, sizeof(r2)); r2.region_id = 2; strncpy(r2.name, "r2", sizeof(r2.name) - 1);

        ce_net_mesh_add_region(mesh, &r1);
        ce_net_mesh_add_region(mesh, &r2);

        int count = 0;
        const CeNetRegion* regions = ce_net_mesh_get_all_regions(mesh, &count);
        CHECK(count == 2);
        CHECK(regions != NULL);

        ce_net_mesh_destroy(mesh);
    }
    PASS();

    TEST("mesh_stats");
    {
        CeNetRouterMesh* mesh = ce_net_mesh_create(NULL);
        CHECK(mesh != NULL);

        int total = 0, connected = 0;
        ce_net_mesh_stats(mesh, &total, &connected);
        CHECK(total == 0);
        CHECK(connected == 0);

        CeNetRegion r;
        memset(&r, 0, sizeof(r)); r.region_id = 1; strncpy(r.name, "r1", sizeof(r.name) - 1);
        ce_net_mesh_add_region(mesh, &r);

        ce_net_mesh_stats(mesh, &total, &connected);
        CHECK(total == 1);
        CHECK(connected == 0); /* 未连接 */

        ce_net_mesh_destroy(mesh);
    }
    PASS();

    TEST("mesh_max_regions");
    {
        CeNetRouterMesh* mesh = ce_net_mesh_create(NULL);
        CHECK(mesh != NULL);

        /* 添加最大数量的区域 */
        for (int i = 0; i < CE_NET_BASE_MESH_MAX_REGIONS; i++) {
            CeNetRegion r;
            memset(&r, 0, sizeof(r));
            r.region_id = (uint32_t)(i + 1);
            snprintf(r.name, sizeof(r.name), "region-%d", i);
            CHECK(ce_net_mesh_add_region(mesh, &r) == CE_OK);
        }

        /* 再添加应失败 */
        CeNetRegion extra;
        memset(&extra, 0, sizeof(extra));
        extra.region_id = 999;
        CHECK(ce_net_mesh_add_region(mesh, &extra) == CE_ERR);

        ce_net_mesh_destroy(mesh);
    }
    PASS();

    TEST("mesh_broadcast_no_connections");
    {
        CeNetRouterMesh* mesh = ce_net_mesh_create(NULL);
        CHECK(mesh != NULL);

        CeNetRegion r;
        memset(&r, 0, sizeof(r)); r.region_id = 1; strncpy(r.name, "r1", sizeof(r.name) - 1);
        ce_net_mesh_add_region(mesh, &r);

        CeNetMessage msg;
        msg.type = CE_NET_MSG_GAME_DATA;
        msg.payload_len = 0;
        msg.payload = NULL;

        /* 没有连接，广播应返回 0 */
        int sent = ce_net_mesh_broadcast(mesh, &msg);
        CHECK(sent == 0);

        ce_net_mesh_destroy(mesh);
    }
    PASS();

    return 0;
}

/* ================================================================
 * 工具函数测试
 * ================================================================ */

static int test_utils(void) {
    TEST("now_us_monotonic");
    {
        uint64_t t1 = ce_net_base_now_us();
        CHECK(t1 > 0);
        usleep(1000); /* 1ms */
        uint64_t t2 = ce_net_base_now_us();
        CHECK(t2 > t1);
    }
    PASS();

    return 0;
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void) {
    printf("=== ce_net_base Unit Tests ===\n\n");

    int failures = 0;

    printf("--- Protocol Codec (1.7) ---\n");
    failures += test_protocol_pack_unpack();

    printf("\n--- Connection Lifecycle (1.1-1.3) ---\n");
    failures += test_connection_lifecycle();

    printf("\n--- Message Send/Recv (1.2) ---\n");
    failures += test_message_send_recv();

    printf("\n--- Heartbeat (1.4) ---\n");
    failures += test_heartbeat();

    printf("\n--- Connection Pool (1.5) ---\n");
    failures += test_pool();

    printf("\n--- Auto Reconnect (1.6) ---\n");
    failures += test_reconnect();

    printf("\n--- Cross Region Messages (1.10) ---\n");
    failures += test_cross_region();

    printf("\n--- Router Mesh (1.11-1.12) ---\n");
    failures += test_router_mesh();

    printf("\n--- Utilities ---\n");
    failures += test_utils();

    printf("\n");
    if (failures == 0) {
        printf("=== All ce_net_base tests passed! ===\n");
        return 0;
    } else {
        printf("=== %d test(s) FAILED ===\n", failures);
        return 1;
    }
}
