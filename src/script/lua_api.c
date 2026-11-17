/* lua_api.c - public interface of the Lua subset interpreter: state, limits, run, host
 * functions and the number conversions.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "lua_priv.h"

lt_lua *lt_lua_new(void)
{
    lt_lua *st = (lt_lua *)lt_calloc(1, sizeof(lt_lua));
    st->max_steps = LT_LUA_DEF_STEPS;
    st->max_mem = LT_LUA_DEF_MEM;
    st->stack = (lt_lv *)lt_calloc(LL_STACK_MAX, sizeof(lt_lv));
    st->chunk = "input";
    st->rnd = 0x2545F491ul;
    lt_buf_init(&st->result);
    lt_buf_init(&st->output);
    lt_buf_init(&st->scratch);
    return st;
}

void lt_lua_free(lt_lua *st)
{
    size_t i;
    if (st == NULL) { return; }
    lt_luai_reset(st);
    for (i = 0; i < st->nhosts; i++) { lt_free(st->hosts[i].name); }
    lt_free(st->hosts);
    lt_free(st->stack);
    lt_buf_free(&st->result);
    lt_buf_free(&st->output);
    lt_buf_free(&st->scratch);
    lt_free(st);
}

void lt_lua_set_limits(lt_lua *st, unsigned long max_steps, size_t max_mem)
{
    if (max_steps != 0) { st->max_steps = max_steps; }
    if (max_mem != 0) { st->max_mem = max_mem; }
}

void lt_lua_set_print(lt_lua *st, lt_lua_out_fn fn, void *ud)
{
    st->out_fn = fn;
    st->out_ud = ud;
}

int lt_lua_register(lt_lua *st, const char *name, lt_lua_cfn fn, void *ud)
{
    size_t i;
    lt_lhost *h;
    if (name == NULL || name[0] == '\0' || fn == NULL) { return -1; }
    for (i = 0; i < st->nhosts; i++) {
        if (strcmp(st->hosts[i].name, name) == 0) {
            st->hosts[i].fn = fn;
            st->hosts[i].ud = ud;
            return 0;
        }
    }
    st->hosts = (lt_lhost *)lt_realloc(st->hosts, (st->nhosts + 1u) * sizeof(lt_lhost));
    h = &st->hosts[st->nhosts++];
    h->name = lt_strdup(name);
    h->fn = fn;
    h->ud = ud;
    return 0;
}

static int host_trampoline(lt_lua *st, int a, int n, void *ud)
{
    lt_lhost *h = (lt_lhost *)ud;
    lt_lua_call c;
    c.st = st;
    c.a = a;
    c.n = n;
    return h->fn(&c, h->ud);
}

/* ------------------------------------------------------------------ run */
static void emit_result(lt_lua *st, int n)
{
    int i;
    lt_buf_clear(&st->result);
    for (i = 0; i < n; i++) {
        lt_lstr *s = lt_luai_tostr(st, st->stack[i]);
        if (i > 0) { lt_buf_putc(&st->result, '\t'); }
        lt_buf_append(&st->result, s->s, s->len);
        if (st->result.len > LT_LUA_MAX_OUT) { break; }
    }
}

static void run_chunk(lt_lua *st, const char *src, size_t len)
{
    size_t i;
    lt_lproto *p;
    int n;
    if (len > LT_LUA_MAX_SRC) { lt_luai_error_plain(st, "%s: chunk too large", st->chunk); }
    st->globals = lt_luai_tab_new(st);
    lt_luai_openlibs(st);
    for (i = 0; i < st->nhosts; i++) {
        lt_lcfun *f = lt_luai_cfun_new(st, host_trampoline, &st->hosts[i], st->hosts[i].name);
        lt_lv v;
        v.t = LV_CFN;
        v.u.n = 0.0;
        v.u.c = f;
        lt_luai_tab_set(st, st->globals,
                        lt_luai_strv(lt_luai_str_new(st, st->hosts[i].name, strlen(st->hosts[i].name))), v);
    }
    p = lt_luai_parse(st, src, len);
    if (st->parse_only) { return; }
    st->steps = 0;
    n = lt_luai_run(st, p);
    emit_result(st, n);
}

int lt_lua_run(lt_lua *st, const char *src, size_t len, const char *chunk)
{
    jmp_buf jb;
    lt_luai_reset(st);
    lt_buf_clear(&st->result);
    lt_buf_clear(&st->output);
    st->errmsg[0] = '\0';
    st->chunk = (chunk != NULL && chunk[0] != '\0') ? chunk : "input";
    st->steps = 0;
    st->fatal = 0;
    st->line = 0;
    st->out_total = 0;
    st->evd = 0;
    st->next_id = 0;
    st->errval = lt_luai_nil();
    st->err_has_val = 0;
    st->jb = &jb;
    if (setjmp(jb) == 0) {
        run_chunk(st, src, len);
        st->jb = NULL;
        st->last_steps = st->steps;
        st->last_mem = st->mem;
        lt_luai_reset(st);
        return 0;
    }
    st->jb = NULL;
    st->last_steps = st->steps;
    st->last_mem = st->mem;
    lt_luai_reset(st);
    lt_buf_clear(&st->result);
    lt_err_set("%s", st->errmsg);
    return -1;
}

int lt_lua_check(lt_lua *st, const char *src, size_t len, const char *chunk)
{
    int rc;
    st->parse_only = 1;
    rc = lt_lua_run(st, src, len, chunk);
    st->parse_only = 0;
    return rc;
}

const char *lt_lua_error(const lt_lua *st) { return st->errmsg; }
const char *lt_lua_result(const lt_lua *st) { return st->result.len > 0 ? st->result.data : ""; }
const char *lt_lua_output(const lt_lua *st) { return st->output.len > 0 ? st->output.data : ""; }
unsigned long lt_lua_steps(const lt_lua *st) { return st->last_steps; }
size_t lt_lua_mem_used(const lt_lua *st) { return st->last_mem; }

/* ------------------------------------------------------------------ host function helpers */
int lt_lua_nargs(const lt_lua_call *c) { return c->n; }

int lt_lua_argtype(const lt_lua_call *c, int i)
{
    lt_lv v;
    if (i < 0 || i >= c->n) { return LT_LUA_TNIL; }
    v = c->st->stack[c->a + i];
    switch (v.t) {
    case LV_BOOL: return LT_LUA_TBOOL;
    case LV_NUM: return LT_LUA_TNUM;
    case LV_STR: return LT_LUA_TSTR;
    case LV_TAB: return LT_LUA_TTABLE;
    case LV_FUN: case LV_CFN: return LT_LUA_TFUNC;
    default: return LT_LUA_TNIL;
    }
}

lt_bool lt_lua_argnum(const lt_lua_call *c, int i, double *out)
{
    if (i < 0 || i >= c->n) { return LT_FALSE; }
    return lt_luai_tonum(c->st->stack[c->a + i], out) ? LT_TRUE : LT_FALSE;
}

const char *lt_lua_argstr(const lt_lua_call *c, int i)
{
    lt_lv v;
    if (i < 0 || i >= c->n) { return NULL; }
    v = c->st->stack[c->a + i];
    return v.t == LV_STR ? v.u.s->s : NULL;
}

void lt_lua_pushnil(lt_lua_call *c) { lt_luai_push(c->st, lt_luai_nil()); }
void lt_lua_pushbool(lt_lua_call *c, int b) { lt_luai_push(c->st, lt_luai_bool(b)); }
void lt_lua_pushnum(lt_lua_call *c, double v) { lt_luai_push(c->st, lt_luai_num(v)); }

void lt_lua_pushstr(lt_lua_call *c, const char *s)
{
    lt_luai_push(c->st, lt_luai_strv(lt_luai_str_new(c->st, s != NULL ? s : "", s != NULL ? strlen(s) : 0u)));
}

void lt_lua_fail(lt_lua_call *c, const char *fmt, ...)
{
    char body[300];
    va_list ap;
    va_start(ap, fmt);
    lt_vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    lt_luai_error(c->st, "%s", body);
}

/* ------------------------------------------------------------------ numbers */
void lt_lua_fmt_number(char *buf, size_t cap, double v)
{
    if (v != v) { lt_strlcpy(buf, "nan", cap); return; }
    if (v > 1.7976931348623157e308) { lt_strlcpy(buf, "inf", cap); return; }
    if (v < -1.7976931348623157e308) { lt_strlcpy(buf, "-inf", cap); return; }
    if (v == floor(v) && fabs(v) < 1e15) {
        if (v == 0.0) { lt_strlcpy(buf, "0", cap); return; }
        lt_snprintf(buf, cap, "%.0f", v);
        return;
    }
    lt_snprintf(buf, cap, "%.14g", v);
}

static int is_ws(int c) { return c == ' ' || (c >= 9 && c <= 13); }

lt_bool lt_lua_parse_number(const char *s, size_t len, double *out)
{
    char tmp[128];
    size_t i = 0, j, ndig = 0;
    int neg = 0;
    double v;
    while (i < len && is_ws((unsigned char)s[i])) { i++; }
    while (len > i && is_ws((unsigned char)s[len - 1])) { len--; }
    if (i >= len) { return LT_FALSE; }
    if (s[i] == '-') { neg = 1; i++; } else if (s[i] == '+') { i++; }
    if (i >= len) { return LT_FALSE; }
    if (len - i > 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        v = 0.0;
        for (i += 2; i < len; i++) {
            int c = (unsigned char)s[i], d;
            if (c >= '0' && c <= '9') { d = c - '0'; }
            else if (c >= 'a' && c <= 'f') { d = c - 'a' + 10; }
            else if (c >= 'A' && c <= 'F') { d = c - 'A' + 10; }
            else { return LT_FALSE; }
            v = v * 16.0 + (double)d;
            ndig++;
        }
        if (ndig == 0) { return LT_FALSE; }
        *out = neg ? -v : v;
        return LT_TRUE;
    }
    j = i;
    while (j < len && s[j] >= '0' && s[j] <= '9') { j++; ndig++; }
    if (j < len && s[j] == '.') {
        j++;
        while (j < len && s[j] >= '0' && s[j] <= '9') { j++; ndig++; }
    }
    if (ndig == 0) { return LT_FALSE; }
    if (j < len && (s[j] == 'e' || s[j] == 'E')) {
        size_t k = j + 1;
        size_t nexp = 0;
        if (k < len && (s[k] == '+' || s[k] == '-')) { k++; }
        while (k < len && s[k] >= '0' && s[k] <= '9') { k++; nexp++; }
        if (nexp == 0) { return LT_FALSE; }
        j = k;
    }
    if (j != len || len - i >= sizeof(tmp)) { return LT_FALSE; }
    memcpy(tmp, s + i, len - i);
    tmp[len - i] = '\0';
    v = strtod(tmp, NULL);
    *out = neg ? -v : v;
    return LT_TRUE;
}
