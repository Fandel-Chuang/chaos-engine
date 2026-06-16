/*
 * ChaosEngine Admin IPC — TCP / Unix Socket JSON-RPC 2.0 Server
 * 纯 C99，单连接模式，独立线程
 *
 * 智能传输层：
 *   127.0.0.1 → Unix Domain Socket (/tmp/chaos_admin.sock)
 *   其他地址   → TCP Socket
 */

#ifndef CE_ADMIN_IPC_H
#define CE_ADMIN_IPC_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CeAdminIpc CeAdminIpc;

/** Admin IPC 自定义方法处理器。
 *  接收 JSON-RPC 请求的 method/params/id，将 JSON 响应写入 buf。
 *  返回写入的字节数，-1 表示内部错误。
 *  user_data 为注册时传入的自定义数据。 */
typedef int (*CeAdminIpcHandler)(const char* method, const char* params_json,
                                  const char* id_str, char* buf, int max_len,
                                  void* user_data);

/** 注册自定义方法处理器。
 *  多个模块可注册各自的 handler，按注册顺序依次尝试。
 *  返回 CE_OK 成功，CE_ERR 失败。 */
CeResult ce_admin_ipc_register_handler(CeAdminIpc* ipc,
                                        CeAdminIpcHandler handler,
                                        void* user_data);

/** 启动 Admin IPC 服务端。
 *  addr 格式: "host:port"，如 "127.0.0.1:9091" 或 "0.0.0.0:9091"
 *  127.0.0.1 → Unix Socket，其他 → TCP */
CeAdminIpc* ce_admin_ipc_start(const char* addr);

void ce_admin_ipc_stop(CeAdminIpc* ipc);
CeBool ce_admin_ipc_is_running(CeAdminIpc* ipc);

#ifdef __cplusplus
}
#endif

#endif /* CE_ADMIN_IPC_H */
