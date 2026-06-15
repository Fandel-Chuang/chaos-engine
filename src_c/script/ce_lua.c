/*
 * ChaosEngine Lua 运行时绑定 — 实现
 * 纯 C99，封装 Lua 5.4 VM
 */

#include "script/ce_lua.h"
#include "public_api/chaos_engine.h"
#include "public_api/ce_ecs.h"
#include "public_api/ce_log.h"
#include "core/ce_memory.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- 内部状态 ---- */

static struct {
    lua_State*   L;
    CeAllocator* allocator;
    CeBool       initialized;
} g_lua;

/* ============================================================
 * 绑定到 Lua 的 C 函数
 * ============================================================ */

/** entity_create() → 创建一个新实体，返回实体 ID */
static int l_entity_create(lua_State* L) {
    CeEntity e = ce_entity_create();
    lua_pushinteger(L, (lua_Integer)e);
    return 1;
}

/** entity_destroy(id) → 销毁实体 */
static int l_entity_destroy(lua_State* L) {
    CeEntity e = (CeEntity)luaL_checkinteger(L, 1);
    ce_entity_destroy(e);
    return 0;
}

/** entity_add_component(id, comp_name) → 为实体添加组件（通过名称查找组件 ID） */
static int l_entity_add_component(lua_State* L) {
    CeEntity e = (CeEntity)luaL_checkinteger(L, 1);
    const char* comp_name = luaL_checkstring(L, 2);

    CeComponentId comp_id = ce_component_find(comp_name);
    if (comp_id == (CeComponentId)-1) {
        /* 组件未注册，尝试自动注册（默认大小和对齐） */
        comp_id = ce_component_register(comp_name, 64, 8);
        if (comp_id == (CeComponentId)-1) {
            lua_pushnil(L);
            lua_pushstring(L, "component registration failed (max components reached)");
            return 2;
        }
    }

    void* comp = ce_entity_add_component(e, comp_id);
    if (comp) {
        lua_pushboolean(L, 1);
        return 1;
    }

    lua_pushnil(L);
    lua_pushstring(L, "entity_add_component failed");
    return 2;
}

/** log_info(msg) → 输出 INFO 级别日志 */
static int l_log_info(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    CE_LOG_INFO("LUA", "%s", msg);
    return 0;
}

/** log_warn(msg) → 输出 WARN 级别日志 */
static int l_log_warn(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    CE_LOG_WARN("LUA", "%s", msg);
    return 0;
}

/** log_error(msg) → 输出 ERROR 级别日志 */
static int l_log_error(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    CE_LOG_ERROR("LUA", "%s", msg);
    return 0;
}

/** get_delta_time() → 返回上一帧的 delta time（秒） */
static int l_get_delta_time(lua_State* L) {
    double dt = ce_time_get_delta();
    lua_pushnumber(L, dt);
    return 1;
}

/** get_total_time() → 返回引擎总运行时间（秒） */
static int l_get_total_time(lua_State* L) {
    double t = ce_time_get_total();
    lua_pushnumber(L, t);
    return 1;
}

/** entity_is_alive(id) → 检查实体是否存活 */
static int l_entity_is_alive(lua_State* L) {
    CeEntity e = (CeEntity)luaL_checkinteger(L, 1);
    CeBool alive = ce_entity_is_alive(e);
    lua_pushboolean(L, alive);
    return 1;
}

/** entity_get_component(id, comp_name) → 获取组件指针（以 lightuserdata 返回） */
static int l_entity_get_component(lua_State* L) {
    CeEntity e = (CeEntity)luaL_checkinteger(L, 1);
    const char* comp_name = luaL_checkstring(L, 2);

    CeComponentId comp_id = ce_component_find(comp_name);
    if (comp_id == (CeComponentId)-1) {
        lua_pushnil(L);
        return 1;
    }

    const void* comp = ce_entity_get_component(e, comp_id);
    if (comp) {
        lua_pushlightuserdata(L, (void*)comp);
        return 1;
    }

    lua_pushnil(L);
    return 1;
}

/* ---- 注册表 ---- */

static const luaL_Reg chaos_lib[] = {
    {"entity_create",        l_entity_create},
    {"entity_destroy",       l_entity_destroy},
    {"entity_add_component", l_entity_add_component},
    {"entity_is_alive",      l_entity_is_alive},
    {"entity_get_component", l_entity_get_component},
    {"log_info",             l_log_info},
    {"log_warn",             l_log_warn},
    {"log_error",            l_log_error},
    {"get_delta_time",       l_get_delta_time},
    {"get_total_time",       l_get_total_time},
    {NULL, NULL}
};

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

CeResult ce_lua_init(CeAllocator* alloc) {
    if (g_lua.initialized) return CE_OK;

    memset(&g_lua, 0, sizeof(g_lua));
    g_lua.allocator = alloc;

    /* 创建 Lua VM */
    g_lua.L = luaL_newstate();
    if (!g_lua.L) {
        CE_LOG_ERROR("LUA", "Failed to create Lua state");
        return CE_ERR;
    }

    /* 加载标准库 */
    luaL_openlibs(g_lua.L);

    /* 注册 ChaosEngine 绑定函数到全局表 */
    lua_getglobal(g_lua.L, "_G");
    luaL_setfuncs(g_lua.L, chaos_lib, 0);
    lua_pop(g_lua.L, 1);

    g_lua.initialized = CE_TRUE;
    CE_LOG_INFO("LUA", "Lua VM initialized (Lua %s)", LUA_VERSION);
    return CE_OK;
}

void ce_lua_shutdown(void) {
    if (!g_lua.initialized) return;

    if (g_lua.L) {
        lua_close(g_lua.L);
        g_lua.L = NULL;
    }

    memset(&g_lua, 0, sizeof(g_lua));
    CE_LOG_INFO("LUA", "Lua VM shut down");
}

CeResult ce_lua_dofile(const char* path) {
    if (!g_lua.initialized || !g_lua.L) return CE_ERR;

    int err = luaL_dofile(g_lua.L, path);
    if (err != LUA_OK) {
        const char* msg = lua_tostring(g_lua.L, -1);
        CE_LOG_ERROR("LUA", "Error executing '%s': %s", path, msg ? msg : "unknown error");
        lua_pop(g_lua.L, 1);
        return CE_ERR;
    }

    return CE_OK;
}

CeResult ce_lua_dostring(const char* code) {
    if (!g_lua.initialized || !g_lua.L) return CE_ERR;

    int err = luaL_dostring(g_lua.L, code);
    if (err != LUA_OK) {
        const char* msg = lua_tostring(g_lua.L, -1);
        CE_LOG_ERROR("LUA", "Error executing string: %s", msg ? msg : "unknown error");
        lua_pop(g_lua.L, 1);
        return CE_ERR;
    }

    return CE_OK;
}

CeResult ce_lua_call(const char* func_name, int nargs, int nresults) {
    if (!g_lua.initialized || !g_lua.L) return CE_ERR;

    lua_getglobal(g_lua.L, func_name);
    if (!lua_isfunction(g_lua.L, -1)) {
        CE_LOG_ERROR("LUA", "Function '%s' not found", func_name);
        lua_pop(g_lua.L, 1);
        return CE_ERR;
    }

    /* 将函数移到参数前面 */
    if (nargs > 0) {
        lua_insert(g_lua.L, -(nargs + 1));
    }

    int err = lua_pcall(g_lua.L, nargs, nresults, 0);
    if (err != LUA_OK) {
        const char* msg = lua_tostring(g_lua.L, -1);
        CE_LOG_ERROR("LUA", "Error calling '%s': %s", func_name, msg ? msg : "unknown error");
        lua_pop(g_lua.L, 1);
        return CE_ERR;
    }

    return CE_OK;
}

void ce_lua_register(const char* name, void* c_func) {
    if (!g_lua.initialized || !g_lua.L) return;

    /* Use a union to avoid -Wpedantic warning about object-to-function pointer cast */
    union { void* v; lua_CFunction f; } u;
    u.v = c_func;
    lua_pushcfunction(g_lua.L, u.f);
    lua_setglobal(g_lua.L, name);
}

void* ce_lua_get_state(void) {
    return g_lua.L;
}
