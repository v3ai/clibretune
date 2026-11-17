/* lua_priv.h - internals shared by the files of the Lua subset interpreter (src/script/).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_LUA_PRIV_H
#define LT_LUA_PRIV_H

#include <setjmp.h>
#include "lua.h"

#define LL_STACK_MAX   65536       /* value stack entries */
#define LL_MAX_LEVELS  100         /* parser nesting */
#define LL_MAX_CALLS   150         /* evaluator nesting of Lua calls */
#define LL_MAX_VARS    200         /* locals per function */
#define LL_MAX_STR     (64ul * 1024ul * 1024ul)

enum { LV_NIL = 0, LV_BOOL, LV_NUM, LV_STR, LV_TAB, LV_FUN, LV_CFN, LV_CELL };
enum { LO_STR = 1, LO_TAB, LO_FUN, LO_CFN, LO_CELL };

struct lt_lstr;
struct lt_ltab;
struct lt_lfun;
struct lt_lcfun;
struct lt_lcell;

typedef struct lt_lv {
    int t;
    union {
        double n;
        int b;
        struct lt_lstr *s;
        struct lt_ltab *tab;
        struct lt_lfun *f;
        struct lt_lcfun *c;
        struct lt_lcell *cell;
    } u;
} lt_lv;

typedef struct lt_lobj {
    struct lt_lobj *prev, *next;
    unsigned long id;
    int kind;
} lt_lobj;

typedef struct lt_lstr {
    lt_lobj o;
    char *s;
    size_t len;
    unsigned long hash;
    int hashed;
    size_t cap;           /* bytes allocated for s (len + 1 when exact) */
} lt_lstr;

typedef struct lt_lcell {
    lt_lobj o;
    lt_lv v;
} lt_lcell;

typedef struct lt_lhnode {
    lt_lv k, v;
} lt_lhnode;

typedef struct lt_ltab {
    lt_lobj o;
    lt_lv *arr;
    size_t narr, carr;
    lt_lhnode *hash;
    size_t hcap, hused;
    struct lt_ltab *meta;
} lt_ltab;

struct lt_lnode;
struct lt_lproto;

typedef struct lt_lvar {
    const char *name;
    int slot;
    int captured;
    struct lt_lvar *next;
} lt_lvar;

typedef struct lt_lupdesc {
    int from_local;     /* 1: slot of the enclosing function, 0: its upvalue */
    int idx;
} lt_lupdesc;

typedef struct lt_lproto {
    int nparams, is_vararg, nslots, nups, line;
    lt_lupdesc *ups;
    lt_lvar *params;       /* chain, slots 0..nparams-1 in order */
    struct lt_lnode *body;
    const char *name;
} lt_lproto;

typedef struct lt_lfun {
    lt_lobj o;
    lt_lproto *p;
    lt_lcell **ups;
    int nups;
} lt_lfun;

typedef struct lt_lcfun {
    lt_lobj o;
    int (*fn)(struct lt_lua *st, int a, int n, void *ud);
    void *ud;
    const char *name;
} lt_lcfun;

/* AST */
enum {
    N_NIL = 1, N_TRUE, N_FALSE, N_NUM, N_STR, N_VARARG, N_LOCAL, N_UPVAL, N_GLOBAL, N_INDEX,
    N_CALL, N_METHOD, N_FUNC, N_BIN, N_UN, N_AND, N_OR, N_TABLE, N_PAREN,
    S_EXPR, S_LOCAL, S_ASSIGN, S_IF, S_WHILE, S_REPEAT, S_FORNUM, S_FORIN, S_LOCALFUNC,
    S_RETURN, S_BREAK, S_BLOCK
};
enum {
    OP_ADD = 1, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW, OP_IDIV, OP_CONCAT,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_NEG, OP_NOT, OP_LEN
};

typedef struct lt_lnode {
    int kind, line, op;
    double num;
    lt_lstr *str;
    struct lt_lnode *a, *b, *c, *d;
    struct lt_lnode *list;   /* chain of children linked by next */
    int n;
    struct lt_lnode *list2;
    int n2;
    struct lt_lnode *next;
    lt_lvar *var;
    lt_lvar *vars;           /* chain */
    int nvars;
    int upidx;
    lt_lproto *proto;
} lt_lnode;

typedef struct lt_lframe {
    lt_lv *slots;
    lt_lfun *fn;
    lt_lv *va;
    int nva;
    int rbase, rn;
} lt_lframe;

typedef struct lt_lchunk {
    struct lt_lchunk *next;
    size_t used, cap;
    double *mem;
} lt_lchunk;

typedef struct lt_lhost {
    char *name;
    lt_lua_cfn fn;
    void *ud;
} lt_lhost;

struct lt_lua {
    /* configuration */
    unsigned long max_steps;
    size_t max_mem;
    lt_lua_out_fn out_fn;
    void *out_ud;
    lt_lhost *hosts;
    size_t nhosts;
    /* per-run state */
    lt_lobj *objs;
    lt_lchunk *arena;
    unsigned long next_id;
    unsigned long steps;
    size_t mem;
    size_t out_total;
    int depth;
    int evd;
    int parse_only;
    int fatal;
    int sp;
    lt_lv *stack;
    jmp_buf *jb;
    lt_lv errval;
    int err_has_val;
    const char *chunk;
    int line;
    lt_ltab *globals;
    lt_ltab *slib;
    lt_lstr *s_index, *s_newindex, *s_call, *s_tostring, *s_len, *s_eq, *s_lt, *s_le, *s_unm,
            *s_concat, *s_name;
    lt_lstr *s_arith[8];
    unsigned long rnd;
    const char *curname;
    lt_lv fn_next, fn_inext;
    lt_lstr *tn[6];
    /* results */
    char errmsg[400];
    lt_buf result;
    lt_buf output;
    lt_buf scratch;
    unsigned long last_steps;
    size_t last_mem;
};

struct lt_lua_call {
    struct lt_lua *st;
    int a, n;
};

/* lua_obj.c: memory, strings, tables */
void *lt_luai_alloc(lt_lua *st, size_t n);
void  lt_luai_release(lt_lua *st, void *p, size_t n);
void *lt_luai_arena(lt_lua *st, size_t n);
void  lt_luai_reset(lt_lua *st);          /* free objects and the arena, clear the stack */
lt_lstr *lt_luai_str_new(lt_lua *st, const char *s, size_t len);
lt_lstr *lt_luai_str_buf(lt_lua *st, size_t len);   /* uninitialised bytes, NUL set; fill s[0..len) */
lt_lstr *lt_luai_sb_new(lt_lua *st, size_t hint);   /* string builder (an unfinished string) */
void  lt_luai_sb_add(lt_lua *st, lt_lstr *sb, const char *data, size_t n);
void  lt_luai_sb_addc(lt_lua *st, lt_lstr *sb, int c);
void  lt_luai_charge(lt_lua *st, unsigned long units);   /* count C-side work as steps */
void  lt_luai_step(lt_lua *st);
lt_ltab *lt_luai_tab_new(lt_lua *st);
lt_lcell *lt_luai_cell_new(lt_lua *st, lt_lv v);
lt_lfun *lt_luai_fun_new(lt_lua *st, lt_lproto *p, int nups);
lt_lcfun *lt_luai_cfun_new(lt_lua *st, int (*fn)(lt_lua *, int, int, void *), void *ud,
                           const char *name);
lt_lv  lt_luai_tab_get(const lt_ltab *t, lt_lv k);
lt_lv  lt_luai_tab_getstr(const lt_ltab *t, lt_lstr *k);
void   lt_luai_tab_set(lt_lua *st, lt_ltab *t, lt_lv k, lt_lv v);  /* key checked by caller */
size_t lt_luai_tab_len(const lt_ltab *t);
int    lt_luai_tab_next(const lt_ltab *t, lt_lv *k, lt_lv *v);     /* 1 = produced, 0 = end */
void   lt_luai_tab_reserve(lt_lua *st, lt_ltab *t, size_t n);
void   lt_luai_tab_append(lt_lua *st, lt_ltab *t, lt_lv v);   /* next array slot, nil allowed */
void   lt_luai_tab_trim(lt_ltab *t);                           /* drop trailing nils */
unsigned long lt_luai_str_hash(lt_lstr *s);
int    lt_luai_raw_equal(lt_lv a, lt_lv b);
lt_lv  lt_luai_nil(void);
lt_lv  lt_luai_num(double d);
lt_lv  lt_luai_bool(int b);
lt_lv  lt_luai_strv(lt_lstr *s);
lt_lv  lt_luai_tabv(lt_ltab *t);

/* lua_parse.c */
lt_lproto *lt_luai_parse(lt_lua *st, const char *src, size_t len);

/* lua_exec.c */
void lt_luai_error(lt_lua *st, const char *fmt, ...);        /* raise with position */
void lt_luai_error_plain(lt_lua *st, const char *fmt, ...);  /* raise, no position */
void lt_luai_fatal(lt_lua *st, const char *fmt, ...);        /* uncatchable limit error */
void lt_luai_throw_value(lt_lua *st, lt_lv v);               /* error(v) */
void lt_luai_push(lt_lua *st, lt_lv v);
int  lt_luai_call(lt_lua *st, lt_lv fn, int a, int n);       /* args at stack[a..a+n); results there */
lt_lv lt_luai_call1(lt_lua *st, lt_lv fn, const lt_lv *args, int n);
const char *lt_luai_typename(lt_lv v);
lt_lstr *lt_luai_tostr(lt_lua *st, lt_lv v);                 /* with __tostring */
int  lt_luai_tonum(lt_lv v, double *out);                    /* number or numeric string */
lt_lv lt_luai_index(lt_lua *st, lt_lv obj, lt_lv key);
void lt_luai_setindex(lt_lua *st, lt_lv obj, lt_lv key, lt_lv val);
int  lt_luai_lessthan(lt_lua *st, lt_lv a, lt_lv b);
int  lt_luai_run(lt_lua *st, lt_lproto *p);                  /* main chunk, results on the stack */
void lt_luai_check_key(lt_lua *st, lt_lv k);

/* lua_lib.c */
void lt_luai_openlibs(lt_lua *st);
void lt_luai_reg(lt_lua *st, lt_ltab *t, const char *name,
                 int (*fn)(lt_lua *, int, int, void *), void *ud);
/* lua_pat.c: Lua patterns; returns 0 no match, 1 match (captures in cap) */
#define LL_MAXCAP 32
typedef struct lt_lmatch {
    lt_lua *st;
    const char *src, *src_end, *pat_end;
    int level, depth;
    struct { const char *init; long len; } cap[LL_MAXCAP];
} lt_lmatch;
const char *lt_luai_pat_match(lt_lmatch *m, const char *s, const char *p);
lt_lv lt_luai_pat_capture(lt_lmatch *m, int i, const char *s, const char *e);
int   lt_luai_pat_ncaps(const lt_lmatch *m, const char *s, int whole);

#endif
