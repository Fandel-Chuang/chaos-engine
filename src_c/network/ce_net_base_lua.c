/*
 * ChaosEngine ce_net_base Lua 绑定
 * 纯 C99，使用 Lua 5.4 C API
 * 暴露 gateway.net_base 模块给 Lua
 *
 * 提供：
 *   - net_base.connect(config) → conn
 *   - conn:send(msg_type, payload)
 *   - conn:recv() → msg_type, payload
 *   - conn:close()
 *   - conn:get_state()
 *   - conn:send_ping()
 *   - conn:send_pong()
 *   - conn:heartbeat_timeout() → bool
 *   - conn:heartbeat_touch()
 *   - net_base.pack(msg_type, payload) → string
 *   - net_base.unpack(data) → msg_type, payload
 *   - net_base.create_pool(max) → pool
 *   - pool:add(conn)
 *   - pool:acquire() → conn
 *   - pool:release(conn)
 *   - pool:stats() → total, available
 *   - net_base.create_mesh(local_region) → mesh
 *   - mesh:add_region(region)
 *   - mesh:connect_all()
 *   - mesh:send(region_id, msg_type, payload)
 *   - mesh:broadcast(msg_type, payload) → count
 */

#include "network/ce_net_base.h"
#include "network/ce_net_base_lua.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <string.h>

/* ================================================================
 * 辅助宏
 * ================================================================ */

#define NET_BASE_CONN_META  "gateway.net_base.conn"
#define NET_BASE_POOL_META  "gateway.net_base.pool"
#define NET_BASE_MESH_META  "gateway.net_base.mesh"

/* ================================================================
 * 连接 userdata 包装
 * ================================================================ */

typedef struct CeNetBaseConnUD {
    CeNetConnection* conn;
} CeNetBaseConnUD;

static CeNetBaseConnUD* check_conn(lua_State* L, int idx) {
    return (CeNetBaseConnUD*)luaL_checkudata(L, idx, NET_BASE_CONN_META);
}

/* ================================================================
 * net_base.connect(config) → conn
 * config: { host, port, timeout_ms, heartbeat_ms, heartbeat_timeout_ms, auto_reconnect, nonblocking }
 * ================================================================ */

static int l_net_base_connect(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    CeNetConnConfig config;
    memset(&config, 0, sizeof(config));

    /* host */
    lua_getfield(L, 1, "host");
    if (lua_isstring(L, -1)) {
        config.host = lua_tostring(L, -1);
    }
    lua_pop(L, 1);

    /* port */
    lua_getfield(L, 1, "port");
    if (lua_isinteger(L, -1)) {
        config.port = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    /* timeout_ms */
    lua_getfield(L, 1, "timeout_ms");
    if (lua_isinteger(L, -1)) {
        config.timeout_ms = (int)lua_tointeger(L, -1);
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

    /* auto_reconnect */
    lua_getfield(L, 1, "auto_reconnect");
    if (lua_isboolean(L, -1)) {
        config.auto_reconnect = lua_toboolean(L, -1) ? CE_TRUE : CE_FALSE;
    }
    lua_pop(L, 1);

    /* nonblocking */
    lua_getfield(L, 1, "nonblocking");
    if (lua_isboolean(L, -1)) {
        config.nonblocking = lua_toboolean(L, -1) ? CE_TRUE : CE_FALSE;
    }
    lua_pop(L, 1);

    CeNetConnection* conn = ce_net_conn_create(&config);
    if (!conn) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create connection");
        return 2;
    }

    /* 尝试连接 */
    CeResult r = ce_net_conn_connect(conn);
    if (r != CE_OK && !config.auto_reconnect) {
        /* 连接失败且不自动重连，仍返回连接对象（调用者可稍后重试） */
    }

    /* 创建 Lua userdata */
    CeNetBaseConnUD* ud = (CeNetBaseConnUD*)lua_newuserdata(L, sizeof(CeNetBaseConnUD));
    ud->conn = conn;

    luaL_getmetatable(L, NET_BASE_CONN_META);
    lua_setmetatable(L, -2);

    return 1;
}

/* ================================================================
 * conn:send(msg_type, payload)
 * ================================================================ */

static int l_conn_send(lua_State* L) {
    CeNetBaseConnUD* ud = check_conn(L, 1);
    uint16_t msg_type = (uint16_t)luaL_checkinteger(L, 2);

    size_t payload_len = 0;
    const char* payload = NULL;
    if (lua_gettop(L) >= 3 && lua_isstring(L, 3)) {
        payload = lua_tolstring(L, 3, &payload_len);
    }

    CeNetMessage msg;
    msg.type = msg_type;
    msg.payload_len = (uint32_t)payload_len;
    msg.payload = (const uint8_t*)payload;

    CeResult r = ce_net_conn_send(ud->conn, &msg);
    lua_pushboolean(L, r == CE_OK ? 1 : 0);
    return 1;
}

/* ================================================================
 * conn:recv() → msg_type, payload | nil, error
 * ================================================================ */

static int l_conn_recv(lua_State* L) {
    CeNetBaseConnUD* ud = check_conn(L, 1);

    CeNetRecvMessage recv_msg;
    CeResult r = ce_net_conn_recv(ud->conn, &recv_msg);

    if (r != CE_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "no message");
        return 2;
    }

    lua_pushinteger(L, recv_msg.type);
    if (recv_msg.payload_len > 0) {
        lua_pushlstring(L, (const char*)recv_msg.payload, recv_msg.payload_len);
    } else {
        lua_pushstring(L, "");
    }
    return 2;
}

/* ================================================================
 * conn:close()
 * ================================================================ */

static int l_conn_close(lua_State* L) {
    CeNetBaseConnUD* ud = check_conn(L, 1);
    ce_net_conn_disconnect(ud->conn);
    lua_pushboolean(L, 1);
    return 1;
}

/* ================================================================
 * conn:get_state() → state_string
 * ================================================================ */

static int l_conn_get_state(lua_State* L) {
    CeNetBaseConnUD* ud = check_conn(L, 1);
    CeNetConnState state = ce_net_conn_get_state(ud->conn);

    switch (state) {
        case CE_NET_CONN_DISCONNECTED: lua_pushstring(L, "disconnected"); break;
        case CE_NET_CONN_CONNECTING:   lua_pushstring(L, "connecting"); break;
        case CE_NET_CONN_CONNECTED:    lua_pushstring(L, "connected"); break;
        case CE_NET_CONN_CLOSING:      lua_pushstring(L, "closing"); break;
        default:                       lua_pushstring(L, "unknown"); break;
    }
    return 1;
}

/* ================================================================
 * conn:get_fd() → fd
 * ================================================================ */

static int l_conn_get_fd(lua_State* L) {
    CeNetBaseConnUD* ud = check_conn(L, 1);
    int fd = ce_net_conn_get_fd(ud->conn);
    lua_pushinteger(L, fd);
    return 1;
}

/* ================================================================
 * conn:send_ping()
 * ================================================================ */

static int l_conn_send_ping(lua_State* L) {
    CeNetBaseConnUD* ud = check_conn(L, 1);
    CeResult r = ce_net_conn_send_ping(ud->conn);
    lua_pushboolean(L, r == CE_OK ? 1 : 0);
    return 1;
}

/* ================================================================
 * conn:send_pong()
 * ================================================================ */

static int l_conn_send_pong(lua_State* L) {
    CeNetBaseConnUD* ud = check_conn(L, 1);
    CeResult r = ce_net_conn_send_pong(ud->conn);
    lua_pushboolean(L, r == CE_OK ? 1 : 0);
    return 1;
}

/* ================================================================
 * conn:heartbeat_timeout() → bool
 * ================================================================ */

static int l_conn_heartbeat_timeout(lua_State* L) {
    CeNetBaseConnUD* ud = check_conn(L, 1);
    CeBool timeout = ce_net_conn_heartbeat_timeout(ud->conn);
    lua_pushboolean(L, timeout == CE_TRUE ? 1 : 0);
    return 1;
}

/* ================================================================
 * conn:heartbeat_touch()
 * ================================================================ */

static int l_conn_heartbeat_touch(lua_State* L) {
    CeNetBaseConnUD* ud = check_conn(L, 1);
    ce_net_conn_heartbeat_touch(ud->conn);
    lua_pushboolean(L, 1);
    return 1;
}

/* ================================================================
 * conn:try_reconnect()
 * ================================================================ */

static int l_conn_try_reconnect(lua_State* L) {
    CeNetBaseConnUD* ud = check_conn(L, 1);
    CeResult r = ce_net_conn_try_reconnect(ud->conn);
    lua_pushboolean(L, r == CE_OK ? 1 : 0);
    return 1;
}

/* ================================================================
 * conn:get_stats() → stats_table
 * ================================================================ */

static int l_conn_get_stats(lua_State* L) {
    CeNetBaseConnUD* ud = check_conn(L, 1);

    CeNetConnStats stats;
    ce_net_conn_get_stats(ud->conn, &stats);

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)stats.bytes_sent);
    lua_setfield(L, -2, "bytes_sent");
    lua_pushinteger(L, (lua_Integer)stats.bytes_recv);
    lua_setfield(L, -2, "bytes_recv");
    lua_pushinteger(L, (lua_Integer)stats.msgs_sent);
    lua_setfield(L, -2, "msgs_sent");
    lua_pushinteger(L, (lua_Integer)stats.msgs_recv);
    lua_setfield(L, -2, "msgs_recv");
    lua_pushinteger(L, stats.reconnect_count);
    lua_setfield(L, -2, "reconnect_count");

    return 1;
}

/* ================================================================
 * conn __gc 元方法
 * ================================================================ */

static int l_conn_gc(lua_State* L) {
    CeNetBaseConnUD* ud = check_conn(L, 1);
    if (ud->conn) {
        ce_net_conn_destroy(ud->conn);
        ud->conn = NULL;
    }
    return 0;
}

/* ================================================================
 * net_base.pack(msg_type, payload) → string
 * ================================================================ */

static int l_net_base_pack(lua_State* L) {
    uint16_t msg_type = (uint16_t)luaL_checkinteger(L, 1);

    size_t payload_len = 0;
    const char* payload = NULL;
    if (lua_gettop(L) >= 2 && lua_isstring(L, 2)) {
        payload = lua_tolstring(L, 2, &payload_len);
    }

    uint8_t buf[CE_NET_BASE_MAX_MSG_SIZE];
    uint32_t total = ce_net_base_pack(buf, sizeof(buf), msg_type,
                                      (const uint8_t*)payload, (uint32_t)payload_len);
    if (total == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "buffer too small");
        return 2;
    }

    lua_pushlstring(L, (const char*)buf, total);
    return 1;
}

/* ================================================================
 * net_base.unpack(data) → msg_type, payload | nil, error
 * ================================================================ */

static int l_net_base_unpack(lua_State* L) {
    size_t data_len = 0;
    const char* data = luaL_checklstring(L, 1, &data_len);

    uint16_t msg_type = 0;
    const uint8_t* payload = NULL;
    uint32_t payload_len = 0;

    CeResult r = ce_net_base_unpack((const uint8_t*)data, (uint32_t)data_len,
                                    &msg_type, &payload, &payload_len);
    if (r != CE_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "unpack failed");
        return 2;
    }

    lua_pushinteger(L, msg_type);
    if (payload_len > 0 && payload) {
        lua_pushlstring(L, (const char*)payload, payload_len);
    } else {
        lua_pushstring(L, "");
    }
    return 2;
}

/* ================================================================
 * net_base.peek_len(data) → total_len | nil
 * ================================================================ */

static int l_net_base_peek_len(lua_State* L) {
    size_t data_len = 0;
    const char* data = luaL_checklstring(L, 1, &data_len);

    uint32_t len = ce_net_base_peek_len((const uint8_t*)data, (uint32_t)data_len);
    if (len == 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushinteger(L, len);
    return 1;
}

/* ================================================================
 * net_base.peek_type(data) → msg_type | nil
 * ================================================================ */

static int l_net_base_peek_type(lua_State* L) {
    size_t data_len = 0;
    const char* data = luaL_checklstring(L, 1, &data_len);

    uint16_t msg_type = ce_net_base_peek_type((const uint8_t*)data, (uint32_t)data_len);
    if (msg_type == 0 && data_len < CE_NET_BASE_HEADER_SIZE) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushinteger(L, msg_type);
    return 1;
}

/* ================================================================
 * 连接池 Lua 绑定
 * ================================================================ */

typedef struct CeNetBasePoolUD {
    CeNetPool* pool;
} CeNetBasePoolUD;

static CeNetBasePoolUD* check_pool(lua_State* L, int idx) {
    return (CeNetBasePoolUD*)luaL_checkudata(L, idx, NET_BASE_POOL_META);
}

static int l_net_base_create_pool(lua_State* L) {
    int max_conn = 16;
    if (lua_gettop(L) >= 1 && lua_isinteger(L, 1)) {
        max_conn = (int)lua_tointeger(L, 1);
    }

    CeNetPool* pool = ce_net_pool_create(max_conn);
    if (!pool) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create pool");
        return 2;
    }

    CeNetBasePoolUD* ud = (CeNetBasePoolUD*)lua_newuserdata(L, sizeof(CeNetBasePoolUD));
    ud->pool = pool;

    luaL_getmetatable(L, NET_BASE_POOL_META);
    lua_setmetatable(L, -2);

    return 1;
}

static int l_pool_add(lua_State* L) {
    CeNetBasePoolUD* pud = check_pool(L, 1);
    CeNetBaseConnUD* cud = check_conn(L, 2);

    CeResult r = ce_net_pool_add(pud->pool, cud->conn);
    lua_pushboolean(L, r == CE_OK ? 1 : 0);
    return 1;
}

static int l_pool_acquire(lua_State* L) {
    CeNetBasePoolUD* pud = check_pool(L, 1);

    CeNetConnection* conn = ce_net_pool_acquire(pud->pool);
    if (!conn) {
        lua_pushnil(L);
        return 1;
    }

    /* 返回连接 userdata（不拥有所有权，仅引用） */
    CeNetBaseConnUD* ud = (CeNetBaseConnUD*)lua_newuserdata(L, sizeof(CeNetBaseConnUD));
    ud->conn = conn;

    luaL_getmetatable(L, NET_BASE_CONN_META);
    lua_setmetatable(L, -2);

    return 1;
}

static int l_pool_release(lua_State* L) {
    CeNetBasePoolUD* pud = check_pool(L, 1);
    CeNetBaseConnUD* cud = check_conn(L, 2);

    ce_net_pool_release(pud->pool, cud->conn);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_pool_stats(lua_State* L) {
    CeNetBasePoolUD* pud = check_pool(L, 1);

    int total = 0, available = 0;
    ce_net_pool_stats(pud->pool, &total, &available);

    lua_pushinteger(L, total);
    lua_pushinteger(L, available);
    return 2;
}

static int l_pool_cleanup(lua_State* L) {
    CeNetBasePoolUD* pud = check_pool(L, 1);
    int removed = ce_net_pool_cleanup(pud->pool);
    lua_pushinteger(L, removed);
    return 1;
}

static int l_pool_gc(lua_State* L) {
    CeNetBasePoolUD* ud = check_pool(L, 1);
    if (ud->pool) {
        ce_net_pool_destroy(ud->pool);
        ud->pool = NULL;
    }
    return 0;
}

/* ================================================================
 * Router 网格 Lua 绑定
 * ================================================================ */

typedef struct CeNetBaseMeshUD {
    CeNetRouterMesh* mesh;
} CeNetBaseMeshUD;

static CeNetBaseMeshUD* check_mesh(lua_State* L, int idx) {
    return (CeNetBaseMeshUD*)luaL_checkudata(L, idx, NET_BASE_MESH_META);
}

static int l_net_base_create_mesh(lua_State* L) {
    CeNetRegion local;
    memset(&local, 0, sizeof(local));

    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "name");
        if (lua_isstring(L, -1)) {
            strncpy(local.name, lua_tostring(L, -1), sizeof(local.name) - 1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "host");
        if (lua_isstring(L, -1)) {
            strncpy(local.host, lua_tostring(L, -1), sizeof(local.host) - 1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "port");
        if (lua_isinteger(L, -1)) {
            local.port = (uint16_t)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "region_id");
        if (lua_isinteger(L, -1)) {
            local.region_id = (uint32_t)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
    }

    CeNetRouterMesh* mesh = ce_net_mesh_create(&local);
    if (!mesh) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create mesh");
        return 2;
    }

    CeNetBaseMeshUD* ud = (CeNetBaseMeshUD*)lua_newuserdata(L, sizeof(CeNetBaseMeshUD));
    ud->mesh = mesh;

    luaL_getmetatable(L, NET_BASE_MESH_META);
    lua_setmetatable(L, -2);

    return 1;
}

static int l_mesh_add_region(lua_State* L) {
    CeNetBaseMeshUD* mud = check_mesh(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    CeNetRegion region;
    memset(&region, 0, sizeof(region));

    lua_getfield(L, 2, "name");
    if (lua_isstring(L, -1)) {
        strncpy(region.name, lua_tostring(L, -1), sizeof(region.name) - 1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "host");
    if (lua_isstring(L, -1)) {
        strncpy(region.host, lua_tostring(L, -1), sizeof(region.host) - 1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "port");
    if (lua_isinteger(L, -1)) {
        region.port = (uint16_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "region_id");
    if (lua_isinteger(L, -1)) {
        region.region_id = (uint32_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    CeResult r = ce_net_mesh_add_region(mud->mesh, &region);
    lua_pushboolean(L, r == CE_OK ? 1 : 0);
    return 1;
}

static int l_mesh_remove_region(lua_State* L) {
    CeNetBaseMeshUD* mud = check_mesh(L, 1);
    uint32_t region_id = (uint32_t)luaL_checkinteger(L, 2);

    CeResult r = ce_net_mesh_remove_region(mud->mesh, region_id);
    lua_pushboolean(L, r == CE_OK ? 1 : 0);
    return 1;
}

static int l_mesh_connect_all(lua_State* L) {
    CeNetBaseMeshUD* mud = check_mesh(L, 1);
    CeResult r = ce_net_mesh_connect_all(mud->mesh);
    lua_pushboolean(L, r == CE_OK ? 1 : 0);
    return 1;
}

static int l_mesh_disconnect_all(lua_State* L) {
    CeNetBaseMeshUD* mud = check_mesh(L, 1);
    ce_net_mesh_disconnect_all(mud->mesh);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_mesh_send(lua_State* L) {
    CeNetBaseMeshUD* mud = check_mesh(L, 1);
    uint32_t region_id = (uint32_t)luaL_checkinteger(L, 2);
    uint16_t msg_type = (uint16_t)luaL_checkinteger(L, 3);

    size_t payload_len = 0;
    const char* payload = NULL;
    if (lua_gettop(L) >= 4 && lua_isstring(L, 4)) {
        payload = lua_tolstring(L, 4, &payload_len);
    }

    CeNetMessage msg;
    msg.type = msg_type;
    msg.payload_len = (uint32_t)payload_len;
    msg.payload = (const uint8_t*)payload;

    CeResult r = ce_net_mesh_send(mud->mesh, region_id, &msg);
    lua_pushboolean(L, r == CE_OK ? 1 : 0);
    return 1;
}

static int l_mesh_broadcast(lua_State* L) {
    CeNetBaseMeshUD* mud = check_mesh(L, 1);
    uint16_t msg_type = (uint16_t)luaL_checkinteger(L, 2);

    size_t payload_len = 0;
    const char* payload = NULL;
    if (lua_gettop(L) >= 3 && lua_isstring(L, 3)) {
        payload = lua_tolstring(L, 3, &payload_len);
    }

    CeNetMessage msg;
    msg.type = msg_type;
    msg.payload_len = (uint32_t)payload_len;
    msg.payload = (const uint8_t*)payload;

    int sent = ce_net_mesh_broadcast(mud->mesh, &msg);
    lua_pushinteger(L, sent);
    return 1;
}

static int l_mesh_stats(lua_State* L) {
    CeNetBaseMeshUD* mud = check_mesh(L, 1);

    int total = 0, connected = 0;
    ce_net_mesh_stats(mud->mesh, &total, &connected);

    lua_pushinteger(L, total);
    lua_pushinteger(L, connected);
    return 2;
}

static int l_mesh_gc(lua_State* L) {
    CeNetBaseMeshUD* ud = check_mesh(L, 1);
    if (ud->mesh) {
        ce_net_mesh_destroy(ud->mesh);
        ud->mesh = NULL;
    }
    return 0;
}

/* ================================================================
 * 模块注册
 * ================================================================ */

static const luaL_Reg conn_methods[] = {
    {"send",              l_conn_send},
    {"recv",              l_conn_recv},
    {"close",             l_conn_close},
    {"get_state",         l_conn_get_state},
    {"get_fd",            l_conn_get_fd},
    {"send_ping",         l_conn_send_ping},
    {"send_pong",         l_conn_send_pong},
    {"heartbeat_timeout", l_conn_heartbeat_timeout},
    {"heartbeat_touch",   l_conn_heartbeat_touch},
    {"try_reconnect",     l_conn_try_reconnect},
    {"get_stats",         l_conn_get_stats},
    {NULL, NULL}
};

static const luaL_Reg pool_methods[] = {
    {"add",      l_pool_add},
    {"acquire",  l_pool_acquire},
    {"release",  l_pool_release},
    {"stats",    l_pool_stats},
    {"cleanup",  l_pool_cleanup},
    {NULL, NULL}
};

static const luaL_Reg mesh_methods[] = {
    {"add_region",      l_mesh_add_region},
    {"remove_region",   l_mesh_remove_region},
    {"connect_all",     l_mesh_connect_all},
    {"disconnect_all",  l_mesh_disconnect_all},
    {"send",            l_mesh_send},
    {"broadcast",       l_mesh_broadcast},
    {"stats",           l_mesh_stats},
    {NULL, NULL}
};

static const luaL_Reg net_base_funcs[] = {
    {"connect",     l_net_base_connect},
    {"pack",        l_net_base_pack},
    {"unpack",      l_net_base_unpack},
    {"peek_len",    l_net_base_peek_len},
    {"peek_type",   l_net_base_peek_type},
    {"create_pool", l_net_base_create_pool},
    {"create_mesh", l_net_base_create_mesh},
    {NULL, NULL}
};

/* ---- 消息类型常量 ---- */

static void register_constants(lua_State* L) {
    lua_pushinteger(L, CE_NET_MSG_PING);          lua_setfield(L, -2, "MSG_PING");
    lua_pushinteger(L, CE_NET_MSG_PONG);          lua_setfield(L, -2, "MSG_PONG");
    lua_pushinteger(L, CE_NET_MSG_LOGIN);         lua_setfield(L, -2, "MSG_LOGIN");
    lua_pushinteger(L, CE_NET_MSG_LOGIN_RESP);    lua_setfield(L, -2, "MSG_LOGIN_RESP");
    lua_pushinteger(L, CE_NET_MSG_GAME_DATA);     lua_setfield(L, -2, "MSG_GAME_DATA");
    lua_pushinteger(L, CE_NET_MSG_DISCONNECT);    lua_setfield(L, -2, "MSG_DISCONNECT");
    lua_pushinteger(L, CE_NET_MSG_CROSS_REGION);  lua_setfield(L, -2, "MSG_CROSS_REGION");
    lua_pushinteger(L, CE_NET_MSG_REGION_SYNC);   lua_setfield(L, -2, "MSG_REGION_SYNC");
    lua_pushinteger(L, CE_NET_MSG_ROUTER_HELLO);  lua_setfield(L, -2, "MSG_ROUTER_HELLO");
    lua_pushinteger(L, CE_NET_MSG_ROUTER_BYE);    lua_setfield(L, -2, "MSG_ROUTER_BYE");
    lua_pushinteger(L, CE_NET_MSG_USER_BASE);     lua_setfield(L, -2, "MSG_USER_BASE");
}

int luaopen_gateway_net_base(lua_State* L) {
    /* 创建连接 metatable */
    luaL_newmetatable(L, NET_BASE_CONN_META);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, conn_methods, 0);
    lua_pushcfunction(L, l_conn_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    /* 创建连接池 metatable */
    luaL_newmetatable(L, NET_BASE_POOL_META);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, pool_methods, 0);
    lua_pushcfunction(L, l_pool_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    /* 创建网格 metatable */
    luaL_newmetatable(L, NET_BASE_MESH_META);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, mesh_methods, 0);
    lua_pushcfunction(L, l_mesh_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    /* 创建模块表：用 lua_createtable + luaL_setfuncs 代替 luaL_newlib */
    lua_createtable(L, 0, 12);
    luaL_setfuncs(L, net_base_funcs, 0);
    register_constants(L);

    return 1;
}
