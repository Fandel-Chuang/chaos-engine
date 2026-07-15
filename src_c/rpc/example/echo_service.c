/*
 * Echo Service - 验证 RPC 框架链路
 *
 * 注册 ping → pong, echo → 原样返回
 * 连接注册中心注册自己
 */

#include "rpc/ce_rpc.h"
#include "rpc/ce_service_registry.h"
#include "public_api/ce_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ping → "pong" */
static CeResult handle_ping(const uint8_t* req, uint32_t req_len,
                              uint8_t** resp, uint32_t* resp_len) {
    (void)req; (void)req_len;
    const char* pong = "pong";
    *resp_len = 4;
    *resp = (uint8_t*)malloc(4);
    if (*resp) {
        memcpy(*resp, pong, 4);
    }
    return CE_OK;
}

/* echo → 原样返回 */
static CeResult handle_echo(const uint8_t* req, uint32_t req_len,
                              uint8_t** resp, uint32_t* resp_len) {
    if (req_len > 0 && req) {
        *resp = (uint8_t*)malloc(req_len);
        if (*resp) {
            memcpy(*resp, req, req_len);
            *resp_len = req_len;
        }
    }
    return CE_OK;
}

int main(void) {
    ce_log_init(CE_LOG_INFO, "logs/echo_service.log");

    /* 创建 RPC 服务端 */
    CeRpcServer* srv = ce_rpc_server_create("echo_service", 9200);
    if (!srv) {
        fprintf(stderr, "Failed to create RPC server\n");
        return 1;
    }

    /* 注册方法 */
    ce_rpc_register(srv, "ping", handle_ping);
    ce_rpc_register(srv, "echo", handle_echo);

    /* 连接注册中心并注册自己 */
    CeServiceRegistry* reg = ce_registry_connect("127.0.0.1", 9300);
    if (reg) {
        ce_registry_register(reg, "echo_service", "127.0.0.1", 9200, "");
        CE_LOG_INFO("ECHO", "Registered to registry");
    } else {
        CE_LOG_WARN("ECHO", "Registry not available, starting without registration");
    }

    /* 运行 RPC 服务（阻塞） */
    CE_LOG_INFO("ECHO", "Starting echo_service on port 9200");
    ce_rpc_server_run(srv);

    /* 清理 */
    if (reg) ce_registry_destroy(reg);
    ce_rpc_server_destroy(srv);

    return 0;
}
