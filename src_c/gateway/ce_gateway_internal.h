/*
 * ChaosEngine Gateway - 内部接口
 *
 * 声明各子模块的结构体和函数，供 ce_gateway.c 主逻辑使用。
 * 不对外暴露，仅 gateway 模块内部引用。
 */

#ifndef CE_GATEWAY_INTERNAL_H
#define CE_GATEWAY_INTERNAL_H

#include "gateway/ce_gateway.h"
#include "public_api/ce_types.h"

#include <stdint.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 连接管理 (ce_gateway_conn.c)
 * ================================================================ */

typedef enum CeGatewayConnType {
    CE_GW_CONN_FREE      = 0,
    CE_GW_CONN_CLIENT    = 1,
    CE_GW_CONN_BACKEND   = 2,
} CeGatewayConnType;

typedef struct CeGatewayConn {
    int             fd;
    uint32_t        conn_id;
    CeGatewayConnType type;
    CeBool          active;
    char            remote_host[64];
    uint16_t        remote_port;
    time_t          connect_time;
    time_t          last_active;
    uint8_t         recv_buf[CE_GATEWAY_RECV_BUF_SIZE];
    int             recv_len;
    int             peer_fd;
    int             backend_index;
} CeGatewayConn;

typedef struct CeGatewayConnTable {
    CeGatewayConn*  slots;
    uint32_t        next_conn_id;
    int             active_count;
} CeGatewayConnTable;

CeGatewayConnTable* ce_gateway_conn_table_create(void);
void ce_gateway_conn_table_destroy(CeGatewayConnTable* table);
CeGatewayConn* ce_gateway_conn_create(CeGatewayConnTable* table, int fd,
                                      CeGatewayConnType type,
                                      const struct sockaddr_in* addr);
void ce_gateway_conn_destroy(CeGatewayConnTable* table, int fd);
CeGatewayConn* ce_gateway_conn_find(CeGatewayConnTable* table, int fd);
void ce_gateway_conn_touch(CeGatewayConnTable* table, int fd);
int ce_gateway_conn_active_count(const CeGatewayConnTable* table);

uint8_t* ce_gateway_conn_recv_buf(CeGatewayConn* conn);
int ce_gateway_conn_recv_len(const CeGatewayConn* conn);
int ce_gateway_conn_recv_space(const CeGatewayConn* conn);
void ce_gateway_conn_recv_append(CeGatewayConn* conn, const uint8_t* data, int len);
void ce_gateway_conn_recv_consume(CeGatewayConn* conn, int consumed);
void ce_gateway_conn_recv_reset(CeGatewayConn* conn);

/* ================================================================
 * 路由表 (ce_gateway_router.c)
 * ================================================================ */

#define CE_GATEWAY_MAX_ROUTES 64

typedef struct CeGatewayRoute {
    uint16_t    msg_type_start;
    uint16_t    msg_type_end;
    int         backend_index;   /* -1 = 本地处理 */
} CeGatewayRoute;

typedef struct CeGatewayRouteTable {
    CeGatewayRoute routes[CE_GATEWAY_MAX_ROUTES];
    int            count;
} CeGatewayRouteTable;

CeGatewayRouteTable* ce_gateway_router_create(void);
void ce_gateway_router_destroy(CeGatewayRouteTable* rt);
int ce_gateway_router_find(const CeGatewayRouteTable* rt, uint16_t msg_type);
CeResult ce_gateway_router_add(CeGatewayRouteTable* rt,
                               uint16_t msg_type_start,
                               uint16_t msg_type_end,
                               int backend_index);

/* ================================================================
 * 后端连接池 (ce_gateway_backend.c)
 * ================================================================ */

typedef enum CeBackendState {
    CE_BACKEND_DISCONNECTED = 0,
    CE_BACKEND_CONNECTING   = 1,
    CE_BACKEND_CONNECTED    = 2,
    CE_BACKEND_UNHEALTHY    = 3,
} CeBackendState;

typedef struct CeGatewayBackend {
    int             fd;
    CeBackendState  state;
    char            host[256];
    uint16_t        port;
    time_t          last_heartbeat;
    time_t          last_ping_sent;
    uint64_t        msgs_sent;
    uint64_t        msgs_recv;
    uint8_t         recv_buf[CE_GATEWAY_RECV_BUF_SIZE];
    int             recv_len;
} CeGatewayBackend;

typedef struct CeGatewayBackendPool {
    CeGatewayBackend   backends[CE_GATEWAY_MAX_BACKENDS];
    int                count;
    int                heartbeat_sec;
} CeGatewayBackendPool;

CeGatewayBackendPool* ce_gateway_backend_pool_create(int heartbeat_sec);
void ce_gateway_backend_pool_destroy(CeGatewayBackendPool* pool);
CeResult ce_gateway_backend_pool_add(CeGatewayBackendPool* pool,
                                    const char* host, uint16_t port);
CeResult ce_gateway_backend_connect(CeGatewayBackendPool* pool, int index);
CeResult ce_gateway_backend_finish_connect(CeGatewayBackendPool* pool, int index);
int ce_gateway_backend_send(CeGatewayBackendPool* pool, int index,
                           const void* data, int len);
int ce_gateway_backend_recv(CeGatewayBackendPool* pool, int index,
                           void* buf, int size);
int ce_gateway_backend_fd(const CeGatewayBackendPool* pool, int index);
CeBool ce_gateway_backend_healthy(const CeGatewayBackendPool* pool, int index);
int ce_gateway_backend_count(const CeGatewayBackendPool* pool);
uint8_t* ce_gateway_backend_recv_buf(CeGatewayBackendPool* pool, int index);
int* ce_gateway_backend_recv_len_ptr(CeGatewayBackendPool* pool, int index);
void ce_gateway_backend_mark_disconnected(CeGatewayBackendPool* pool, int index);
int ce_gateway_backend_health_check(CeGatewayBackendPool* pool);
int ce_gateway_backend_try_reconnect(CeGatewayBackendPool* pool);

/* ================================================================
 * 协议编解码 (ce_gateway_protocol.c)
 * ================================================================ */

int ce_gateway_protocol_parse(const uint8_t* buf, int len,
                              uint16_t* out_msg_type,
                              const uint8_t** out_payload,
                              int* out_payload_len);
int ce_gateway_protocol_pack(uint16_t msg_type,
                             const uint8_t* payload, int payload_len,
                             uint8_t* out_buf, int out_buf_size);

#ifdef __cplusplus
}
#endif

#endif /* CE_GATEWAY_INTERNAL_H */
