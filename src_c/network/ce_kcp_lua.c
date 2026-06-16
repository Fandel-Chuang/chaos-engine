/*
 * ChaosEngine KCP Lua 绑定
 * 纯 C99，使用 Lua 5.4 C API（最小头文件兼容）
 * 暴露 gateway.kcp 模块给 Lua
 */

#include "network/ce_kcp.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <string.h>

/* ---- KCP Lua userdata 结构 ---- */

typedef struct CeKcpLuaUserdata {
    CeKcpContext* kcp;       /* KCP 上下文 */
    int           cb_ref;    /* 输出回调 Lua 函数引用（LUA_NOREF 表示未设置） */
} CeKcpLuaUserdata;

/* ---- 辅助：从 userdata 获取 CeKcpLuaUserdata* ---- */

static CeKcpLuaUserdata* ce_kcp_lua_check(lua_State* L, int idx) {
    return (CeKcpLuaUserdata*)luaL_checkudata(L, idx, "gateway.kcp");
}

/* ---- 注册表键（用于存储输出回调） ---- */

static int kcp_output_key = 0;  /* 用作注册表键的静态变量地址 */

/* ---- 输出回调的 Lua 桥接 ---- */

static int ce_kcp_lua_output(const char* buf, int len, void* user_data) {
    lua_State* L = (lua_State*)user_data;
    if (!L) return 0;

    /* 从注册表获取回调函数引用 */
    lua_pushlightuserdata(L, (void*)&kcp_output_key);
    lua_rawget(L, LUA_REGISTRYINDEX);

    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }

    /* 推送数据字符串 */
    lua_pushlstring(L, buf, (size_t)len);

    /* 调用回调函数 */
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return -1;
    }

    /* 获取返回值（发送字节数） */
    int sent = 0;
    if (lua_isinteger(L, -1)) {
        sent = (int)lua_tointeger(L, -1);
    } else if (lua_isnumber(L, -1)) {
        sent = (int)lua_tonumber(L, -1);
    }
    lua_pop(L, 1);

    return sent;
}

/* ---- kcp.create(conv, user_data) ---- */

static int l_kcp_create(lua_State* L) {
    uint32_t conv = (uint32_t)luaL_checkinteger(L, 1);

    /* user_data 可选 */
    void* user_data = NULL;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        user_data = (void*)lua_touserdata(L, 2);
    }

    CeKcpContext* kcp = ce_kcp_create(conv, user_data);
    if (!kcp) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create KCP context");
        return 2;
    }

    /* 创建 userdata 包装 */
    CeKcpLuaUserdata* ud = (CeKcpLuaUserdata*)lua_newuserdata(L, sizeof(CeKcpLuaUserdata));
    ud->kcp    = kcp;
    ud->cb_ref = LUA_NOREF;

    /* 设置 metatable */
    luaL_getmetatable(L, "gateway.kcp");
    lua_setmetatable(L, -2);

    return 1;
}

/* ---- kcp.destroy(ctx) ---- */

static int l_kcp_destroy(lua_State* L) {
    CeKcpLuaUserdata* ud = ce_kcp_lua_check(L, 1);
    if (ud->kcp) {
        /* 释放回调引用 */
        if (ud->cb_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_ref);
            ud->cb_ref = LUA_NOREF;
        }
        ce_kcp_destroy(ud->kcp);
        ud->kcp = NULL;
    }
    return 0;
}

/* ---- kcp.input(ctx, data) ---- */

static int l_kcp_input(lua_State* L) {
    CeKcpLuaUserdata* ud = ce_kcp_lua_check(L, 1);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);

    int ret = ce_kcp_input(ud->kcp, data, (int)len);
    lua_pushinteger(L, ret);
    return 1;
}

/* ---- kcp.recv(ctx, max_len) ---- */

static int l_kcp_recv(lua_State* L) {
    CeKcpLuaUserdata* ud = ce_kcp_lua_check(L, 1);
    int max_len = (int)luaL_checkinteger(L, 2);

    if (max_len <= 0) {
        lua_pushnil(L);
        lua_pushstring(L, "max_len must be positive");
        return 2;
    }

    char* buf = (char*)malloc((size_t)max_len);
    if (!buf) {
        lua_pushnil(L);
        lua_pushstring(L, "memory allocation failed");
        return 2;
    }

    int ret = ce_kcp_recv(ud->kcp, buf, max_len);
    if (ret < 0) {
        free(buf);
        lua_pushnil(L);
        return 1;
    }

    lua_pushlstring(L, buf, (size_t)ret);
    free(buf);
    return 1;
}

/* ---- kcp.send(ctx, data) ---- */

static int l_kcp_send(lua_State* L) {
    CeKcpLuaUserdata* ud = ce_kcp_lua_check(L, 1);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);

    int ret = ce_kcp_send(ud->kcp, data, (int)len);
    lua_pushinteger(L, ret);
    return 1;
}

/* ---- kcp.update(ctx, current_ms) ---- */

static int l_kcp_update(lua_State* L) {
    CeKcpLuaUserdata* ud = ce_kcp_lua_check(L, 1);
    uint32_t current_ms = (uint32_t)luaL_checkinteger(L, 2);

    ce_kcp_update(ud->kcp, current_ms);
    return 0;
}

/* ---- kcp.check(ctx, current_ms) ---- */

static int l_kcp_check(lua_State* L) {
    CeKcpLuaUserdata* ud = ce_kcp_lua_check(L, 1);
    uint32_t current_ms = (uint32_t)luaL_checkinteger(L, 2);

    uint32_t next = ce_kcp_check(ud->kcp, current_ms);
    lua_pushinteger(L, (lua_Integer)next);
    return 1;
}

/* ---- kcp.set_config(ctx, nodelay, interval, resend, nc) ---- */

static int l_kcp_set_config(lua_State* L) {
    CeKcpLuaUserdata* ud = ce_kcp_lua_check(L, 1);
    int nodelay  = (int)luaL_checkinteger(L, 2);
    int interval = (int)luaL_checkinteger(L, 3);
    int resend   = (int)luaL_checkinteger(L, 4);
    int nc       = (int)luaL_checkinteger(L, 5);

    int ret = ce_kcp_set_config(ud->kcp, nodelay, interval, resend, nc);
    lua_pushinteger(L, ret);
    return 1;
}

/* ---- kcp.set_wndsize(ctx, sndwnd, rcvwnd) ---- */

static int l_kcp_set_wndsize(lua_State* L) {
    CeKcpLuaUserdata* ud = ce_kcp_lua_check(L, 1);
    int sndwnd = (int)luaL_checkinteger(L, 2);
    int rcvwnd = (int)luaL_checkinteger(L, 3);

    int ret = ce_kcp_set_wndsize(ud->kcp, sndwnd, rcvwnd);
    lua_pushinteger(L, ret);
    return 1;
}

/* ---- kcp.set_mtu(ctx, mtu) ---- */

static int l_kcp_set_mtu(lua_State* L) {
    CeKcpLuaUserdata* ud = ce_kcp_lua_check(L, 1);
    int mtu = (int)luaL_checkinteger(L, 2);

    int ret = ce_kcp_set_mtu(ud->kcp, mtu);
    lua_pushinteger(L, ret);
    return 1;
}

/* ---- kcp.set_output(ctx, callback_fn) ---- */

static int l_kcp_set_output(lua_State* L) {
    CeKcpLuaUserdata* ud = ce_kcp_lua_check(L, 1);

    if (!lua_isfunction(L, 2)) {
        lua_pushnil(L);
        lua_pushstring(L, "callback must be a function");
        return 2;
    }

    /* 释放旧的回调引用 */
    if (ud->cb_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_ref);
        ud->cb_ref = LUA_NOREF;
    }

    /* 保存回调函数到注册表 */
    lua_pushvalue(L, 2);
    ud->cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    /* 设置输出回调：user_data 传 L，回调通过注册表查找函数 */
    ce_kcp_set_output_callback(ud->kcp, ce_kcp_lua_output);

    /* 存储 L 到注册表以便回调使用 */
    lua_pushlightuserdata(L, (void*)&kcp_output_key);
    lua_pushvalue(L, 2);
    lua_rawset(L, LUA_REGISTRYINDEX);

    lua_pushboolean(L, 1);
    return 1;
}

/* ---- kcp.get_user_data(ctx) ---- */

static int l_kcp_get_user_data(lua_State* L) {
    CeKcpLuaUserdata* ud = ce_kcp_lua_check(L, 1);
    void* data = ce_kcp_get_user_data(ud->kcp);
    if (data) {
        lua_pushlightuserdata(L, data);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/* ---- __gc 元方法 ---- */

static int l_kcp_gc(lua_State* L) {
    CeKcpLuaUserdata* ud = ce_kcp_lua_check(L, 1);
    if (ud->kcp) {
        /* 释放回调引用 */
        if (ud->cb_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_ref);
            ud->cb_ref = LUA_NOREF;
        }
        ce_kcp_destroy(ud->kcp);
        ud->kcp = NULL;
    }
    return 0;
}

/* ---- 模块注册表 ---- */

static const luaL_Reg kcp_methods[] = {
    {"create",         l_kcp_create},
    {"destroy",        l_kcp_destroy},
    {"input",          l_kcp_input},
    {"recv",           l_kcp_recv},
    {"send",           l_kcp_send},
    {"update",         l_kcp_update},
    {"check",          l_kcp_check},
    {"set_config",     l_kcp_set_config},
    {"set_wndsize",    l_kcp_set_wndsize},
    {"set_mtu",        l_kcp_set_mtu},
    {"set_output",     l_kcp_set_output},
    {"get_user_data",  l_kcp_get_user_data},
    {NULL, NULL}
};

/* ---- luaopen_gateway_kcp ---- */

int luaopen_gateway_kcp(lua_State* L) {
    /* 创建 metatable */
    luaL_newmetatable(L, "gateway.kcp");

    /* __index 指向自身 */
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    /* __gc */
    lua_pushcfunction(L, l_kcp_gc);
    lua_setfield(L, -2, "__gc");

    /* 创建模块表：用 lua_createtable + luaL_setfuncs 代替 luaL_newlib */
    lua_createtable(L, 0, 12);
    luaL_setfuncs(L, kcp_methods, 0);

    return 1;
}
