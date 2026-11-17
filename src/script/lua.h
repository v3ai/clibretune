/* lua.h - sandboxed interpreter for a practical Lua 5.x subset (G-120).
 *
 * Written from scratch (no third-party code): lexer, recursive-descent parser to an AST and a
 * tree-walking evaluator.  Values: nil, boolean, number (double), string, table, function.
 * Each lt_lua_run starts from a fresh heap and fresh globals (like the original console), so
 * nothing survives between runs except the registered host functions and the limits.
 *
 * Language: local, assignment (multiple), if/elseif/else, while, repeat, numeric and generic
 * for, break, return, functions with closures and varargs, method calls, table constructors,
 * the operators + - * / // % ^ .. # == ~= < <= > >= and or not, and metatables with
 * __index __newindex __call __tostring __add .. __concat __eq __lt __le __len __unm.
 * Not supported: goto, integer/float split, bitwise operators, coroutines, os/io/require/load.
 * Libraries: base (print type tostring tonumber pairs ipairs next select pcall error assert
 * rawget rawset rawequal rawlen setmetatable getmetatable unpack), string (len sub upper lower rep
 * reverse byte char format find match gmatch gsub), math, table (insert remove concat sort
 * unpack pack).
 *
 * Limits (all enforced, errors are reported to the host, never crashes): an instruction budget
 * (evaluator steps), a memory budget (every allocation is counted; there is no collector, so the
 * budget bounds the whole run), a call depth limit, a value stack limit and an output limit.
 * Single-threaded: run it with an instruction budget small enough for one UI frame.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_LUA_H
#define LT_LUA_H

#include "../lt.h"

typedef struct lt_lua lt_lua;
typedef struct lt_lua_call lt_lua_call;

#define LT_LUA_DEF_STEPS 5000000ul      /* default instruction budget of one run */
#define LT_LUA_DEF_MEM   (16ul * 1024ul * 1024ul)
#define LT_LUA_MAX_DEPTH 150            /* nested Lua calls */
#define LT_LUA_MAX_SRC   (512ul * 1024ul)
#define LT_LUA_MAX_OUT   (1024ul * 1024ul)

/* host function: read arguments, push results, return the number of results */
typedef int (*lt_lua_cfn)(lt_lua_call *c, void *ud);
/* one call per print(): the line without the newline */
typedef void (*lt_lua_out_fn)(void *ud, const char *line, size_t len);

enum { LT_LUA_TNIL = 0, LT_LUA_TBOOL, LT_LUA_TNUM, LT_LUA_TSTR, LT_LUA_TTABLE, LT_LUA_TFUNC };

lt_lua *lt_lua_new(void);
void    lt_lua_free(lt_lua *L);
/* 0 keeps the current value (defaults above) */
void    lt_lua_set_limits(lt_lua *L, unsigned long max_steps, size_t max_mem);
/* print() sink; without one the lines are collected (lt_lua_output) */
void    lt_lua_set_print(lt_lua *L, lt_lua_out_fn fn, void *ud);
/* make a host function available to every later run as global `name` */
int     lt_lua_register(lt_lua *L, const char *name, lt_lua_cfn fn, void *ud);
/* run a chunk. 0 = ok, <0 = error (lt_lua_error). A returned value is formatted into
 * lt_lua_result (several values joined by a tab). chunk names the source in messages. */
int     lt_lua_run(lt_lua *L, const char *src, size_t len, const char *chunk);
/* parse only (syntax check, nothing runs); same results as lt_lua_run */
int     lt_lua_check(lt_lua *L, const char *src, size_t len, const char *chunk);
const char *lt_lua_error(const lt_lua *L);     /* "" after a successful run */
const char *lt_lua_result(const lt_lua *L);    /* "" when nothing was returned */
const char *lt_lua_output(const lt_lua *L);    /* collected print output (no sink set) */
unsigned long lt_lua_steps(const lt_lua *L);   /* steps used by the last run */
size_t  lt_lua_mem_used(const lt_lua *L);      /* bytes counted at the end of the last run */

/* host function helpers (valid inside a lt_lua_cfn) */
int     lt_lua_nargs(const lt_lua_call *c);
int     lt_lua_argtype(const lt_lua_call *c, int i);              /* i from 0 */
lt_bool lt_lua_argnum(const lt_lua_call *c, int i, double *out);  /* number or numeric string */
const char *lt_lua_argstr(const lt_lua_call *c, int i);           /* string argument or NULL */
void    lt_lua_pushnil(lt_lua_call *c);
void    lt_lua_pushbool(lt_lua_call *c, int b);
void    lt_lua_pushnum(lt_lua_call *c, double v);
void    lt_lua_pushstr(lt_lua_call *c, const char *s);
void    lt_lua_fail(lt_lua_call *c, const char *fmt, ...);        /* raise a script error, no return */

/* number <-> text exactly as the interpreter does it (tostring / tonumber) */
void    lt_lua_fmt_number(char *buf, size_t cap, double v);
lt_bool lt_lua_parse_number(const char *s, size_t len, double *out);

/* syntax classes for editors: one byte per input byte */
enum { LT_LUA_HL_TEXT = 0, LT_LUA_HL_KEYWORD, LT_LUA_HL_STRING, LT_LUA_HL_COMMENT, LT_LUA_HL_NUMBER,
       LT_LUA_HL_BUILTIN };
void    lt_lua_highlight(const char *text, size_t len, unsigned char *cls);

#endif
