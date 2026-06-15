/*
 * ChaosEngine 网络层
 * TCP/UDP Socket 封装，跨平台
 */

#ifndef CE_NETWORK_H
#define CE_NETWORK_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 地址 ---- */

typedef struct CeNetAddress {
    char   host[256];
    uint16_t port;
} CeNetAddress;

/** 解析地址 */
CeResult ce_net_resolve(const char* host, uint16_t port, CeNetAddress* out_addr);

/** 地址转字符串 */
const char* ce_net_addr_to_string(const CeNetAddress* addr, char* buffer, size_t size);

/* ---- Socket ---- */

typedef struct CeSocket CeSocket;

typedef enum CeSocketType {
    CE_SOCKET_TCP = 0,
    CE_SOCKET_UDP = 1,
} CeSocketType;

/* TCP */
CeSocket* ce_socket_create_tcp(void);
CeResult  ce_socket_connect(CeSocket* sock, const CeNetAddress* addr);
CeSocket* ce_socket_accept(CeSocket* listen_sock, CeNetAddress* out_addr);
CeResult  ce_socket_bind(CeSocket* sock, const CeNetAddress* addr);
CeResult  ce_socket_listen(CeSocket* sock, int backlog);

/* UDP */
CeSocket* ce_socket_create_udp(void);
int       ce_socket_sendto(CeSocket* sock, const void* data, size_t size,
                           const CeNetAddress* addr);
int       ce_socket_recvfrom(CeSocket* sock, void* buffer, size_t size,
                             CeNetAddress* out_addr);

/* 通用 */
int       ce_socket_send(CeSocket* sock, const void* data, size_t size);
int       ce_socket_recv(CeSocket* sock, void* buffer, size_t size);
void      ce_socket_close(CeSocket* sock);
CeBool    ce_socket_is_valid(CeSocket* sock);
CeResult  ce_socket_set_nonblocking(CeSocket* sock, CeBool nonblocking);
CeResult  ce_socket_set_nodelay(CeSocket* sock, CeBool nodelay);  /* TCP_NODELAY */

/* ---- 简易消息 ---- */

/** 发送带长度前缀的消息（4字节长度头 + 数据） */
int ce_net_send_message(CeSocket* sock, const void* data, size_t size);

/** 接收带长度前缀的消息，返回实际数据大小 */
int ce_net_recv_message(CeSocket* sock, void* buffer, size_t max_size);

/* ---- 网络初始化/清理 ---- */

CeResult ce_net_init(void);
void     ce_net_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* CE_NETWORK_H */
