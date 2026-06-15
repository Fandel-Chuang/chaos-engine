/*
 * Lua 5.4 standard library header (minimal, for embedding)
 * Based on Lua 5.4.8 lualib.h
 */
#ifndef LUALIB_H
#define LUALIB_H

#include "lua.h"

/* Key for the registry table for loaded libraries */
#define LUA_LOADLIBNAME "package"

LUA_API int              luaopen_base(lua_State* L);

#define LUA_COLIBNAME   "coroutine"
LUA_API int              luaopen_coroutine(lua_State* L);

#define LUA_TABLIBNAME  "table"
LUA_API int              luaopen_table(lua_State* L);

#define LUA_IOLIBNAME   "io"
LUA_API int              luaopen_io(lua_State* L);

#define LUA_OSLIBNAME   "os"
LUA_API int              luaopen_os(lua_State* L);

#define LUA_STRLIBNAME  "string"
LUA_API int              luaopen_string(lua_State* L);

#define LUA_UTF8LIBNAME "utf8"
LUA_API int              luaopen_utf8(lua_State* L);

#define LUA_MATHLIBNAME "math"
LUA_API int              luaopen_math(lua_State* L);

#define LUA_DBLIBNAME   "debug"
LUA_API int              luaopen_debug(lua_State* L);

#define LUA_LOADLIBNAME "package"
LUA_API int              luaopen_package(lua_State* L);

/* open all previous libraries */
LUA_API void             luaL_openlibs(lua_State* L);

#endif /* LUALIB_H */
