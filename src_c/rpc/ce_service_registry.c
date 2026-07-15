/*
 * ChaosEngine 服务注册/发现 - 实现
 *
 * 内置 TCP 注册中心，简单文本协议:
 *   REGISTER name host port [metadata]\n
 *   LOOKUP name\n
 *   DEREGISTER name\n
 *   OK [data]\n  /  ERROR [message]\n
 *
 * 纯 C99。
 */

#define _POSIX_C_SOURCE 200112L

#include "rpc/ce_service_registry.h"
#include "public_api/ce_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---- 内部结构 ---- */

/** 服务条目 */
typedef struct CeRegEntry {
    CeServiceInstance  instance;
    struct CeRegEntry* next;
} CeRegEntry;

struct CeServiceRegistry {
    int         is_server;       /* 是否为服务端 */
    int         listen_fd;       /* 服务端监听 fd */
    int         client_fd;       /* 客户端连接 fd */
    int         port;            /* 端口 */
    char        server_host[64]; /* 服务端地址 */
    CeRegEntry* entries;         /* 服务条目链表（仅服务端） */
};

/* ---- 工具函数 ---- */

/** 从 socket 读取一行 */
static int read_line(int fd, char* buf, int max_len) {
    int n = 0;
    while (n < max_len - 1) {
        char ch;
        int r = recv(fd, &ch, 1, 0);
        if (r <= 0) return r < 0 ? -1 : n;
        if (ch == '\n') break;
        buf[n++] = ch;
    }
    buf[n] = '\0';
    return n;
}

/** 向 socket 写一行 */
static int write_line(int fd, const char* line) {
    int len = (int)strlen(line);
    int total = 0;
    while (total < len) {
        int n = send(fd, line + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    send(fd, "\n", 1, 0);
    return total;
}

/* ---- 服务端 ---- */

CeServiceRegistry* ce_registry_create_server(int port) {
    CeServiceRegistry* reg = (CeServiceRegistry*)calloc(1, sizeof(*reg));
    if (!reg) return NULL;

    reg->is_server = CE_TRUE;
    reg->port = (port > 0) ? port : CE_REGISTRY_DEFAULT_PORT;
    reg->entries = NULL;
    reg->client_fd = -1;

    /* 创建监听 socket */
    reg->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (reg->listen_fd < 0) {
        CE_LOG_ERROR("REGISTRY", "create_server: socket failed");
        free(reg);
        return NULL;
    }

    int opt = 1;
    setsockopt(reg->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)reg->port);

    if (bind(reg->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CE_LOG_ERROR("REGISTRY", "create_server: bind failed port=%d", reg->port);
        close(reg->listen_fd);
        free(reg);
        return NULL;
    }

    if (listen(reg->listen_fd, 32) < 0) {
        CE_LOG_ERROR("REGISTRY", "create_server: listen failed");
        close(reg->listen_fd);
        free(reg);
        return NULL;
    }

    CE_LOG_INFO("REGISTRY", "Server started on port %d", reg->port);
    return reg;
}

CeResult ce_registry_run(CeServiceRegistry* reg) {
    if (!reg || !reg->is_server) return CE_ERR;

    CE_LOG_INFO("REGISTRY", "Running on port %d...", reg->port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(reg->listen_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            CE_LOG_ERROR("REGISTRY", "accept failed");
            continue;
        }

        char line[1024];
        while (read_line(client_fd, line, sizeof(line)) > 0) {
            /* 解析命令 */
            if (strncmp(line, "REGISTER ", 9) == 0) {
                /* REGISTER name host port [metadata] */
                char name[64], host[64], meta[256];
                int port;
                meta[0] = '\0';
                int n = sscanf(line + 9, "%63s %63s %d %255[^\n]",
                               name, host, &port, meta);
                if (n >= 3) {
                    /* 添加或更新条目 */
                    CeRegEntry* e = reg->entries;
                    CeBool found = CE_FALSE;
                    while (e) {
                        if (strcmp(e->instance.name, name) == 0) {
                            strncpy(e->instance.host, host, sizeof(e->instance.host) - 1);
                            e->instance.port = port;
                            if (n >= 4) {
                                strncpy(e->instance.metadata, meta, sizeof(e->instance.metadata) - 1);
                            }
                            found = CE_TRUE;
                            break;
                        }
                        e = e->next;
                    }
                    if (!found) {
                        e = (CeRegEntry*)calloc(1, sizeof(*e));
                        strncpy(e->instance.name, name, sizeof(e->instance.name) - 1);
                        strncpy(e->instance.host, host, sizeof(e->instance.host) - 1);
                        e->instance.port = port;
                        if (n >= 4) {
                            strncpy(e->instance.metadata, meta, sizeof(e->instance.metadata) - 1);
                        }
                        e->next = reg->entries;
                        reg->entries = e;
                    }
                    CE_LOG_INFO("REGISTRY", "Registered: %s -> %s:%d", name, host, port);
                    write_line(client_fd, "OK");
                } else {
                    write_line(client_fd, "ERROR invalid REGISTER format");
                }
            } else if (strncmp(line, "LOOKUP ", 7) == 0) {
                char name[64];
                sscanf(line + 7, "%63s", name);
                CeRegEntry* e = reg->entries;
                CeBool found = CE_FALSE;
                while (e) {
                    if (strcmp(e->instance.name, name) == 0) {
                        char resp[256];
                        snprintf(resp, sizeof(resp), "OK %s %d %s",
                                 e->instance.host, e->instance.port,
                                 e->instance.metadata[0] ? e->instance.metadata : "{}");
                        write_line(client_fd, resp);
                        found = CE_TRUE;
                        break;
                    }
                    e = e->next;
                }
                if (!found) {
                    write_line(client_fd, "ERROR not_found");
                }
            } else if (strncmp(line, "DEREGISTER ", 11) == 0) {
                char name[64];
                sscanf(line + 11, "%63s", name);
                CeRegEntry** pp = &reg->entries;
                while (*pp) {
                    if (strcmp((*pp)->instance.name, name) == 0) {
                        CeRegEntry* del = *pp;
                        *pp = del->next;
                        free(del);
                        CE_LOG_INFO("REGISTRY", "Deregistered: %s", name);
                        break;
                    }
                    pp = &(*pp)->next;
                }
                write_line(client_fd, "OK");
            } else if (strncmp(line, "COUNT", 5) == 0) {
                int count = 0;
                CeRegEntry* e = reg->entries;
                while (e) { count++; e = e->next; }
                char resp[64];
                snprintf(resp, sizeof(resp), "OK %d", count);
                write_line(client_fd, resp);
            } else {
                write_line(client_fd, "ERROR unknown_command");
            }
        }

        close(client_fd);
    }

    return CE_OK;
}

/* ---- 客户端 ---- */

CeServiceRegistry* ce_registry_connect(const char* host, int port) {
    if (!host) host = "127.0.0.1";
    if (port <= 0) port = CE_REGISTRY_DEFAULT_PORT;

    CeServiceRegistry* reg = (CeServiceRegistry*)calloc(1, sizeof(*reg));
    if (!reg) return NULL;

    reg->is_server = CE_FALSE;
    reg->listen_fd = -1;
    reg->port = port;
    strncpy(reg->server_host, host, sizeof(reg->server_host) - 1);

    reg->client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (reg->client_fd < 0) {
        CE_LOG_ERROR("REGISTRY", "connect: socket failed");
        free(reg);
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(reg->client_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CE_LOG_ERROR("REGISTRY", "connect: failed to %s:%d", host, port);
        close(reg->client_fd);
        free(reg);
        return NULL;
    }

    CE_LOG_INFO("REGISTRY", "Connected to %s:%d", host, port);
    return reg;
}

void ce_registry_destroy(CeServiceRegistry* reg) {
    if (!reg) return;

    if (reg->client_fd >= 0) close(reg->client_fd);
    if (reg->listen_fd >= 0) close(reg->listen_fd);

    /* 释放服务条目 */
    CeRegEntry* e = reg->entries;
    while (e) {
        CeRegEntry* next = e->next;
        free(e);
        e = next;
    }

    free(reg);
}

CeResult ce_registry_register(CeServiceRegistry* reg,
                                const char* name,
                                const char* host, int port,
                                const char* metadata) {
    if (!reg || !name || !host) return CE_ERR;

    char cmd[512];
    if (metadata && metadata[0]) {
        snprintf(cmd, sizeof(cmd), "REGISTER %s %s %d %s", name, host, port, metadata);
    } else {
        snprintf(cmd, sizeof(cmd), "REGISTER %s %s %d", name, host, port);
    }

    if (write_line(reg->client_fd, cmd) < 0) return CE_ERR;

    char resp[256];
    if (read_line(reg->client_fd, resp, sizeof(resp)) <= 0) return CE_ERR;

    if (strncmp(resp, "OK", 2) == 0) {
        return CE_OK;
    }
    CE_LOG_ERROR("REGISTRY", "register failed: %s", resp);
    return CE_ERR;
}

CeResult ce_registry_lookup(CeServiceRegistry* reg,
                              const char* name,
                              char* out_host, int* out_port) {
    if (!reg || !name || !out_host || !out_port) return CE_ERR;

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "LOOKUP %s", name);

    if (write_line(reg->client_fd, cmd) < 0) return CE_ERR;

    char resp[256];
    if (read_line(reg->client_fd, resp, sizeof(resp)) <= 0) return CE_ERR;

    if (strncmp(resp, "OK ", 3) == 0) {
        char meta[256];
        meta[0] = '\0';
        int n = sscanf(resp + 3, "%63s %d %255[^\n]", out_host, out_port, meta);
        if (n >= 2) {
            return CE_OK;
        }
    }

    return CE_ERR;
}

CeResult ce_registry_deregister(CeServiceRegistry* reg, const char* name) {
    if (!reg || !name) return CE_ERR;

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "DEREGISTER %s", name);

    if (write_line(reg->client_fd, cmd) < 0) return CE_ERR;

    char resp[256];
    if (read_line(reg->client_fd, resp, sizeof(resp)) <= 0) return CE_ERR;

    return (strncmp(resp, "OK", 2) == 0) ? CE_OK : CE_ERR;
}

int ce_registry_service_count(CeServiceRegistry* reg) {
    if (!reg || !reg->is_server) return 0;
    int count = 0;
    CeRegEntry* e = reg->entries;
    while (e) { count++; e = e->next; }
    return count;
}
