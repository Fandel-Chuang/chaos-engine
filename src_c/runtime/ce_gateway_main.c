/*
 * ChaosEngine Gateway 进程入口
 *
 * C 进程，加载 src_lua/gateway/init.lua 作为业务脚本。
 * Gateway 提供：
 *   - Port 9000: TCP 客户端接入
 *   - Port 9001: KCP 客户端接入
 *   - Port 9002: WebSocket 客户端接入
 *
 * 网络 I/O 由 LuaSocket 协程驱动，C 层提供 ce_net_base 绑定。
 *
 * 用法:
 *   ./chaos_gateway [--port PORT] [--backend HOST:PORT]
 */

#define _POSIX_C_SOURCE 200112L
#include "public_api/chaos_engine.h"
#include "network/ce_net_base_lua.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <libgen.h>

#define GATEWAY_SCRIPT "src_lua/gateway/init.lua"

static volatile int g_running = 1;
static void signal_handler(int sig) { (void)sig; g_running = 0; }

static int find_project_root(char* buf, size_t sz) {
    char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd))) return -1;
    snprintf(buf, sz, "%s/%s", cwd, GATEWAY_SCRIPT);
    if (access(buf, R_OK) == 0) { snprintf(buf, sz, "%s", cwd); return 0; }
    char path[1024]; snprintf(path, sizeof(path), "%s", cwd);
    for (int i = 0; i < 5; i++) {
        char tmp[1024]; snprintf(tmp, sizeof(tmp), "%s", path);
        char* parent = dirname(tmp);
        if (strcmp(parent, "/") == 0) break;
        snprintf(path, sizeof(path), "%s", parent);
        char test[1024];
        snprintf(test, sizeof(test), "%s/%s", path, GATEWAY_SCRIPT);
        if (access(test, R_OK) == 0) { snprintf(buf, sz, "%s", path); return 0; }
    }
    return -1;
}

static void setup_lua_path(lua_State* L, const char* root) {
    lua_getglobal(L, "package"); lua_getfield(L, -1, "path");
    const char* cur = lua_tostring(L, -1); lua_pop(L, 1);
    char np[4096];
    snprintf(np, sizeof(np), "%s/src_lua/?.lua;%s/src_lua/?/init.lua;%s",
             root, root, cur ? cur : "");
    lua_pushstring(L, np); lua_setfield(L, -2, "path"); lua_pop(L, 1);
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler);

    char root[1024];
    if (find_project_root(root, sizeof(root)) != 0) {
        fprintf(stderr, "[Gateway] Cannot find project root\n");
        return 1;
    }
    printf("[Gateway] Project root: %s\n", root);

    CeEngineConfig cfg = {
        .app_name = "ChaosEngine-Gateway", .window_width = 0, .window_height = 0,
        .fullscreen = CE_FALSE, .vsync = CE_FALSE,
        .log_level = CE_LOG_INFO, .log_file_path = "logs/chaos_gateway.log"
    };
    if (ce_init(&cfg) != CE_OK) { fprintf(stderr, "[Gateway] Engine init failed\n"); return 1; }

    lua_State* L = luaL_newstate();
    if (!L) { fprintf(stderr, "[Gateway] Lua VM failed\n"); ce_shutdown(); return 1; }
    luaL_openlibs(L);
    setup_lua_path(L, root);

    /* 注册 gateway.net_base 模块 */
    lua_pushcfunction(L, luaopen_gateway_net_base);
    lua_call(L, 0, 1);
    lua_setglobal(L, "gateway.net_base");

    char sp[1024]; snprintf(sp, sizeof(sp), "%s/%s", root, GATEWAY_SCRIPT);
    if (luaL_loadfile(L, sp) != LUA_OK) {
        fprintf(stderr, "[Gateway] Load error: %s\n", lua_tostring(L, -1));
        lua_close(L); ce_shutdown(); return 1;
    }
    int nargs = 0;
    for (int i = 1; i < argc; i++) { lua_pushstring(L, argv[i]); nargs++; }
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
        fprintf(stderr, "[Gateway] Runtime error: %s\n", lua_tostring(L, -1));
        lua_close(L); ce_shutdown(); return 1;
    }

    lua_close(L); ce_shutdown();
    printf("[Gateway] Shutdown complete.\n");
    return 0;
}
