/*
 * ChaosEngine ce_net_base Lua 绑定 — 头文件
 * 纯 C99
 */

#ifndef CE_NET_BASE_LUA_H
#define CE_NET_BASE_LUA_H

#include <lua.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 注册 gateway.net_base 模块到 Lua VM */
int luaopen_gateway_net_base(lua_State* L);

#ifdef __cplusplus
}
#endif

#endif /* CE_NET_BASE_LUA_H */
