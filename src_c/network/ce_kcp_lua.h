/*
 * ChaosEngine KCP Lua 绑定 — 头文件
 * 纯 C99
 */

#ifndef CE_KCP_LUA_H
#define CE_KCP_LUA_H

#include <lua.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 注册 gateway.kcp 模块到 Lua VM */
int luaopen_gateway_kcp(lua_State* L);

#ifdef __cplusplus
}
#endif

#endif /* CE_KCP_LUA_H */
