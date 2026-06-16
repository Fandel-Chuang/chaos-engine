/*
 * ChaosEngine ce_rpc_call Lua 绑定 — 头文件
 * 纯 C99
 */

#ifndef CE_RPC_CALL_LUA_H
#define CE_RPC_CALL_LUA_H

#include <lua.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 注册 services.rpc 模块到 Lua VM */
int luaopen_services_rpc(lua_State* L);

#ifdef __cplusplus
}
#endif

#endif /* CE_RPC_CALL_LUA_H */
