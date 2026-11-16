/* expr.h - INI expression language: compile once, evaluate many times.
 *
 * Used for { ... } bodies in INI files: menu/dialog/field visibility and
 * enable conditions, computed output channels, gauge limits, dynamic scale
 * expressions, table axis labels, indicator conditions, autotune custom
 * filters, datalog math channels. Spec: docs/spec/02-ini-expr-ui.md section 1.
 *
 * Variables are resolved through a caller supplied lt_expr_env (callbacks), so
 * this module knows nothing about tunes, pages or channels. An unknown
 * variable evaluates to 0.0 (never an error). The surrounding braces of
 * "{ expr }" are ignored by the lexer, so INI text can be passed unchanged.
 *
 * Ownership: lt_expr is owned by whoever compiled it (lt_expr_free). Cached
 * expressions (lt_expr_cache) are owned by the cache. Strings in lt_value are
 * owned by the value (lt_value_clear).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_EXPR_H
#define LT_EXPR_H

#include "../lt.h"
#include "../lt_codes.h"

typedef struct lt_expr lt_expr;   /* opaque compiled AST */

typedef enum lt_vtype { LT_V_NUM = 0, LT_V_BOOL, LT_V_STR } lt_vtype;
typedef struct lt_value {
    lt_vtype t;
    double   n;     /* LT_V_NUM */
    int      b;     /* LT_V_BOOL */
    char    *s;     /* LT_V_STR (malloc'd, never NULL for STR) */
} lt_value;
void    lt_value_init(lt_value *v);            /* NUM 0.0 */
void    lt_value_clear(lt_value *v);           /* frees s */
double  lt_value_f64(const lt_value *v);       /* Num n; Bool 1/0; Str 0.0 */
lt_bool lt_value_bool(const lt_value *v);      /* Num != 0 (NaN true); Str non-empty */
char   *lt_value_to_string(const lt_value *v); /* malloc'd; spec 1.1 to_display_string */

/* Host environment. Every callback may be NULL (feature then yields defaults:
 * 0.0 / "" / INVALID[idx] / false). Callbacks must not retain returned pointers
 * beyond the call unless documented. */
typedef struct lt_expr_env {
    void *ud;
    /* numeric variable (constant, pc variable, output channel...). Return
     * LT_TRUE and set *out if known. Names are case-SENSITIVE. */
    lt_bool (*get_var)(void *ud, const char *name, double *out);
    /* stringValue(name): string constant value, or NULL */
    const char *(*get_string)(void *ud, const char *name);
    /* bitStringValue(name, idx): option labels of a bits constant, positional,
     * NULL entries = holes. Returns NULL if unknown. *n = number of entries. */
    const char *const *(*get_bit_options)(void *ud, const char *name, size_t *n);
    /* table(x, "file.inc"): lookup in an .inc table (see ini.h lt_inc_*) */
    lt_bool (*table_lookup)(void *ud, const char *file, double x, double *out);
    /* arrayValue(name, idx) with interpolation */
    lt_bool (*array_value)(void *ud, const char *name, double idx, double *out);
    lt_bool (*is_online)(void *ud);                 /* isOnline() */
    const char *(*get_dir)(void *ud, int which);    /* 0 projects dir, 1 working dir */
    double (*time_now)(void *ud);                   /* timeNow(): seconds since session start */
} lt_expr_env;

void lt_expr_env_init(lt_expr_env *e);                       /* all NULL */
/* Env whose get_var reads `vars`: lt_map of name -> double* (map owned by the
 * caller and must outlive the env). */
void lt_expr_env_from_map(lt_expr_env *e, const lt_map *vars);

/* ---- compile / evaluate ---- */
/* Returns NULL on lex/parse error; message copied into err (may be NULL). */
lt_expr *lt_expr_compile(const char *src, char *err, size_t errlen);
void     lt_expr_free(lt_expr *e);
/* 0 ok, LT_EDATA on evaluation error (unknown function, bad args...; message
 * via lt_err_get). On error *out is a NUM 0.0. */
int lt_expr_eval(const lt_expr *e, const lt_expr_env *env, lt_value *out);
int lt_expr_eval_f64(const lt_expr *e, const lt_expr_env *env, double *out);
int lt_expr_eval_bool(const lt_expr *e, const lt_expr_env *env, lt_bool *out);

/* Add every identifier (variable) referenced by the AST to `set` (key = name,
 * value = non-NULL placeholder). Function names are not included. */
void lt_expr_collect_vars(const lt_expr *e, lt_map *set);
/* Unique identifiers of raw text (spec 1.9 identifiers()): array of malloc'd
 * strings, free with lt_expr_names_free. Returns count. */
size_t lt_expr_identifiers(const char *src, char ***names_out);
void   lt_expr_names_free(char **names, size_t n);

/* ---- one-shot helpers (compile on every call: use the cache in hot paths) ---- */
/* Visibility/enable semantics (spec 1.9): NULL/empty cond -> TRUE, any error -> TRUE. */
lt_bool lt_expr_test(const char *cond, const lt_expr_env *env);
/* evaluate_display_string: if raw is exactly "{ expr }" evaluate it and format
 * the result, otherwise (or on error) return a copy of raw. malloc'd. */
char *lt_expr_display(const char *raw, const lt_expr_env *env);

/* ---- compile cache keyed by source text (for per-frame UI conditions) ---- */
typedef struct lt_expr_cache {
    lt_map map;   /* src text -> internal entry (expr or "invalid" marker) */
} lt_expr_cache;
void lt_expr_cache_init(lt_expr_cache *c);
void lt_expr_cache_free(lt_expr_cache *c);          /* frees all cached lt_expr */
/* Compiled expr or NULL if src is NULL/empty/invalid. Owned by the cache. */
const lt_expr *lt_expr_cache_get(lt_expr_cache *c, const char *src);
lt_bool lt_expr_cache_test(lt_expr_cache *c, const char *cond, const lt_expr_env *env);
double  lt_expr_cache_f64(lt_expr_cache *c, const char *src, const lt_expr_env *env,
                          double dflt);
char   *lt_expr_cache_display(lt_expr_cache *c, const char *raw, const lt_expr_env *env);

#endif /* LT_EXPR_H */
