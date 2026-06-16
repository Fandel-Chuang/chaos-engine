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
