/*
 * ChaosEngine ce_rpc_call Lua 绑定
 * 纯 C99，使用 Lua 5.4 C API
 * 暴露 services.rpc 模块给 Lua
 *
 * 提供：
 *   - rpc.init(config) → ctx
 *   - rpc.register(ctx)
 *   - rpc.deregister(ctx)
 *   - rpc.call(ctx, target_service, method, params) → ok
 *   - rpc.recv(ctx) → msg_type, payload | nil, error
 *   - rpc.send_heartbeat(ctx) → ok
 *   - rpc.is_connected(ctx) → bool
 *   - rpc.is_registered(ctx) → bool
 *   - rpc.get_endpoint(ctx) → string
 *   - rpc.shutdown(ctx)
 */

#include "network/ce_rpc_call.h"
#include "network/ce_rpc_call_lua.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <string.h>

/* ================================================================
 * 辅助宏
 * ================================================================ */

#define RPC_CTX_META  "services.rpc.ctx"

/* ================================================================
 * RPC 上下文 userdata 包装
 * ================================================================ */

typedef struct CeRpcCtxUD {
    CeRpcContext* ctx;
} CeRpcCtxUD;

static CeRpcCtxUD* check_rpc_ctx(lua_State* L, int idx) {
    return (CeRpcCtxUD*)luaL_checkudata(L, idx, RPC_CTX_META);
}

/* ================================================================
 * rpc.init(config) → ctx | nil, error
 * config: { router_host, router_port, service_type, service_name,
 *           heartbeat_ms, heartbeat_timeout_ms, timeout_ms }
 * ================================================================ */

static int l_rpc_init(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    CeRpcConfig config;
    memset(&config, 0, sizeof(config));

    /* router_host */
    lua_getfield(L, 1, "router_host");
    if (lua_isstring(L, -1)) {
        config.router_host = lua_tostring(L, -1);
    }
    lua_pop(L, 1);

    /* router_port */
    lua_getfield(L, 1, "router_port");
    if (lua_isinteger(L, -1)) {
        config.router_port = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    /* service_type */
    lua_getfield(L, 1, "service_type");
    if (lua_isinteger(L, -1)) {
        config.service_type = (CeServiceType)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    /* service_name */
    lua_getfield(L, 1, "service_name");
    if (lua_isstring(L, -1)) {
        config.service_name = lua_tostring(L, -1);
    }
    lua_pop(L, 1);

    /* heartbeat_ms */
    lua_getfield(L, 1, "heartbeat_ms");
    if (lua_isinteger(L, -1)) {
        config.heartbeat_ms = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    /* heartbeat_timeout_ms */
    lua_getfield(L, 1, "heartbeat_timeout_ms");
    if (lua_isinteger(L, -1)) {
        config.heartbeat_timeout_ms = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    /* timeout_ms */
    lua_getfield(L, 1, "timeout_ms");
    if (lua_isinteger(L, -1)) {
        config.timeout_ms = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    CeRpcContext* rpc_ctx = ce_rpc_init(&config);
    if (!rpc_ctx) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to initialize RPC context");
        return 2;
    }

    /* 创建 Lua userdata */
    CeRpcCtxUD* ud = (CeRpcCtxUD*)lua_newuserdata(L, sizeof(CeRpcCtxUD));
    ud->ctx = rpc_ctx;

    luaL_getmetatable(L, RPC_CTX_META);
    lua_setmetatable(L, -2);

    return 1;
}

/* ================================================================
 * rpc.register(ctx) → ok
 * ================================================================ */

static int l_rpc_register(lua_State* L) {
    CeRpcCtxUD* ud = check_rpc_ctx(L, 1);
    CeResult r = ce_rpc_register(ud->ctx);
    lua_pushboolean(L, r == CE_OK ? 1 : 0);
    return 1;
}

/* ================================================================
 * rpc.deregister(ctx) → ok
 * ================================================================ */

static int l_rpc_deregister(lua_State* L) {
    CeRpcCtxUD* ud = check_rpc_ctx(L, 1);
    CeResult r = ce_rpc_deregister(ud->ctx);
    lua_pushboolean(L, r == CE_OK ? 1 : 0);
    return 1;
}

/* ================================================================
 * rpc.call(ctx, target_service, method, params) → ok
 * ================================================================ */

static int l_rpc_call(lua_State* L) {
    CeRpcCtxUD* ud = check_rpc_ctx(L, 1);
    CeServiceType target_service = (CeServiceType)luaL_checkinteger(L, 2);
    const char* method = luaL_checkstring(L, 3);

    size_t params_len = 0;
    const char* params = NULL;
    if (lua_gettop(L) >= 4 && lua_isstring(L, 4)) {
        params = lua_tolstring(L, 4, &params_len);
    }

    CeResult r = ce_rpc_call(ud->ctx, target_service, method, params, (uint32_t)params_len);
    lua_pushboolean(L, r == CE_OK ? 1 : 0);
    return 1;
}

/* ================================================================
 * rpc.recv(ctx) → msg_type, payload | nil, error
 * ================================================================ */

static int l_rpc_recv(lua_State* L) {
    CeRpcCtxUD* ud = check_rpc_ctx(L, 1);

    CeRpcResponse response;
    CeResult r = ce_rpc_recv(ud->ctx, &response);

    if (r != CE_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "no message");
        return 2;
    }

    lua_pushinteger(L, response.type);
    if (response.payload_len > 0) {
        lua_pushlstring(L, (const char*)response.payload, response.payload_len);
    } else {
        lua_pushstring(L, "");
    }
    return 2;
}

/* ================================================================
 * rpc.send_heartbeat(ctx) → ok
 * ================================================================ */

static int l_rpc_send_heartbeat(lua_State* L) {
    CeRpcCtxUD* ud = check_rpc_ctx(L, 1);
    CeResult r = ce_rpc_send_heartbeat(ud->ctx);
    lua_pushboolean(L, r == CE_OK ? 1 : 0);
    return 1;
}

/* ================================================================
 * rpc.is_connected(ctx) → bool
 * ================================================================ */

static int l_rpc_is_connected(lua_State* L) {
    CeRpcCtxUD* ud = check_rpc_ctx(L, 1);
    CeBool connected = ce_rpc_is_connected(ud->ctx);
    lua_pushboolean(L, connected == CE_TRUE ? 1 : 0);
    return 1;
}

/* ================================================================
 * rpc.is_registered(ctx) → bool
 * ================================================================ */

static int l_rpc_is_registered(lua_State* L) {
    CeRpcCtxUD* ud = check_rpc_ctx(L, 1);
    CeBool registered = ce_rpc_is_registered(ud->ctx);
    lua_pushboolean(L, registered == CE_TRUE ? 1 : 0);
    return 1;
}

/* ================================================================
 * rpc.get_endpoint(ctx) → string
 * ================================================================ */

static int l_rpc_get_endpoint(lua_State* L) {
    CeRpcCtxUD* ud = check_rpc_ctx(L, 1);
    const char* endpoint = ce_rpc_current_endpoint(ud->ctx);
    lua_pushstring(L, endpoint);
    return 1;
}

/* ================================================================
 * rpc.shutdown(ctx)
 * ================================================================ */

static int l_rpc_shutdown(lua_State* L) {
    CeRpcCtxUD* ud = check_rpc_ctx(L, 1);
    if (ud->ctx) {
        ce_rpc_shutdown(ud->ctx);
        ud->ctx = NULL;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ================================================================
 * ctx __gc 元方法
 * ================================================================ */

static int l_rpc_ctx_gc(lua_State* L) {
    CeRpcCtxUD* ud = check_rpc_ctx(L, 1);
    if (ud->ctx) {
        ce_rpc_shutdown(ud->ctx);
        ud->ctx = NULL;
    }
    return 0;
}

/* ================================================================
 * 模块注册表
 * ================================================================ */

static const luaL_Reg rpc_funcs[] = {
    {"init",           l_rpc_init},
    {"register",       l_rpc_register},
    {"deregister",     l_rpc_deregister},
    {"call",           l_rpc_call},
    {"recv",           l_rpc_recv},
    {"send_heartbeat", l_rpc_send_heartbeat},
    {"is_connected",   l_rpc_is_connected},
    {"is_registered",  l_rpc_is_registered},
    {"get_endpoint",   l_rpc_get_endpoint},
    {"shutdown",       l_rpc_shutdown},
    {NULL, NULL}
};

/* ================================================================
 * luaopen_services_rpc
 * ================================================================ */

int luaopen_services_rpc(lua_State* L) {
    /* 创建 ctx metatable */
    luaL_newmetatable(L, RPC_CTX_META);

    /* ctx.__index = ctx metatable */
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    /* ctx.__gc */
    lua_pushcfunction(L, l_rpc_ctx_gc);
    lua_setfield(L, -2, "__gc");

    lua_pop(L, 1); /* pop metatable */

    /* 创建模块表：用 lua_createtable + luaL_setfuncs */
    lua_createtable(L, 0, 10);
    luaL_setfuncs(L, rpc_funcs, 0);

    return 1;
}
