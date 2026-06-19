/*
 * ChaosEngine DBProxy 进程入口
 *
 * C 进程，加载 src_lua/dbproxy/init.lua 作为业务脚本。
 * DBProxy 提供：
 *   - Port 9001: 状态同步接收 (Game → DBProxy)
 *   - Port 9003: 数据库代理 (CRUD 操作)
 *
 * 网络 I/O 由 LuaSocket 协程驱动。
 *
 * 用法:
 *   ./chaos_dbproxy [--role primary|backup] [--peer-host HOST] [--peer-port PORT]
 */

#define _POSIX_C_SOURCE 200112L
#include "public_api/chaos_engine.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <libgen.h>

#define DBPROXY_SCRIPT "src_lua/dbproxy/init.lua"

static volatile int g_running = 1;
static void signal_handler(int sig) { (void)sig; g_running = 0; }

static int find_project_root(char* buf, size_t sz) {
    char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd))) return -1;
    snprintf(buf, sz, "%s/%s", cwd, DBPROXY_SCRIPT);
    if (access(buf, R_OK) == 0) { snprintf(buf, sz, "%s", cwd); return 0; }
    char path[1024]; snprintf(path, sizeof(path), "%s", cwd);
    for (int i = 0; i < 5; i++) {
        char tmp[1024]; snprintf(tmp, sizeof(tmp), "%s", path);
        char* parent = dirname(tmp);
        if (strcmp(parent, "/") == 0) break;
        snprintf(path, sizeof(path), "%s", parent);
        char test[1024];
        snprintf(test, sizeof(test), "%s/%s", path, DBPROXY_SCRIPT);
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
        fprintf(stderr, "[DBProxy] Cannot find project root\n");
        return 1;
    }
    printf("[DBProxy] Project root: %s\n", root);

    CeEngineConfig cfg = {
        .app_name = "ChaosEngine-DBProxy", .window_width = 0, .window_height = 0,
        .fullscreen = CE_FALSE, .vsync = CE_FALSE,
        .log_level = CE_LOG_INFO, .log_file_path = "logs/chaos_dbproxy.log"
    };
    if (ce_init(&cfg) != CE_OK) { fprintf(stderr, "[DBProxy] Engine init failed\n"); return 1; }

    lua_State* L = luaL_newstate();
    if (!L) { fprintf(stderr, "[DBProxy] Lua VM failed\n"); ce_shutdown(); return 1; }
    luaL_openlibs(L);
    setup_lua_path(L, root);

    char sp[1024]; snprintf(sp, sizeof(sp), "%s/%s", root, DBPROXY_SCRIPT);
    if (luaL_loadfile(L, sp) != LUA_OK) {
        fprintf(stderr, "[DBProxy] Load error: %s\n", lua_tostring(L, -1));
        lua_close(L); ce_shutdown(); return 1;
    }
    int nargs = 0;
    for (int i = 1; i < argc; i++) { lua_pushstring(L, argv[i]); nargs++; }
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
        fprintf(stderr, "[DBProxy] Runtime error: %s\n", lua_tostring(L, -1));
        lua_close(L); ce_shutdown(); return 1;
    }

    lua_close(L); ce_shutdown();
    printf("[DBProxy] Shutdown complete.\n");
    return 0;
}
