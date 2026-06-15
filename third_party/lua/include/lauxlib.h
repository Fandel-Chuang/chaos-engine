/*
 * Lua 5.4 auxiliary library header (minimal, for embedding)
 * Based on Lua 5.4.8 lauxlib.h
 */
#ifndef LAUXLIB_H
#define LAUXLIB_H

#include "lua.h"

#include <stddef.h>
#include <stdio.h>

/* type of numbers in Lua */
typedef double lua_Number;

/* extra error code for 'luaL_loadfilex' */
#define LUA_ERRFILE (LUA_ERRERR + 1)

/* key, in the registry, for table of loaded modules */
#define LUA_LOADED_TABLE    "_LOADED"

/* key, in the registry, for table of preloaded loaders */
#define LUA_PRELOAD_TABLE   "_PRELOAD"

typedef struct luaL_Reg {
    const char* name;
    lua_CFunction func;
} luaL_Reg;

LUA_API void             luaL_checkversion_(lua_State* L, lua_Number ver, size_t sz);
#define luaL_checkversion(L)  luaL_checkversion_(L, LUA_VERSION_NUM, LUAL_NUMSIZES)

LUA_API int              luaL_getmetafield(lua_State* L, int obj, const char* e);
LUA_API int              luaL_callmeta(lua_State* L, int obj, const char* e);
LUA_API const char*      luaL_tolstring(lua_State* L, int idx, size_t* len);
LUA_API int              luaL_argerror(lua_State* L, int arg, const char* extramsg);
LUA_API int              luaL_typeerror(lua_State* L, int arg, const char* tname);
LUA_API const char*      luaL_checklstring(lua_State* L, int arg, size_t* l);
LUA_API const char*      luaL_optlstring(lua_State* L, int arg, const char* def, size_t* l);
LUA_API lua_Number       luaL_checknumber(lua_State* L, int arg);
LUA_API lua_Number       luaL_optnumber(lua_State* L, int arg, lua_Number def);
LUA_API lua_Integer      luaL_checkinteger(lua_State* L, int arg);
LUA_API lua_Integer      luaL_optinteger(lua_State* L, int arg, lua_Integer def);

LUA_API void             luaL_checkstack(lua_State* L, int sz, const char* msg);
LUA_API void             luaL_checktype(lua_State* L, int arg, int t);
LUA_API void             luaL_checkany(lua_State* L, int arg);

LUA_API int              luaL_newmetatable(lua_State* L, const char* tname);
LUA_API void             luaL_setmetatable(lua_State* L, const char* tname);
LUA_API void*            luaL_testudata(lua_State* L, int arg, const char* tname);
LUA_API void*            luaL_checkudata(lua_State* L, int arg, const char* tname);

LUA_API void             luaL_where(lua_State* L, int lvl);
LUA_API int              luaL_checkoption(lua_State* L, int arg, const char* def, const char* const lst[]);

LUA_API int              luaL_fileresult(lua_State* L, int stat, const char* fname);
LUA_API int              luaL_execresult(lua_State* L, int stat);

/* pre-defined references */
#define LUA_NOREF       (-2)
#define LUA_REFNIL      (-1)

LUA_API int              luaL_ref(lua_State* L, int t);
LUA_API void             luaL_unref(lua_State* L, int t, int ref);
LUA_API int              luaL_loadfilex(lua_State* L, const char* filename, const char* mode);

#define luaL_loadfile(L, f) luaL_loadfilex(L, f, NULL)

LUA_API int              luaL_loadbufferx(lua_State* L, const char* buff, size_t sz, const char* name, const char* mode);
LUA_API int              luaL_loadstring(lua_State* L, const char* s);

LUA_API lua_State*       luaL_newstate(void);

LUA_API int              luaL_len(lua_State* L, int idx);

LUA_API void             luaL_addgsub(lua_State* L, const char* s, const char* p, const char* r);
LUA_API const char*      luaL_gsub(lua_State* L, const char* s, const char* p, const char* r);

LUA_API void             luaL_setfuncs(lua_State* L, const luaL_Reg* l, int nup);

/* Buffer for building strings */
typedef struct luaL_Buffer {
    char* b;
    size_t size;
    size_t n;
    lua_State* L;
    union {
        double dummy;   /* ensure largest alignment */
        void* ptr;
        long l;
    } u;
    char initb[LUAL_BUFFERSIZE];
} luaL_Buffer;

#define luaL_addchar(B, c)  ((void)((B)->n < (B)->size || luaL_prepbuffsize((B), 1)), ((char*)((B)->b))[(B)->n++] = (c))
#define luaL_addsize(B, s)  ((B)->n += (s))

LUA_API void             luaL_buffinit(lua_State* L, luaL_Buffer* B);
LUA_API char*            luaL_prepbuffsize(luaL_Buffer* B, size_t sz);
LUA_API void             luaL_addlstring(luaL_Buffer* B, const char* s, size_t l);
LUA_API void             luaL_addstring(luaL_Buffer* B, const char* s);
LUA_API void             luaL_addvalue(luaL_Buffer* B);
LUA_API void             luaL_pushresult(luaL_Buffer* B);
LUA_API void             luaL_pushresultsize(luaL_Buffer* B, size_t sz);
LUA_API char*            luaL_buffinitsize(lua_State* L, luaL_Buffer* B, size_t sz);

#define luaL_prepbuffer(B)  luaL_prepbuffsize(B, LUAL_BUFFERSIZE)

/* File abstraction */
typedef struct luaL_Stream {
    FILE* f;
    lua_CFunction closef;
} luaL_Stream;

/* Compatibility macros */
#define luaL_argcheck(L, cond, arg, extramsg)   ((void)((cond) || luaL_argerror(L, (arg), (extramsg))))
#define luaL_checkstring(L, n)                  (luaL_checklstring(L, (n), NULL))
#define luaL_optstring(L, n, d)                 (luaL_optlstring(L, (n), (d), NULL))
#define luaL_typename(L, i)                     lua_typename(L, lua_type(L, (i)))
#define luaL_dofile(L, fn)                      (luaL_loadfile(L, fn) || lua_pcall(L, 0, LUA_MULTRET, 0))
#define luaL_dostring(L, s)                     (luaL_loadstring(L, s) || lua_pcall(L, 0, LUA_MULTRET, 0))
#define luaL_getmetatable(L, n)                 (lua_getfield(L, LUA_REGISTRYINDEX, (n)))
#define luaL_opt(L, f, n, d)                    (lua_isnoneornil(L, (n)) ? (d) : f(L, (n)))
#define luaL_loadbuffer(L, s, sz, n)            luaL_loadbufferx(L, s, sz, n, NULL)

#endif /* LAUXLIB_H */
