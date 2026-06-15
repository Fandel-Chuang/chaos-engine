/*
 * Lua 5.4 configuration header (minimal, for embedding)
 * Based on Lua 5.4.8 luaconf.h
 */
#ifndef LUA_CONF_H
#define LUA_CONF_H

#include <stddef.h>
#include <limits.h>
#include <stdint.h>

/* Lua uses double for numbers by default */
#define LUA_32BITS      0
#define LUA_FLOORN2I     0
#define LUA_NUMBER      double
#define LUA_NUMBER_FMT  "%.14g"

typedef double          lua_Number;
typedef intptr_t        lua_Integer;
typedef uintptr_t       lua_Unsigned;

#define LUA_INTEGER     lua_Integer
#define LUA_UNSIGNED    lua_Unsigned

#define LUA_IDSIZE      60

/* Default memory allocator */
#define LUA_API         extern

#define LUAI_MAXSTACK   1000000
#define LUAI_MAXCSTACK  2000
#define LUAI_GCPAUSE    200
#define LUAI_GCMUL      200
#define LUAI_GENMAJORMUL 100
#define LUAI_MINORGC     20
#define LUAI_MAXSHORTLEN 40

#define LUAL_BUFFERSIZE 8192

#define lua_writestringerror(s, p)  (fprintf(stderr, (s), (p)), fflush(stderr))
#define lua_writeline()             (fprintf(stderr, "\n"), fflush(stderr))
#define lua_writestring(s, l)       (fwrite((s), sizeof(char), (l), stdout), fflush(stdout))

#define lua_stdin_is_tty()  1

#endif /* LUA_CONF_H */
