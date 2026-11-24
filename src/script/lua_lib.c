/* lua_lib.c - the standard library subset: base functions, string, math and table.
 * Library functions take their arguments from the value stack (stack[a..a+n)), push their
 * results and return the count.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <ctype.h>
#include <math.h>
#include <string.h>
#include "lua_priv.h"

#define LIBFN(name) static int name(lt_lua *st, int a, int n, void *ud)
#define ARG(i) ((i) < n ? st->stack[a + (i)] : lt_luai_nil())

static int truthy(lt_lv v) { return !(v.t == LV_NIL || (v.t == LV_BOOL && v.u.b == 0)); }

/* ------------------------------------------------------------------ argument checks */
static void argerr(lt_lua *st, int i, const char *msg)
{
    lt_luai_error(st, "bad argument #%d to '%s' (%s)", i + 1, st->curname != NULL ? st->curname : "?", msg);
}

static void typeerr_v(lt_lua *st, int i, const char *want, lt_lv got, int present)
{
    char m[96];
    lt_snprintf(m, sizeof(m), "%s expected, got %s", want, present ? lt_luai_typename(got) : "no value");
    argerr(st, i, m);
}

static lt_lstr *chk_str(lt_lua *st, int a, int n, int i)
{
    lt_lv v = i < n ? st->stack[a + i] : lt_luai_nil();
    if (v.t == LV_STR) { return v.u.s; }
    if (v.t == LV_NUM) {
        lt_lstr *s = lt_luai_tostr(st, v);
        st->stack[a + i] = lt_luai_strv(s);
        return s;
    }
    typeerr_v(st, i, "string", v, i < n);
    return NULL;
}

static double chk_num(lt_lua *st, int a, int n, int i)
{
    lt_lv v = i < n ? st->stack[a + i] : lt_luai_nil();
    double d;
    if (!lt_luai_tonum(v, &d)) { typeerr_v(st, i, "number", v, i < n); }
    return d;
}

static long to_long(lt_lua *st, double d, int i)
{
    if (d != d || d != floor(d)) { argerr(st, i, "number has no integer representation"); }
    if (d > 2147483647.0) { return 2147483647L; }
    if (d < -2147483647.0) { return -2147483647L; }
    return (long)d;
}

static long chk_int(lt_lua *st, int a, int n, int i) { return to_long(st, chk_num(st, a, n, i), i); }

static long opt_int(lt_lua *st, int a, int n, int i, long def)
{
    if (i >= n || st->stack[a + i].t == LV_NIL) { return def; }
    return chk_int(st, a, n, i);
}

static lt_ltab *chk_tab(lt_lua *st, int a, int n, int i)
{
    lt_lv v = i < n ? st->stack[a + i] : lt_luai_nil();
    if (v.t != LV_TAB) { typeerr_v(st, i, "table", v, i < n); }
    return v.u.tab;
}

static void pushv(lt_lua *st, lt_lv v) { lt_luai_push(st, v); }
static void pushnum(lt_lua *st, double d) { lt_luai_push(st, lt_luai_num(d)); }
static void pushstr(lt_lua *st, const char *s, size_t len)
{
    lt_luai_push(st, lt_luai_strv(lt_luai_str_new(st, s, len)));
}

void lt_luai_reg(lt_lua *st, lt_ltab *t, const char *name,
                 int (*fn)(lt_lua *, int, int, void *), void *ud)
{
    lt_lv v;
    v.t = LV_CFN;
    v.u.n = 0.0;
    v.u.c = lt_luai_cfun_new(st, fn, ud, name);
    lt_luai_tab_set(st, t, lt_luai_strv(lt_luai_str_new(st, name, strlen(name))), v);
}

static lt_lv lib_table(lt_lua *st, const char *name)
{
    lt_ltab *t = lt_luai_tab_new(st);
    lt_luai_tab_set(st, st->globals, lt_luai_strv(lt_luai_str_new(st, name, strlen(name))),
                    lt_luai_tabv(t));
    return lt_luai_tabv(t);
}

static void set_field(lt_lua *st, lt_ltab *t, const char *name, lt_lv v)
{
    lt_luai_tab_set(st, t, lt_luai_strv(lt_luai_str_new(st, name, strlen(name))), v);
}

/* ------------------------------------------------------------------ base */
static void emit_line(lt_lua *st, const char *s, size_t len)
{
    st->out_total += len + 1u;
    if (st->out_total > LT_LUA_MAX_OUT) { lt_luai_fatal(st, "output limit exceeded"); }
    if (st->out_fn != NULL) {
        st->out_fn(st->out_ud, s, len);
    } else {
        lt_buf_append(&st->output, s, len);
        lt_buf_putc(&st->output, '\n');
    }
}

LIBFN(b_print)
{
    int i;
    lt_lstr *sb = lt_luai_sb_new(st, 64);
    LT_UNUSED(ud);
    for (i = 0; i < n; i++) {
        lt_lstr *s = lt_luai_tostr(st, st->stack[a + i]);
        if (i > 0) { lt_luai_sb_addc(st, sb, '\t'); }
        lt_luai_sb_add(st, sb, s->s, s->len);
    }
    emit_line(st, sb->s, sb->len);
    return 0;
}

LIBFN(b_type)
{
    lt_lv v;
    LT_UNUSED(ud);
    if (n < 1) { argerr(st, 0, "value expected"); }
    v = st->stack[a];
    pushv(st, lt_luai_strv(st->tn[v.t == LV_CFN ? LV_FUN : v.t]));
    return 1;
}

LIBFN(b_tostring)
{
    LT_UNUSED(ud);
    if (n < 1) { argerr(st, 0, "value expected"); }
    pushv(st, lt_luai_strv(lt_luai_tostr(st, st->stack[a])));
    return 1;
}

LIBFN(b_tonumber)
{
    lt_lv v = ARG(0);
    LT_UNUSED(ud);
    if (n < 1) { argerr(st, 0, "value expected"); }
    if (n < 2 || st->stack[a + 1].t == LV_NIL) {
        double d;
        if (v.t == LV_NUM) { pushv(st, v); }
        else if (v.t == LV_STR && lt_lua_parse_number(v.u.s->s, v.u.s->len, &d)) { pushnum(st, d); }
        else { pushv(st, lt_luai_nil()); }
        return 1;
    }
    {
        long base = chk_int(st, a, n, 1);
        lt_lstr *s = chk_str(st, a, n, 0);
        const char *p = s->s;
        double r = 0.0;
        int neg = 0, any = 0;
        if (base < 2 || base > 36) { argerr(st, 1, "base out of range"); }
        while (*p != '\0' && isspace((unsigned char)*p)) { p++; }
        if (*p == '-') { neg = 1; p++; } else if (*p == '+') { p++; }
        while (isalnum((unsigned char)*p)) {
            int c = (unsigned char)*p, d;
            d = isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10);
            if (d >= base) { pushv(st, lt_luai_nil()); return 1; }
            r = r * (double)base + (double)d;
            any = 1;
            p++;
        }
        while (*p != '\0' && isspace((unsigned char)*p)) { p++; }
        if (!any || *p != '\0') { pushv(st, lt_luai_nil()); return 1; }
        pushnum(st, neg ? -r : r);
        return 1;
    }
}

LIBFN(b_next)
{
    lt_ltab *t = chk_tab(st, a, n, 0);
    lt_lv k = ARG(1), v;
    int r;
    LT_UNUSED(ud);
    v = lt_luai_nil();
    r = lt_luai_tab_next(t, &k, &v);
    if (r < 0) { lt_luai_error(st, "invalid key to 'next'"); }
    if (r == 0) { pushv(st, lt_luai_nil()); return 1; }
    pushv(st, k);
    pushv(st, v);
    return 2;
}

LIBFN(b_pairs)
{
    lt_lv t = ARG(0);
    LT_UNUSED(ud);
    if (t.t != LV_TAB) { typeerr_v(st, 0, "table", t, n > 0); }
    pushv(st, st->fn_next);
    pushv(st, t);
    pushv(st, lt_luai_nil());
    return 3;
}

LIBFN(b_inext)
{
    lt_lv t = ARG(0);
    double i = chk_num(st, a, n, 1) + 1.0;
    lt_lv v;
    LT_UNUSED(ud);
    v = lt_luai_index(st, t, lt_luai_num(i));
    if (v.t == LV_NIL) { pushv(st, lt_luai_nil()); return 1; }
    pushnum(st, i);
    pushv(st, v);
    return 2;
}

LIBFN(b_ipairs)
{
    LT_UNUSED(ud);
    if (n < 1) { argerr(st, 0, "table expected, got no value"); }
    pushv(st, st->fn_inext);
    pushv(st, st->stack[a]);
    pushnum(st, 0.0);
    return 3;
}

LIBFN(b_select)
{
    lt_lv v = ARG(0);
    long k;
    int i;
    LT_UNUSED(ud);
    if (v.t == LV_STR && v.u.s->len == 1 && v.u.s->s[0] == '#') {
        pushnum(st, (double)(n - 1));
        return 1;
    }
    k = chk_int(st, a, n, 0);
    if (k < 0) { k = (long)(n - 1) + k + 1; }
    else if (k == 0) { argerr(st, 0, "index out of range"); }
    if (k < 1) { argerr(st, 0, "index out of range"); }
    for (i = (int)k; i < n; i++) { pushv(st, st->stack[a + i]); }
    return n > (int)k ? n - (int)k : 0;
}

LIBFN(b_unpack)
{
    lt_ltab *t = chk_tab(st, a, n, 0);
    long i = opt_int(st, a, n, 1, 1);
    long j = n > 2 && st->stack[a + 2].t != LV_NIL ? chk_int(st, a, n, 2) : (long)lt_luai_tab_len(t);
    long k;
    LT_UNUSED(ud);
    if (i > j) { return 0; }
    if (j - i >= 7000) { lt_luai_error(st, "too many results to unpack"); }
    for (k = i; k <= j; k++) { pushv(st, lt_luai_tab_get(t, lt_luai_num((double)k))); }
    return (int)(j - i + 1);
}

LIBFN(b_error)
{
    lt_lv v = ARG(0);
    long level = opt_int(st, a, n, 1, 1);
    LT_UNUSED(ud);
    if (v.t == LV_STR && level > 0) {
        char pos[120];
        lt_lstr *r;
        lt_snprintf(pos, sizeof(pos), "%s:%d: ", st->chunk, st->line);
        r = lt_luai_sb_new(st, v.u.s->len + 64u);
        lt_luai_sb_add(st, r, pos, strlen(pos));
        lt_luai_sb_add(st, r, v.u.s->s, v.u.s->len);
        v = lt_luai_strv(r);
    }
    lt_luai_throw_value(st, v);
    return 0;
}

LIBFN(b_assert)
{
    int i;
    LT_UNUSED(ud);
    if (n < 1) { argerr(st, 0, "value expected"); }
    if (!truthy(st->stack[a])) {
        lt_lv m = n > 1 ? st->stack[a + 1] : lt_luai_strv(lt_luai_str_new(st, "assertion failed!", 17));
        lt_luai_throw_value(st, m);
    }
    for (i = 0; i < n; i++) { pushv(st, st->stack[a + i]); }
    return n;
}

LIBFN(b_pcall)
{
    jmp_buf jb;
    jmp_buf *old = st->jb;
    int depth0 = st->depth, evd0 = st->evd, nres;
    LT_UNUSED(ud);
    if (n < 1) { argerr(st, 0, "value expected"); }
    st->jb = &jb;
    if (setjmp(jb) == 0) {
        nres = lt_luai_call(st, st->stack[a], a + 1, n - 1);
        st->jb = old;
        st->stack[a] = lt_luai_bool(1);
        st->sp = a + 1 + nres;
        return nres + 1;
    }
    st->jb = old;
    st->depth = depth0;
    st->evd = evd0;
    if (st->fatal) { longjmp(*old, 1); }
    st->sp = a + n;
    pushv(st, lt_luai_bool(0));
    pushv(st, st->errval);
    return 2;
}

LIBFN(b_xpcall)
{
    jmp_buf jb;
    jmp_buf *old = st->jb;
    int depth0 = st->depth, evd0 = st->evd, nres, i;
    lt_lv f, h;
    LT_UNUSED(ud);
    if (n < 2) { argerr(st, 1, "value expected"); }
    f = st->stack[a];
    h = st->stack[a + 1];
    for (i = 2; i < n; i++) { st->stack[a + i - 1] = st->stack[a + i]; }
    st->jb = &jb;
    if (setjmp(jb) == 0) {
        nres = lt_luai_call(st, f, a + 1, n - 2);
        st->jb = old;
        st->stack[a] = lt_luai_bool(1);
        st->sp = a + 1 + nres;
        return nres + 1;
    }
    st->jb = old;
    st->depth = depth0;
    st->evd = evd0;
    if (st->fatal) { longjmp(*old, 1); }
    {
        lt_lv ev = st->errval, r;
        st->sp = a + n;
        r = lt_luai_call1(st, h, &ev, 1);
        pushv(st, lt_luai_bool(0));
        pushv(st, r);
    }
    return 2;
}

LIBFN(b_rawget)
{
    lt_ltab *t = chk_tab(st, a, n, 0);
    LT_UNUSED(ud);
    pushv(st, lt_luai_tab_get(t, ARG(1)));
    return 1;
}

LIBFN(b_rawset)
{
    lt_ltab *t = chk_tab(st, a, n, 0);
    LT_UNUSED(ud);
    lt_luai_check_key(st, ARG(1));
    lt_luai_tab_set(st, t, ARG(1), ARG(2));
    pushv(st, st->stack[a]);
    return 1;
}

LIBFN(b_rawequal)
{
    LT_UNUSED(ud);
    pushv(st, lt_luai_bool(lt_luai_raw_equal(ARG(0), ARG(1))));
    return 1;
}

LIBFN(b_rawlen)
{
    lt_lv v = ARG(0);
    LT_UNUSED(ud);
    if (v.t == LV_TAB) { pushnum(st, (double)lt_luai_tab_len(v.u.tab)); }
    else if (v.t == LV_STR) { pushnum(st, (double)v.u.s->len); }
    else { argerr(st, 0, "table or string expected"); }
    return 1;
}

LIBFN(b_setmetatable)
{
    lt_ltab *t = chk_tab(st, a, n, 0);
    lt_lv m = ARG(1);
    LT_UNUSED(ud);
    if (m.t != LV_NIL && m.t != LV_TAB) { argerr(st, 1, "nil or table expected"); }
    t->meta = m.t == LV_TAB ? m.u.tab : NULL;
    pushv(st, st->stack[a]);
    return 1;
}

LIBFN(b_getmetatable)
{
    lt_lv v = ARG(0);
    LT_UNUSED(ud);
    if (v.t == LV_TAB && v.u.tab->meta != NULL) { pushv(st, lt_luai_tabv(v.u.tab->meta)); }
    else if (v.t == LV_STR) { pushv(st, lt_luai_tabv(st->slib)); }
    else { pushv(st, lt_luai_nil()); }
    return 1;
}

LIBFN(b_collectgarbage)
{
    lt_lv o = ARG(0);
    LT_UNUSED(ud);
    if (o.t == LV_STR && strcmp(o.u.s->s, "count") == 0) { pushnum(st, (double)st->mem / 1024.0); }
    else { pushnum(st, 0.0); }
    return 1;
}

/* ------------------------------------------------------------------ string */
static size_t pos_start(long pos, size_t len)
{
    if (pos > 0) { return (size_t)pos; }
    if (pos == 0) { return 1; }
    if (pos < -(long)len) { return 1; }
    return len - (size_t)(-pos) + 1u;
}

static size_t pos_end(long pos, size_t len)
{
    if (pos > (long)len) { return len; }
    if (pos >= 0) { return (size_t)pos; }
    if (pos < -(long)len) { return 0; }
    return len - (size_t)(-pos) + 1u;
}

LIBFN(s_len)
{
    LT_UNUSED(ud);
    pushnum(st, (double)chk_str(st, a, n, 0)->len);
    return 1;
}

LIBFN(s_sub)
{
    lt_lstr *s = chk_str(st, a, n, 0);
    size_t i = pos_start(chk_int(st, a, n, 1), s->len);
    size_t j = pos_end(opt_int(st, a, n, 2, -1), s->len);
    LT_UNUSED(ud);
    if (i > j) { pushstr(st, "", 0); }
    else { pushstr(st, s->s + i - 1u, j - i + 1u); }
    return 1;
}

LIBFN(s_upper)
{
    lt_lstr *s = chk_str(st, a, n, 0);
    lt_lstr *r = lt_luai_str_buf(st, s->len);
    size_t i;
    LT_UNUSED(ud);
    for (i = 0; i < s->len; i++) { r->s[i] = (char)toupper((unsigned char)s->s[i]); }
    pushv(st, lt_luai_strv(r));
    return 1;
}

LIBFN(s_lower)
{
    lt_lstr *s = chk_str(st, a, n, 0);
    lt_lstr *r = lt_luai_str_buf(st, s->len);
    size_t i;
    LT_UNUSED(ud);
    for (i = 0; i < s->len; i++) { r->s[i] = (char)tolower((unsigned char)s->s[i]); }
    pushv(st, lt_luai_strv(r));
    return 1;
}

LIBFN(s_reverse)
{
    lt_lstr *s = chk_str(st, a, n, 0);
    lt_lstr *r = lt_luai_str_buf(st, s->len);
    size_t i;
    LT_UNUSED(ud);
    for (i = 0; i < s->len; i++) { r->s[i] = s->s[s->len - 1u - i]; }
    pushv(st, lt_luai_strv(r));
    return 1;
}

LIBFN(s_rep)
{
    lt_lstr *s = chk_str(st, a, n, 0);
    long cnt = chk_int(st, a, n, 1);
    lt_lstr *sep = n > 2 && st->stack[a + 2].t != LV_NIL ? chk_str(st, a, n, 2) : NULL;
    size_t seplen = sep != NULL ? sep->len : 0u;
    lt_lstr *r;
    size_t pos = 0;
    long i;
    double total;
    LT_UNUSED(ud);
    if (cnt <= 0) { pushstr(st, "", 0); return 1; }
    total = (double)s->len * (double)cnt + (double)seplen * (double)(cnt - 1);
    if (total > (double)st->max_mem) { lt_luai_fatal(st, "not enough memory"); }
    lt_luai_charge(st, (unsigned long)(total / 64.0));
    r = lt_luai_str_buf(st, (size_t)total);
    for (i = 0; i < cnt; i++) {
        memcpy(r->s + pos, s->s, s->len);
        pos += s->len;
        if (i + 1 < cnt && seplen > 0) {
            memcpy(r->s + pos, sep->s, seplen);
            pos += seplen;
        }
    }
    r->s[pos] = '\0';
    pushv(st, lt_luai_strv(r));
    return 1;
}

LIBFN(s_byte)
{
    lt_lstr *s = chk_str(st, a, n, 0);
    long i0 = opt_int(st, a, n, 1, 1);
    size_t i = pos_start(i0, s->len);
    size_t j = pos_end(opt_int(st, a, n, 2, (long)i), s->len);
    size_t k;
    LT_UNUSED(ud);
    if (i > j) { return 0; }
    if (j - i >= 7000) { lt_luai_error(st, "string slice too long"); }
    for (k = i; k <= j; k++) { pushnum(st, (double)(unsigned char)s->s[k - 1u]); }
    return (int)(j - i + 1u);
}

LIBFN(s_char)
{
    lt_lstr *r = lt_luai_str_buf(st, (size_t)n);
    int i;
    LT_UNUSED(ud);
    for (i = 0; i < n; i++) {
        long c = chk_int(st, a, n, i);
        if (c < 0 || c > 255) { argerr(st, i, "value out of range"); }
        r->s[i] = (char)c;
    }
    pushv(st, lt_luai_strv(r));
    return 1;
}

/* ---- format ---- */
static void pad_to(lt_lua *st, lt_lstr *sb, const char *s, size_t len, int width, int left, int zero)
{
    size_t w = width > 0 ? (size_t)width : 0u;
    size_t i;
    if (len >= w) { lt_luai_sb_add(st, sb, s, len); return; }
    if (left) {
        lt_luai_sb_add(st, sb, s, len);
        for (i = len; i < w; i++) { lt_luai_sb_addc(st, sb, ' '); }
    } else {
        for (i = len; i < w; i++) { lt_luai_sb_addc(st, sb, zero ? '0' : ' '); }
        lt_luai_sb_add(st, sb, s, len);
    }
}

static void fmt_integer(lt_lua *st, lt_lstr *sb, double v, const char *flags, int width, int prec, int argi)
{
    char digits[400], out[420];
    size_t dl, ol = 0, i;
    int neg = v < 0.0;
    int left = strchr(flags, '-') != NULL, zero = strchr(flags, '0') != NULL && prec < 0 && !left;
    char sign = neg ? '-' : (strchr(flags, '+') != NULL ? '+' : (strchr(flags, ' ') != NULL ? ' ' : '\0'));
    if (v != v || v != floor(v) || fabs(v) >= 9007199254740992.0) {
        argerr(st, argi, "number has no integer representation");
    }
    lt_snprintf(digits, sizeof(digits), "%.0f", fabs(v));
    dl = strlen(digits);
    if (prec == 0 && v == 0.0) { dl = 0; digits[0] = '\0'; }
    if (sign != '\0') { out[ol++] = sign; }
    if (zero && width > 0) {
        size_t need = (size_t)width > ol + dl ? (size_t)width - ol - dl : 0u;
        for (i = 0; i < need; i++) { out[ol++] = '0'; }
    }
    if (prec > 0 && dl < (size_t)prec) {
        for (i = dl; i < (size_t)prec; i++) { out[ol++] = '0'; }
    }
    memcpy(out + ol, digits, dl);
    ol += dl;
    pad_to(st, sb, out, ol, width, left, 0);
}

static void fmt_quoted(lt_lua *st, lt_lstr *sb, const lt_lstr *s)
{
    size_t i;
    lt_luai_sb_addc(st, sb, '"');
    for (i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->s[i];
        if (c == '"' || c == '\\') { lt_luai_sb_addc(st, sb, '\\'); lt_luai_sb_addc(st, sb, c); }
        else if (c == '\n') { lt_luai_sb_addc(st, sb, '\\'); lt_luai_sb_addc(st, sb, 'n'); }
        else if (c == '\r') { lt_luai_sb_add(st, sb, "\\r", 2); }
        else if (c == 0) { lt_luai_sb_add(st, sb, "\\0", 2); }
        else { lt_luai_sb_addc(st, sb, c); }
    }
    lt_luai_sb_addc(st, sb, '"');
}

LIBFN(s_format)
{
    lt_lstr *fmt = chk_str(st, a, n, 0);
    lt_lstr *sb = lt_luai_sb_new(st, fmt->len + 16u);
    const char *p = fmt->s, *end = fmt->s + fmt->len;
    int argi = 0;
    LT_UNUSED(ud);
    while (p < end) {
        char flags[8], spec[24], out[560];
        int width = -1, prec = -1, nf = 0, conv;
        if (*p != '%') { lt_luai_sb_addc(st, sb, *p++); continue; }
        p++;
        if (p < end && *p == '%') { lt_luai_sb_addc(st, sb, '%'); p++; continue; }
        flags[0] = '\0';
        while (p < end && strchr("-+ #0", *p) != NULL && *p != '\0') {
            if (nf < 5) { flags[nf++] = *p; flags[nf] = '\0'; }
            p++;
        }
        if (p < end && isdigit((unsigned char)*p)) {
            width = 0;
            while (p < end && isdigit((unsigned char)*p) && width < 100) { width = width * 10 + (*p - '0'); p++; }
            if (width > 99) { lt_luai_error(st, "invalid conversion (width too long)"); }
        }
        if (p < end && *p == '.') {
            p++;
            prec = 0;
            while (p < end && isdigit((unsigned char)*p) && prec < 100) { prec = prec * 10 + (*p - '0'); p++; }
            if (prec > 99) { lt_luai_error(st, "invalid conversion (precision too long)"); }
        }
        if (p >= end) { lt_luai_error(st, "invalid conversion '%%' to 'format'"); }
        conv = (unsigned char)*p++;
        argi++;
        switch (conv) {
        case 'd': case 'i':
            fmt_integer(st, sb, chk_num(st, a, n, argi), flags, width, prec, argi);
            break;
        case 'c': {
            char c = (char)chk_int(st, a, n, argi);
            pad_to(st, sb, &c, 1, width, strchr(flags, '-') != NULL, 0);
            break;
        }
        case 'x': case 'X': case 'o': case 'u': {
            double v = chk_num(st, a, n, argi);
            if (v != floor(v) || v < 0.0 || v > 4294967295.0) { argerr(st, argi, "value out of range"); }
            lt_snprintf(spec, sizeof(spec), "%%%s", flags);
            if (width >= 0) { lt_snprintf(spec + strlen(spec), 8, "%d", width); }
            if (prec >= 0) { lt_snprintf(spec + strlen(spec), 8, ".%d", prec); }
            lt_snprintf(spec + strlen(spec), 4, "l%c", conv == 'u' ? 'u' : conv);
            lt_snprintf(out, sizeof(out), spec, (unsigned long)v);
            lt_luai_sb_add(st, sb, out, strlen(out));
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
            double v = chk_num(st, a, n, argi);
            if (!lt_isfinite(v)) {
                const char *t = v != v ? "nan" : (v > 0 ? (strchr(flags, '+') != NULL ? "+inf" : "inf") : "-inf");
                pad_to(st, sb, t, strlen(t), width, strchr(flags, '-') != NULL, 0);
                break;
            }
            lt_snprintf(spec, sizeof(spec), "%%%s", flags);
            if (width >= 0) { lt_snprintf(spec + strlen(spec), 8, "%d", width); }
            if (prec >= 0) { lt_snprintf(spec + strlen(spec), 8, ".%d", prec); }
            lt_snprintf(spec + strlen(spec), 4, "%c", conv);
            lt_snprintf(out, sizeof(out), spec, v);
            lt_luai_sb_add(st, sb, out, strlen(out));
            break;
        }
        case 's': {
            lt_lstr *s;
            size_t len;
            if (argi >= n) { argerr(st, argi, "no value"); }
            s = lt_luai_tostr(st, st->stack[a + argi]);
            len = s->len;
            if (prec >= 0 && (size_t)prec < len) { len = (size_t)prec; }
            pad_to(st, sb, s->s, len, width, strchr(flags, '-') != NULL, 0);
            break;
        }
        case 'q':
            fmt_quoted(st, sb, chk_str(st, a, n, argi));
            break;
        default: {
            char m[48];
            lt_snprintf(m, sizeof(m), "invalid conversion '%%%c' to 'format'", conv);
            lt_luai_error(st, "%s", m);
            break;
        }
        }
    }
    pushv(st, lt_luai_strv(sb));
    return 1;
}

/* ---- find / match / gmatch / gsub ---- */
static int has_specials(const lt_lstr *p)
{
    size_t i;
    for (i = 0; i < p->len; i++) {
        if (strchr("^$*+?.([%-", p->s[i]) != NULL && p->s[i] != '\0') { return 1; }
    }
    return 0;
}

static int push_captures(lt_lua *st, lt_lmatch *ms, const char *s, const char *e)
{
    int i, nl = lt_luai_pat_ncaps(ms, s, s != NULL);
    for (i = 0; i < nl; i++) { pushv(st, lt_luai_pat_capture(ms, i, s, e)); }
    return nl;
}

static int find_aux(lt_lua *st, int a, int n, int find)
{
    lt_lstr *s = chk_str(st, a, n, 0);
    lt_lstr *p = chk_str(st, a, n, 1);
    size_t init = pos_start(opt_int(st, a, n, 2, 1), s->len) - 1u;
    lt_lmatch ms;
    const char *s1, *pp;
    int anchor;
    if (init > s->len) { pushv(st, lt_luai_nil()); return 1; }
    if (find && (truthy(ARG(3)) || !has_specials(p))) {
        size_t i;
        lt_luai_charge(st, (unsigned long)(s->len / 32u));
        if (p->len == 0) {
            pushnum(st, (double)init + 1.0);
            pushnum(st, (double)init);
            return 2;
        }
        for (i = init; i + p->len <= s->len; i++) {
            if (s->s[i] == p->s[0] && memcmp(s->s + i, p->s, p->len) == 0) {
                pushnum(st, (double)i + 1.0);
                pushnum(st, (double)(i + p->len));
                return 2;
            }
        }
        pushv(st, lt_luai_nil());
        return 1;
    }
    memset(&ms, 0, sizeof(ms));
    ms.st = st;
    ms.src = s->s;
    ms.src_end = s->s + s->len;
    ms.pat_end = p->s + p->len;
    anchor = p->len > 0 && p->s[0] == '^';
    pp = p->s + (anchor ? 1 : 0);
    for (;;) {
        const char *e;
        s1 = s->s + init;
        e = lt_luai_pat_match(&ms, s1, pp);
        if (e != NULL) {
            if (find) {
                pushnum(st, (double)(s1 - s->s) + 1.0);
                pushnum(st, (double)(e - s->s));
                return push_captures(st, &ms, NULL, NULL) + 2;
            }
            return push_captures(st, &ms, s1, e);
        }
        if (anchor || init >= s->len) { break; }
        init++;
    }
    pushv(st, lt_luai_nil());
    return 1;
}

LIBFN(s_find) { LT_UNUSED(ud); return find_aux(st, a, n, 1); }
LIBFN(s_match) { LT_UNUSED(ud); return find_aux(st, a, n, 0); }

/* gmatch state: table {1 = source, 2 = pattern, 3 = next position (0 based), 4 = last match end} */
LIBFN(s_gmatch_iter)
{
    lt_lv self = ARG(0);
    lt_ltab *t;
    lt_lstr *s, *p;
    lt_lmatch ms;
    const char *src, *pp;
    double pos, last;
    LT_UNUSED(ud);
    if (self.t != LV_TAB) { return 0; }
    t = self.u.tab;
    s = lt_luai_tab_get(t, lt_luai_num(1.0)).u.s;
    p = lt_luai_tab_get(t, lt_luai_num(2.0)).u.s;
    pos = lt_luai_tab_get(t, lt_luai_num(3.0)).u.n;
    last = lt_luai_tab_get(t, lt_luai_num(4.0)).u.n;
    memset(&ms, 0, sizeof(ms));
    ms.st = st;
    ms.src = s->s;
    ms.src_end = s->s + s->len;
    ms.pat_end = p->s + p->len;
    pp = p->s;
    if (pos <= (double)s->len) {
        size_t i;
        for (i = (size_t)pos; i <= s->len; i++) {
            const char *e;
            src = s->s + i;
            e = lt_luai_pat_match(&ms, src, pp);
            if (e != NULL && (double)(e - s->s) != last) {
                lt_luai_tab_set(st, t, lt_luai_num(3.0), lt_luai_num((double)(e - s->s)));
                lt_luai_tab_set(st, t, lt_luai_num(4.0), lt_luai_num((double)(e - s->s)));
                return push_captures(st, &ms, src, e);
            }
        }
    }
    lt_luai_tab_set(st, t, lt_luai_num(3.0), lt_luai_num((double)s->len + 1.0));
    pushv(st, lt_luai_nil());
    return 1;
}

LIBFN(s_gmatch)
{
    lt_lstr *s = chk_str(st, a, n, 0);
    lt_lstr *p = chk_str(st, a, n, 1);
    lt_ltab *t = lt_luai_tab_new(st);
    lt_ltab *mt = lt_luai_tab_new(st);
    LT_UNUSED(ud);
    lt_luai_tab_set(st, t, lt_luai_num(1.0), lt_luai_strv(s));
    lt_luai_tab_set(st, t, lt_luai_num(2.0), lt_luai_strv(p));
    lt_luai_tab_set(st, t, lt_luai_num(3.0), lt_luai_num(0.0));
    lt_luai_tab_set(st, t, lt_luai_num(4.0), lt_luai_num(-1.0));
    lt_luai_reg(st, mt, "__call", s_gmatch_iter, NULL);
    t->meta = mt;
    pushv(st, lt_luai_tabv(t));
    return 1;
}

static void add_repl_string(lt_lua *st, lt_lstr *sb, lt_lmatch *ms, const lt_lstr *r,
                            const char *s, const char *e)
{
    size_t i;
    for (i = 0; i < r->len; i++) {
        if (r->s[i] != '%') { lt_luai_sb_addc(st, sb, r->s[i]); continue; }
        i++;
        if (i >= r->len) { lt_luai_error(st, "invalid use of '%%' in replacement string"); }
        if (r->s[i] == '%') { lt_luai_sb_addc(st, sb, '%'); }
        else if (isdigit((unsigned char)r->s[i])) {
            lt_lv v;
            lt_lstr *cs;
            if (r->s[i] == '0') { lt_luai_sb_add(st, sb, s, (size_t)(e - s)); continue; }
            v = lt_luai_pat_capture(ms, r->s[i] - '1', s, e);
            cs = lt_luai_tostr(st, v);
            lt_luai_sb_add(st, sb, cs->s, cs->len);
        } else {
            lt_luai_error(st, "invalid use of '%%' in replacement string");
        }
    }
}

LIBFN(s_gsub)
{
    lt_lstr *s = chk_str(st, a, n, 0);
    lt_lstr *p = chk_str(st, a, n, 1);
    lt_lv repl = ARG(2);
    long max_s = opt_int(st, a, n, 3, (long)s->len + 1L);
    lt_lstr *sb = lt_luai_sb_new(st, s->len + 16u);
    lt_lmatch ms;
    const char *src = s->s, *pp, *last = NULL;
    int anchor;
    long cnt = 0;
    LT_UNUSED(ud);
    if (repl.t != LV_STR && repl.t != LV_NUM && repl.t != LV_TAB && repl.t != LV_FUN && repl.t != LV_CFN) {
        argerr(st, 2, "string/function/table expected");
    }
    if (repl.t == LV_NUM) { repl = lt_luai_strv(lt_luai_tostr(st, repl)); }
    memset(&ms, 0, sizeof(ms));
    ms.st = st;
    ms.src = s->s;
    ms.src_end = s->s + s->len;
    ms.pat_end = p->s + p->len;
    anchor = p->len > 0 && p->s[0] == '^';
    pp = p->s + (anchor ? 1 : 0);
    while (cnt < max_s) {
        const char *e = lt_luai_pat_match(&ms, src, pp);
        if (e != NULL && e != last) {
            lt_lv rv;
            cnt++;
            if (repl.t == LV_STR) {
                add_repl_string(st, sb, &ms, repl.u.s, src, e);
                src = last = e;
                if (anchor) { break; }
                continue;
            }
            if (repl.t == LV_TAB) {
                rv = lt_luai_index(st, repl, lt_luai_pat_capture(&ms, 0, src, e));
            } else {
                int base = st->sp, nc, i;
                lt_lv res;
                nc = push_captures(st, &ms, src, e);
                res = lt_luai_nil();
                (void)i;
                nc = lt_luai_call(st, repl, base, nc);
                if (nc > 0) { res = st->stack[base]; }
                st->sp = base;
                rv = res;
            }
            if (!truthy(rv)) {
                lt_luai_sb_add(st, sb, src, (size_t)(e - src));
            } else if (rv.t == LV_STR || rv.t == LV_NUM) {
                lt_lstr *rs = lt_luai_tostr(st, rv);
                lt_luai_sb_add(st, sb, rs->s, rs->len);
            } else {
                lt_luai_error(st, "invalid replacement value (a %s)", lt_luai_typename(rv));
            }
            src = last = e;
        } else if (src < ms.src_end) {
            lt_luai_sb_addc(st, sb, *src++);
        } else {
            break;
        }
        if (anchor) { break; }
    }
    if (src < ms.src_end) { lt_luai_sb_add(st, sb, src, (size_t)(ms.src_end - src)); }
    pushv(st, lt_luai_strv(sb));
    pushnum(st, (double)cnt);
    return 2;
}

/* ------------------------------------------------------------------ math */
#define MATH1(name, expr) \
    LIBFN(name) { double x = chk_num(st, a, n, 0); LT_UNUSED(ud); pushnum(st, expr); return 1; }

MATH1(m_abs, fabs(x))
MATH1(m_floor, floor(x))
MATH1(m_ceil, ceil(x))
MATH1(m_sqrt, sqrt(x))
MATH1(m_sin, sin(x))
MATH1(m_cos, cos(x))
MATH1(m_tan, tan(x))
MATH1(m_asin, asin(x))
MATH1(m_acos, acos(x))
MATH1(m_exp, exp(x))
MATH1(m_deg, x * (180.0 / 3.14159265358979323846))
MATH1(m_rad, x * (3.14159265358979323846 / 180.0))

LIBFN(m_atan)
{
    double y = chk_num(st, a, n, 0);
    double x = n > 1 && st->stack[a + 1].t != LV_NIL ? chk_num(st, a, n, 1) : 1.0;
    LT_UNUSED(ud);
    pushnum(st, atan2(y, x));
    return 1;
}

LIBFN(m_log)
{
    double x = chk_num(st, a, n, 0);
    LT_UNUSED(ud);
    if (n > 1 && st->stack[a + 1].t != LV_NIL) {
        double b = chk_num(st, a, n, 1);
        pushnum(st, b == 10.0 ? log10(x) : log(x) / log(b));
    } else {
        pushnum(st, log(x));
    }
    return 1;
}

LIBFN(m_pow)
{
    double x = chk_num(st, a, n, 0), y = chk_num(st, a, n, 1);
    LT_UNUSED(ud);
    pushnum(st, pow(x, y));
    return 1;
}

LIBFN(m_fmod)
{
    double x = chk_num(st, a, n, 0), y = chk_num(st, a, n, 1);
    LT_UNUSED(ud);
    pushnum(st, fmod(x, y));
    return 1;
}

LIBFN(m_modf)
{
    double x = chk_num(st, a, n, 0), ip;
    LT_UNUSED(ud);
    ip = x < 0.0 ? ceil(x) : floor(x);
    pushnum(st, ip);
    pushnum(st, lt_isfinite(x) ? x - ip : 0.0);
    return 2;
}

LIBFN(m_max)
{
    double r;
    int i;
    LT_UNUSED(ud);
    r = chk_num(st, a, n, 0);
    for (i = 1; i < n; i++) { double v = chk_num(st, a, n, i); if (v > r) { r = v; } }
    pushnum(st, r);
    return 1;
}

LIBFN(m_min)
{
    double r;
    int i;
    LT_UNUSED(ud);
    r = chk_num(st, a, n, 0);
    for (i = 1; i < n; i++) { double v = chk_num(st, a, n, i); if (v < r) { r = v; } }
    pushnum(st, r);
    return 1;
}

static double rnd_unit(lt_lua *st)
{
    unsigned long x = st->rnd & 0xFFFFFFFFul;
    if (x == 0) { x = 0x2545F491ul; }
    x ^= (x << 13) & 0xFFFFFFFFul;
    x ^= x >> 17;
    x ^= (x << 5) & 0xFFFFFFFFul;
    st->rnd = x;
    return (double)x / 4294967296.0;
}

LIBFN(m_random)
{
    double r = rnd_unit(st);
    LT_UNUSED(ud);
    if (n == 0) { pushnum(st, r); return 1; }
    {
        double lo = 1.0, hi;
        if (n == 1) { hi = chk_num(st, a, n, 0); }
        else { lo = chk_num(st, a, n, 0); hi = chk_num(st, a, n, 1); }
        if (lo != floor(lo) || hi != floor(hi)) { argerr(st, 0, "number has no integer representation"); }
        if (lo > hi) { argerr(st, n == 1 ? 0 : 1, "interval is empty"); }
        pushnum(st, lo + floor(r * (hi - lo + 1.0)));
    }
    return 1;
}

LIBFN(m_randomseed)
{
    double x = n > 0 ? chk_num(st, a, n, 0) : 0.0;
    LT_UNUSED(ud);
    st->rnd = ((unsigned long)fmod(fabs(x), 4294967296.0) & 0xFFFFFFFFul) | 1ul;
    return 0;
}

/* ------------------------------------------------------------------ table */
LIBFN(t_insert)
{
    lt_ltab *t = chk_tab(st, a, n, 0);
    size_t len = lt_luai_tab_len(t), pos, i;
    LT_UNUSED(ud);
    if (n == 2) {
        lt_luai_tab_set(st, t, lt_luai_num((double)len + 1.0), st->stack[a + 1]);
        return 0;
    }
    if (n != 3) { lt_luai_error(st, "wrong number of arguments to 'insert'"); }
    {
        long p = chk_int(st, a, n, 1);
        if (p < 1 || (size_t)p > len + 1u) { argerr(st, 1, "position out of bounds"); }
        pos = (size_t)p;
    }
    lt_luai_charge(st, (unsigned long)((len - pos + 1u) / 8u));
    for (i = len + 1u; i > pos; i--) {
        lt_luai_tab_set(st, t, lt_luai_num((double)i), lt_luai_tab_get(t, lt_luai_num((double)(i - 1u))));
    }
    lt_luai_tab_set(st, t, lt_luai_num((double)pos), st->stack[a + 2]);
    return 0;
}

LIBFN(t_remove)
{
    lt_ltab *t = chk_tab(st, a, n, 0);
    size_t len = lt_luai_tab_len(t), pos, i;
    lt_lv v;
    LT_UNUSED(ud);
    if (n < 2) {
        pos = len;
    } else {
        long p = chk_int(st, a, n, 1);
        if (p < 0 || (p == 0 && len != 0) || (size_t)p > len + 1u) { argerr(st, 1, "position out of bounds"); }
        pos = (size_t)p;
    }
    if (pos == 0 || pos > len) {
        pushv(st, lt_luai_tab_get(t, lt_luai_num((double)pos)));
        return 1;
    }
    v = lt_luai_tab_get(t, lt_luai_num((double)pos));
    lt_luai_charge(st, (unsigned long)((len - pos + 1u) / 8u));
    for (i = pos; i < len; i++) {
        lt_luai_tab_set(st, t, lt_luai_num((double)i), lt_luai_tab_get(t, lt_luai_num((double)(i + 1u))));
    }
    lt_luai_tab_set(st, t, lt_luai_num((double)len), lt_luai_nil());
    pushv(st, v);
    return 1;
}

LIBFN(t_concat)
{
    lt_ltab *t = chk_tab(st, a, n, 0);
    lt_lstr *sep = n > 1 && st->stack[a + 1].t != LV_NIL ? chk_str(st, a, n, 1) : NULL;
    long i = opt_int(st, a, n, 2, 1);
    long j = n > 3 && st->stack[a + 3].t != LV_NIL ? chk_int(st, a, n, 3) : (long)lt_luai_tab_len(t);
    lt_lstr *sb = lt_luai_sb_new(st, 64);
    long k;
    LT_UNUSED(ud);
    if (j >= i) { lt_luai_charge(st, (unsigned long)(j - i + 1) / 4u); }
    for (k = i; k <= j; k++) {
        lt_lv v = lt_luai_tab_get(t, lt_luai_num((double)k));
        lt_lstr *s;
        if (v.t != LV_STR && v.t != LV_NUM) {
            lt_luai_error(st, "invalid value (at index %ld) in table for 'concat'", k);
        }
        s = lt_luai_tostr(st, v);
        lt_luai_sb_add(st, sb, s->s, s->len);
        if (k < j && sep != NULL) { lt_luai_sb_add(st, sb, sep->s, sep->len); }
    }
    pushv(st, lt_luai_strv(sb));
    return 1;
}

LIBFN(t_pack)
{
    lt_ltab *t = lt_luai_tab_new(st);
    int i;
    LT_UNUSED(ud);
    lt_luai_tab_reserve(st, t, (size_t)n);
    for (i = 0; i < n; i++) { lt_luai_tab_append(st, t, st->stack[a + i]); }
    lt_luai_tab_trim(t);
    lt_luai_tab_set(st, t, lt_luai_strv(lt_luai_str_new(st, "n", 1)), lt_luai_num((double)n));
    pushv(st, lt_luai_tabv(t));
    return 1;
}

typedef struct sort_ctx {
    lt_lua *st;
    lt_ltab *t;
    lt_lv cmp;
    int has_cmp;
} sort_ctx;

static lt_lv sget(const sort_ctx *c, size_t i) { return lt_luai_tab_get(c->t, lt_luai_num((double)i)); }
static void sset(const sort_ctx *c, size_t i, lt_lv v) { lt_luai_tab_set(c->st, c->t, lt_luai_num((double)i), v); }

static int sless(const sort_ctx *c, lt_lv x, lt_lv y)
{
    if (c->has_cmp) {
        lt_lv args[2], r;
        args[0] = x;
        args[1] = y;
        r = lt_luai_call1(c->st, c->cmp, args, 2);
        return truthy(r);
    }
    return lt_luai_lessthan(c->st, x, y);
}

static void sift(const sort_ctx *c, size_t root, size_t end)
{
    for (;;) {
        size_t child = root * 2u;
        lt_lv vr, vc;
        if (child > end) { return; }
        if (child < end && sless(c, sget(c, child), sget(c, child + 1u))) { child++; }
        vr = sget(c, root);
        vc = sget(c, child);
        if (!sless(c, vr, vc)) { return; }
        sset(c, root, vc);
        sset(c, child, vr);
        root = child;
    }
}

LIBFN(t_sort)
{
    sort_ctx c;
    size_t len, i;
    LT_UNUSED(ud);
    c.st = st;
    c.t = chk_tab(st, a, n, 0);
    c.has_cmp = n > 1 && st->stack[a + 1].t != LV_NIL;
    c.cmp = ARG(1);
    if (c.has_cmp && c.cmp.t != LV_FUN && c.cmp.t != LV_CFN) { argerr(st, 1, "function expected"); }
    len = lt_luai_tab_len(c.t);
    if (len < 2) { return 0; }
    {
        unsigned long lg = 1, v = (unsigned long)len;
        while (v > 1) { v >>= 1; lg++; }
        lt_luai_charge(st, (unsigned long)len * lg / 4u);
    }
    for (i = len / 2u; i >= 1u; i--) { sift(&c, i, len); }
    for (i = len; i > 1u; i--) {
        lt_lv top = sget(&c, 1), last = sget(&c, i);
        sset(&c, 1, last);
        sset(&c, i, top);
        sift(&c, 1, i - 1u);
    }
    return 0;
}

/* ------------------------------------------------------------------ open */
void lt_luai_openlibs(lt_lua *st)
{
    static const char *const tnames[6] = { "nil", "boolean", "number", "string", "table", "function" };
    lt_ltab *g = st->globals, *t;
    lt_lv lib;
    int i;
    for (i = 0; i < 6; i++) { st->tn[i] = lt_luai_str_new(st, tnames[i], strlen(tnames[i])); }
    lt_luai_reg(st, g, "print", b_print, NULL);
    lt_luai_reg(st, g, "type", b_type, NULL);
    lt_luai_reg(st, g, "tostring", b_tostring, NULL);
    lt_luai_reg(st, g, "tonumber", b_tonumber, NULL);
    lt_luai_reg(st, g, "next", b_next, NULL);
    lt_luai_reg(st, g, "pairs", b_pairs, NULL);
    lt_luai_reg(st, g, "ipairs", b_ipairs, NULL);
    lt_luai_reg(st, g, "select", b_select, NULL);
    lt_luai_reg(st, g, "unpack", b_unpack, NULL);
    lt_luai_reg(st, g, "error", b_error, NULL);
    lt_luai_reg(st, g, "assert", b_assert, NULL);
    lt_luai_reg(st, g, "pcall", b_pcall, NULL);
    lt_luai_reg(st, g, "xpcall", b_xpcall, NULL);
    lt_luai_reg(st, g, "rawget", b_rawget, NULL);
    lt_luai_reg(st, g, "rawset", b_rawset, NULL);
    lt_luai_reg(st, g, "rawequal", b_rawequal, NULL);
    lt_luai_reg(st, g, "rawlen", b_rawlen, NULL);
    lt_luai_reg(st, g, "setmetatable", b_setmetatable, NULL);
    lt_luai_reg(st, g, "getmetatable", b_getmetatable, NULL);
    lt_luai_reg(st, g, "collectgarbage", b_collectgarbage, NULL);
    st->fn_next = lt_luai_tab_getstr(g, lt_luai_str_new(st, "next", 4));
    {
        lt_lv v;
        v.t = LV_CFN;
        v.u.n = 0.0;
        v.u.c = lt_luai_cfun_new(st, b_inext, NULL, "ipairs_iter");
        st->fn_inext = v;
    }
    set_field(st, g, "_G", lt_luai_tabv(g));
    set_field(st, g, "_VERSION", lt_luai_strv(lt_luai_str_new(st, "Lua 5.4 (clibretune subset)", 27)));

    lib = lib_table(st, "string");
    t = lib.u.tab;
    st->slib = t;
    lt_luai_reg(st, t, "len", s_len, NULL);
    lt_luai_reg(st, t, "sub", s_sub, NULL);
    lt_luai_reg(st, t, "upper", s_upper, NULL);
    lt_luai_reg(st, t, "lower", s_lower, NULL);
    lt_luai_reg(st, t, "rep", s_rep, NULL);
    lt_luai_reg(st, t, "reverse", s_reverse, NULL);
    lt_luai_reg(st, t, "byte", s_byte, NULL);
    lt_luai_reg(st, t, "char", s_char, NULL);
    lt_luai_reg(st, t, "format", s_format, NULL);
    lt_luai_reg(st, t, "find", s_find, NULL);
    lt_luai_reg(st, t, "match", s_match, NULL);
    lt_luai_reg(st, t, "gmatch", s_gmatch, NULL);
    lt_luai_reg(st, t, "gsub", s_gsub, NULL);

    lib = lib_table(st, "math");
    t = lib.u.tab;
    lt_luai_reg(st, t, "abs", m_abs, NULL);
    lt_luai_reg(st, t, "floor", m_floor, NULL);
    lt_luai_reg(st, t, "ceil", m_ceil, NULL);
    lt_luai_reg(st, t, "sqrt", m_sqrt, NULL);
    lt_luai_reg(st, t, "sin", m_sin, NULL);
    lt_luai_reg(st, t, "cos", m_cos, NULL);
    lt_luai_reg(st, t, "tan", m_tan, NULL);
    lt_luai_reg(st, t, "asin", m_asin, NULL);
    lt_luai_reg(st, t, "acos", m_acos, NULL);
    lt_luai_reg(st, t, "atan", m_atan, NULL);
    lt_luai_reg(st, t, "exp", m_exp, NULL);
    lt_luai_reg(st, t, "log", m_log, NULL);
    lt_luai_reg(st, t, "pow", m_pow, NULL);
    lt_luai_reg(st, t, "fmod", m_fmod, NULL);
    lt_luai_reg(st, t, "modf", m_modf, NULL);
    lt_luai_reg(st, t, "deg", m_deg, NULL);
    lt_luai_reg(st, t, "rad", m_rad, NULL);
    lt_luai_reg(st, t, "max", m_max, NULL);
    lt_luai_reg(st, t, "min", m_min, NULL);
    lt_luai_reg(st, t, "random", m_random, NULL);
    lt_luai_reg(st, t, "randomseed", m_randomseed, NULL);
    set_field(st, t, "pi", lt_luai_num(3.14159265358979323846));
    set_field(st, t, "huge", lt_luai_num(HUGE_VAL));

    lib = lib_table(st, "table");
    t = lib.u.tab;
    lt_luai_reg(st, t, "insert", t_insert, NULL);
    lt_luai_reg(st, t, "remove", t_remove, NULL);
    lt_luai_reg(st, t, "concat", t_concat, NULL);
    lt_luai_reg(st, t, "sort", t_sort, NULL);
    lt_luai_reg(st, t, "unpack", b_unpack, NULL);
    lt_luai_reg(st, t, "pack", t_pack, NULL);

    st->s_index = lt_luai_str_new(st, "__index", 7);
    st->s_newindex = lt_luai_str_new(st, "__newindex", 10);
    st->s_call = lt_luai_str_new(st, "__call", 6);
    st->s_tostring = lt_luai_str_new(st, "__tostring", 10);
    st->s_len = lt_luai_str_new(st, "__len", 5);
    st->s_eq = lt_luai_str_new(st, "__eq", 4);
    st->s_lt = lt_luai_str_new(st, "__lt", 4);
    st->s_le = lt_luai_str_new(st, "__le", 4);
    st->s_unm = lt_luai_str_new(st, "__unm", 5);
    {
        static const char *const an[8] = { "__add", "__sub", "__mul", "__div", "__mod", "__pow",
                                           "__idiv", "__concat" };
        for (i = 0; i < 8; i++) { st->s_arith[i] = lt_luai_str_new(st, an[i], strlen(an[i])); }
    }
}
