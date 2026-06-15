/*
 * ChaosEngine 网络层 — Linux/POSIX 实现
 */

#define _POSIX_C_SOURCE 200112L
#include "network/ce_network.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ---- Socket 结构 ---- */

struct CeSocket {
    int         fd;
    CeSocketType type;
    CeBool      valid;
};

/* ---- 网络初始化 ---- */

CeResult ce_net_init(void) {
    /* POSIX 不需要显式初始化 */
    return CE_OK;
}

void ce_net_shutdown(void) {
    /* POSIX 不需要显式清理 */
}

/* ---- 地址解析 ---- */

CeResult ce_net_resolve(const char* host, uint16_t port, CeNetAddress* out_addr) {
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;  /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(host, port_str, &hints, &result) != 0) {
        return CE_ERR;
    }

    struct sockaddr_in* addr = (struct sockaddr_in*)result->ai_addr;
    strncpy(out_addr->host, inet_ntoa(addr->sin_addr), sizeof(out_addr->host) - 1);
    out_addr->port = port;

    freeaddrinfo(result);
    return CE_OK;
}

const char* ce_net_addr_to_string(const CeNetAddress* addr, char* buffer, size_t size) {
    snprintf(buffer, size, "%s:%u", addr->host, addr->port);
    return buffer;
}

/* ---- Socket 创建 ---- */

static CeSocket* socket_create(CeSocketType type, int sock_type) {
    int fd = socket(AF_INET, sock_type, 0);
    if (fd < 0) return NULL;

    CeSocket* sock = (CeSocket*)malloc(sizeof(CeSocket));
    sock->fd    = fd;
    sock->type  = type;
    sock->valid = CE_TRUE;
    return sock;
}

CeSocket* ce_socket_create_tcp(void) {
    return socket_create(CE_SOCKET_TCP, SOCK_STREAM);
}

CeSocket* ce_socket_create_udp(void) {
    return socket_create(CE_SOCKET_UDP, SOCK_DGRAM);
}

/* ---- 辅助：地址转换 ---- */

static struct sockaddr_in to_sockaddr(const CeNetAddress* addr) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(addr->port);
    inet_pton(AF_INET, addr->host, &sa.sin_addr);
    return sa;
}

/* ---- TCP ---- */

CeResult ce_socket_connect(CeSocket* sock, const CeNetAddress* addr) {
    if (!sock || !sock->valid) return CE_ERR;
    struct sockaddr_in sa = to_sockaddr(addr);
    if (connect(sock->fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        return CE_ERR;
    }
    return CE_OK;
}

CeSocket* ce_socket_accept(CeSocket* listen_sock, CeNetAddress* out_addr) {
    if (!listen_sock || !listen_sock->valid) return NULL;

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(listen_sock->fd, (struct sockaddr*)&client_addr, &addr_len);

    if (client_fd < 0) return NULL;

    CeSocket* client = (CeSocket*)malloc(sizeof(CeSocket));
    client->fd    = client_fd;
    client->type  = CE_SOCKET_TCP;
    client->valid = CE_TRUE;

    if (out_addr) {
        strncpy(out_addr->host, inet_ntoa(client_addr.sin_addr), sizeof(out_addr->host) - 1);
        out_addr->port = ntohs(client_addr.sin_port);
    }

    return client;
}

CeResult ce_socket_bind(CeSocket* sock, const CeNetAddress* addr) {
    if (!sock || !sock->valid) return CE_ERR;
    struct sockaddr_in sa = to_sockaddr(addr);

    /* 允许端口复用 */
    int opt = 1;
    setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(sock->fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        return CE_ERR;
    }
    return CE_OK;
}

CeResult ce_socket_listen(CeSocket* sock, int backlog) {
    if (!sock || !sock->valid) return CE_ERR;
    if (listen(sock->fd, backlog) < 0) {
        return CE_ERR;
    }
    return CE_OK;
}

/* ---- UDP ---- */

int ce_socket_sendto(CeSocket* sock, const void* data, size_t size,
                     const CeNetAddress* addr) {
    if (!sock || !sock->valid) return -1;
    struct sockaddr_in sa = to_sockaddr(addr);
    return (int)sendto(sock->fd, data, size, 0, (struct sockaddr*)&sa, sizeof(sa));
}

int ce_socket_recvfrom(CeSocket* sock, void* buffer, size_t size,
                       CeNetAddress* out_addr) {
    if (!sock || !sock->valid) return -1;
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = (int)recvfrom(sock->fd, buffer, size, 0, (struct sockaddr*)&from, &from_len);

    if (n > 0 && out_addr) {
        strncpy(out_addr->host, inet_ntoa(from.sin_addr), sizeof(out_addr->host) - 1);
        out_addr->port = ntohs(from.sin_port);
    }

    return n;
}

/* ---- 通用收发 ---- */

int ce_socket_send(CeSocket* sock, const void* data, size_t size) {
    if (!sock || !sock->valid) return -1;
    return (int)send(sock->fd, data, size, 0);
}

int ce_socket_recv(CeSocket* sock, void* buffer, size_t size) {
    if (!sock || !sock->valid) return -1;
    return (int)recv(sock->fd, buffer, size, 0);
}

void ce_socket_close(CeSocket* sock) {
    if (!sock) return;
    if (sock->fd >= 0) {
        close(sock->fd);
    }
    sock->valid = CE_FALSE;
    free(sock);
}

CeBool ce_socket_is_valid(CeSocket* sock) {
    return sock && sock->valid ? CE_TRUE : CE_FALSE;
}

CeResult ce_socket_set_nonblocking(CeSocket* sock, CeBool nonblocking) {
    if (!sock || !sock->valid) return CE_ERR;
    int flags = fcntl(sock->fd, F_GETFL, 0);
    if (flags < 0) return CE_ERR;

    if (nonblocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }

    return fcntl(sock->fd, F_SETFL, flags) < 0 ? CE_ERR : CE_OK;
}

CeResult ce_socket_set_nodelay(CeSocket* sock, CeBool nodelay) {
    if (!sock || !sock->valid) return CE_ERR;
    int opt = nodelay ? 1 : 0;
    return setsockopt(sock->fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0
           ? CE_ERR : CE_OK;
}

/* ---- 简易消息（长度前缀） ---- */

int ce_net_send_message(CeSocket* sock, const void* data, size_t size) {
    if (!sock || !sock->valid) return -1;
    if (size > 0xFFFFFFFF) return -1;  /* 超过 4GB 不支持 */

    uint32_t len = (uint32_t)size;
    /* 发送长度头（网络字节序） */
    uint32_t net_len = htonl(len);
    int sent = ce_socket_send(sock, &net_len, sizeof(net_len));
    if (sent != sizeof(net_len)) return -1;

    /* 发送数据 */
    size_t total = 0;
    while (total < size) {
        int n = ce_socket_send(sock, (const uint8_t*)data + total, size - total);
        if (n <= 0) return -1;
        total += (size_t)n;
    }

    return (int)total;
}

int ce_net_recv_message(CeSocket* sock, void* buffer, size_t max_size) {
    if (!sock || !sock->valid) return -1;

    /* 接收长度头 */
    uint32_t net_len;
    int n = ce_socket_recv(sock, &net_len, sizeof(net_len));
    if (n != sizeof(net_len)) return -1;

    uint32_t len = ntohl(net_len);
    if (len > max_size) return -1;  /* 缓冲区不够 */

    /* 接收数据 */
    size_t total = 0;
    while (total < len) {
        n = ce_socket_recv(sock, (uint8_t*)buffer + total, len - total);
        if (n <= 0) return -1;
        total += (size_t)n;
    }

    return (int)total;
}
