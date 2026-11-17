/* expr.c - INI expression language: lexer, parser (AST), evaluator, helpers.
 * Spec: docs/spec/02-ini-expr-ui.md section 1.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include "expr.h"

#define LT_EXPR_MAX_DEPTH 64
#define LT_EXPR_MAX_UNARY 256
#define LT_EXPR_MAX_TOKENS 20000

/* ------------------------------------------------------------------ */
/* values                                                              */
/* ------------------------------------------------------------------ */

void lt_value_init(lt_value *v)
{
    v->t = LT_V_NUM;
    v->n = 0.0;
    v->b = 0;
    v->s = NULL;
}

void lt_value_clear(lt_value *v)
{
    if (v->s) lt_free(v->s);
    v->s = NULL;
}

double lt_value_f64(const lt_value *v)
{
    if (v->t == LT_V_NUM) return v->n;
    if (v->t == LT_V_BOOL) return v->b ? 1.0 : 0.0;
    return 0.0;
}

lt_bool lt_value_bool(const lt_value *v)
{
    if (v->t == LT_V_NUM) return v->n != 0.0 ? LT_TRUE : LT_FALSE;
    if (v->t == LT_V_BOOL) return v->b ? LT_TRUE : LT_FALSE;
    return (v->s && v->s[0]) ? LT_TRUE : LT_FALSE;
}

char *lt_value_to_string(const lt_value *v)
{
    char tmp[512];
    double n;
    int d;

    if (v->t == LT_V_BOOL) return lt_strdup(v->b ? "true" : "false");
    if (v->t == LT_V_STR) return lt_strdup(v->s ? v->s : "");
    n = v->n;
    if (lt_isnan(n)) return lt_strdup("NaN");
    if (!lt_isfinite(n)) return lt_strdup(n < 0 ? "-inf" : "inf");
    if (n == floor(n) && fabs(n) < 1e15) {
        lt_snprintf(tmp, sizeof tmp, "%.0f", n);
        if (strcmp(tmp, "-0") == 0) strcpy(tmp, "0");
        return lt_strdup(tmp);
    }
    /* shortest fixed-notation decimal that round-trips */
    for (d = 1; d <= 40; d++) {
        double back;
        lt_snprintf(tmp, sizeof tmp, "%.*f", d, n);
        if (lt_parse_double(tmp, &back) && back == n) return lt_strdup(tmp);
    }
    lt_snprintf(tmp, sizeof tmp, "%.17g", n);
    return lt_strdup(tmp);
}

/* ------------------------------------------------------------------ */
/* 64-bit integer emulation (no long long in C89)                      */
/* ------------------------------------------------------------------ */

typedef struct i64v { lt_u32 hi, lo; } i64v;

static i64v i64_neg(i64v a)
{
    i64v r;
    r.lo = (~a.lo + 1UL) & LT_U32_MASK;
    r.hi = (~a.hi + (r.lo == 0 ? 1UL : 0UL)) & LT_U32_MASK;
    return r;
}

static i64v i64_from_d(double d)
{
    i64v r;
    int neg;
    double hi;

    r.hi = 0;
    r.lo = 0;
    if (lt_isnan(d)) return r;
    if (d >= 9223372036854775808.0) {
        r.hi = 0x7FFFFFFFUL;
        r.lo = 0xFFFFFFFFUL;
        return r;
    }
    if (d <= -9223372036854775808.0) {
        r.hi = 0x80000000UL;
        r.lo = 0;
        return r;
    }
    d = lt_trunc(d);
    neg = d < 0;
    if (neg) d = -d;
    hi = floor(d / 4294967296.0);
    r.hi = (lt_u32)hi;
    r.lo = (lt_u32)(d - hi * 4294967296.0);
    return neg ? i64_neg(r) : r;
}

static double i64_to_d(i64v v)
{
    int neg = (v.hi & 0x80000000UL) != 0;
    double d;
    if (neg) v = i64_neg(v);
    d = (double)v.hi * 4294967296.0 + (double)v.lo;
    return neg ? -d : d;
}

static i64v i64_shl(i64v a, int n)
{
    i64v r;
    if (n == 0) return a;
    if (n >= 32) {
        r.hi = (a.lo << (n - 32)) & LT_U32_MASK;
        r.lo = 0;
    } else {
        r.hi = ((a.hi << n) | (a.lo >> (32 - n))) & LT_U32_MASK;
        r.lo = (a.lo << n) & LT_U32_MASK;
    }
    return r;
}

static i64v i64_shr(i64v a, int n)
{
    i64v r;
    int neg = (a.hi & 0x80000000UL) != 0;
    lt_u32 fill = neg ? LT_U32_MASK : 0UL;
    if (n == 0) return a;
    if (n >= 32) {
        int m = n - 32;
        r.lo = m == 0 ? a.hi : (((a.hi >> m) | (neg ? (fill << (32 - m)) : 0UL)) & LT_U32_MASK);
        r.hi = fill;
    } else {
        r.lo = ((a.lo >> n) | (a.hi << (32 - n))) & LT_U32_MASK;
        r.hi = ((a.hi >> n) | (neg ? (fill << (32 - n)) : 0UL)) & LT_U32_MASK;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* AST                                                                 */
/* ------------------------------------------------------------------ */

typedef enum node_type { N_NUM, N_BOOL, N_STR, N_VAR, N_BIN, N_TERN, N_UN, N_CALL } node_type;

typedef enum binop {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR, OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR
} binop;

typedef enum unop { UN_NEG, UN_NOT, UN_BITNOT } unop;

typedef enum fn_id {
    F_UNKNOWN = 0, F_ABS, F_ROUND, F_FLOOR, F_CEIL, F_SQRT, F_LOG, F_EXP, F_SIN, F_COS, F_TAN,
    F_ASIN, F_ACOS, F_ATAN, F_POW, F_ATAN2, F_MIN, F_MAX, F_ISNAN, F_ISADV, F_LOG10, F_RECIP,
    F_IF, F_INT, F_NOT, F_BOOL, F_PASTVALUE, F_GETAUX, F_BITSTR, F_STRVAL, F_TABLE, F_TIMENOW,
    F_ISONLINE, F_ARRAYVAL, F_PROJDIR, F_WORKDIR
} fn_id;

typedef struct node node;
struct node {
    node_type type;
    double num;
    int boolean;
    char *s;           /* string literal / var name / function name */
    int op;
    int fn;
    node *a, *b, *c;
    node **args;
    size_t nargs;
};

struct lt_expr {
    node *root;
};

static node *node_new(node_type t)
{
    node *n = (node *)lt_calloc(1, sizeof(node));
    n->type = t;
    return n;
}

static void node_free(node *n)
{
    size_t i;
    if (!n) return;
    node_free(n->a);
    node_free(n->b);
    node_free(n->c);
    for (i = 0; i < n->nargs; i++) node_free(n->args[i]);
    if (n->args) lt_free(n->args);
    if (n->s) lt_free(n->s);
    lt_free(n);
}

void lt_expr_free(lt_expr *e)
{
    if (!e) return;
    node_free(e->root);
    lt_free(e);
}

/* ------------------------------------------------------------------ */
/* lexer                                                               */
/* ------------------------------------------------------------------ */

typedef enum lt_tok_kind {
    T_EOF = 0, T_NUM, T_STR, T_ID,
    T_LP, T_RP, T_COMMA, T_Q, T_COLON, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PCT, T_TILDE, T_CARET,
    T_NE, T_BANG, T_EQEQ, T_LE, T_SHL, T_LT, T_GE, T_SHR, T_GT, T_AMPAMP, T_AMP, T_PIPEPIPE,
    T_PIPE
} lt_tok_kind;

typedef struct lt_tok {
    lt_tok_kind t;
    double num;
    char *s;
} lt_tok;

typedef struct parser {
    lt_tok *items;
    size_t n, pos;
    int depth;
    int udepth;
    char err[160];
} parser;

static int is_alpha_(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

static int is_digit_(int c)
{
    return c >= '0' && c <= '9';
}

static void perr(parser *p, const char *fmt, const char *arg)
{
    if (p->err[0]) return;
    lt_snprintf(p->err, sizeof p->err, fmt, arg ? arg : "");
}

static void lt_tok_add(lt_vec *v, lt_tok_kind t, double num, char *s)
{
    lt_tok *k = (lt_tok *)lt_vec_push(v);
    k->t = t;
    k->num = num;
    k->s = s;
}

static int lex(const char *src, lt_vec *out, parser *p)
{
    size_t i = 0, n = strlen(src);

    while (i < n) {
        int c = (unsigned char)src[i];
        int d = i + 1 < n ? (unsigned char)src[i + 1] : 0;

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '{' || c == '}') {
            i++;
            continue;
        }
        if (out->len >= LT_EXPR_MAX_TOKENS) {
            perr(p, "Expression too long%s", "");
            return -1;
        }
        switch (c) {
        case '(': lt_tok_add(out, T_LP, 0, NULL); i++; continue;
        case ')': lt_tok_add(out, T_RP, 0, NULL); i++; continue;
        case ',': lt_tok_add(out, T_COMMA, 0, NULL); i++; continue;
        case '?': lt_tok_add(out, T_Q, 0, NULL); i++; continue;
        case ':': lt_tok_add(out, T_COLON, 0, NULL); i++; continue;
        case '+': lt_tok_add(out, T_PLUS, 0, NULL); i++; continue;
        case '-': lt_tok_add(out, T_MINUS, 0, NULL); i++; continue;
        case '*': lt_tok_add(out, T_STAR, 0, NULL); i++; continue;
        case '/': lt_tok_add(out, T_SLASH, 0, NULL); i++; continue;
        case '%': lt_tok_add(out, T_PCT, 0, NULL); i++; continue;
        case '~': lt_tok_add(out, T_TILDE, 0, NULL); i++; continue;
        case '^': lt_tok_add(out, T_CARET, 0, NULL); i++; continue;
        case '!':
            if (d == '=') { lt_tok_add(out, T_NE, 0, NULL); i += 2; }
            else { lt_tok_add(out, T_BANG, 0, NULL); i++; }
            continue;
        case '=':
            if (d == '=') { lt_tok_add(out, T_EQEQ, 0, NULL); i += 2; continue; }
            perr(p, "Unexpected '=' in expression (use '==' for comparison)%s", "");
            return -1;
        case '<':
            if (d == '=') { lt_tok_add(out, T_LE, 0, NULL); i += 2; }
            else if (d == '<') { lt_tok_add(out, T_SHL, 0, NULL); i += 2; }
            else { lt_tok_add(out, T_LT, 0, NULL); i++; }
            continue;
        case '>':
            if (d == '=') { lt_tok_add(out, T_GE, 0, NULL); i += 2; }
            else if (d == '>') { lt_tok_add(out, T_SHR, 0, NULL); i += 2; }
            else { lt_tok_add(out, T_GT, 0, NULL); i++; }
            continue;
        case '&':
            if (d == '&') { lt_tok_add(out, T_AMPAMP, 0, NULL); i += 2; }
            else { lt_tok_add(out, T_AMP, 0, NULL); i++; }
            continue;
        case '|':
            if (d == '|') { lt_tok_add(out, T_PIPEPIPE, 0, NULL); i += 2; }
            else { lt_tok_add(out, T_PIPE, 0, NULL); i++; }
            continue;
        default:
            break;
        }
        if (c == '"') {
            size_t st = i + 1, e = st;
            while (e < n && src[e] != '"') e++;
            lt_tok_add(out, T_STR, 0, lt_strndup(src + st, e - st));
            i = e < n ? e + 1 : e;
            continue;
        }
        if (is_digit_(c) || (c == '.' && is_digit_(d))) {
            size_t st = i;
            char *txt;
            double v;
            while (i < n && (is_digit_((unsigned char)src[i]) || src[i] == '.')) i++;
            if (i < n && (src[i] == 'e' || src[i] == 'E')) {
                size_t j = i + 1;
                if (j < n && (src[j] == '+' || src[j] == '-')) j++;
                if (j < n && is_digit_((unsigned char)src[j])) {
                    while (j < n && is_digit_((unsigned char)src[j])) j++;
                    i = j;
                }
            }
            txt = lt_strndup(src + st, i - st);
            if (!lt_parse_double(txt, &v)) {
                lt_free(txt);
                perr(p, "Malformed number literal%s", "");
                return -1;
            }
            lt_free(txt);
            lt_tok_add(out, T_NUM, v, NULL);
            continue;
        }
        if (c == '$' || is_alpha_(c)) {
            size_t st = i;
            i++;
            while (i < n && (is_alpha_((unsigned char)src[i]) || is_digit_((unsigned char)src[i])))
                i++;
            lt_tok_add(out, T_ID, 0, lt_strndup(src + st, i - st));
            continue;
        }
        {
            char cs[2];
            cs[0] = (char)c;
            cs[1] = '\0';
            perr(p, "Unexpected character '%s' in expression", cs);
        }
        return -1;
    }
    lt_tok_add(out, T_EOF, 0, NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* parser                                                              */
/* ------------------------------------------------------------------ */

static node *parse_expr(parser *p);

static const lt_tok *peek(parser *p)
{
    return &p->items[p->pos];
}

static int binop_info(lt_tok_kind t, int *level, int *op)
{
    switch (t) {
    case T_PIPEPIPE: *level = 0; *op = OP_OR; return 1;
    case T_AMPAMP:   *level = 1; *op = OP_AND; return 1;
    case T_PIPE:     *level = 2; *op = OP_BOR; return 1;
    case T_CARET:    *level = 3; *op = OP_BXOR; return 1;
    case T_AMP:      *level = 4; *op = OP_BAND; return 1;
    case T_EQEQ:     *level = 5; *op = OP_EQ; return 1;
    case T_NE:       *level = 5; *op = OP_NE; return 1;
    case T_LT:       *level = 6; *op = OP_LT; return 1;
    case T_GT:       *level = 6; *op = OP_GT; return 1;
    case T_LE:       *level = 6; *op = OP_LE; return 1;
    case T_GE:       *level = 6; *op = OP_GE; return 1;
    case T_SHL:      *level = 7; *op = OP_SHL; return 1;
    case T_SHR:      *level = 7; *op = OP_SHR; return 1;
    case T_PLUS:     *level = 8; *op = OP_ADD; return 1;
    case T_MINUS:    *level = 8; *op = OP_SUB; return 1;
    case T_STAR:     *level = 9; *op = OP_MUL; return 1;
    case T_SLASH:    *level = 9; *op = OP_DIV; return 1;
    case T_PCT:      *level = 9; *op = OP_MOD; return 1;
    default: return 0;
    }
}

static const struct { const char *name; int id; } fn_table[] = {
    {"abs", F_ABS}, {"round", F_ROUND}, {"floor", F_FLOOR}, {"ceil", F_CEIL},
    {"sqrt", F_SQRT}, {"log", F_LOG}, {"exp", F_EXP}, {"sin", F_SIN}, {"cos", F_COS},
    {"tan", F_TAN}, {"asin", F_ASIN}, {"acos", F_ACOS}, {"atan", F_ATAN}, {"pow", F_POW},
    {"atan2", F_ATAN2}, {"min", F_MIN}, {"max", F_MAX}, {"isnan", F_ISNAN},
    {"isadvancedmathavailable", F_ISADV}, {"log10", F_LOG10}, {"recip", F_RECIP},
    {"if", F_IF}, {"int", F_INT}, {"trunc", F_INT}, {"not", F_NOT}, {"boolean", F_BOOL},
    {"bool", F_BOOL}, {"pastvalue", F_PASTVALUE}, {"getauxdigital", F_GETAUX},
    {"bitstringvalue", F_BITSTR}, {"stringvalue", F_STRVAL}, {"table", F_TABLE},
    {"timenow", F_TIMENOW}, {"isonline", F_ISONLINE}, {"arrayvalue", F_ARRAYVAL},
    {"$getprojectsdirpath", F_PROJDIR}, {"$getprojectsdir", F_PROJDIR},
    {"$getworkingdirpath", F_WORKDIR}, {"$getworkingdir", F_WORKDIR}
};

static int fn_lookup(const char *name)
{
    size_t i;
    for (i = 0; i < LT_COUNTOF(fn_table); i++)
        if (lt_stricmp(name, fn_table[i].name) == 0) return fn_table[i].id;
    return F_UNKNOWN;
}

static node *parse_primary(parser *p)
{
    const lt_tok *k = peek(p);
    node *n;

    switch (k->t) {
    case T_NUM:
        n = node_new(N_NUM);
        n->num = k->num;
        p->pos++;
        return n;
    case T_STR:
        n = node_new(N_STR);
        n->s = lt_strdup(k->s);
        p->pos++;
        return n;
    case T_ID:
        p->pos++;
        if (strcmp(k->s, "true") == 0 || strcmp(k->s, "false") == 0) {
            n = node_new(N_BOOL);
            n->boolean = k->s[0] == 't';
            return n;
        }
        if (peek(p)->t == T_LP) {
            lt_vec args;
            size_t i;
            p->pos++;
            lt_vec_init(&args, sizeof(node *));
            n = node_new(N_CALL);
            n->s = lt_strdup(k->s);
            n->fn = fn_lookup(k->s);
            if (peek(p)->t != T_RP) {
                for (;;) {
                    node *a = parse_expr(p);
                    if (!a) {
                        for (i = 0; i < args.len; i++) node_free(LT_VEC_AT(&args, node *, i));
                        lt_vec_free(&args);
                        node_free(n);
                        return NULL;
                    }
                    *(node **)lt_vec_push(&args) = a;
                    if (peek(p)->t == T_COMMA) {
                        p->pos++;
                        continue;
                    }
                    if (peek(p)->t == T_RP) break;
                    perr(p, "Expected ',' or ')'%s", "");
                    for (i = 0; i < args.len; i++) node_free(LT_VEC_AT(&args, node *, i));
                    lt_vec_free(&args);
                    node_free(n);
                    return NULL;
                }
            }
            p->pos++; /* ')' */
            n->nargs = args.len;
            n->args = (node **)args.data;
            if (args.len == 0) {
                lt_vec_free(&args);
                n->args = NULL;
            }
            return n;
        }
        n = node_new(N_VAR);
        n->s = lt_strdup(k->s);
        return n;
    case T_LP:
        p->pos++;
        n = parse_expr(p);
        if (!n) return NULL;
        if (peek(p)->t != T_RP) {
            perr(p, "Expected ')'%s", "");
            node_free(n);
            return NULL;
        }
        p->pos++;
        return n;
    default:
        perr(p, "Unexpected token%s", "");
        return NULL;
    }
}

static node *parse_unary(parser *p)
{
    lt_tok_kind t = peek(p)->t;
    node *n, *inner;

    if (t == T_MINUS || t == T_BANG || t == T_TILDE) {
        if (++p->udepth > LT_EXPR_MAX_UNARY) {
            perr(p, "Expression nested too deeply%s", "");
            return NULL;
        }
        p->pos++;
        inner = parse_unary(p);
        p->udepth--;
        if (!inner) return NULL;
        n = node_new(N_UN);
        n->op = t == T_MINUS ? UN_NEG : (t == T_BANG ? UN_NOT : UN_BITNOT);
        n->a = inner;
        return n;
    }
    return parse_primary(p);
}

static node *parse_bin(parser *p, int level)
{
    node *l, *r, *n;
    int lv, op;

    if (level > 9) return parse_unary(p);
    l = parse_bin(p, level + 1);
    if (!l) return NULL;
    while (binop_info(peek(p)->t, &lv, &op) && lv == level) {
        p->pos++;
        r = parse_bin(p, level + 1);
        if (!r) {
            node_free(l);
            return NULL;
        }
        n = node_new(N_BIN);
        n->op = op;
        n->a = l;
        n->b = r;
        l = n;
    }
    return l;
}

static node *parse_expr(parser *p)
{
    node *c, *a, *b, *n;

    if (++p->depth > LT_EXPR_MAX_DEPTH) {
        perr(p, "Expression nested deeper than 64 levels%s", "");
        p->depth--;
        return NULL;
    }
    c = parse_bin(p, 0);
    if (c && peek(p)->t == T_Q) {
        p->pos++;
        a = parse_expr(p);
        if (!a) {
            node_free(c);
            p->depth--;
            return NULL;
        }
        if (peek(p)->t != T_COLON) {
            perr(p, "Expected ':' in ternary expression%s", "");
            node_free(c);
            node_free(a);
            p->depth--;
            return NULL;
        }
        p->pos++;
        b = parse_expr(p);
        if (!b) {
            node_free(c);
            node_free(a);
            p->depth--;
            return NULL;
        }
        n = node_new(N_TERN);
        n->a = c;
        n->b = a;
        n->c = b;
        c = n;
    }
    p->depth--;
    return c;
}

lt_expr *lt_expr_compile(const char *src, char *err, size_t errlen)
{
    parser p;
    lt_vec lexed;
    node *root = NULL;
    lt_expr *e = NULL;
    size_t i;

    memset(&p, 0, sizeof p);
    lt_vec_init(&lexed, sizeof(lt_tok));
    if (err && errlen) err[0] = '\0';
    if (!src) src = "";
    if (lex(src, &lexed, &p) == 0) {
        p.items = (lt_tok *)lexed.data;
        p.n = lexed.len;
        root = parse_expr(&p);
        if (root && peek(&p)->t != T_EOF) {
            char nb[48];
            lt_snprintf(nb, sizeof nb, "%u of %u", (unsigned)(p.pos + 1), (unsigned)p.n);
            perr(&p, "Unexpected trailing input after expression (token %s)", nb);
            node_free(root);
            root = NULL;
        }
    }
    for (i = 0; i < lexed.len; i++) {
        lt_tok *k = &LT_VEC_AT(&lexed, lt_tok, i);
        if (k->s) lt_free(k->s);
    }
    lt_vec_free(&lexed);
    if (!root) {
        if (!p.err[0]) lt_strlcpy(p.err, "Unexpected lt_tok", sizeof p.err);
        if (err && errlen) lt_strlcpy(err, p.err, errlen);
        return NULL;
    }
    e = (lt_expr *)lt_calloc(1, sizeof(lt_expr));
    e->root = root;
    return e;
}

/* ------------------------------------------------------------------ */
/* evaluation                                                          */
/* ------------------------------------------------------------------ */

static int ev(const node *n, const lt_expr_env *env, lt_value *out);

static void set_num(lt_value *v, double d)
{
    lt_value_clear(v);
    v->t = LT_V_NUM;
    v->n = d;
}

static void set_bool(lt_value *v, int b)
{
    lt_value_clear(v);
    v->t = LT_V_BOOL;
    v->b = b ? 1 : 0;
}

static void set_str(lt_value *v, const char *s)
{
    lt_value_clear(v);
    v->t = LT_V_STR;
    v->s = lt_strdup(s ? s : "");
}

static double ev_num(const node *n, const lt_expr_env *env, int *rc)
{
    lt_value v;
    double d;
    lt_value_init(&v);
    if (ev(n, env, &v) != 0) *rc = -1;
    d = lt_value_f64(&v);
    lt_value_clear(&v);
    return d;
}

static double fmin_(double a, double b)
{
    if (lt_isnan(a)) return b;
    if (lt_isnan(b)) return a;
    return a < b ? a : b;
}

static double fmax_(double a, double b)
{
    if (lt_isnan(a)) return b;
    if (lt_isnan(b)) return a;
    return a > b ? a : b;
}

static size_t to_index(double d)
{
    if (lt_isnan(d) || d <= 0) return 0;
    if (d >= 4294967295.0) return (size_t)4294967295UL;
    return (size_t)d;
}

/* name from a bare variable or (if allow_str) a string literal */
static const char *name_arg(const node *a, int allow_str)
{
    if (a->type == N_VAR) return a->s;
    if (allow_str && a->type == N_STR) return a->s;
    return NULL;
}

static int arity_err(const node *n, int want)
{
    lt_err_set("Function %s requires %d argument(s), got %u", n->s, want, (unsigned)n->nargs);
    return -1;
}

static int ev_call(const node *n, const lt_expr_env *env, lt_value *out)
{
    int rc = 0;
    double a = 0, b = 0;
    lt_expr_env empty;
    const lt_expr_env *e = env;
    size_t i;

    if (!e) {
        lt_expr_env_init(&empty);
        e = &empty;
    }
    switch (n->fn) {
    case F_ABS: case F_ROUND: case F_FLOOR: case F_CEIL: case F_SQRT: case F_LOG: case F_EXP:
    case F_SIN: case F_COS: case F_TAN: case F_ASIN: case F_ACOS: case F_ATAN: case F_LOG10:
    case F_RECIP: case F_INT: case F_ISNAN:
        if (n->nargs != 1) return arity_err(n, 1);
        a = ev_num(n->args[0], env, &rc);
        if (rc) return rc;
        switch (n->fn) {
        case F_ABS: set_num(out, fabs(a)); break;
        case F_ROUND: set_num(out, lt_round(a)); break;
        case F_FLOOR: set_num(out, floor(a)); break;
        case F_CEIL: set_num(out, ceil(a)); break;
        case F_SQRT: set_num(out, sqrt(a)); break;
        case F_LOG: set_num(out, log(a)); break;
        case F_EXP: set_num(out, exp(a)); break;
        case F_SIN: set_num(out, sin(a)); break;
        case F_COS: set_num(out, cos(a)); break;
        case F_TAN: set_num(out, tan(a)); break;
        case F_ASIN: set_num(out, asin(a)); break;
        case F_ACOS: set_num(out, acos(a)); break;
        case F_ATAN: set_num(out, atan(a)); break;
        case F_LOG10: {
            double r = a > 0 ? log10(a) : 0.0;
            set_num(out, lt_isfinite(r) ? r : 0.0);
            break;
        }
        case F_RECIP: set_num(out, a == 0.0 ? 0.0 : 1.0 / a); break;
        case F_INT: set_num(out, lt_trunc(a)); break;
        default: set_bool(out, lt_isnan(a)); break;
        }
        return 0;
    case F_POW: case F_ATAN2:
        if (n->nargs != 2) return arity_err(n, 2);
        a = ev_num(n->args[0], env, &rc);
        b = ev_num(n->args[1], env, &rc);
        if (rc) return rc;
        set_num(out, n->fn == F_POW ? pow(a, b) : atan2(a, b));
        return 0;
    case F_MIN: case F_MAX:
        if (n->nargs < 2) {
            lt_err_set("Function %s requires at least 2 arguments, got %u", n->s,
                       (unsigned)n->nargs);
            return -1;
        }
        a = ev_num(n->args[0], env, &rc);
        for (i = 1; i < n->nargs; i++) {
            b = ev_num(n->args[i], env, &rc);
            a = n->fn == F_MIN ? fmin_(a, b) : fmax_(a, b);
        }
        if (rc) return rc;
        set_num(out, a);
        return 0;
    case F_ISADV:
        set_bool(out, 1);
        return 0;
    case F_IF: {
        lt_value c;
        int r;
        if (n->nargs != 3) return arity_err(n, 3);
        lt_value_init(&c);
        r = ev(n->args[0], env, &c);
        if (r != 0) {
            lt_value_clear(&c);
            return r;
        }
        i = lt_value_bool(&c) ? 1 : 2;
        lt_value_clear(&c);
        return ev(n->args[i], env, out);
    }
    case F_NOT: case F_BOOL:
        if (n->nargs != 1) return arity_err(n, 1);
        {
            lt_value c;
            int r;
            lt_value_init(&c);
            r = ev(n->args[0], env, &c);
            if (r == 0) set_bool(out, n->fn == F_NOT ? !lt_value_bool(&c) : lt_value_bool(&c));
            lt_value_clear(&c);
            return r;
        }
    case F_PASTVALUE:
        if (n->nargs != 2) return arity_err(n, 2);
        return ev(n->args[0], env, out);
    case F_GETAUX:
        if (n->nargs != 1) return arity_err(n, 1);
        (void)ev_num(n->args[0], env, &rc);
        if (rc) return rc;
        set_num(out, 0.0);
        return 0;
    case F_BITSTR: {
        const char *nm;
        const char *const *opts;
        size_t cnt = 0, idx;
        if (n->nargs != 2) return arity_err(n, 2);
        nm = name_arg(n->args[0], 0);
        if (!nm) {
            lt_err_set("bitStringValue first argument must be a constant name");
            return -1;
        }
        a = ev_num(n->args[1], env, &rc);
        if (rc) return rc;
        idx = to_index(a);
        opts = e->get_bit_options ? e->get_bit_options(e->ud, nm, &cnt) : NULL;
        if (opts && idx < cnt && opts[idx]) {
            set_str(out, opts[idx]);
        } else {
            char tmp[48];
            lt_snprintf(tmp, sizeof tmp, "INVALID[%lu]", (unsigned long)idx);
            set_str(out, tmp);
        }
        return 0;
    }
    case F_STRVAL: {
        const char *nm, *s = NULL;
        if (n->nargs != 1) return arity_err(n, 1);
        nm = name_arg(n->args[0], 1);
        if (!nm) {
            lt_err_set("stringValue argument must be a constant name");
            return -1;
        }
        if (e->get_string) s = e->get_string(e->ud, nm);
        set_str(out, s ? s : "");
        return 0;
    }
    case F_TABLE: {
        const char *nm;
        double r = 0.0;
        if (n->nargs != 2) return arity_err(n, 2);
        a = ev_num(n->args[0], env, &rc);
        if (rc) return rc;
        nm = name_arg(n->args[1], 1);
        if (!nm) {
            lt_err_set("table second argument must be a file name");
            return -1;
        }
        if (!e->table_lookup || !e->table_lookup(e->ud, nm, a, &r)) r = 0.0;
        set_num(out, r);
        return 0;
    }
    case F_TIMENOW:
        if (n->nargs != 0) return arity_err(n, 0);
        set_num(out, e->time_now ? e->time_now(e->ud) : 0.0);
        return 0;
    case F_ISONLINE:
        if (n->nargs != 0) return arity_err(n, 0);
        set_bool(out, e->is_online ? e->is_online(e->ud) : 0);
        return 0;
    case F_ARRAYVAL: {
        const char *nm;
        double r = 0.0;
        if (n->nargs != 2) return arity_err(n, 2);
        nm = name_arg(n->args[0], 1);
        if (!nm) {
            lt_err_set("arrayValue first argument must be an array name");
            return -1;
        }
        a = ev_num(n->args[1], env, &rc);
        if (rc) return rc;
        if (!e->array_value || !e->array_value(e->ud, nm, a, &r)) r = 0.0;
        set_num(out, r);
        return 0;
    }
    case F_PROJDIR: case F_WORKDIR: {
        const char *s = e->get_dir ? e->get_dir(e->ud, n->fn == F_PROJDIR ? 0 : 1) : NULL;
        if (n->nargs != 0) return arity_err(n, 0);
        set_str(out, s ? s : "");
        return 0;
    }
    default:
        lt_err_set("Unknown function: %s", n->s);
        return -1;
    }
}

static int ev_bin(const node *n, const lt_expr_env *env, lt_value *out)
{
    lt_value l, r;
    int rc;
    double a, b;
    i64v x, y, z;

    lt_value_init(&l);
    lt_value_init(&r);
    rc = ev(n->a, env, &l);
    if (rc == 0) rc = ev(n->b, env, &r);
    if (rc != 0) {
        lt_value_clear(&l);
        lt_value_clear(&r);
        return rc;
    }
    a = lt_value_f64(&l);
    b = lt_value_f64(&r);
    switch (n->op) {
    case OP_ADD:
        if (l.t == LT_V_STR && r.t == LT_V_STR) {
            size_t la = strlen(l.s), lb = strlen(r.s);
            char *s = (char *)lt_malloc(la + lb + 1);
            memcpy(s, l.s, la);
            memcpy(s + la, r.s, lb + 1);
            lt_value_clear(out);
            out->t = LT_V_STR;
            out->s = s;
        } else {
            set_num(out, a + b);
        }
        break;
    case OP_SUB: set_num(out, a - b); break;
    case OP_MUL: set_num(out, a * b); break;
    case OP_DIV: set_num(out, b == 0.0 ? 0.0 : a / b); break;
    case OP_MOD: set_num(out, b == 0.0 ? 0.0 : fmod(a, b)); break;
    case OP_EQ: case OP_NE: {
        int eq;
        if (l.t == LT_V_STR && r.t == LT_V_STR) eq = strcmp(l.s, r.s) == 0;
        else eq = a == b;
        set_bool(out, n->op == OP_EQ ? eq : !eq);
        break;
    }
    case OP_LT: set_bool(out, a < b); break;
    case OP_GT: set_bool(out, a > b); break;
    case OP_LE: set_bool(out, a <= b); break;
    case OP_GE: set_bool(out, a >= b); break;
    case OP_AND: set_bool(out, lt_value_bool(&l) && lt_value_bool(&r)); break;
    case OP_OR: set_bool(out, lt_value_bool(&l) || lt_value_bool(&r)); break;
    case OP_BAND: case OP_BOR: case OP_BXOR:
        x = i64_from_d(a);
        y = i64_from_d(b);
        if (n->op == OP_BAND) { z.hi = x.hi & y.hi; z.lo = x.lo & y.lo; }
        else if (n->op == OP_BOR) { z.hi = x.hi | y.hi; z.lo = x.lo | y.lo; }
        else { z.hi = x.hi ^ y.hi; z.lo = x.lo ^ y.lo; }
        set_num(out, i64_to_d(z));
        break;
    case OP_SHL: case OP_SHR:
        if (!lt_isfinite(b) || b < 0 || b >= 64) {
            set_num(out, 0.0);
        } else {
            int cnt = (int)b;
            x = i64_from_d(a);
            z = n->op == OP_SHL ? i64_shl(x, cnt) : i64_shr(x, cnt);
            set_num(out, i64_to_d(z));
        }
        break;
    default:
        set_num(out, 0.0);
        break;
    }
    lt_value_clear(&l);
    lt_value_clear(&r);
    return 0;
}

static int ev(const node *n, const lt_expr_env *env, lt_value *out)
{
    switch (n->type) {
    case N_NUM:
        set_num(out, n->num);
        return 0;
    case N_BOOL:
        set_bool(out, n->boolean);
        return 0;
    case N_STR:
        set_str(out, n->s);
        return 0;
    case N_VAR: {
        double v = 0.0;
        if (!env || !env->get_var || !env->get_var(env->ud, n->s, &v)) v = 0.0;
        set_num(out, v);
        return 0;
    }
    case N_BIN:
        return ev_bin(n, env, out);
    case N_TERN: {
        lt_value c;
        int rc;
        lt_value_init(&c);
        rc = ev(n->a, env, &c);
        if (rc == 0) rc = ev(lt_value_bool(&c) ? n->b : n->c, env, out);
        lt_value_clear(&c);
        return rc;
    }
    case N_UN: {
        lt_value v;
        int rc;
        lt_value_init(&v);
        rc = ev(n->a, env, &v);
        if (rc == 0) {
            if (n->op == UN_NEG) {
                set_num(out, -lt_value_f64(&v));
            } else if (n->op == UN_NOT) {
                set_bool(out, !lt_value_bool(&v));
            } else {
                i64v x = i64_from_d(lt_value_f64(&v));
                x.hi = ~x.hi & LT_U32_MASK;
                x.lo = ~x.lo & LT_U32_MASK;
                set_num(out, i64_to_d(x));
            }
        }
        lt_value_clear(&v);
        return rc;
    }
    case N_CALL:
        return ev_call(n, env, out);
    }
    return -1;
}

int lt_expr_eval(const lt_expr *e, const lt_expr_env *env, lt_value *out)
{
    lt_value tmp;
    int rc;

    lt_value_init(out); /* caller must have cleared any previous string */
    if (!e || !e->root) {
        lt_err_set("empty expression");
        return LT_EDATA;
    }
    lt_value_init(&tmp);
    rc = ev(e->root, env, &tmp);
    if (rc != 0) {
        lt_value_clear(&tmp);
        lt_value_init(out);
        return LT_EDATA;
    }
    *out = tmp;
    return 0;
}

int lt_expr_eval_f64(const lt_expr *e, const lt_expr_env *env, double *out)
{
    lt_value v;
    int rc;
    lt_value_init(&v);
    rc = lt_expr_eval(e, env, &v);
    *out = rc == 0 ? lt_value_f64(&v) : 0.0;
    lt_value_clear(&v);
    return rc;
}

int lt_expr_eval_bool(const lt_expr *e, const lt_expr_env *env, lt_bool *out)
{
    lt_value v;
    int rc;
    lt_value_init(&v);
    rc = lt_expr_eval(e, env, &v);
    *out = rc == 0 ? lt_value_bool(&v) : LT_FALSE;
    lt_value_clear(&v);
    return rc;
}

/* ------------------------------------------------------------------ */
/* env                                                                 */
/* ------------------------------------------------------------------ */

void lt_expr_env_init(lt_expr_env *e)
{
    memset(e, 0, sizeof *e);
}

static lt_bool map_get_var(void *ud, const char *name, double *out)
{
    const lt_map *m = (const lt_map *)ud;
    const double *p = (const double *)lt_map_get(m, name);
    if (!p) return LT_FALSE;
    *out = *p;
    return LT_TRUE;
}

void lt_expr_env_from_map(lt_expr_env *e, const lt_map *vars)
{
    lt_expr_env_init(e);
    e->ud = (void *)vars;
    e->get_var = map_get_var;
}

/* ------------------------------------------------------------------ */
/* analysis helpers                                                    */
/* ------------------------------------------------------------------ */

static char lt_placeholder_;

static void collect(const node *n, lt_map *set)
{
    size_t i;
    if (!n) return;
    if (n->type == N_VAR) lt_map_put(set, n->s, &lt_placeholder_);
    collect(n->a, set);
    collect(n->b, set);
    collect(n->c, set);
    for (i = 0; i < n->nargs; i++) collect(n->args[i], set);
}

void lt_expr_collect_vars(const lt_expr *e, lt_map *set)
{
    if (e) collect(e->root, set);
}

size_t lt_expr_identifiers(const char *src, char ***names_out)
{
    lt_map seen;
    lt_vec out;
    size_t i = 0, n, cnt;
    char **arr;

    lt_map_init(&seen, LT_FALSE);
    lt_vec_init(&out, sizeof(char *));
    n = src ? strlen(src) : 0;
    while (i < n) {
        int c = (unsigned char)src[i];
        if (is_alpha_(c) || is_digit_(c)) {
            size_t st = i;
            int lead = is_alpha_(c);
            while (i < n && (is_alpha_((unsigned char)src[i]) || is_digit_((unsigned char)src[i])))
                i++;
            if (lead) {
                char *id = lt_strndup(src + st, i - st);
                if (!lt_map_has(&seen, id)) {
                    lt_map_put(&seen, id, &lt_placeholder_);
                    *(char **)lt_vec_push(&out) = id;
                } else {
                    lt_free(id);
                }
            }
        } else {
            i++;
        }
    }
    lt_map_free(&seen);
    cnt = out.len;
    arr = (char **)out.data;
    if (cnt == 0) {
        lt_vec_free(&out);
        arr = NULL;
    }
    if (names_out) *names_out = arr;
    else lt_expr_names_free(arr, cnt);
    return cnt;
}

void lt_expr_names_free(char **names, size_t n)
{
    size_t i;
    if (!names) return;
    for (i = 0; i < n; i++) lt_free(names[i]);
    lt_free(names);
}

/* ------------------------------------------------------------------ */
/* one-shot helpers                                                    */
/* ------------------------------------------------------------------ */

static lt_bool test_with(const lt_expr *e, const lt_expr_env *env)
{
    lt_bool b;
    if (!e) return LT_TRUE;
    if (lt_expr_eval_bool(e, env, &b) != 0) return LT_TRUE;
    return b;
}

lt_bool lt_expr_test(const char *cond, const lt_expr_env *env)
{
    lt_expr *e;
    lt_bool r;
    char err[96];

    if (!cond || !cond[0]) return LT_TRUE;
    e = lt_expr_compile(cond, err, sizeof err);
    if (!e) return LT_TRUE;
    r = test_with(e, env);
    lt_expr_free(e);
    return r;
}

/* returns inner text (trimmed, malloc'd) if raw is exactly "{ ... }", else NULL */
static char *braced_inner(const char *raw)
{
    char *t = lt_strdup(raw);
    size_t n;
    char *inner;

    lt_str_trim(t);
    n = strlen(t);
    if (n < 2 || t[0] != '{' || t[n - 1] != '}') {
        lt_free(t);
        return NULL;
    }
    t[n - 1] = '\0';
    inner = lt_strdup(t + 1);
    lt_free(t);
    lt_str_trim(inner);
    return inner;
}

static char *display_with(const lt_expr *e, const char *raw, const lt_expr_env *env)
{
    lt_value v;
    char *r;
    if (!e) return lt_strdup(raw);
    lt_value_init(&v);
    if (lt_expr_eval(e, env, &v) != 0) {
        lt_value_clear(&v);
        return lt_strdup(raw);
    }
    r = lt_value_to_string(&v);
    lt_value_clear(&v);
    return r;
}

char *lt_expr_display(const char *raw, const lt_expr_env *env)
{
    char *inner;
    lt_expr *e;
    char *r;
    char err[96];

    if (!raw) return lt_strdup("");
    inner = braced_inner(raw);
    if (!inner) return lt_strdup(raw);
    if (!inner[0]) {
        lt_free(inner);
        return lt_strdup(raw);
    }
    e = lt_expr_compile(inner, err, sizeof err);
    lt_free(inner);
    r = display_with(e, raw, env);
    lt_expr_free(e);
    return r;
}

/* ------------------------------------------------------------------ */
/* cache                                                               */
/* ------------------------------------------------------------------ */

static char cache_invalid_;

void lt_expr_cache_init(lt_expr_cache *c)
{
    lt_map_init(&c->map, LT_FALSE);
}

void lt_expr_cache_free(lt_expr_cache *c)
{
    size_t i;
    for (i = 0; i < c->map.cap; i++) {
        if (c->map.slots[i].used == 1 && c->map.slots[i].val != (void *)&cache_invalid_)
            lt_expr_free((lt_expr *)c->map.slots[i].val);
    }
    lt_map_free(&c->map);
}

const lt_expr *lt_expr_cache_get(lt_expr_cache *c, const char *src)
{
    void *v;
    lt_expr *e;
    char err[96];

    if (!src || !src[0]) return NULL;
    v = lt_map_get(&c->map, src);
    if (v) return v == (void *)&cache_invalid_ ? NULL : (const lt_expr *)v;
    e = lt_expr_compile(src, err, sizeof err);
    lt_map_put(&c->map, src, e ? (void *)e : (void *)&cache_invalid_);
    return e;
}

lt_bool lt_expr_cache_test(lt_expr_cache *c, const char *cond, const lt_expr_env *env)
{
    return test_with(lt_expr_cache_get(c, cond), env);
}

double lt_expr_cache_f64(lt_expr_cache *c, const char *src, const lt_expr_env *env, double dflt)
{
    const lt_expr *e = lt_expr_cache_get(c, src);
    double v;
    if (!e) return dflt;
    if (lt_expr_eval_f64(e, env, &v) != 0) return dflt;
    return v;
}

char *lt_expr_cache_display(lt_expr_cache *c, const char *raw, const lt_expr_env *env)
{
    char *inner;
    const lt_expr *e;
    char *r;

    if (!raw) return lt_strdup("");
    inner = braced_inner(raw);
    if (!inner) return lt_strdup(raw);
    if (!inner[0]) {
        lt_free(inner);
        return lt_strdup(raw);
    }
    e = lt_expr_cache_get(c, inner);
    lt_free(inner);
    r = display_with(e, raw, env);
    return r;
}
