/* lt_base.c - implementation of the portability layer declared in lt.h.
 * Strict C89 (also valid C99..C23). SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <errno.h>
#include "lt.h"

/* ------------------------------------------------------------------ */
/* memory                                                              */
/* ------------------------------------------------------------------ */

static void oom(void)
{
    fputs("clibretune: out of memory\n", stderr);
    abort();
}

void *lt_malloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

void *lt_calloc(size_t count, size_t size)
{
    void *p;
    if (count == 0 || size == 0) {
        count = 1;
        size = 1;
    }
    p = calloc(count, size);
    if (!p) oom();
    return p;
}

void *lt_realloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) oom();
    return q;
}

void lt_free(void *p)
{
    free(p);
}

/* ------------------------------------------------------------------ */
/* overflow-checked arithmetic and byte serialization (UNIVERSAL_C 5)  */
/* ------------------------------------------------------------------ */

lt_bool lt_add_int(int a, int b, int *out)
{
    if ((b > 0 && a > INT_MAX - b) || (b < 0 && a < INT_MIN - b)) return LT_FALSE;
    *out = a + b;
    return LT_TRUE;
}

lt_bool lt_mul_int(int a, int b, int *out)
{
    if (a > 0) {
        if (b > 0) {
            if (a > INT_MAX / b) return LT_FALSE;
        } else {
            if (b < INT_MIN / a) return LT_FALSE;
        }
    } else if (a < 0) {
        if (b > 0) {
            if (a < INT_MIN / b) return LT_FALSE;
        } else {
            if (b != 0 && a < INT_MAX / b) return LT_FALSE;
        }
    }
    *out = a * b;
    return LT_TRUE;
}

lt_bool lt_mul_size(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > LT_SIZE_MAX / a) return LT_FALSE;
    *out = a * b;
    return LT_TRUE;
}

lt_bool lt_add_size(size_t a, size_t b, size_t *out)
{
    if (b > LT_SIZE_MAX - a) return LT_FALSE;
    *out = a + b;
    return LT_TRUE;
}

void lt_store_u32le(lt_u8 *p, lt_u32 v)
{
    p[0] = (lt_u8)(v & 0xFFu);
    p[1] = (lt_u8)((v >> 8) & 0xFFu);
    p[2] = (lt_u8)((v >> 16) & 0xFFu);
    p[3] = (lt_u8)((v >> 24) & 0xFFu);
}

lt_u32 lt_load_u32le(const lt_u8 *p)
{
    return (lt_u32)p[0] | ((lt_u32)p[1] << 8) | ((lt_u32)p[2] << 16) | ((lt_u32)p[3] << 24);
}

void lt_store_u32be(lt_u8 *p, lt_u32 v)
{
    p[0] = (lt_u8)((v >> 24) & 0xFFu);
    p[1] = (lt_u8)((v >> 16) & 0xFFu);
    p[2] = (lt_u8)((v >> 8) & 0xFFu);
    p[3] = (lt_u8)(v & 0xFFu);
}

lt_u32 lt_load_u32be(const lt_u8 *p)
{
    return ((lt_u32)p[0] << 24) | ((lt_u32)p[1] << 16) | ((lt_u32)p[2] << 8) | (lt_u32)p[3];
}

lt_u32 lt_rotl32(lt_u32 x, unsigned n)
{
    n &= 31u;
    return (lt_u32)(((x << n) | (x >> ((32u - n) & 31u))) & 0xFFFFFFFFUL);
}

#if LT_HAS_64
char *lt_u64_to_dec(char *buf, lt_u64 v)
{
    char tmp[21];
    int i = 0, j = 0;
    do {
        tmp[i++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0);
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}
#endif

/* ------------------------------------------------------------------ */
/* math helpers                                                        */
/* ------------------------------------------------------------------ */

double lt_trunc(double x)
{
    return x < 0.0 ? ceil(x) : floor(x);
}

double lt_round(double x)
{
    double ax = x < 0.0 ? -x : x;
    double t = floor(ax);
    if (ax - t >= 0.5) t += 1.0;
    return x < 0.0 ? -t : t;
}

lt_bool lt_isnan(double x)
{
    return x != x ? LT_TRUE : LT_FALSE;
}

lt_bool lt_isfinite(double x)
{
    if (x != x) return LT_FALSE;
    return (x - x == 0.0) ? LT_TRUE : LT_FALSE;
}

static double make_nan(void)
{
    volatile double z = 0.0;
    return z / z;
}

static int is_negative(double v)
{
    if (v < 0.0) return 1;
    if (v == 0.0) return (1.0 / v) < 0.0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* errors                                                              */
/* ------------------------------------------------------------------ */

static char lt_err_buf[512];

void lt_err_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    lt_vsnprintf(lt_err_buf, sizeof lt_err_buf, fmt, ap);
    va_end(ap);
}

const char *lt_err_get(void)
{
    return lt_err_buf;
}

void lt_err_clear(void)
{
    lt_err_buf[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* formatting                                                          */
/* ------------------------------------------------------------------ */

/* Output sink: either a fixed char array (dst/cap) or a growing lt_buf.
 * Formatting is single-pass so a va_list is never walked twice. */
typedef struct lt_out {
    char   *dst;
    size_t  cap;
    size_t  n;
    lt_buf *buf;
} lt_out;

#if LT_HAS_64
typedef lt_u64 fmt_umax;
#else
typedef unsigned long fmt_umax;
#endif

typedef struct lt_spec {
    int left, plus, space, zero, alt;
    int width;
    int prec; /* -1 = none */
    int kind; /* 0 int, 1 long (no size_t: print sizes as %lu with a cast) */
} lt_spec;

#define FMT_MAXPREC 90
#define FMT_MAXWIDTH 100000

static void out_ch(lt_out *o, char c)
{
    if (o->buf != NULL) {
        lt_buf_putc(o->buf, (int)c);
    } else if (o->dst != NULL && o->n + 1 < o->cap) {
        o->dst[o->n] = c;
    }
    o->n++;
}

static void out_rep(lt_out *o, char c, size_t count)
{
    while (count-- > 0) out_ch(o, c);
}

static void out_mem(lt_out *o, const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) out_ch(o, s[i]);
}

/* prefix, then nzero zeros, then body; padded to width. */
static void emit(lt_out *o, const lt_spec *sp, const char *prefix, size_t plen,
                 size_t nzero, const char *body, size_t blen, int zero_ok)
{
    size_t total = plen + nzero + blen;
    size_t pad = (sp->width > 0 && (size_t)sp->width > total) ? (size_t)sp->width - total : 0;

    if (sp->left) {
        out_mem(o, prefix, plen);
        out_rep(o, '0', nzero);
        out_mem(o, body, blen);
        out_rep(o, ' ', pad);
    } else if (sp->zero && zero_ok) {
        out_mem(o, prefix, plen);
        out_rep(o, '0', nzero + pad);
        out_mem(o, body, blen);
    } else {
        out_rep(o, ' ', pad);
        out_mem(o, prefix, plen);
        out_rep(o, '0', nzero);
        out_mem(o, body, blen);
    }
}

static void do_int(lt_out *o, const lt_spec *sp, int conv, fmt_umax mag, int neg)
{
    char tmp[80];
    int pos = (int)sizeof tmp;
    fmt_umax base = 10;
    const char *digs = "0123456789abcdef";
    const char *prefix = "";
    size_t nzero = 0, len;

    if (conv == 'x') base = 16;
    else if (conv == 'X') {
        base = 16;
        digs = "0123456789ABCDEF";
    } else if (conv == 'o') base = 8;

    if (!(mag == 0 && sp->prec == 0)) {
        do {
            tmp[--pos] = digs[(int)(mag % base)];
            mag /= base;
        } while (mag != 0);
    }
    len = sizeof tmp - (size_t)pos;
    if (sp->prec > 0 && (size_t)sp->prec > len) nzero = (size_t)sp->prec - len;

    if (conv == 'd') {
        if (neg) prefix = "-";
        else if (sp->plus) prefix = "+";
        else if (sp->space) prefix = " ";
    } else if ((conv == 'x' || conv == 'X') && sp->alt && len > 0 && !(len == 1 && tmp[pos] == '0')) {
        prefix = (conv == 'x') ? "0x" : "0X";
    } else if (conv == 'o' && sp->alt) {
        if (nzero == 0 && (len == 0 || tmp[pos] != '0')) nzero = 1;
    }
    emit(o, sp, prefix, strlen(prefix), nzero, tmp + pos, len, sp->prec < 0);
}

static double p10(int k)
{
    double r = 1.0;
    while (k-- > 0) r *= 10.0;
    return r;
}

/* scale v by 10^k (k may be negative), in exact-power chunks */
static double scale10(double v, int k)
{
    int step;
    while (k > 0) {
        step = k > 22 ? 22 : k;
        v *= p10(step);
        k -= step;
    }
    while (k < 0) {
        step = -k > 22 ? 22 : -k;
        v /= p10(step);
        k += step;
    }
    return v;
}

/* Fixed notation of non-negative finite av with prec decimals into out; returns length. */
static size_t fmt_fixed(char *out, double av, int prec, int alt)
{
    char dig[480];
    char rev[340];
    int nd = 0, r = 0, i, start, total, ilen;
    size_t pos = 0;
    double s;

    if (prec > FMT_MAXPREC + 10) prec = FMT_MAXPREC + 10;
    s = av * p10(prec);
    start = 0;
    if (s < 9.0e15) {
        double n = floor(s + 0.5);
        while (n >= 1.0) {
            double d = fmod(n, 10.0);
            rev[r++] = (char)('0' + (int)d);
            n = (n - d) / 10.0;
        }
        while (r < prec + 1) rev[r++] = '0';
        for (i = r - 1; i >= 0; i--) dig[nd++] = rev[i];
    } else {
        double ip = floor(av);
        double fr = av - ip;
        int d;
        while (ip >= 1.0 && r < 330) {
            double dd = fmod(ip, 10.0);
            rev[r++] = (char)('0' + (int)dd);
            ip = (ip - dd) / 10.0;
        }
        if (r == 0) rev[r++] = '0';
        dig[nd++] = '0'; /* carry sentinel */
        for (i = r - 1; i >= 0; i--) dig[nd++] = rev[i];
        for (i = 0; i < prec; i++) {
            fr *= 10.0;
            d = (int)fr;
            fr -= (double)d;
            dig[nd++] = (char)('0' + d);
        }
        fr *= 10.0;
        if ((int)fr >= 5) {
            i = nd - 1;
            while (dig[i] == '9') {
                dig[i] = '0';
                i--;
            }
            dig[i]++;
        }
        if (dig[0] == '0') start = 1;
    }
    total = nd - start;
    ilen = total - prec;
    for (i = 0; i < ilen; i++) out[pos++] = dig[start + i];
    if (prec > 0 || alt) out[pos++] = '.';
    for (i = 0; i < prec; i++) out[pos++] = dig[start + ilen + i];
    return pos;
}

/* Generate nd (1..15) significant decimal digits of av; returns decimal exponent. */
static int sci_digits(double av, int nd, char *out)
{
    int e, iter, i;
    double s, n;

    if (!(av > 0.0)) {
        for (i = 0; i < nd; i++) out[i] = '0';
        return 0;
    }
    e = (int)floor(log10(av));
    n = 0.0;
    for (iter = 0; iter < 6; iter++) {
        s = scale10(av, nd - 1 - e);
        n = floor(s + 0.5);
        if (n >= p10(nd)) {
            e++;
            continue;
        }
        if (n < p10(nd - 1)) {
            e--;
            continue;
        }
        break;
    }
    if (n >= p10(nd)) n = p10(nd) - 1.0;
    for (i = nd - 1; i >= 0; i--) {
        double d = fmod(n, 10.0);
        out[i] = (char)('0' + (int)d);
        n = (n - d) / 10.0;
    }
    return e;
}

static size_t put_exp(char *out, int e, int upper)
{
    size_t pos = 0;
    char t[8];
    int k = 0;
    out[pos++] = upper ? 'E' : 'e';
    if (e < 0) {
        out[pos++] = '-';
        e = -e;
    } else {
        out[pos++] = '+';
    }
    do {
        t[k++] = (char)('0' + e % 10);
        e /= 10;
    } while (e > 0);
    if (k < 2) t[k++] = '0';
    while (k > 0) out[pos++] = t[--k];
    if (!upper) out[0] = 'e';
    return pos;
}

/* mantissa d.ddd from digits dg[0..nfrac]; nfrac fractional digits */
static size_t put_mant(char *out, const char *dg, int nfrac, int alt)
{
    size_t pos = 0;
    int i;
    out[pos++] = dg[0];
    if (nfrac > 0 || alt) out[pos++] = '.';
    for (i = 1; i <= nfrac; i++) out[pos++] = dg[i];
    return pos;
}

static void do_float(lt_out *o, const lt_spec *sp, int conv, double v)
{
    char body[640];
    char dg[128];
    size_t blen = 0;
    const char *prefix = "";
    int neg, upper, finite = 1, prec, e, gen, i;
    double av;
    int lc = conv;

    upper = (conv == 'F' || conv == 'E' || conv == 'G');
    if (upper) lc = conv - 'A' + 'a';

    if (v != v) {
        strcpy(body, upper ? "NAN" : "nan");
        blen = 3;
        finite = 0;
        neg = 0;
    } else {
        neg = is_negative(v);
        av = neg ? -v : v;
        if (av > DBL_MAX) {
            strcpy(body, upper ? "INF" : "inf");
            blen = 3;
            finite = 0;
        } else {
            prec = sp->prec < 0 ? 6 : sp->prec;
            if (prec > FMT_MAXPREC) prec = FMT_MAXPREC;
            if (lc == 'f') {
                blen = fmt_fixed(body, av, prec, sp->alt);
            } else if (lc == 'e') {
                gen = prec + 1 > 15 ? 15 : prec + 1;
                e = sci_digits(av, gen, dg);
                for (i = gen; i < prec + 1; i++) dg[i] = '0';
                blen = put_mant(body, dg, prec, sp->alt);
                blen += put_exp(body + blen, e, upper);
            } else { /* g */
                int P = prec == 0 ? 1 : prec;
                gen = P > 15 ? 15 : P;
                e = sci_digits(av, gen, dg);
                if (e >= -4 && e < P) {
                    blen = fmt_fixed(body, av, P - 1 - e, sp->alt);
                    if (!sp->alt && memchr(body, '.', blen)) {
                        while (blen > 0 && body[blen - 1] == '0') blen--;
                        if (blen > 0 && body[blen - 1] == '.') blen--;
                    }
                } else {
                    int nfrac = P - 1;
                    for (i = gen; i < P; i++) dg[i] = '0';
                    if (!sp->alt) {
                        while (nfrac > 0 && dg[nfrac] == '0') nfrac--;
                    }
                    blen = put_mant(body, dg, nfrac, sp->alt);
                    blen += put_exp(body + blen, e, upper);
                }
            }
        }
    }
    if (neg) prefix = "-";
    else if (sp->plus && v == v) prefix = "+";
    else if (sp->space && v == v) prefix = " ";
    emit(o, sp, prefix, strlen(prefix), 0, body, blen, finite);
}

static void do_str(lt_out *o, const lt_spec *sp, const char *s)
{
    size_t len = 0;
    if (!s) s = "(null)";
    if (sp->prec >= 0) {
        while (len < (size_t)sp->prec && s[len]) len++;
    } else {
        len = strlen(s);
    }
    emit(o, sp, "", 0, 0, s, len, 0);
}

/* The one formatting engine: walks fmt once, writing to the sink in o. */
static void fmt_run(lt_out *op, const char *fmt, va_list ap)
{
    lt_out o;
    const char *p = fmt;
    lt_spec sp;
    int conv, v;

    o = *op;
    while (*p) {
        if (*p != '%') {
            out_ch(&o, *p++);
            continue;
        }
        p++;
        memset(&sp, 0, sizeof sp);
        sp.prec = -1;
        for (;; p++) {
            if (*p == '-') sp.left = 1;
            else if (*p == '+') sp.plus = 1;
            else if (*p == ' ') sp.space = 1;
            else if (*p == '0') sp.zero = 1;
            else if (*p == '#') sp.alt = 1;
            else break;
        }
        if (*p == '*') {
            v = va_arg(ap, int);
            if (v < 0) {
                sp.left = 1;
                v = (v == INT_MIN) ? INT_MAX : -v;
            }
            sp.width = v > FMT_MAXWIDTH ? FMT_MAXWIDTH : v;
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                if (sp.width < FMT_MAXWIDTH) sp.width = sp.width * 10 + (*p - '0');
                p++;
            }
        }
        if (*p == '.') {
            p++;
            if (*p == '*') {
                v = va_arg(ap, int);
                sp.prec = v < 0 ? -1 : (v > FMT_MAXWIDTH ? FMT_MAXWIDTH : v);
                p++;
            } else {
                sp.prec = 0;
                while (*p >= '0' && *p <= '9') {
                    if (sp.prec < FMT_MAXWIDTH) sp.prec = sp.prec * 10 + (*p - '0');
                    p++;
                }
            }
        }
        for (;; p++) {
            if (*p == 'l') sp.kind = 1;
            else if (*p == 'h') continue;
            else break;
        }
        if (sp.left) sp.zero = 0;
        conv = (unsigned char)*p;
        if (conv == 0) break;
        p++;
        switch (conv) {
        case '%':
            out_ch(&o, '%');
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            emit(&o, &sp, "", 0, 0, &c, 1, 0);
            break;
        }
        case 's':
            do_str(&o, &sp, va_arg(ap, const char *));
            break;
        case 'd':
        case 'i': {
            long lv;
            if (sp.kind == 1) lv = va_arg(ap, long);
            else lv = (long)va_arg(ap, int);
            do_int(&o, &sp, 'd', lv < 0 ? 0UL - (unsigned long)lv : (unsigned long)lv, lv < 0);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long uv;
            if (sp.kind == 1) uv = va_arg(ap, unsigned long);
            else uv = (unsigned long)va_arg(ap, unsigned int);
            do_int(&o, &sp, conv, uv, 0);
            break;
        }
        case 'p': {
            void *pv = va_arg(ap, void *);
            sp.alt = 1;
            do_int(&o, &sp, 'x', (fmt_umax)(size_t)pv, 0);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            do_float(&o, &sp, conv, va_arg(ap, double));
            break;
        default:
            /* unknown conversion: emit literally */
            out_ch(&o, '%');
            out_ch(&o, (char)conv);
            break;
        }
    }
    *op = o;
}

int lt_vsnprintf(char *dst, size_t cap, const char *fmt, va_list ap)
{
    lt_out o;

    o.dst = dst;
    o.cap = cap;
    o.n = 0;
    o.buf = NULL;
    fmt_run(&o, fmt, ap);
    if (dst != NULL && cap > 0) dst[o.n < cap ? o.n : cap - 1] = '\0';
    return o.n > (size_t)INT_MAX ? INT_MAX : (int)o.n;
}

int lt_snprintf(char *dst, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = lt_vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    return n;
}

void lt_fmt_double(char *dst, size_t cap, double v, int max_decimals)
{
    char tmp[640];
    size_t len;

    if (max_decimals < 0) max_decimals = 0;
    if (max_decimals > FMT_MAXPREC) max_decimals = FMT_MAXPREC;
    lt_snprintf(tmp, sizeof tmp, "%.*f", max_decimals, v);
    len = strlen(tmp);
    if (strchr(tmp, '.')) {
        while (len > 0 && tmp[len - 1] == '0') len--;
        if (len > 0 && tmp[len - 1] == '.') len--;
        tmp[len] = '\0';
    }
    if (strcmp(tmp, "-0") == 0) strcpy(tmp, "0");
    lt_strlcpy(dst, tmp, cap);
}

/* ------------------------------------------------------------------ */
/* strings                                                             */
/* ------------------------------------------------------------------ */

static int lower_ascii(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

char *lt_strdup(const char *s)
{
    size_t n = strlen(s);
    char *p = (char *)lt_malloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *lt_strndup(const char *s, size_t n)
{
    size_t len = 0;
    char *p;
    while (len < n && s[len]) len++;
    p = (char *)lt_malloc(len + 1);
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

int lt_strnicmp(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int ca = lower_ascii((unsigned char)a[i]);
        int cb = lower_ascii((unsigned char)b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
        if (ca == 0) return 0;
    }
    return 0;
}

int lt_stricmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = lower_ascii((unsigned char)*a);
        int cb = lower_ascii((unsigned char)*b);
        if (ca != cb) return ca < cb ? -1 : 1;
        if (ca == 0) return 0;
    }
}

lt_bool lt_str_starts(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0 ? LT_TRUE : LT_FALSE;
}

lt_bool lt_str_ends(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    if (lx > ls) return LT_FALSE;
    return strcmp(s + ls - lx, suffix) == 0 ? LT_TRUE : LT_FALSE;
}

lt_bool lt_str_istarts(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return lt_strnicmp(s, prefix, n) == 0 && strlen(s) >= n ? LT_TRUE : LT_FALSE;
}

char *lt_str_trim(char *s)
{
    size_t len = strlen(s), start = 0;
    while (len > 0 && is_space((unsigned char)s[len - 1])) len--;
    while (start < len && is_space((unsigned char)s[start])) start++;
    if (start > 0) memmove(s, s + start, len - start);
    s[len - start] = '\0';
    return s;
}

size_t lt_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (cap > 0) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return n;
}

size_t lt_strlcat(char *dst, const char *src, size_t cap)
{
    size_t dlen = 0, slen = strlen(src);
    while (dlen < cap && dst[dlen]) dlen++;
    if (dlen == cap) return cap + slen;
    lt_strlcpy(dst + dlen, src, cap - dlen);
    return dlen + slen;
}

lt_bool lt_parse_double(const char *s, double *out)
{
    char *end;
    double v;

    while (is_space((unsigned char)*s)) s++;
    if (*s == '\0') return LT_FALSE;
    errno = 0;
    v = strtod(s, &end);
    if (end == s) return LT_FALSE;
    while (is_space((unsigned char)*end)) end++;
    if (*end != '\0') return LT_FALSE;
    if (out) *out = v;
    return LT_TRUE;
}

lt_bool lt_parse_long(const char *s, long *out)
{
    int neg = 0, base = 10, any = 0, d;
    unsigned long acc = 0, limit, ulimit_;

    while (is_space((unsigned char)*s)) s++;
    if (*s == '+' || *s == '-') {
        neg = (*s == '-');
        s++;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    ulimit_ = (unsigned long)LONG_MAX;
    limit = neg ? ulimit_ + 1UL : ulimit_;
    for (;; s++) {
        int c = (unsigned char)*s;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (acc > (limit - (unsigned long)d) / (unsigned long)base) return LT_FALSE;
        acc = acc * (unsigned long)base + (unsigned long)d;
        any = 1;
    }
    if (!any) return LT_FALSE;
    while (is_space((unsigned char)*s)) s++;
    if (*s != '\0') return LT_FALSE;
    if (out) {
        if (neg) *out = (acc == ulimit_ + 1UL) ? LONG_MIN : -(long)acc;
        else *out = (long)acc;
    }
    return LT_TRUE;
}

/* ------------------------------------------------------------------ */
/* buffer                                                              */
/* ------------------------------------------------------------------ */

void lt_buf_init(lt_buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void lt_buf_free(lt_buf *b)
{
    lt_free(b->data);
    lt_buf_init(b);
}

void lt_buf_clear(lt_buf *b)
{
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

void lt_buf_reserve(lt_buf *b, size_t extra)
{
    size_t need;
    if (!lt_add_size(b->len, extra, &need) || !lt_add_size(need, 1, &need)) {
        fputs("clibretune: buffer size overflow\n", stderr);
        abort();
    }
    if (need > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (nc < need) {
            if (!lt_mul_size(nc, 2, &nc)) {
                nc = need;
                break;
            }
        }
        b->data = (char *)lt_realloc(b->data, nc);
        b->cap = nc;
    }
}

void lt_buf_append(lt_buf *b, const void *data, size_t n)
{
    lt_buf_reserve(b, n);
    if (n > 0) memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void lt_buf_puts(lt_buf *b, const char *s)
{
    lt_buf_append(b, s, strlen(s));
}

void lt_buf_putc(lt_buf *b, int c)
{
    unsigned char ch = (unsigned char)(c & 0xFF);
    lt_buf_append(b, &ch, 1);
}

void lt_buf_vprintf(lt_buf *b, const char *fmt, va_list ap)
{
    lt_out o;

    o.dst = NULL;
    o.cap = 0;
    o.n = 0;
    o.buf = b;
    lt_buf_reserve(b, 0);
    fmt_run(&o, fmt, ap);
    b->data[b->len] = '\0';
}

void lt_buf_printf(lt_buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    lt_buf_vprintf(b, fmt, ap);
    va_end(ap);
}

char *lt_buf_take(lt_buf *b)
{
    char *p = b->data;
    if (!p) p = lt_strdup("");
    lt_buf_init(b);
    return p;
}

/* ------------------------------------------------------------------ */
/* vec                                                                 */
/* ------------------------------------------------------------------ */

void lt_vec_init(lt_vec *v, size_t elem_size)
{
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elem = elem_size;
}

void lt_vec_free(lt_vec *v)
{
    lt_free(v->data);
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
}

void *lt_vec_push(lt_vec *v)
{
    char *slot;
    if (v->len == v->cap) {
        size_t nc = v->cap ? v->cap : 4, bytes;
        if (!lt_mul_size(nc, 2, &nc) || !lt_mul_size(nc, v->elem, &bytes)) {
            fputs("clibretune: vector size overflow\n", stderr);
            abort();
        }
        v->data = lt_realloc(v->data, bytes);
        v->cap = nc;
    }
    slot = (char *)v->data + v->len * v->elem;
    memset(slot, 0, v->elem);
    v->len++;
    return slot;
}

void *lt_vec_at(const lt_vec *v, size_t i)
{
    if (i >= v->len) return NULL;
    return (char *)v->data + i * v->elem;
}

void lt_vec_remove(lt_vec *v, size_t i)
{
    char *base;
    if (i >= v->len) return;
    base = (char *)v->data;
    memmove(base + i * v->elem, base + (i + 1) * v->elem, (v->len - i - 1) * v->elem);
    v->len--;
}

void lt_vec_clear(lt_vec *v)
{
    v->len = 0;
}

/* ------------------------------------------------------------------ */
/* hash map                                                            */
/* ------------------------------------------------------------------ */

lt_u32 lt_hash_str(const char *s, lt_bool icase)
{
    unsigned long h = 2166136261UL;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (icase) c = lower_ascii(c);
        h ^= (unsigned long)c;
        h = (h * 16777619UL) & 0xFFFFFFFFUL;
    }
    return (lt_u32)h;
}

void lt_map_init(lt_map *m, lt_bool case_insensitive)
{
    m->slots = NULL;
    m->cap = 0;
    m->len = 0;
    m->icase = case_insensitive;
}

void lt_map_free(lt_map *m)
{
    size_t i;
    for (i = 0; i < m->cap; i++) {
        if (m->slots[i].used) lt_free(m->slots[i].key);
    }
    lt_free(m->slots);
    m->slots = NULL;
    m->cap = 0;
    m->len = 0;
}

static int map_keyeq(const lt_map *m, const char *a, const char *b)
{
    return m->icase ? lt_stricmp(a, b) == 0 : strcmp(a, b) == 0;
}

/* returns slot of matching key, or the empty slot where it would go. cap must be > 0 */
static size_t map_find(const lt_map *m, const char *key, lt_u32 h)
{
    size_t mask = m->cap - 1;
    size_t i = (size_t)h & mask;
    while (m->slots[i].used) {
        if (m->slots[i].hash == h && map_keyeq(m, m->slots[i].key, key)) return i;
        i = (i + 1) & mask;
    }
    return i;
}

static void map_grow(lt_map *m)
{
    size_t ncap = m->cap ? m->cap * 2 : 16;
    lt_map_entry *old = m->slots;
    size_t ocap = m->cap, i, j, mask;

    m->slots = (lt_map_entry *)lt_calloc(ncap, sizeof(lt_map_entry));
    m->cap = ncap;
    mask = ncap - 1;
    for (i = 0; i < ocap; i++) {
        if (!old[i].used) continue;
        j = (size_t)old[i].hash & mask;
        while (m->slots[j].used) j = (j + 1) & mask;
        m->slots[j] = old[i];
    }
    lt_free(old);
}

void lt_map_put(lt_map *m, const char *key, void *val)
{
    lt_u32 h = lt_hash_str(key, m->icase);
    size_t i;

    if (m->cap == 0 || (m->len + 1) * 10 > m->cap * 7) map_grow(m);
    i = map_find(m, key, h);
    if (m->slots[i].used) {
        m->slots[i].val = val;
        return;
    }
    m->slots[i].key = lt_strdup(key);
    m->slots[i].val = val;
    m->slots[i].hash = h;
    m->slots[i].used = 1;
    m->len++;
}

void *lt_map_get(const lt_map *m, const char *key)
{
    size_t i;
    if (m->cap == 0) return NULL;
    i = map_find(m, key, lt_hash_str(key, m->icase));
    return m->slots[i].used ? m->slots[i].val : NULL;
}

lt_bool lt_map_has(const lt_map *m, const char *key)
{
    size_t i;
    if (m->cap == 0) return LT_FALSE;
    i = map_find(m, key, lt_hash_str(key, m->icase));
    return m->slots[i].used ? LT_TRUE : LT_FALSE;
}

/* ------------------------------------------------------------------ */
/* files and paths                                                     */
/* ------------------------------------------------------------------ */

char *lt_file_read(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    lt_buf b;
    char chunk[4096];
    size_t n;

    if (!f) {
        lt_err_set("cannot open '%s' for reading: %s", path, strerror(errno));
        return NULL;
    }
    lt_buf_init(&b);
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) lt_buf_append(&b, chunk, n);
    if (ferror(f)) {
        lt_err_set("read error on '%s'", path);
        fclose(f);
        lt_buf_free(&b);
        return NULL;
    }
    fclose(f);
    if (len_out) *len_out = b.len;
    return lt_buf_take(&b);
}

int lt_file_write(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    size_t w = 0;

    if (!f) {
        lt_err_set("cannot open '%s' for writing: %s", path, strerror(errno));
        return -1;
    }
    if (len > 0) w = fwrite(data, 1, len, f);
    if (w != len || fclose(f) != 0) {
        lt_err_set("write error on '%s'", path);
        return -1;
    }
    return 0;
}

lt_bool lt_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return LT_FALSE;
    fclose(f);
    return LT_TRUE;
}

char *lt_path_join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *r;
    int sep;

    if (la == 0) return lt_strdup(b);
    sep = !(a[la - 1] == '/' || a[la - 1] == '\\');
    r = (char *)lt_malloc(la + lb + 2);
    memcpy(r, a, la);
    if (sep) r[la++] = '/';
    memcpy(r + la, b, lb + 1);
    return r;
}

const char *lt_path_base(const char *path)
{
    const char *base = path;
    for (; *path; path++) {
        if (*path == '/' || *path == '\\') base = path + 1;
    }
    return base;
}

const char *lt_path_ext(const char *path)
{
    const char *base = lt_path_base(path);
    const char *dot = NULL, *p;
    for (p = base; *p; p++) {
        if (*p == '.') dot = p;
    }
    if (!dot || dot == base) return p;
    return dot;
}

/* ------------------------------------------------------------------ */
/* checksums and byte order                                            */
/* ------------------------------------------------------------------ */

static unsigned long crc_table[256];
static int crc_ready = 0;

static void crc_init(void)
{
    unsigned long c;
    int n, k;
    for (n = 0; n < 256; n++) {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++) {
            if (c & 1UL) c = 0xEDB88320UL ^ (c >> 1);
            else c >>= 1;
        }
        crc_table[n] = c & 0xFFFFFFFFUL;
    }
    crc_ready = 1;
}

lt_u32 lt_crc32(lt_u32 crc, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned long c;
    size_t i;

    if (!crc_ready) crc_init();
    c = ((unsigned long)crc & 0xFFFFFFFFUL) ^ 0xFFFFFFFFUL;
    for (i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFFUL] ^ (c >> 8);
    return (lt_u32)((c ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL);
}

lt_u32 lt_rd_be(const lt_u8 *p, int nbytes)
{
    unsigned long v = 0;
    int i;
    for (i = 0; i < nbytes; i++) v = ((v << 8) | p[i]) & 0xFFFFFFFFUL;
    return (lt_u32)v;
}

lt_u32 lt_rd_le(const lt_u8 *p, int nbytes)
{
    unsigned long v = 0;
    int i;
    for (i = nbytes - 1; i >= 0; i--) v = ((v << 8) | p[i]) & 0xFFFFFFFFUL;
    return (lt_u32)v;
}

void lt_wr_be(lt_u8 *p, int nbytes, lt_u32 v)
{
    unsigned long u = (unsigned long)v & 0xFFFFFFFFUL;
    int i;
    for (i = nbytes - 1; i >= 0; i--) {
        p[i] = (lt_u8)(u & 0xFFUL);
        u >>= 8;
    }
}

void lt_wr_le(lt_u8 *p, int nbytes, lt_u32 v)
{
    unsigned long u = (unsigned long)v & 0xFFFFFFFFUL;
    int i;
    for (i = 0; i < nbytes; i++) {
        p[i] = (lt_u8)(u & 0xFFUL);
        u >>= 8;
    }
}

double lt_f32_from_bits(lt_u32 bits)
{
    unsigned long b = (unsigned long)bits & 0xFFFFFFFFUL;
    int neg = (int)(b >> 31);
    int ex = (int)((b >> 23) & 0xFFUL);
    unsigned long man = b & 0x7FFFFFUL;
    double r;

    if (ex == 255) {
        if (man) return make_nan();
        r = HUGE_VAL;
    } else if (ex == 0) {
        r = ldexp((double)man, -149);
    } else {
        r = ldexp((double)(man | 0x800000UL), ex - 150);
    }
    return neg ? -r : r;
}

lt_u32 lt_f32_to_bits(double v)
{
    unsigned long sign = 0, bits;
    double av, m, f, diff;
    int e, biased;

    if (v != v) return (lt_u32)0x7FC00000UL;
    if (is_negative(v)) {
        sign = 0x80000000UL;
        av = -v;
    } else {
        av = v;
    }
    if (av == 0.0) return (lt_u32)sign;
    if (av > DBL_MAX) return (lt_u32)(sign | 0x7F800000UL);

    m = frexp(av, &e); /* av = m * 2^e, m in [0.5, 1) */
    biased = e + 126;
    if (biased >= 255) return (lt_u32)(sign | 0x7F800000UL);
    if (biased <= 0) {
        /* denormal (or underflow to zero): value = man * 2^-149 */
        double x = ldexp(av, 149);
        f = floor(x);
        diff = x - f;
        if (diff > 0.5 || (diff == 0.5 && fmod(f, 2.0) == 1.0)) f += 1.0;
        bits = (unsigned long)f; /* may equal 0x800000 -> smallest normal, which is right */
        return (lt_u32)(sign | bits);
    }
    {
        double x = ldexp(m, 24); /* [2^23, 2^24) */
        f = floor(x);
        diff = x - f;
        if (diff > 0.5 || (diff == 0.5 && fmod(f, 2.0) == 1.0)) f += 1.0;
        if (f >= 16777216.0) {
            f /= 2.0;
            biased++;
            if (biased >= 255) return (lt_u32)(sign | 0x7F800000UL);
        }
        bits = ((unsigned long)biased << 23) | ((unsigned long)f - 0x800000UL);
    }
    return (lt_u32)(sign | bits);
}
