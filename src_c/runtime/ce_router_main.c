/*
 * ChaosEngine Router 进程入口
 *
 * 启动 Lua VM，加载 src_lua/router/init.lua 作为 Router 进程。
 * Router 提供：
 *   - Port 9100: Game ↔ Router (Game 进程连接)
 *   - Port 9101: Router ↔ Router (集群同步)
 *
 * Router 使用 LuaSocket 进行网络 I/O，不需要 C 网络层。
 * 仅使用 ce_net_base 的 Lua 绑定进行二进制协议编解码。
 *
 * 用法:
 *   ./chaos_router [--node-id ID] [--region NAME]
 *                   [--game-port PORT] [--cluster-port PORT]
 *                   [--peer NODE:HOST:PORT]
 *                   [--remote-region REGION:HOST:PORT]
 *                   [--log-level LEVEL]
 *
 * 纯 C99，链接 engine_core (含 Lua 5.4)。
 */

#define _POSIX_C_SOURCE 200112L
#include "public_api/chaos_engine.h"
#include "network/ce_net_base_lua.h"
#include "script/ce_lua.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <libgen.h>

/* ================================================================
 * 常量
 * ================================================================ */

#define ROUTER_SCRIPT_REL_PATH  "src_lua/router/init.lua"

/* ================================================================
 * 全局状态
 * ================================================================ */

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ================================================================
 * 辅助函数
 * ================================================================ */

/**
 * 查找项目根目录。
 * 从当前工作目录向上查找包含 src_lua/ 的目录。
 */
static int find_project_root(char* buf, size_t buf_size) {
    /* 策略 1: 从当前工作目录开始 */
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return -1;
    }

    /* 检查当前目录是否有 src_lua/router/init.lua */
    snprintf(buf, buf_size, "%s/%s", cwd, ROUTER_SCRIPT_REL_PATH);
    if (access(buf, R_OK) == 0) {
        snprintf(buf, buf_size, "%s", cwd);
        return 0;
    }

    /* 策略 2: 向上查找 (最多 5 层) */
    char path[1024];
    snprintf(path, sizeof(path), "%s", cwd);
    for (int i = 0; i < 5; i++) {
        char* parent = dirname(path);
        if (strcmp(parent, path) == 0 || strcmp(parent, "/") == 0) {
            break;  /* 到达根目录 */
        }
        snprintf(path, sizeof(path), "%s", parent);

        char test[1024];
        snprintf(test, sizeof(test), "%s/%s", path, ROUTER_SCRIPT_REL_PATH);
        if (access(test, R_OK) == 0) {
            snprintf(buf, buf_size, "%s", path);
            return 0;
        }
    }

    return -1;
}

/* ================================================================
 * 注册 C 模块到 Lua
 * ================================================================ */

/**
 * 注册 gateway.net_base 模块到 Lua VM。
 * Router 复用 ce_net_base 的 Lua 绑定进行二进制协议编解码。
 *
 * 使用 luaL_setfuncs 方式注册（兼容最小化 Lua 5.4 头文件）。
 */
static int register_net_base_module(lua_State* L) {
    /* 调用 luaopen_gateway_net_base 获取模块表 */
    lua_pushcfunction(L, luaopen_gateway_net_base);
    lua_call(L, 0, 1);  /* 栈顶现在是模块表 */

    /* 注册为全局 gateway.net_base */
    lua_setglobal(L, "gateway.net_base");

    return 0;
}

/**
 * 设置 Lua 搜索路径，使 require("router.xxx") 能正常工作。
 */
static int setup_lua_path(lua_State* L, const char* project_root) {
    /* 获取当前 package.path */
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    const char* current_path = lua_tostring(L, -1);
    lua_pop(L, 1);  /* pop path value */

    /* 构造新的 package.path */
    char new_path[4096];
    snprintf(new_path, sizeof(new_path),
        "%s/src_lua/?.lua;%s/src_lua/?/init.lua;%s",
        project_root, project_root,
        current_path ? current_path : "");

    lua_pushstring(L, new_path);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);  /* pop package table */

    return 0;
}

/* ================================================================
 * 加载并运行 Router Lua 脚本
 * ================================================================ */

/**
 * 加载 src_lua/router/init.lua 并传入命令行参数。
 */
static int run_router_script(lua_State* L, const char* project_root,
                              int argc, char** argv) {
    char script_path[1024];
    snprintf(script_path, sizeof(script_path), "%s/%s",
             project_root, ROUTER_SCRIPT_REL_PATH);

    /* 检查脚本是否存在 */
    if (access(script_path, R_OK) != 0) {
        fprintf(stderr, "[ERROR] Router script not found: %s\n", script_path);
        return -1;
    }

    /* 加载脚本文件 */
    if (luaL_loadfile(L, script_path) != LUA_OK) {
        fprintf(stderr, "[ERROR] Failed to load router script: %s\n",
                lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }

    /* 将命令行参数压入栈 (跳过 argv[0] 即程序名) */
    int nargs = 0;
    for (int i = 1; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        nargs++;
    }

    /* 执行脚本 */
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
        fprintf(stderr, "[ERROR] Router script error: %s\n",
                lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }

    return 0;
}

/* ================================================================
 * 主函数
 * ================================================================ */

int main(int argc, char** argv) {
    /* 信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 查找项目根目录 */
    char project_root[1024];
    if (find_project_root(project_root, sizeof(project_root)) != 0) {
        fprintf(stderr, "[ERROR] Cannot find project root directory.\n");
        fprintf(stderr, "  Run from the ChaosEngine project directory.\n");
        return 1;
    }

    printf("[Router] Project root: %s\n", project_root);

    /* 初始化引擎 (最小化初始化，Router 不需要渲染/网络层) */
    CeEngineConfig engine_config = {
        .app_name      = "ChaosEngine-Router",
        .window_width  = 0,
        .window_height = 0,
        .fullscreen    = CE_FALSE,
        .vsync         = CE_FALSE,
        .log_level     = CE_LOG_INFO,
        .log_file_path = "logs/chaos_router.log"
    };

    if (ce_init(&engine_config) != CE_OK) {
        fprintf(stderr, "[ERROR] Failed to initialize ChaosEngine\n");
        return 1;
    }

    /* 创建 Lua VM */
    lua_State* L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "[ERROR] Failed to create Lua VM\n");
        ce_shutdown();
        return 1;
    }

    /* 打开标准库 */
    luaL_openlibs(L);

    /* 设置 Lua 搜索路径 */
    setup_lua_path(L, project_root);

    /* 注册 gateway.net_base 模块 */
    register_net_base_module(L);

    /* 运行 Router Lua 脚本 */
    printf("[Router] Starting Router Lua script...\n");
    int result = run_router_script(L, project_root, argc, argv);

    /* 清理 */
    lua_close(L);
    ce_shutdown();

    if (result != 0) {
        fprintf(stderr, "[Router] Exited with error.\n");
        return 1;
    }

    printf("[Router] Shutdown complete.\n");
    return 0;
}
