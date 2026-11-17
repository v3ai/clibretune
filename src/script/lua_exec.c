/* lua_exec.c - tree-walking evaluator, calls, errors and metamethods.
 * Values and call arguments live on one fixed value stack (never reallocated, so pointers into it
 * stay valid); errors unwind with longjmp to the nearest pcall or to the run. Every evaluated
 * node and loop iteration counts one step against the instruction budget.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include "lua_priv.h"

#define LL_MAX_EVAL 2200      /* nested evaluator frames (C stack guard) */

static lt_lv do_index(lt_lua *st, lt_lv obj, lt_lv key, const lt_lnode *desc);
static lt_lv eval(lt_lua *st, lt_lframe *fr, lt_lnode *e);
static int exec_block(lt_lua *st, lt_lframe *fr, lt_lnode *b);

/* ------------------------------------------------------------------ errors */
static void throw_err(lt_lua *st)
{
    if (st->jb == NULL) { return; }
    longjmp(*st->jb, 1);
}

void lt_luai_error_plain(lt_lua *st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    lt_vsnprintf(st->errmsg, sizeof(st->errmsg), fmt, ap);
    va_end(ap);
    st->errval = lt_luai_strv(lt_luai_str_new(st, st->errmsg, strlen(st->errmsg)));
    st->err_has_val = 1;
    throw_err(st);
}

void lt_luai_throw_value(lt_lua *st, lt_lv v)
{
    st->errval = v;
    st->err_has_val = 1;
    if (v.t == LV_STR) { lt_strlcpy(st->errmsg, v.u.s->s, sizeof(st->errmsg)); }
    else if (v.t == LV_NUM) { lt_lua_fmt_number(st->errmsg, sizeof(st->errmsg), v.u.n); }
    else {
        lt_snprintf(st->errmsg, sizeof(st->errmsg), "%s:%d: (error object is a %s value)",
                    st->chunk, st->line, lt_luai_typename(v));
    }
    throw_err(st);
}

void lt_luai_error(lt_lua *st, const char *fmt, ...)
{
    char body[300];
    va_list ap;
    va_start(ap, fmt);
    lt_vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    lt_luai_error_plain(st, "%s:%d: %s", st->chunk, st->line, body);
}

void lt_luai_fatal(lt_lua *st, const char *fmt, ...)
{
    char body[300];
    va_list ap;
    va_start(ap, fmt);
    lt_vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    lt_snprintf(st->errmsg, sizeof(st->errmsg), "%s:%d: %s", st->chunk != NULL ? st->chunk : "?",
                st->line, body);
    st->fatal = 1;
    st->errval = lt_luai_nil();
    st->err_has_val = 0;
    throw_err(st);
}

const char *lt_luai_typename(lt_lv v)
{
    switch (v.t) {
    case LV_NIL: return "nil";
    case LV_BOOL: return "boolean";
    case LV_NUM: return "number";
    case LV_STR: return "string";
    case LV_TAB: return "table";
    default: return "function";
    }
}

static void describe(const lt_lnode *n, char *out, size_t cap)
{
    out[0] = '\0';
    if (n == NULL) { return; }
    switch (n->kind) {
    case N_LOCAL: lt_snprintf(out, cap, " (local '%s')", n->str->s); break;
    case N_UPVAL: lt_snprintf(out, cap, " (upvalue '%s')", n->str->s); break;
    case N_GLOBAL: lt_snprintf(out, cap, " (global '%s')", n->str->s); break;
    case N_METHOD: lt_snprintf(out, cap, " (method '%s')", n->str->s); break;
    case N_INDEX:
        if (n->b != NULL && n->b->kind == N_STR) { lt_snprintf(out, cap, " (field '%s')", n->b->str->s); }
        break;
    default: break;
    }
}

static void type_error(lt_lua *st, const char *what, lt_lv v, const lt_lnode *desc)
{
    char d[96];
    describe(desc, d, sizeof(d));
    lt_luai_error(st, "attempt to %s a %s value%s", what, lt_luai_typename(v), d);
}

/* ------------------------------------------------------------------ stack */
void lt_luai_push(lt_lua *st, lt_lv v)
{
    if (st->sp >= LL_STACK_MAX) { lt_luai_error(st, "stack overflow"); }
    st->stack[st->sp++] = v;
}

void lt_luai_step(lt_lua *st)
{
    if (++st->steps > st->max_steps) { lt_luai_fatal(st, "instruction limit exceeded"); }
}

void lt_luai_charge(lt_lua *st, unsigned long units)
{
    if (units > st->max_steps || st->steps > st->max_steps - units) {
        lt_luai_fatal(st, "instruction limit exceeded");
    }
    st->steps += units;
}
#define step(st) lt_luai_step(st)

/* ------------------------------------------------------------------ conversions */
static int truthy(lt_lv v) { return !(v.t == LV_NIL || (v.t == LV_BOOL && v.u.b == 0)); }

int lt_luai_tonum(lt_lv v, double *out)
{
    if (v.t == LV_NUM) { *out = v.u.n; return 1; }
    if (v.t == LV_STR) { return lt_lua_parse_number(v.u.s->s, v.u.s->len, out); }
    return 0;
}

static lt_lv getmeta(lt_lv v, const lt_lstr *name)
{
    if (v.t == LV_TAB && v.u.tab->meta != NULL) {
        return lt_luai_tab_getstr(v.u.tab->meta, (lt_lstr *)name);
    }
    return lt_luai_nil();
}

static int is_callable(lt_lv v) { return v.t == LV_FUN || v.t == LV_CFN; }

lt_lstr *lt_luai_tostr(lt_lua *st, lt_lv v)
{
    char buf[64];
    switch (v.t) {
    case LV_NIL: return lt_luai_str_new(st, "nil", 3);
    case LV_BOOL: return v.u.b ? lt_luai_str_new(st, "true", 4) : lt_luai_str_new(st, "false", 5);
    case LV_NUM:
        lt_lua_fmt_number(buf, sizeof(buf), v.u.n);
        return lt_luai_str_new(st, buf, strlen(buf));
    case LV_STR: return v.u.s;
    case LV_TAB: {
        lt_lv h = getmeta(v, st->s_tostring);
        if (h.t != LV_NIL) {
            lt_lv r = lt_luai_call1(st, h, &v, 1);
            if (r.t != LV_STR) { lt_luai_error(st, "'__tostring' must return a string"); }
            return r.u.s;
        }
        lt_snprintf(buf, sizeof(buf), "table: 0x%06lx", v.u.tab->o.id);
        return lt_luai_str_new(st, buf, strlen(buf));
    }
    case LV_FUN:
        lt_snprintf(buf, sizeof(buf), "function: 0x%06lx", v.u.f->o.id);
        return lt_luai_str_new(st, buf, strlen(buf));
    default:
        lt_snprintf(buf, sizeof(buf), "function: builtin: %s", v.u.c->name != NULL ? v.u.c->name : "?");
        return lt_luai_str_new(st, buf, strlen(buf));
    }
}

void lt_luai_check_key(lt_lua *st, lt_lv k)
{
    if (k.t == LV_NIL) { lt_luai_error(st, "table index is nil"); }
    if (k.t == LV_NUM && k.u.n != k.u.n) { lt_luai_error(st, "table index is NaN"); }
}

/* ------------------------------------------------------------------ index */
lt_lv lt_luai_index(lt_lua *st, lt_lv obj, lt_lv key) { return do_index(st, obj, key, NULL); }

static lt_lv do_index(lt_lua *st, lt_lv obj, lt_lv key, const lt_lnode *desc)
{
    int loop;
    for (loop = 0; loop < 100; loop++) {
        lt_lv h;
        if (obj.t == LV_TAB) {
            lt_lv v = lt_luai_tab_get(obj.u.tab, key);
            if (v.t != LV_NIL || obj.u.tab->meta == NULL) { return v; }
            h = lt_luai_tab_getstr(obj.u.tab->meta, st->s_index);
            if (h.t == LV_NIL) { return v; }
        } else if (obj.t == LV_STR) {
            return lt_luai_tab_get(st->slib, key);
        } else {
            type_error(st, "index", obj, loop == 0 ? desc : NULL);
            return lt_luai_nil();
        }
        if (is_callable(h)) {
            lt_lv args[2];
            args[0] = obj;
            args[1] = key;
            return lt_luai_call1(st, h, args, 2);
        }
        obj = h;
    }
    lt_luai_error(st, "'__index' chain too long; possible loop");
    return lt_luai_nil();
}

static void do_setindex(lt_lua *st, lt_lv obj, lt_lv key, lt_lv val, const lt_lnode *desc);

void lt_luai_setindex(lt_lua *st, lt_lv obj, lt_lv key, lt_lv val)
{
    do_setindex(st, obj, key, val, NULL);
}

static void do_setindex(lt_lua *st, lt_lv obj, lt_lv key, lt_lv val, const lt_lnode *desc)
{
    int loop;
    for (loop = 0; loop < 100; loop++) {
        lt_lv h;
        if (obj.t != LV_TAB) { type_error(st, "index", obj, loop == 0 ? desc : NULL); return; }
        if (obj.u.tab->meta == NULL ||
            lt_luai_tab_get(obj.u.tab, key).t != LV_NIL ||
            (h = lt_luai_tab_getstr(obj.u.tab->meta, st->s_newindex)).t == LV_NIL) {
            lt_luai_check_key(st, key);
            lt_luai_tab_set(st, obj.u.tab, key, val);
            return;
        }
        if (is_callable(h)) {
            lt_lv args[3];
            args[0] = obj;
            args[1] = key;
            args[2] = val;
            (void)lt_luai_call1(st, h, args, 3);
            return;
        }
        obj = h;
    }
    lt_luai_error(st, "'__newindex' chain too long; possible loop");
}

/* ------------------------------------------------------------------ calls */
static int call_c(lt_lua *st, lt_lcfun *f, int a, int n)
{
    int nr;
    st->sp = a + n;
    st->curname = f->name;
    nr = f->fn(st, a, n, f->ud);
    if (nr < 0) { nr = 0; }
    if (nr > st->sp - a) { nr = st->sp - a; }
    if (nr > 0) { memmove(&st->stack[a], &st->stack[st->sp - nr], (size_t)nr * sizeof(lt_lv)); }
    st->sp = a + nr;
    return nr;
}

static lt_lv read_var(const lt_lframe *fr, const lt_lvar *v)
{
    lt_lv s = fr->slots[v->slot];
    return v->captured && s.t == LV_CELL ? s.u.cell->v : s;
}

static void write_var(lt_lua *st, lt_lframe *fr, const lt_lvar *v, lt_lv val)
{
    lt_lv *s = &fr->slots[v->slot];
    LT_UNUSED(st);
    if (v->captured && s->t == LV_CELL) { s->u.cell->v = val; } else { *s = val; }
}

/* a fresh variable (new cell when closures capture it) */
static void declare(lt_lua *st, lt_lframe *fr, const lt_lvar *v, lt_lv val)
{
    if (v->captured) {
        lt_lv c;
        c.t = LV_CELL;
        c.u.n = 0.0;
        c.u.cell = lt_luai_cell_new(st, val);
        fr->slots[v->slot] = c;
    } else {
        fr->slots[v->slot] = val;
    }
}

static int call_lua(lt_lua *st, lt_lfun *f, int a, int n)
{
    lt_lproto *p = f->p;
    lt_lframe fr;
    lt_lvar *pv;
    int i, rc, sbase = a + n, nres;
    if (st->depth >= LL_MAX_CALLS) { lt_luai_error(st, "stack overflow"); }
    if (sbase + p->nslots + 8 >= LL_STACK_MAX) { lt_luai_error(st, "stack overflow"); }
    st->sp = sbase + p->nslots;
    fr.slots = &st->stack[sbase];
    for (i = 0; i < p->nslots; i++) { fr.slots[i] = lt_luai_nil(); }
    fr.fn = f;
    fr.va = NULL;
    fr.nva = 0;
    fr.rbase = 0;
    fr.rn = 0;
    if (p->is_vararg && n > p->nparams) {
        fr.va = &st->stack[a + p->nparams];
        fr.nva = n - p->nparams;
    }
    st->depth++;
    for (pv = p->params, i = 0; pv != NULL; pv = pv->next, i++) {
        declare(st, &fr, pv, i < n ? st->stack[a + i] : lt_luai_nil());
    }
    rc = exec_block(st, &fr, p->body);
    st->depth--;
    if (rc == 2) {
        nres = fr.rn;
        if (nres > 0) { memmove(&st->stack[a], &st->stack[fr.rbase], (size_t)nres * sizeof(lt_lv)); }
    } else {
        nres = 0;
    }
    st->sp = a + nres;
    return nres;
}

int lt_luai_call(lt_lua *st, lt_lv fn, int a, int n)
{
    if (fn.t == LV_FUN) { return call_lua(st, fn.u.f, a, n); }
    if (fn.t == LV_CFN) { return call_c(st, fn.u.c, a, n); }
    {
        lt_lv h = getmeta(fn, st->s_call);
        if (is_callable(h)) {
            if (a + n + 2 >= LL_STACK_MAX) { lt_luai_error(st, "stack overflow"); }
            memmove(&st->stack[a + 1], &st->stack[a], (size_t)n * sizeof(lt_lv));
            st->stack[a] = fn;
            st->sp = a + n + 1;
            return lt_luai_call(st, h, a, n + 1);
        }
    }
    type_error(st, "call", fn, NULL);
    return 0;
}

lt_lv lt_luai_call1(lt_lua *st, lt_lv fn, const lt_lv *args, int n)
{
    int a = st->sp, i, r;
    lt_lv v;
    for (i = 0; i < n; i++) { lt_luai_push(st, args[i]); }
    r = lt_luai_call(st, fn, a, n);
    v = r > 0 ? st->stack[a] : lt_luai_nil();
    st->sp = a;
    return v;
}

/* ------------------------------------------------------------------ operators */
static double lmod(double a, double b)
{
    double r;
    if (b == 0.0) { return fmod(a, b); }
    r = fmod(a, b);
    if (r != 0.0 && ((r < 0.0) != (b < 0.0))) { r += b; }
    return r;
}

static double arith_num(int op, double a, double b)
{
    switch (op) {
    case OP_ADD: return a + b;
    case OP_SUB: return a - b;
    case OP_MUL: return a * b;
    case OP_DIV: return a / b;
    case OP_MOD: return lmod(a, b);
    case OP_POW: return pow(a, b);
    default: return floor(a / b);   /* OP_IDIV */
    }
}

static lt_lv arith(lt_lua *st, int op, lt_lv a, lt_lv b, const lt_lnode *na, const lt_lnode *nb)
{
    double x, y;
    lt_lv h;
    if (lt_luai_tonum(a, &x) && lt_luai_tonum(b, &y)) { return lt_luai_num(arith_num(op, x, y)); }
    h = getmeta(a, st->s_arith[op - 1]);
    if (h.t == LV_NIL) { h = getmeta(b, st->s_arith[op - 1]); }
    if (h.t != LV_NIL) {
        lt_lv args[2];
        args[0] = a;
        args[1] = b;
        return lt_luai_call1(st, h, args, 2);
    }
    if (lt_luai_tonum(a, &x)) { type_error(st, "perform arithmetic on", b, nb); }
    else { type_error(st, "perform arithmetic on", a, na); }
    return lt_luai_nil();
}

static int str_cmp(const lt_lstr *a, const lt_lstr *b)
{
    size_t n = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->s, b->s, n);
    if (c != 0) { return c; }
    return a->len < b->len ? -1 : (a->len > b->len ? 1 : 0);
}

static int values_equal(lt_lua *st, lt_lv a, lt_lv b)
{
    if (lt_luai_raw_equal(a, b)) { return 1; }
    if (a.t == LV_TAB && b.t == LV_TAB) {
        lt_lv h = getmeta(a, st->s_eq);
        if (h.t == LV_NIL) { h = getmeta(b, st->s_eq); }
        if (h.t != LV_NIL) {
            lt_lv args[2];
            args[0] = a;
            args[1] = b;
            return truthy(lt_luai_call1(st, h, args, 2));
        }
    }
    return 0;
}

static void compare_error(lt_lua *st, lt_lv a, lt_lv b)
{
    const char *ta = lt_luai_typename(a), *tb = lt_luai_typename(b);
    if (strcmp(ta, tb) == 0) { lt_luai_error(st, "attempt to compare two %s values", ta); }
    lt_luai_error(st, "attempt to compare %s with %s", ta, tb);
}

/* a < b (le = 0) or a <= b (le = 1) */
static int compare(lt_lua *st, lt_lv a, lt_lv b, int le)
{
    lt_lv h;
    if (a.t == LV_NUM && b.t == LV_NUM) { return le ? a.u.n <= b.u.n : a.u.n < b.u.n; }
    if (a.t == LV_STR && b.t == LV_STR) {
        int c = str_cmp(a.u.s, b.u.s);
        return le ? c <= 0 : c < 0;
    }
    h = getmeta(a, le ? st->s_le : st->s_lt);
    if (h.t == LV_NIL) { h = getmeta(b, le ? st->s_le : st->s_lt); }
    if (h.t != LV_NIL) {
        lt_lv args[2];
        args[0] = a;
        args[1] = b;
        return truthy(lt_luai_call1(st, h, args, 2));
    }
    compare_error(st, a, b);
    return 0;
}

int lt_luai_lessthan(lt_lua *st, lt_lv a, lt_lv b) { return compare(st, a, b, 0); }

static lt_lv concat(lt_lua *st, lt_lv a, lt_lv b, const lt_lnode *na, const lt_lnode *nb)
{
    char b1[40], b2[40];
    const char *s1, *s2;
    size_t l1, l2;
    lt_lstr *r;
    if ((a.t == LV_STR || a.t == LV_NUM) && (b.t == LV_STR || b.t == LV_NUM)) {
        if (a.t == LV_STR) { s1 = a.u.s->s; l1 = a.u.s->len; }
        else { lt_lua_fmt_number(b1, sizeof(b1), a.u.n); s1 = b1; l1 = strlen(b1); }
        if (b.t == LV_STR) { s2 = b.u.s->s; l2 = b.u.s->len; }
        else { lt_lua_fmt_number(b2, sizeof(b2), b.u.n); s2 = b2; l2 = strlen(b2); }
        if (l1 > LL_MAX_STR || l2 > LL_MAX_STR) { lt_luai_fatal(st, "string too large"); }
        r = lt_luai_str_buf(st, l1 + l2);
        memcpy(r->s, s1, l1);
        memcpy(r->s + l1, s2, l2);
        r->s[l1 + l2] = '\0';
        return lt_luai_strv(r);
    }
    {
        lt_lv h = getmeta(a, st->s_arith[7]);
        if (h.t == LV_NIL) { h = getmeta(b, st->s_arith[7]); }
        if (h.t != LV_NIL) {
            lt_lv args[2];
            args[0] = a;
            args[1] = b;
            return lt_luai_call1(st, h, args, 2);
        }
    }
    if (a.t == LV_STR || a.t == LV_NUM) { type_error(st, "concatenate", b, nb); }
    else { type_error(st, "concatenate", a, na); }
    return lt_luai_nil();
}

/* ------------------------------------------------------------------ expressions */
static int is_multi(const lt_lnode *e)
{
    return e->kind == N_CALL || e->kind == N_METHOD || e->kind == N_VARARG;
}

/* runs a call node; results stay at stack[*abase..sp) */
static int call_node(lt_lua *st, lt_lframe *fr, lt_lnode *e, int *abase)
{
    lt_lv fv;
    lt_lnode *arg;
    int a, n, r;
    if (e->kind == N_METHOD) {
        lt_lv obj = eval(st, fr, e->a);
        st->line = e->line;
        fv = do_index(st, obj, lt_luai_strv(e->str), e->a);
        a = st->sp;
        lt_luai_push(st, obj);
        if (!is_callable(fv) && getmeta(fv, st->s_call).t == LV_NIL) {
            type_error(st, "call", fv, e);
        }
    } else {
        fv = eval(st, fr, e->a);
        a = st->sp;
        if (!is_callable(fv) && getmeta(fv, st->s_call).t == LV_NIL) {
            st->line = e->line;
            type_error(st, "call", fv, e->a);
        }
    }
    for (arg = e->list; arg != NULL; arg = arg->next) {
        if (arg->next == NULL && is_multi(arg)) {
            if (arg->kind == N_VARARG) {
                int i;
                for (i = 0; i < fr->nva; i++) { lt_luai_push(st, fr->va[i]); }
            } else {
                int b2;
                (void)call_node(st, fr, arg, &b2);   /* results become the trailing arguments */
            }
        } else {
            lt_luai_push(st, eval(st, fr, arg));
        }
    }
    n = st->sp - a;
    st->line = e->line;
    r = lt_luai_call(st, fv, a, n);
    *abase = a;
    return r;
}

static lt_lv make_closure(lt_lua *st, lt_lframe *fr, lt_lproto *p)
{
    lt_lfun *f = lt_luai_fun_new(st, p, p->nups);
    lt_lv v;
    int i;
    for (i = 0; i < p->nups; i++) {
        const lt_lupdesc *d = &p->ups[i];
        if (d->from_local) {
            lt_lv *s = &fr->slots[d->idx];
            if (s->t != LV_CELL) {
                lt_lv c;
                c.t = LV_CELL;
                c.u.n = 0.0;
                c.u.cell = lt_luai_cell_new(st, *s);
                *s = c;
            }
            f->ups[i] = s->u.cell;
        } else {
            f->ups[i] = fr->fn->ups[d->idx];
        }
    }
    v.t = LV_FUN;
    v.u.n = 0.0;
    v.u.f = f;
    return v;
}

static lt_lv eval_table(lt_lua *st, lt_lframe *fr, lt_lnode *e)
{
    lt_ltab *t = lt_luai_tab_new(st);
    lt_lnode *it;
    size_t pos = 0;
    for (it = e->list; it != NULL; it = it->next) { if (it->a == NULL) { pos++; } }
    lt_luai_tab_reserve(st, t, pos);
    for (it = e->list; it != NULL; it = it->next) {
        step(st);
        if (it->a != NULL) {
            lt_lv k = eval(st, fr, it->a);
            lt_lv v = eval(st, fr, it->b);
            st->line = it->line;
            lt_luai_check_key(st, k);
            lt_luai_tab_set(st, t, k, v);
        } else if (it->next == NULL && is_multi(it->b)) {
            int base = st->sp, i, cnt;
            if (it->b->kind == N_VARARG) {
                for (i = 0; i < fr->nva; i++) { lt_luai_push(st, fr->va[i]); }
            } else {
                (void)call_node(st, fr, it->b, &base);
            }
            cnt = st->sp - base;
            for (i = 0; i < cnt; i++) {
                lt_luai_tab_append(st, t, st->stack[base + i]);
            }
            st->sp = base;
        } else {
            lt_lv v = eval(st, fr, it->b);
            lt_luai_tab_append(st, t, v);
        }
    }
    lt_luai_tab_trim(t);
    return lt_luai_tabv(t);
}

static lt_lv eval_bin(lt_lua *st, lt_lframe *fr, lt_lnode *e)
{
    lt_lv a = eval(st, fr, e->a);
    lt_lv b = eval(st, fr, e->b);
    st->line = e->line;
    switch (e->op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW: case OP_IDIV:
        if (a.t == LV_NUM && b.t == LV_NUM) { return lt_luai_num(arith_num(e->op, a.u.n, b.u.n)); }
        return arith(st, e->op, a, b, e->a, e->b);
    case OP_CONCAT: return concat(st, a, b, e->a, e->b);
    case OP_EQ: return lt_luai_bool(values_equal(st, a, b));
    case OP_NE: return lt_luai_bool(!values_equal(st, a, b));
    case OP_LT: return lt_luai_bool(compare(st, a, b, 0));
    case OP_LE: return lt_luai_bool(compare(st, a, b, 1));
    case OP_GT: return lt_luai_bool(compare(st, b, a, 0));
    default: return lt_luai_bool(compare(st, b, a, 1));   /* OP_GE */
    }
}

static lt_lv eval_un(lt_lua *st, lt_lframe *fr, lt_lnode *e)
{
    lt_lv a = eval(st, fr, e->a);
    st->line = e->line;
    if (e->op == OP_NOT) { return lt_luai_bool(!truthy(a)); }
    if (e->op == OP_NEG) {
        double x;
        if (lt_luai_tonum(a, &x)) { return lt_luai_num(-x); }
        {
            lt_lv h = getmeta(a, st->s_unm);
            if (h.t != LV_NIL) {
                lt_lv args[2];
                args[0] = a;
                args[1] = a;
                return lt_luai_call1(st, h, args, 2);
            }
        }
        type_error(st, "perform arithmetic on", a, e->a);
    }
    /* OP_LEN */
    if (a.t == LV_STR) { return lt_luai_num((double)a.u.s->len); }
    if (a.t == LV_TAB) {
        lt_lv h = getmeta(a, st->s_len);
        if (h.t != LV_NIL) { return lt_luai_call1(st, h, &a, 1); }
        return lt_luai_num((double)lt_luai_tab_len(a.u.tab));
    }
    type_error(st, "get length of", a, e->a);
    return lt_luai_nil();
}

static lt_lv eval(lt_lua *st, lt_lframe *fr, lt_lnode *e)
{
    lt_lv v;
    step(st);
    if (st->evd >= LL_MAX_EVAL) { lt_luai_error(st, "stack overflow (expression too deep)"); }
    st->evd++;
    switch (e->kind) {
    case N_NIL: v = lt_luai_nil(); break;
    case N_TRUE: v = lt_luai_bool(1); break;
    case N_FALSE: v = lt_luai_bool(0); break;
    case N_NUM: v = lt_luai_num(e->num); break;
    case N_STR: v = lt_luai_strv(e->str); break;
    case N_VARARG: v = fr->nva > 0 ? fr->va[0] : lt_luai_nil(); break;
    case N_LOCAL: v = read_var(fr, e->var); break;
    case N_UPVAL: v = fr->fn->ups[e->upidx]->v; break;
    case N_GLOBAL: v = lt_luai_tab_getstr(st->globals, e->str); break;
    case N_INDEX: {
        lt_lv obj = eval(st, fr, e->a);
        lt_lv key = eval(st, fr, e->b);
        st->line = e->line;
        v = do_index(st, obj, key, e->a);
        break;
    }
    case N_CALL: case N_METHOD: {
        int a, r;
        r = call_node(st, fr, e, &a);
        v = r > 0 ? st->stack[a] : lt_luai_nil();
        st->sp = a;
        break;
    }
    case N_FUNC: v = make_closure(st, fr, e->proto); break;
    case N_BIN: v = eval_bin(st, fr, e); break;
    case N_UN: v = eval_un(st, fr, e); break;
    case N_AND:
        v = eval(st, fr, e->a);
        if (truthy(v)) { v = eval(st, fr, e->b); }
        break;
    case N_OR:
        v = eval(st, fr, e->a);
        if (!truthy(v)) { v = eval(st, fr, e->b); }
        break;
    case N_TABLE: v = eval_table(st, fr, e); break;
    case N_PAREN: v = eval(st, fr, e->a); break;
    default: v = lt_luai_nil(); break;
    }
    st->evd--;
    return v;
}

/* pushes every value of an expression list, last call/vararg expanded; returns the count */
static int eval_list(lt_lua *st, lt_lframe *fr, lt_lnode *list)
{
    int base = st->sp;
    lt_lnode *e;
    for (e = list; e != NULL; e = e->next) {
        if (e->next == NULL && is_multi(e)) {
            if (e->kind == N_VARARG) {
                int i;
                for (i = 0; i < fr->nva; i++) { lt_luai_push(st, fr->va[i]); }
            } else {
                int ab;
                (void)call_node(st, fr, e, &ab);
            }
        } else {
            lt_luai_push(st, eval(st, fr, e));
        }
    }
    return st->sp - base;
}

/* ------------------------------------------------------------------ statements */
static void store(lt_lua *st, lt_lframe *fr, lt_lnode *target, lt_lv val)
{
    switch (target->kind) {
    case N_LOCAL: write_var(st, fr, target->var, val); break;
    case N_UPVAL: fr->fn->ups[target->upidx]->v = val; break;
    default: /* N_GLOBAL */
        lt_luai_tab_set(st, st->globals, lt_luai_strv(target->str), val);
        break;
    }
}

static void exec_assign(lt_lua *st, lt_lframe *fr, lt_lnode *s)
{
    lt_lnode *t;
    int base, vbase, cnt, j, pair;
    if (s->n == 1 && s->n2 == 1) {
        t = s->list;
        if (t->kind == N_INDEX) {
            lt_lv obj = eval(st, fr, t->a);
            lt_lv key = eval(st, fr, t->b);
            lt_lv val = eval(st, fr, s->list2);
            st->line = t->line;
            do_setindex(st, obj, key, val, t->a);
        } else {
            store(st, fr, t, eval(st, fr, s->list2));
        }
        return;
    }
    base = st->sp;
    for (t = s->list; t != NULL; t = t->next) {
        if (t->kind == N_INDEX) {
            lt_luai_push(st, eval(st, fr, t->a));
            lt_luai_push(st, eval(st, fr, t->b));
        }
    }
    vbase = st->sp;
    cnt = eval_list(st, fr, s->list2);
    for (; cnt < s->n; cnt++) { lt_luai_push(st, lt_luai_nil()); }
    j = 0;
    pair = base;
    for (t = s->list; t != NULL; t = t->next, j++) {
        lt_lv val = st->stack[vbase + j];
        if (t->kind == N_INDEX) {
            st->line = t->line;
            do_setindex(st, st->stack[pair], st->stack[pair + 1], val, t->a);
            pair += 2;
        } else {
            store(st, fr, t, val);
        }
    }
    st->sp = base;
}

static void exec_local(lt_lua *st, lt_lframe *fr, lt_lnode *s)
{
    lt_lvar *v;
    int base, cnt, j;
    if (s->nvars == 1 && s->n == 1) {
        declare(st, fr, s->vars, eval(st, fr, s->list));
        return;
    }
    base = st->sp;
    cnt = eval_list(st, fr, s->list);
    for (; cnt < s->nvars; cnt++) { lt_luai_push(st, lt_luai_nil()); }
    for (v = s->vars, j = 0; v != NULL; v = v->next, j++) { declare(st, fr, v, st->stack[base + j]); }
    st->sp = base;
}

static int exec_fornum(lt_lua *st, lt_lframe *fr, lt_lnode *s)
{
    double start, limit, stp = 1.0, x;
    lt_lv v = eval(st, fr, s->a);
    st->line = s->line;
    if (!lt_luai_tonum(v, &start)) { lt_luai_error(st, "'for' initial value must be a number"); }
    v = eval(st, fr, s->b);
    if (!lt_luai_tonum(v, &limit)) { lt_luai_error(st, "'for' limit must be a number"); }
    if (s->c != NULL) {
        v = eval(st, fr, s->c);
        if (!lt_luai_tonum(v, &stp)) { lt_luai_error(st, "'for' step must be a number"); }
        if (stp == 0.0) { lt_luai_error(st, "'for' step is zero"); }
    }
    for (x = start; stp > 0.0 ? x <= limit : x >= limit; x += stp) {
        int rc;
        step(st);
        declare(st, fr, s->var, lt_luai_num(x));
        rc = exec_block(st, fr, s->d);
        if (rc == 1) { break; }
        if (rc == 2) { return 2; }
    }
    return 0;
}

static int exec_forin(lt_lua *st, lt_lframe *fr, lt_lnode *s)
{
    int base = st->sp, cnt, i;
    lt_lv f, state, ctl;
    cnt = eval_list(st, fr, s->list);
    f = cnt > 0 ? st->stack[base] : lt_luai_nil();
    state = cnt > 1 ? st->stack[base + 1] : lt_luai_nil();
    ctl = cnt > 2 ? st->stack[base + 2] : lt_luai_nil();
    st->sp = base;
    for (;;) {
        lt_lvar *v;
        int a = st->sp, nr, rc;
        step(st);
        lt_luai_push(st, state);
        lt_luai_push(st, ctl);
        st->line = s->line;
        if (!is_callable(f) && getmeta(f, st->s_call).t == LV_NIL) {
            lt_luai_error(st, "attempt to call a %s value", lt_luai_typename(f));
        }
        nr = lt_luai_call(st, f, a, 2);
        if (nr < 1 || st->stack[a].t == LV_NIL) { st->sp = a; break; }
        ctl = st->stack[a];
        for (v = s->vars, i = 0; v != NULL; v = v->next, i++) {
            declare(st, fr, v, i < nr ? st->stack[a + i] : lt_luai_nil());
        }
        st->sp = a;
        rc = exec_block(st, fr, s->d);
        if (rc == 1) { break; }
        if (rc == 2) { return 2; }
    }
    return 0;
}

static int exec(lt_lua *st, lt_lframe *fr, lt_lnode *s)
{
    int rc = 0;
    step(st);
    if (st->evd >= LL_MAX_EVAL) { lt_luai_error(st, "stack overflow (block too deep)"); }
    st->evd++;
    st->line = s->line;
    switch (s->kind) {
    case S_EXPR: {
        int a, r;
        r = call_node(st, fr, s->a, &a);
        LT_UNUSED(r);
        st->sp = a;
        break;
    }
    case S_LOCAL: exec_local(st, fr, s); break;
    case S_ASSIGN: exec_assign(st, fr, s); break;
    case S_IF: {
        lt_lnode *arm;
        int done = 0;
        for (arm = s->list; arm != NULL && !done; arm = arm->next) {
            if (truthy(eval(st, fr, arm->a))) {
                rc = exec_block(st, fr, arm->b);
                done = 1;
            }
        }
        if (!done && s->b != NULL) { rc = exec_block(st, fr, s->b); }
        break;
    }
    case S_WHILE:
        while (truthy(eval(st, fr, s->a))) {
            step(st);
            rc = exec_block(st, fr, s->b);
            if (rc == 1) { rc = 0; break; }
            if (rc == 2) { break; }
        }
        break;
    case S_REPEAT:
        for (;;) {
            step(st);
            rc = exec_block(st, fr, s->b);
            if (rc == 1) { rc = 0; break; }
            if (rc == 2) { break; }
            if (truthy(eval(st, fr, s->a))) { break; }
        }
        break;
    case S_FORNUM: rc = exec_fornum(st, fr, s); break;
    case S_FORIN: rc = exec_forin(st, fr, s); break;
    case S_LOCALFUNC: {
        lt_lv nilv = lt_luai_nil();
        declare(st, fr, s->var, nilv);
        write_var(st, fr, s->var, make_closure(st, fr, s->a->proto));
        break;
    }
    case S_RETURN: {
        int base = st->sp;
        int cnt = eval_list(st, fr, s->list);
        fr->rbase = base;
        fr->rn = cnt;
        rc = 2;
        break;
    }
    case S_BREAK: rc = 1; break;
    case S_BLOCK: rc = exec_block(st, fr, s); break;
    default: break;
    }
    st->evd--;
    return rc;
}

static int exec_block(lt_lua *st, lt_lframe *fr, lt_lnode *b)
{
    lt_lnode *s;
    for (s = b->list; s != NULL; s = s->next) {
        int rc = exec(st, fr, s);
        if (rc != 0) { return rc; }
    }
    return 0;
}

int lt_luai_run(lt_lua *st, lt_lproto *p)
{
    lt_lfun *f = lt_luai_fun_new(st, p, 0);
    lt_lframe fr;
    int i, rc, nres = 0;
    st->evd = 0;
    st->sp = p->nslots;
    fr.slots = st->stack;
    for (i = 0; i < p->nslots; i++) { fr.slots[i] = lt_luai_nil(); }
    fr.fn = f;
    fr.va = NULL;
    fr.nva = 0;
    fr.rbase = 0;
    fr.rn = 0;
    rc = exec_block(st, &fr, p->body);
    if (rc == 2) {
        nres = fr.rn;
        if (nres > 0) { memmove(&st->stack[0], &st->stack[fr.rbase], (size_t)nres * sizeof(lt_lv)); }
    }
    st->sp = nres;
    return nres;
}
