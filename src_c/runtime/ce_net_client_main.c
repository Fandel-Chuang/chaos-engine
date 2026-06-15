/*
 * ChaosEngine 网络客户端测试
 * 连接到 TCP echo 服务器，发送数据并验证回显
 */

#define _POSIX_C_SOURCE 200112L
#include "public_api/chaos_engine.h"
#include "network/ce_network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVER_HOST "127.0.0.1"
#define SERVER_PORT 7777
#define BUFFER_SIZE  4096

int main(void) {
    /* 初始化引擎 */
    CeEngineConfig config = {
        .app_name      = "ChaosEngine-NetClient",
        .window_width  = 0,
        .window_height = 0,
        .fullscreen    = CE_FALSE,
        .vsync         = CE_FALSE,
        .log_level     = CE_LOG_INFO,
        .log_file_path = "logs/chaos_net_client.log"
    };

    if (ce_init(&config) != CE_OK) {
        fprintf(stderr, "Failed to initialize engine\n");
        return 1;
    }

    if (ce_net_init() != CE_OK) {
        fprintf(stderr, "Failed to init network\n");
        ce_shutdown();
        return 1;
    }

    /* 解析服务器地址 */
    CeNetAddress addr;
    if (ce_net_resolve(SERVER_HOST, SERVER_PORT, &addr) != CE_OK) {
        fprintf(stderr, "Failed to resolve %s:%d\n", SERVER_HOST, SERVER_PORT);
        ce_net_shutdown();
        ce_shutdown();
        return 1;
    }

    printf("========================================\n");
    printf("  ChaosEngine Network Client Test\n");
    printf("  Connecting to %s:%d\n", SERVER_HOST, SERVER_PORT);
    printf("========================================\n\n");

    /* 创建 TCP socket 并连接 */
    CeSocket* sock = ce_socket_create_tcp();
    if (!sock) {
        fprintf(stderr, "Failed to create socket\n");
        ce_net_shutdown();
        ce_shutdown();
        return 1;
    }

    if (ce_socket_connect(sock, &addr) != CE_OK) {
        fprintf(stderr, "Failed to connect to server\n");
        ce_socket_close(sock);
        ce_net_shutdown();
        ce_shutdown();
        return 1;
    }
    printf("✅ Connected to server\n");

    /* 测试 1: 简单 echo */
    const char* msg1 = "Hello ChaosEngine!";
    int sent = ce_socket_send(sock, msg1, strlen(msg1));
    printf("📤 Sent (%d bytes): %s\n", sent, msg1);

    char buffer[BUFFER_SIZE];
    int n = ce_socket_recv(sock, buffer, BUFFER_SIZE - 1);
    if (n > 0) {
        buffer[n] = '\0';
        printf("📥 Echo (%d bytes): %s\n", n, buffer);
        if (strcmp(buffer, msg1) == 0) {
            printf("✅ Data sync OK (echo matches)\n");
        } else {
            printf("❌ Data mismatch!\n");
        }
    }

    /* 测试 2: 模拟战斗数据 */
    const char* msg2 = "{\"type\":\"combat\",\"attacker\":1,\"defender\":2,\"damage\":42.5}";
    sent = ce_socket_send(sock, msg2, strlen(msg2));
    printf("\n📤 Combat data (%d bytes): %s\n", sent, msg2);

    n = ce_socket_recv(sock, buffer, BUFFER_SIZE - 1);
    if (n > 0) {
        buffer[n] = '\0';
        printf("📥 Echo (%d bytes): %s\n", n, buffer);
        if (strcmp(buffer, msg2) == 0) {
            printf("✅ Combat data sync OK\n");
        }
    }

    /* 测试 3: 长度前缀消息 */
    const char* msg3 = "Length-prefixed message test";
    sent = ce_net_send_message(sock, msg3, strlen(msg3));
    printf("\n📤 Length-prefixed (%d bytes): %s\n", sent, msg3);

    n = ce_net_recv_message(sock, buffer, BUFFER_SIZE - 1);
    if (n > 0) {
        buffer[n] = '\0';
        printf("📥 Received (%d bytes): %s\n", n, buffer);
    }

    /* 清理 */
    ce_socket_close(sock);
    ce_net_shutdown();
    ce_shutdown();

    printf("\n🎉 Network client test complete!\n");
    return 0;
}
