/*
 * ChaosEngine 协程化 RPC 框架 - 实现
 *
 * 同步 TCP 实现（先跑通逻辑，后续可替换为 io_uring + 协程）。
 * 纯 C99。
 */

#define _POSIX_C_SOURCE 200112L

#include "rpc/ce_rpc.h"
#include "public_api/ce_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

/* ---- 内部结构 ---- */

/** RPC 方法注册项 */
typedef struct CeRpcMethod {
    char           method[CE_RPC_MAX_METHOD];
    CeRpcHandlerFn handler;
    struct CeRpcMethod* next;
} CeRpcMethod;

struct CeRpcServer {
    char          service_name[64];
    int           listen_fd;
    int           port;
    int           running;
    CeRpcMethod*  methods;
};

struct CeRpcClient {
    int placeholder;  /* 客户端无状态，每次调用创建新连接 */
};

/* ---- 协议编解码 ---- */

/** 大端写入 16 位 */
static void write_u16_be(uint8_t* buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xFF);
}

/** 大端写入 32 位 */
static void write_u32_be(uint8_t* buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val & 0xFF);
}

/** 大端读取 16 位 */
static uint16_t read_u16_be(const uint8_t* buf) {
    return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}

/** 大端读取 32 位 */
static uint32_t read_u32_be(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

/** 完整读取 n 字节 */
static int recv_all(int fd, uint8_t* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = recv(fd, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return total;
}

/** 完整发送 n 字节 */
static int send_all(int fd, const uint8_t* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = send(fd, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return total;
}

/* ---- RPC 服务端 ---- */

CeRpcServer* ce_rpc_server_create(const char* service_name, int port) {
    if (!service_name || port <= 0) return NULL;

    CeRpcServer* srv = (CeRpcServer*)calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    strncpy(srv->service_name, service_name, sizeof(srv->service_name) - 1);
    srv->port = port;
    srv->methods = NULL;
    srv->running = 0;

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        CE_LOG_ERROR("RPC", "server_create: socket failed");
        free(srv);
        return NULL;
    }

    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(srv->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CE_LOG_ERROR("RPC", "server_create: bind failed port=%d", port);
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }

    if (listen(srv->listen_fd, 64) < 0) {
        CE_LOG_ERROR("RPC", "server_create: listen failed");
        close(srv->listen_fd);
        free(srv);
        return NULL;
    }

    CE_LOG_INFO("RPC", "Server '%s' listening on port %d", service_name, port);
    return srv;
}

CeResult ce_rpc_register(CeRpcServer* srv, const char* method, CeRpcHandlerFn handler) {
    if (!srv || !method || !handler) return CE_ERR;

    CeRpcMethod* m = (CeRpcMethod*)calloc(1, sizeof(*m));
    if (!m) return CE_ERR;

    strncpy(m->method, method, sizeof(m->method) - 1);
    m->handler = handler;
    m->next = srv->methods;
    srv->methods = m;

    CE_LOG_INFO("RPC", "Method '%s' registered on '%s'", method, srv->service_name);
    return CE_OK;
}

CeResult ce_rpc_server_run(CeRpcServer* srv) {
    if (!srv) return CE_ERR;
    srv->running = 1;

    CE_LOG_INFO("RPC", "Server '%s' running...", srv->service_name);

    while (srv->running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(srv->listen_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) continue;

        /* 读取消息头 */
        uint8_t header[CE_RPC_HEADER_SIZE];
        if (recv_all(client_fd, header, CE_RPC_HEADER_SIZE) < 0) {
            close(client_fd);
            continue;
        }

        uint32_t total_len = read_u32_be(header);
        uint16_t msg_type = read_u16_be(header + 4);
        uint32_t call_id = read_u32_be(header + 8);
        uint16_t method_len = read_u16_be(header + 12);

        (void)call_id;

        if (msg_type != CE_RPC_MSG_REQUEST || total_len < CE_RPC_HEADER_SIZE) {
            close(client_fd);
            continue;
        }

        /* 读取方法名 */
        char method[CE_RPC_MAX_METHOD] = {0};
        if (method_len > 0 && method_len < CE_RPC_MAX_METHOD) {
            if (recv_all(client_fd, (uint8_t*)method, method_len) < 0) {
                close(client_fd);
                continue;
            }
            method[method_len] = '\0';
        }

        /* 读取 payload */
        int payload_len = (int)total_len - CE_RPC_HEADER_SIZE - method_len;
        uint8_t* payload = NULL;
        if (payload_len > 0 && payload_len < CE_RPC_MAX_PAYLOAD) {
            payload = (uint8_t*)malloc(payload_len);
            if (payload && recv_all(client_fd, payload, payload_len) < 0) {
                free(payload);
                payload = NULL;
                payload_len = 0;
            }
        }

        /* 查找处理函数 */
        CeRpcMethod* m = srv->methods;
        CeRpcHandlerFn handler = NULL;
        while (m) {
            if (strcmp(m->method, method) == 0) {
                handler = m->handler;
                break;
            }
            m = m->next;
        }

        if (!handler) {
            /* 方法不存在，返回 ERROR */
            uint8_t err_resp[CE_RPC_HEADER_SIZE];
            write_u32_be(err_resp, CE_RPC_HEADER_SIZE);
            write_u16_be(err_resp + 4, CE_RPC_MSG_ERROR);
            write_u32_be(err_resp + 8, call_id);
            write_u16_be(err_resp + 12, 0);
            send_all(client_fd, err_resp, CE_RPC_HEADER_SIZE);
            CE_LOG_WARN("RPC", "Method '%s' not found", method);
        } else {
            /* 调用处理函数 */
            uint8_t* resp_data = NULL;
            uint32_t resp_len = 0;
            CeResult result = handler(payload, (uint32_t)payload_len, &resp_data, &resp_len);

            /* 构造响应 */
            uint16_t resp_method_len = 0;  /* 响应不带方法名 */
            uint32_t resp_total = CE_RPC_HEADER_SIZE + resp_len;

            uint8_t* resp_buf = (uint8_t*)malloc(resp_total);
            if (resp_buf) {
                write_u32_be(resp_buf, resp_total);
                if (result == CE_OK) {
                    write_u16_be(resp_buf + 4, CE_RPC_MSG_RESPONSE);
                } else {
                    write_u16_be(resp_buf + 4, CE_RPC_MSG_ERROR);
                }
                write_u32_be(resp_buf + 8, call_id);
                write_u16_be(resp_buf + 12, resp_method_len);
                if (resp_data && resp_len > 0) {
                    memcpy(resp_buf + CE_RPC_HEADER_SIZE, resp_data, resp_len);
                }
                send_all(client_fd, resp_buf, (int)resp_total);
                free(resp_buf);
            }

            if (resp_data) free(resp_data);
        }

        if (payload) free(payload);
        close(client_fd);
    }

    return CE_OK;
}

void ce_rpc_server_destroy(CeRpcServer* srv) {
    if (!srv) return;
    if (srv->listen_fd >= 0) close(srv->listen_fd);

    CeRpcMethod* m = srv->methods;
    while (m) {
        CeRpcMethod* next = m->next;
        free(m);
        m = next;
    }
    free(srv);
}

/* ---- RPC 客户端 ---- */

CeRpcClient* ce_rpc_client_create(void) {
    CeRpcClient* cli = (CeRpcClient*)calloc(1, sizeof(*cli));
    return cli;
}

void ce_rpc_client_destroy(CeRpcClient* cli) {
    if (cli) free(cli);
}

CeResult ce_rpc_call(CeRpcClient* cli,
                      const char* host, int port,
                      const char* method,
                      const uint8_t* req, uint32_t req_len,
                      uint8_t** resp, uint32_t* resp_len) {
    if (!cli || !host || !method) return CE_ERR;
    if (resp) *resp = NULL;
    if (resp_len) *resp_len = 0;

    (void)cli;  /* 客户端无状态 */

    /* 创建 TCP 连接 */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return CE_ERR;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CE_LOG_ERROR("RPC", "call: connect failed to %s:%d", host, port);
        close(fd);
        return CE_ERR;
    }

    /* 构造请求 */
    uint16_t method_len = (uint16_t)strlen(method);
    uint32_t total_len = CE_RPC_HEADER_SIZE + method_len + req_len;

    uint8_t* buf = (uint8_t*)malloc(total_len);
    if (!buf) {
        close(fd);
        return CE_ERR;
    }

    write_u32_be(buf, total_len);
    write_u16_be(buf + 4, CE_RPC_MSG_REQUEST);
    write_u32_be(buf + 8, 1);  /* call_id = 1 (同步模式) */
    write_u16_be(buf + 12, method_len);
    memcpy(buf + CE_RPC_HEADER_SIZE, method, method_len);
    if (req && req_len > 0) {
        memcpy(buf + CE_RPC_HEADER_SIZE + method_len, req, req_len);
    }

    /* 发送请求 */
    if (send_all(fd, buf, (int)total_len) < 0) {
        free(buf);
        close(fd);
        return CE_ERR;
    }
    free(buf);

    /* 读取响应头 */
    uint8_t header[CE_RPC_HEADER_SIZE];
    if (recv_all(fd, header, CE_RPC_HEADER_SIZE) < 0) {
        close(fd);
        return CE_ERR;
    }

    uint32_t resp_total = read_u32_be(header);
    uint16_t resp_type = read_u16_be(header + 4);
    /* uint32_t resp_call_id = read_u32_be(header + 8); */
    uint16_t resp_method_len = read_u16_be(header + 12);

    /* 跳过方法名（响应中通常为 0） */
    if (resp_method_len > 0) {
        uint8_t tmp[CE_RPC_MAX_METHOD];
        if (resp_method_len < CE_RPC_MAX_METHOD) {
            recv_all(fd, tmp, resp_method_len);
        }
    }

    /* 读取 payload */
    int payload_len = (int)resp_total - CE_RPC_HEADER_SIZE - resp_method_len;
    if (payload_len > 0 && resp && resp_len) {
        *resp = (uint8_t*)malloc(payload_len);
        if (*resp) {
            recv_all(fd, *resp, payload_len);
            *resp_len = (uint32_t)payload_len;
        }
    }

    close(fd);

    return (resp_type == CE_RPC_MSG_RESPONSE) ? CE_OK : CE_ERR;
}

/* ---- 异步 RPC ---- */

typedef struct CeRpcAsyncArg {
    CeRpcClient*  cli;
    char          host[64];
    int           port;
    char          method[CE_RPC_MAX_METHOD];
    uint8_t*      req;
    uint32_t      req_len;
    CeRpcCallbackFn callback;
    void*         user_data;
} CeRpcAsyncArg;

static void* async_thread_fn(void* arg) {
    CeRpcAsyncArg* a = (CeRpcAsyncArg*)arg;

    uint8_t* resp = NULL;
    uint32_t resp_len = 0;
    CeResult result = ce_rpc_call(a->cli, a->host, a->port, a->method,
                                    a->req, a->req_len, &resp, &resp_len);

    if (a->callback) {
        a->callback(result, resp, resp_len, a->user_data);
    }

    if (resp) free(resp);
    if (a->req) free(a->req);
    free(a);

    return NULL;
}

CeResult ce_rpc_call_async(CeRpcClient* cli,
                             const char* host, int port,
                             const char* method,
                             const uint8_t* req, uint32_t req_len,
                             CeRpcCallbackFn callback, void* user_data) {
    if (!cli || !host || !method) return CE_ERR;

    CeRpcAsyncArg* arg = (CeRpcAsyncArg*)calloc(1, sizeof(*arg));
    if (!arg) return CE_ERR;

    arg->cli = cli;
    strncpy(arg->host, host, sizeof(arg->host) - 1);
    arg->port = port;
    strncpy(arg->method, method, sizeof(arg->method) - 1);
    arg->callback = callback;
    arg->user_data = user_data;

    if (req && req_len > 0) {
        arg->req = (uint8_t*)malloc(req_len);
        if (arg->req) {
            memcpy(arg->req, req, req_len);
            arg->req_len = req_len;
        }
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, async_thread_fn, arg) != 0) {
        free(arg->req);
        free(arg);
        return CE_ERR;
    }
    pthread_detach(tid);

    return CE_OK;
}
