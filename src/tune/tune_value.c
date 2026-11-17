/* tune_value.c - tune values, number formatting/parsing, entries.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include "tune_priv.h"

void lt_tune_value_init(lt_tune_value *v)
{
    memset(v, 0, sizeof *v);
    v->kind = LT_TV_SCALAR;
}

void lt_tune_value_clear(lt_tune_value *v)
{
    if (v->arr) lt_free(v->arr);
    if (v->str) lt_free(v->str);
    lt_tune_value_init(v);
}

void lt_tune_value_copy(lt_tune_value *dst, const lt_tune_value *src)
{
    lt_tune_value_init(dst);
    dst->kind = src->kind;
    dst->scalar = src->scalar;
    dst->boolean = src->boolean;
    dst->n = src->n;
    if (src->arr && src->n) {
        dst->arr = (double *)lt_malloc(src->n * sizeof(double));
        memcpy(dst->arr, src->arr, src->n * sizeof(double));
    } else {
        dst->n = 0;
    }
    if (src->str) dst->str = lt_strdup(src->str);
    else if (src->kind == LT_TV_STRING) dst->str = lt_strdup("");
}

static lt_bool near_eq(double a, double b)
{
    double d = a - b;
    if (d < 0) d = -d;
    return d < 1e-9;
}

lt_bool lt_tune_value_equal(const lt_tune_value *a, const lt_tune_value *b)
{
    size_t i;
    if (a->kind != b->kind) {
        /* INTEGRATION FIX: a "true"/"false" option label (STRING, from the INI bits
         * options) re-reads from .msq as BOOL; treat the pair as equal. */
        const lt_tune_value *s = a->kind == LT_TV_STRING ? a : b;
        const lt_tune_value *o = a->kind == LT_TV_STRING ? b : a;
        if (s->kind == LT_TV_STRING && o->kind == LT_TV_BOOL && s->str) {
            if (lt_stricmp(s->str, "true") == 0) return o->boolean != 0;
            if (lt_stricmp(s->str, "false") == 0) return o->boolean == 0;
        }
        return LT_FALSE;
    }
    switch (a->kind) {
    case LT_TV_SCALAR: return near_eq(a->scalar, b->scalar);
    case LT_TV_BOOL: return (a->boolean != 0) == (b->boolean != 0);
    case LT_TV_STRING: return strcmp(a->str ? a->str : "", b->str ? b->str : "") == 0;
    case LT_TV_ARRAY:
        if (a->n != b->n) return LT_FALSE;
        for (i = 0; i < a->n; i++)
            if (!near_eq(a->arr[i], b->arr[i])) return LT_FALSE;
        return LT_TRUE;
    }
    return LT_FALSE;
}

/* ---------------------------------------------------------------------- */
/* number formatting                                                       */
/* ---------------------------------------------------------------------- */
/* Exact decimal expansion of a finite positive double with a base 10000 bignum (lt_snprintf is
 * only exact to 15 digits). digits receives the significant digits (no trailing zeros, cap
 * >= 1100), *ex the decimal exponent of the first digit. Returns the digit count. */
#define BN_BASE 10000UL
#define BN_MAX 400

static void bn_mul(unsigned long *l, int *n, unsigned long m)
{
    unsigned long carry = 0;
    int i;
    for (i = 0; i < *n; i++) {
        unsigned long v = l[i] * m + carry;
        l[i] = v % BN_BASE;
        carry = v / BN_BASE;
    }
    while (carry && *n < BN_MAX) {
        l[(*n)++] = carry % BN_BASE;
        carry /= BN_BASE;
    }
}

static int exact_digits(double a, char *digits, int *ex)
{
    unsigned long l[BN_MAX];
    int n = 0, e2, k, i, len = 0;
    double fr, mant;
    char tmp[BN_MAX * 4 + 8] = { 0 };

    fr = frexp(a, &e2);
    mant = ldexp(fr, 53);
    e2 -= 53;
    while (mant >= 1.0 && n < BN_MAX) {
        double q = floor(mant / 10000.0);
        l[n++] = (unsigned long)(mant - q * 10000.0);
        mant = q;
    }
    if (n == 0) l[n++] = 0;
    if (e2 >= 0) {
        while (e2 >= 15) {
            bn_mul(l, &n, 32768UL);
            e2 -= 15;
        }
        if (e2 > 0) bn_mul(l, &n, 1UL << e2);
        k = 0;
    } else {
        k = -e2;
        e2 = k;
        while (e2 >= 6) {
            bn_mul(l, &n, 15625UL);
            e2 -= 6;
        }
        while (e2 > 0) {
            bn_mul(l, &n, 5UL);
            e2--;
        }
    }
    /* integer M = l, value = M / 10^k */
    for (i = n - 1; i >= 0; i--) {
        lt_snprintf(tmp + len, 8, i == n - 1 ? "%lu" : "%04lu", l[i]);
        len += (int)strlen(tmp + len);
    }
    *ex = len - 1 - k;
    while (len > 1 && tmp[len - 1] == '0') len--;
    memcpy(digits, tmp, (size_t)len);
    digits[len] = '\0';
    return len;
}

/* Shortest digit string whose correctly rounded p-digit form parses back to a. */
static int shortest_digits(double a, char *digits, int *ex)
{
    static char full[BN_MAX * 4 + 8];
    int nfull, p, i, ex0;
    char buf[64];

    nfull = exact_digits(a, full, &ex0);
    for (p = 1; p <= 17; p++) {
        int e = ex0;
        char d[24];
        if (p >= nfull) {
            memcpy(d, full, (size_t)nfull);
            for (i = nfull; i < p; i++) d[i] = '0';
        } else {
            int up;
            memcpy(d, full, (size_t)p);
            if (full[p] > '5') up = 1;
            else if (full[p] < '5') up = 0;
            else {
                int more = 0;
                for (i = p + 1; i < nfull; i++)
                    if (full[i] != '0') more = 1;
                up = more ? 1 : ((d[p - 1] - '0') & 1);
            }
            if (up) {
                for (i = p - 1; i >= 0; i--) {
                    if (d[i] == '9') d[i] = '0';
                    else {
                        d[i]++;
                        break;
                    }
                }
                if (i < 0) {   /* 99..9 -> 100..0 */
                    d[0] = '1';
                    for (i = 1; i < p; i++) d[i] = '0';
                    e++;
                }
            }
        }
        d[p] = '\0';
        lt_snprintf(buf, sizeof buf, "%c.%se%d", d[0], d + 1, e);
        if (strtod(buf, NULL) == a || p == 17) {
            memcpy(digits, d, (size_t)p + 1);
            *ex = e;
            return p;
        }
    }
    return 0;
}

void lt_tune_fmt_num(char *dst, size_t cap, double v, int rust_mode)
{
    char digits[40], out[420];
    int nd, ex, pp, neg;
    size_t o = 0;
    double a;

    if (!rust_mode) {
        if (!lt_isfinite(v) || v == 0.0) {
            lt_strlcpy(dst, "0", cap);
            return;
        }
    } else {
        if (lt_isnan(v)) {
            lt_strlcpy(dst, "NaN", cap);
            return;
        }
        if (!lt_isfinite(v)) {
            lt_strlcpy(dst, v < 0 ? "-inf" : "inf", cap);
            return;
        }
        if (v == 0.0) {
            lt_strlcpy(dst, (1.0 / v < 0) ? "-0" : "0", cap);
            return;
        }
    }
    neg = v < 0;
    a = neg ? -v : v;
    nd = shortest_digits(a, digits, &ex);
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    digits[nd] = '\0';
    pp = ex + 1;   /* digits before the decimal point */
    if (neg) out[o++] = '-';
    if (pp <= 0) {
        int z;
        out[o++] = '0';
        out[o++] = '.';
        for (z = 0; z < -pp && o < sizeof out - 40; z++) out[o++] = '0';
        memcpy(out + o, digits, (size_t)nd);
        o += (size_t)nd;
    } else if (pp >= nd) {
        int z;
        memcpy(out + o, digits, (size_t)nd);
        o += (size_t)nd;
        for (z = 0; z < pp - nd && o < sizeof out - 2; z++) out[o++] = '0';
    } else {
        memcpy(out + o, digits, (size_t)pp);
        o += (size_t)pp;
        out[o++] = '.';
        memcpy(out + o, digits + pp, (size_t)(nd - pp));
        o += (size_t)(nd - pp);
    }
    out[o] = '\0';
    lt_strlcpy(dst, out, cap);
}

void lt_fmt_f64_shortest(char *dst, size_t cap, double v)
{
    lt_tune_fmt_num(dst, cap, v, 0);
}

/* ---------------------------------------------------------------------- */
/* number parsing                                                          */
/* ---------------------------------------------------------------------- */
lt_bool lt_tune_parse_num(const char *s, size_t n, double *out)
{
    char buf[128];
    char low[16];
    const char *body;
    size_t i, bn;
    int neg = 0;
    char *end;
    double v;

    if (n == 0 || n >= sizeof buf) return LT_FALSE;
    memcpy(buf, s, n);
    buf[n] = '\0';
    body = buf;
    if (*body == '+' || *body == '-') {
        neg = (*body == '-');
        body++;
    }
    bn = strlen(body);
    if (bn == 0) return LT_FALSE;
    if (bn <= 8 && !(body[0] >= '0' && body[0] <= '9') && body[0] != '.') {
        for (i = 0; i < bn; i++) low[i] = (char)tolower((unsigned char)body[i]);
        low[bn] = '\0';
        if (strcmp(low, "inf") == 0 || strcmp(low, "infinity") == 0) {
            *out = neg ? -HUGE_VAL : HUGE_VAL;
            return LT_TRUE;
        }
        if (strcmp(low, "nan") == 0) {
            volatile double z = 0.0;
            *out = z / z;
            return LT_TRUE;
        }
        return LT_FALSE;
    }
    for (i = 0; i < bn; i++) {
        char c = body[i];
        if (!((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-'))
            return LT_FALSE;
    }
    v = strtod(buf, &end);
    if (end == buf || *end != '\0') return LT_FALSE;
    *out = v;
    return LT_TRUE;
}

lt_bool lt_tune_parse_u8(const char *s, int *out)
{
    long v = 0;
    size_t i = 0;
    if (!s) return LT_FALSE;
    if (s[0] == '+') i = 1;
    if (!s[i]) return LT_FALSE;
    for (; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return LT_FALSE;
        v = v * 10 + (s[i] - '0');
        if (v > 255) return LT_FALSE;
    }
    *out = (int)v;
    return LT_TRUE;
}

/* ---------------------------------------------------------------------- */
/* parse_value (spec 1.3)                                                  */
/* ---------------------------------------------------------------------- */
typedef struct vtok { const char *p; size_t n; } vtok;

static int is_sep(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == '\f' || c == '\v';
}

static void split_tokens(const char *s, lt_vec *out)
{
    size_t i = 0;
    while (s[i]) {
        size_t st;
        vtok *t;
        while (s[i] && is_sep(s[i])) i++;
        if (!s[i]) break;
        st = i;
        while (s[i] && !is_sep(s[i])) i++;
        t = (vtok *)lt_vec_push(out);
        t->p = s + st;
        t->n = i - st;
    }
}

static void set_string(lt_tune_value *out, const char *s)
{
    out->kind = LT_TV_STRING;
    out->str = lt_strdup(s);
}

static void set_array_from(lt_tune_value *out, const double *v, size_t n)
{
    out->kind = LT_TV_ARRAY;
    out->arr = (double *)lt_malloc(n * sizeof(double));
    memcpy(out->arr, v, n * sizeof(double));
    out->n = n;
}

void lt_tune_value_parse(lt_tune_value *out, const char *text)
{
    char *tr = lt_strdup(text ? text : "");
    char *t;
    size_t len, i;
    double d;

    lt_tune_value_init(out);
    t = lt_str_trim(tr);
    len = strlen(t);
    if (len >= 2 && t[0] == '"' && t[len - 1] == '"') {
        char *inner = lt_strndup(t + 1, len - 2);
        if (strcmp(inner, "true") == 0) {
            out->kind = LT_TV_BOOL;
            out->boolean = 1;
        } else if (strcmp(inner, "false") == 0) {
            out->kind = LT_TV_BOOL;
            out->boolean = 0;
        } else {
            char *u = lt_xml_unescape(inner);
            out->kind = LT_TV_STRING;
            out->str = u;
        }
        lt_free(inner);
        lt_free(tr);
        return;
    }
    if (len >= 2 && t[0] == '[' && t[len - 1] == ']') {
        lt_vec toks, vals;
        const char *s = t;
        char *stripped;
        size_t a = 0, b = len;
        while (a < b && s[a] == '[') a++;
        while (b > a && s[b - 1] == ']') b--;
        stripped = lt_strndup(s + a, b - a);
        lt_vec_init(&toks, sizeof(vtok));
        lt_vec_init(&vals, sizeof(double));
        split_tokens(stripped, &toks);
        for (i = 0; i < toks.len; i++) {
            vtok *k = &LT_VEC_AT(&toks, vtok, i);
            if (lt_tune_parse_num(k->p, k->n, &d)) *(double *)lt_vec_push(&vals) = d;
        }
        if (vals.len > 1) set_array_from(out, (const double *)vals.data, vals.len);
        else if (vals.len == 1) out->scalar = ((double *)vals.data)[0];
        else set_string(out, t);
        lt_vec_free(&toks);
        lt_vec_free(&vals);
        lt_free(stripped);
        lt_free(tr);
        return;
    }
    if (strchr(t, ' ') || strchr(t, '\n') || strchr(t, '\t') || strchr(t, '\r')) {
        lt_vec toks, vals;
        int all = 1;
        lt_vec_init(&toks, sizeof(vtok));
        lt_vec_init(&vals, sizeof(double));
        split_tokens(t, &toks);
        for (i = 0; i < toks.len; i++) {
            vtok *k = &LT_VEC_AT(&toks, vtok, i);
            if (lt_tune_parse_num(k->p, k->n, &d)) {
                *(double *)lt_vec_push(&vals) = d;
            } else {
                all = 0;
                break;
            }
        }
        if (all && vals.len > 1) set_array_from(out, (const double *)vals.data, vals.len);
        else if (all && vals.len == 1) out->scalar = ((double *)vals.data)[0];
        else set_string(out, t);
        lt_vec_free(&toks);
        lt_vec_free(&vals);
        lt_free(tr);
        return;
    }
    if (lt_tune_parse_num(t, len, &d)) {
        out->scalar = d;
    } else if (strcmp(t, "true") == 0) {
        out->kind = LT_TV_BOOL;
        out->boolean = 1;
    } else if (strcmp(t, "false") == 0) {
        out->kind = LT_TV_BOOL;
    } else {
        set_string(out, t);
    }
    lt_free(tr);
}

/* ---------------------------------------------------------------------- */
/* entries / misc                                                          */
/* ---------------------------------------------------------------------- */
lt_tune_entry *lt_tune_entry_new(void)
{
    lt_tune_entry *e = (lt_tune_entry *)lt_calloc(1, sizeof *e);
    lt_tune_value_init(&e->value);
    return e;
}

void lt_tune_entry_free(lt_tune_entry *e)
{
    if (!e) return;
    lt_tune_value_clear(&e->value);
    lt_free(e->attrs.digits);
    lt_free(e->attrs.units);
    lt_free(e->attrs.rows);
    lt_free(e->attrs.cols);
    lt_free(e);
}

char *lt_tune_now_rfc3339(void)
{
    char buf[40];
    time_t now = time(NULL);
    struct tm *g = gmtime(&now);
    if (!g || strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S+00:00", g) == 0)
        lt_strlcpy(buf, "1970-01-01T00:00:00+00:00", sizeof buf);
    return lt_strdup(buf);
}
