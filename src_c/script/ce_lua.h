/*
 * ChaosEngine Lua 运行时绑定 — 头文件
 * 纯 C99，封装 Lua 5.4 VM，提供脚本驱动引擎的能力
 */

#ifndef CE_LUA_H
#define CE_LUA_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Lua VM 生命周期 ---- */

/** 初始化 Lua VM，注册所有 ChaosEngine 绑定函数 */
CeResult ce_lua_init(CeAllocator* alloc);

/** 关闭 Lua VM，释放所有资源 */
void ce_lua_shutdown(void);

/* ---- 脚本执行 ---- */

/** 加载并执行 Lua 文件 */
CeResult ce_lua_dofile(const char* path);

/** 执行一段 Lua 代码字符串 */
CeResult ce_lua_dostring(const char* code);

/* ---- 函数调用 ---- */

/** 调用全局 Lua 函数（函数名需先在栈上或全局表中） */
CeResult ce_lua_call(const char* func_name, int nargs, int nresults);

/* ---- C 函数注册 ---- */

/** 注册一个 C 函数到 Lua 全局表 */
void ce_lua_register(const char* name, void* c_func);

/* ---- 高级访问 ---- */

/** 获取原始 lua_State 指针（供高级用户直接操作 Lua API） */
void* ce_lua_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* CE_LUA_H */
