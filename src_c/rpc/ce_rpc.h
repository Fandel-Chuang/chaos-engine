/*
 * ChaosEngine 协程化 RPC 框架 - 头文件
 *
 * 支持协程模式（同步写法异步执行）和异步回调模式。
 * 纯 C99，ce_ 前缀。
 */

#ifndef CE_RPC_H
#define CE_RPC_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- RPC 协议 ---- */

/*
 * 消息格式: [4B total_len][2B msg_type][4B call_id][2B method_len][method][payload]
 *
 * msg_type:
 *   0x01 = REQUEST
 *   0x02 = RESPONSE
 *   0x03 = ERROR
 */

#define CE_RPC_MSG_REQUEST  0x01
#define CE_RPC_MSG_RESPONSE 0x02
#define CE_RPC_MSG_ERROR    0x03

#define CE_RPC_HEADER_SIZE  12  /* 4 + 2 + 4 + 2 */
#define CE_RPC_MAX_METHOD   64
#define CE_RPC_MAX_PAYLOAD  (64 * 1024)

/* ---- RPC 服务端 ---- */

typedef struct CeRpcServer CeRpcServer;

/** RPC 处理函数签名 */
typedef CeResult (*CeRpcHandlerFn)(const uint8_t* req, uint32_t req_len,
                                     uint8_t** resp, uint32_t* resp_len);

/**
 * 创建 RPC 服务端
 *
 * @param service_name 服务名
 * @param port         监听端口
 */
CeRpcServer* ce_rpc_server_create(const char* service_name, int port);

/** 注册 RPC 方法 */
CeResult ce_rpc_register(CeRpcServer* srv, const char* method, CeRpcHandlerFn handler);

/** 运行 RPC 服务（阻塞） */
CeResult ce_rpc_server_run(CeRpcServer* srv);

/** 销毁 */
void ce_rpc_server_destroy(CeRpcServer* srv);

/* ---- RPC 客户端 ---- */

typedef struct CeRpcClient CeRpcClient;

/** 创建 RPC 客户端 */
CeRpcClient* ce_rpc_client_create(void);

/** 销毁 */
void ce_rpc_client_destroy(CeRpcClient* cli);

/**
 * 同步 RPC 调用（阻塞，内部用 TCP）
 *
 * @param cli       客户端
 * @param host      目标地址
 * @param port      目标端口
 * @param method    方法名
 * @param req       请求数据
 * @param req_len   请求长度
 * @param resp      输出响应（调用方 free）
 * @param resp_len  输出响应长度
 */
CeResult ce_rpc_call(CeRpcClient* cli,
                      const char* host, int port,
                      const char* method,
                      const uint8_t* req, uint32_t req_len,
                      uint8_t** resp, uint32_t* resp_len);

/** 异步 RPC 回调函数 */
typedef void (*CeRpcCallbackFn)(CeResult result,
                                  const uint8_t* resp, uint32_t resp_len,
                                  void* user_data);

/**
 * 异步 RPC 调用（非阻塞，通过回调返回结果）
 */
CeResult ce_rpc_call_async(CeRpcClient* cli,
                             const char* host, int port,
                             const char* method,
                             const uint8_t* req, uint32_t req_len,
                             CeRpcCallbackFn callback, void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* CE_RPC_H */
