/* ini_text.c - INI lexical helpers: encoding, comments, splitting, escapes, shapes.
 * Spec: docs/spec/01-ini-core.md sections 1.2, 2, 5.3, 5.4, 6.3, 6.4.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "ini_priv.h"

/* ------------------------------------------------------------------ */
/* encoding                                                            */
/* ------------------------------------------------------------------ */

static const unsigned short cp1252_hi[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

/* length of the valid UTF-8 sequence at p (0 if invalid) */
static size_t utf8_seq(const lt_u8 *p, size_t n)
{
    lt_u8 c = p[0];
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) {
        if (n >= 2 && (p[1] & 0xC0) == 0x80) return 2;
        return 0;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (n < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && p[1] < 0xA0) return 0;   /* overlong */
        if (c == 0xED && p[1] >= 0xA0) return 0;  /* surrogates */
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (n < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80)
            return 0;
        if (c == 0xF0 && p[1] < 0x90) return 0;
        if (c == 0xF4 && p[1] >= 0x90) return 0;
        return 4;
    }
    return 0;
}

static void put_utf8(lt_buf *b, unsigned cp)
{
    if (cp < 0x80) {
        lt_buf_putc(b, (int)cp);
    } else if (cp < 0x800) {
        lt_buf_putc(b, (int)(0xC0 | (cp >> 6)));
        lt_buf_putc(b, (int)(0x80 | (cp & 0x3F)));
    } else {
        lt_buf_putc(b, (int)(0xE0 | (cp >> 12)));
        lt_buf_putc(b, (int)(0x80 | ((cp >> 6) & 0x3F)));
        lt_buf_putc(b, (int)(0x80 | (cp & 0x3F)));
    }
}

char *lt_ini_decode(const lt_u8 *bytes, size_t n, size_t *out_len)
{
    size_t i = 0;
    int valid = 1;
    char *res;

    if (n >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        bytes += 3;
        n -= 3;
    }
    while (i < n) {
        size_t k = utf8_seq(bytes + i, n - i);
        if (k == 0) {
            valid = 0;
            break;
        }
        i += k;
    }
    if (valid) {
        res = (char *)lt_malloc(n + 1);
        if (n) memcpy(res, bytes, n);
        res[n] = '\0';
        if (out_len) *out_len = n;
        return res;
    } else {
        lt_buf b;
        lt_buf_init(&b);
        lt_buf_reserve(&b, n + n / 8 + 1);
        for (i = 0; i < n; i++) {
            lt_u8 c = bytes[i];
            if (c >= 0x80 && c <= 0x9F) put_utf8(&b, cp1252_hi[c - 0x80]);
            else put_utf8(&b, c);
        }
        if (!b.data) lt_buf_puts(&b, "");
        if (out_len) *out_len = b.len;
        return lt_buf_take(&b);
    }
}

/* ------------------------------------------------------------------ */
/* comments and splitting                                              */
/* ------------------------------------------------------------------ */

size_t ini_comment_cut(const char *line)
{
    const char *p = line;
    int in_q = 0, has_eq = 0, found_eq = 0;
    size_t i;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '\f' || *p == '\v') p++;
    if (*p == ';') return 0;
    for (i = 0; line[i]; i++) {
        if (line[i] == '"') in_q = !in_q;
        else if (line[i] == '=' && !in_q) {
            has_eq = 1;
            break;
        }
    }
    in_q = 0;
    for (i = 0; line[i]; i++) {
        char c = line[i];
        if (c == '"') {
            in_q = !in_q;
        } else if (c == '=' && !in_q) {
            found_eq = 1;
        } else if (c == ';' && !in_q) {
            if (has_eq) {
                if (found_eq) return i;
            } else {
                return i;
            }
        }
    }
    return i;
}

static char *dup_trim(const char *s, size_t n)
{
    char *r = lt_strndup(s, n);
    lt_str_trim(r);
    return r;
}

size_t ini_split(const char *v, char ***parts)
{
    lt_vec out;
    size_t i = 0, st = 0;
    int in_q = 0;
    long depth = 0;
    size_t n = v ? strlen(v) : 0, cnt;

    lt_vec_init(&out, sizeof(char *));
    for (i = 0; i < n; i++) {
        char c = v[i];
        if (c == '"') {
            in_q = !in_q;
        } else if (!in_q && c == '{') {
            depth++;
        } else if (!in_q && c == '}') {
            depth--;
        } else if (!in_q && c == ',' && depth == 0) {
            *(char **)lt_vec_push(&out) = dup_trim(v + st, i - st);
            st = i + 1;
        }
    }
    *(char **)lt_vec_push(&out) = dup_trim(v ? v + st : "", n - st);
    cnt = out.len;
    *parts = (char **)out.data;
    return cnt;
}

size_t ini_split_naive(const char *v, char ***parts)
{
    lt_vec out;
    size_t i, st = 0, n = v ? strlen(v) : 0, cnt;

    lt_vec_init(&out, sizeof(char *));
    for (i = 0; i < n; i++) {
        if (v[i] == ',') {
            *(char **)lt_vec_push(&out) = dup_trim(v + st, i - st);
            st = i + 1;
        }
    }
    *(char **)lt_vec_push(&out) = dup_trim(v ? v + st : "", n - st);
    cnt = out.len;
    *parts = (char **)out.data;
    return cnt;
}

void ini_strv_free(char **p, size_t n)
{
    size_t i;
    if (!p) return;
    for (i = 0; i < n; i++) lt_free(p[i]);
    lt_free(p);
}

char *ini_dequote(const char *s)
{
    char *t = lt_strdup(s ? s : "");
    size_t n, a = 0;
    char *r;

    lt_str_trim(t);
    n = strlen(t);
    while (n > 0 && t[n - 1] == '"') n--;
    while (a < n && t[a] == '"') a++;
    r = lt_strndup(t + a, n - a);
    lt_free(t);
    return r;
}

char *ini_unbrace(const char *s, int do_trim)
{
    size_t n = strlen(s), a = 0;
    char *r;
    while (n > 0 && s[n - 1] == '}') n--;
    while (a < n && s[a] == '{') a++;
    r = lt_strndup(s + a, n - a);
    if (do_trim) lt_str_trim(r);
    return r;
}

lt_bool ini_is_braced(const char *s)
{
    size_t n;
    while (*s == ' ' || *s == '\t') s++;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    return (n >= 2 && s[0] == '{' && s[n - 1] == '}') ? LT_TRUE : LT_FALSE;
}

char *ini_cond(const char *s)
{
    char *r = ini_unbrace(s, 1);
    if (!r[0] || strcmp(r, "0") == 0 || strcmp(r, "1") == 0) {
        lt_free(r);
        return NULL;
    }
    return r;
}

char *ini_deferred_expr(const char *s)
{
    char *t;
    size_t n;
    char *inner;
    double d;

    if (!s) return NULL;
    t = lt_strdup(s);
    lt_str_trim(t);
    if (ini_parse_f64(t, &d)) {
        lt_free(t);
        return NULL;
    }
    n = strlen(t);
    if (n < 2 || t[0] != '{' || t[n - 1] != '}') {
        lt_free(t);
        return NULL;
    }
    inner = ini_unbrace(t, 1);
    lt_free(t);
    if (!inner[0]) {
        lt_free(inner);
        return NULL;
    }
    return inner;
}

/* ------------------------------------------------------------------ */
/* number parsing                                                      */
/* ------------------------------------------------------------------ */

lt_bool ini_parse_uint(const char *s, unsigned long *out)
{
    unsigned long v = 0;
    int any = 0;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s == '+') s++;
    for (; *s >= '0' && *s <= '9'; s++) {
        unsigned d = (unsigned)(*s - '0');
        if (v > (0xFFFFFFFFUL - d) / 10UL) return LT_FALSE;
        v = v * 10UL + d;
        any = 1;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (!any || *s) return LT_FALSE;
    if (out) *out = v;
    return LT_TRUE;
}

lt_bool ini_parse_f64(const char *s, double *out)
{
    return lt_parse_double(s, out);
}

char *ini_clean(const char *s)
{
    const char *semi = strchr(s, ';');
    return dup_trim(s, semi ? (size_t)(semi - s) : strlen(s));
}

int ini_parse_bool(const char *s)
{
    char *c = ini_clean(s);
    int r = lt_stricmp(c, "true") == 0 || strcmp(c, "1") == 0 || lt_stricmp(c, "on") == 0 ||
            lt_stricmp(c, "yes") == 0;
    lt_free(c);
    return r;
}

/* ------------------------------------------------------------------ */
/* blobs, escapes, substitution                                        */
/* ------------------------------------------------------------------ */

lt_ini_blob ini_blob_new(const char *p, size_t n)
{
    lt_ini_blob b;
    b.p = (char *)lt_malloc(n + 1);
    if (n) memcpy(b.p, p, n);
    b.p[n] = '\0';
    b.n = n;
    return b;
}

lt_ini_blob ini_blob_from_str(const char *s)
{
    return ini_blob_new(s, strlen(s));
}

void ini_blob_free(lt_ini_blob *b)
{
    if (b->p) lt_free(b->p);
    b->p = NULL;
    b->n = 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

lt_ini_blob ini_parse_escape(const char *s, size_t n)
{
    lt_buf b;
    size_t i = 0;
    lt_ini_blob r;

    lt_buf_init(&b);
    while (i < n) {
        if (s[i] == '\\' && i + 1 < n) {
            char c = s[i + 1];
            if ((c == 'x' || c == 'X') && i + 3 < n) {
                int h = hexval((unsigned char)s[i + 2]), l = hexval((unsigned char)s[i + 3]);
                if (h >= 0 && l >= 0) lt_buf_putc(&b, h * 16 + l);
                i += 4;
            } else if (c == '0') {
                lt_buf_putc(&b, 0);
                i += 2;
            } else if (c == 'n') {
                lt_buf_putc(&b, '\n');
                i += 2;
            } else if (c == 'r') {
                lt_buf_putc(&b, '\r');
                i += 2;
            } else if (c == '\\') {
                lt_buf_putc(&b, '\\');
                i += 2;
            } else {
                lt_buf_putc(&b, '\\');
                i += 1;
            }
        } else {
            lt_buf_putc(&b, (unsigned char)s[i]);
            i++;
        }
    }
    r = ini_blob_new(b.data ? b.data : "", b.len);
    lt_buf_free(&b);
    return r;
}

static int is_name_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

lt_ini_blob ini_substitute(const char *s, size_t n, const lt_map *pcvars)
{
    lt_buf b;
    size_t i = 0;
    lt_ini_blob r;

    lt_buf_init(&b);
    while (i < n) {
        size_t ns = 0;
        if (s[i] == '\\' && i + 1 < n && s[i + 1] == '$') ns = i + 2;
        else if (s[i] == '$') ns = i + 1;
        if (ns) {
            size_t ne = ns;
            while (ne < n && is_name_char((unsigned char)s[ne])) ne++;
            if (ne > ns) {
                char *name = lt_strndup(s + ns, ne - ns);
                void *v = pcvars ? lt_map_get(pcvars, name) : NULL;
                lt_free(name);
                if (v) {
                    lt_buf_putc(&b, (int)(((size_t)v - 1) & 0xFF));
                    i = ne;
                    continue;
                }
                lt_buf_append(&b, s + i, ne - i);
                i = ne;
                continue;
            }
        }
        lt_buf_putc(&b, (unsigned char)s[i]);
        i++;
    }
    r = ini_blob_new(b.data ? b.data : "", b.len);
    lt_buf_free(&b);
    return r;
}

/* ------------------------------------------------------------------ */
/* shapes and bit ranges                                               */
/* ------------------------------------------------------------------ */

size_t lt_shape_count(const lt_shape *s)
{
    if (s->kind == LT_SH_1D) return s->n;
    if (s->kind == LT_SH_2D) return s->rows * s->cols;
    return 1;
}

size_t lt_shape_x_size(const lt_shape *s)
{
    if (s->kind == LT_SH_1D) return s->n;
    if (s->kind == LT_SH_2D) return s->cols;
    return 1;
}

size_t lt_shape_y_size(const lt_shape *s)
{
    return s->kind == LT_SH_2D ? s->rows : 1;
}

/* "{name}" -> name (malloc'd) else NULL */
static char *dyn_name(const char *s)
{
    size_t n = strlen(s);
    if (n >= 3 && s[0] == '{' && s[n - 1] == '}' && !strchr(s + 1, '{')) {
        char *r = lt_strndup(s + 1, n - 2);
        lt_str_trim(r);
        return r;
    }
    return NULL;
}

int ini_parse_shape(const char *s, lt_shape *sh, lt_dynsize **dyn_out)
{
    char *t = lt_strdup(s ? s : "");
    char *inner;
    size_t a = 0, n;
    char *x;
    unsigned long u;

    memset(sh, 0, sizeof *sh);
    sh->kind = LT_SH_SCALAR;
    if (dyn_out) *dyn_out = NULL;
    lt_str_trim(t);
    if (!t[0]) {
        lt_free(t);
        return 0;
    }
    n = strlen(t);
    while (a < n && t[a] == '[') a++;
    while (n > a && t[n - 1] == ']') n--;
    inner = lt_strndup(t + a, n - a);
    lt_free(t);
    lt_str_trim(inner);

    /* a dynamic name may itself contain 'x': split only at an x outside braces */
    {
        size_t i;
        int depth = 0;
        x = NULL;
        for (i = 0; inner[i]; i++) {
            if (inner[i] == '{') depth++;
            else if (inner[i] == '}') depth--;
            else if (depth == 0 && (inner[i] == 'x' || inner[i] == 'X')) {
                x = inner + i;
                break;
            }
        }
    }
    if (x) {
        char *l = lt_strndup(inner, (size_t)(x - inner));
        char *r = lt_strdup(x + 1);
        char *dl, *dr;
        unsigned long c, rr;
        lt_str_trim(l);
        lt_str_trim(r);
        dl = dyn_name(l);
        dr = dyn_name(r);
        if (dl && dr) {
            lt_dynsize *d = (lt_dynsize *)lt_calloc(1, sizeof(lt_dynsize));
            d->cols_const = dl;
            d->rows_const = dr;
            sh->kind = LT_SH_2D;
            if (dyn_out) *dyn_out = d;
            else {
                lt_free(dl);
                lt_free(dr);
                lt_free(d);
            }
            dl = dr = NULL;
        } else if (ini_parse_uint(l, &c) && ini_parse_uint(r, &rr)) {
            sh->kind = LT_SH_2D;
            sh->cols = (size_t)(c > 65535UL ? 65535UL : c);
            sh->rows = (size_t)(rr > 65535UL ? 65535UL : rr);
        }
        if (dl) lt_free(dl);
        if (dr) lt_free(dr);
        lt_free(l);
        lt_free(r);
    } else {
        char *dn = dyn_name(inner);
        if (dn) {
            lt_dynsize *d = (lt_dynsize *)lt_calloc(1, sizeof(lt_dynsize));
            d->rows_const = dn;
            sh->kind = LT_SH_1D;
            if (dyn_out) *dyn_out = d;
            else {
                lt_free(dn);
                lt_free(d);
            }
        } else if (ini_parse_uint(inner, &u)) {
            sh->kind = LT_SH_1D;
            sh->n = (size_t)(u > 65535UL ? 65535UL : u);
        }
    }
    lt_free(inner);
    return 0;
}

lt_bool ini_parse_bit_range(const char *s, int *pos, int *size, int *disp)
{
    char *t = lt_strdup(s);
    char *colon;
    size_t a = 0, n;
    unsigned long st, en;
    int d = 0;
    lt_bool ok = LT_FALSE;

    lt_str_trim(t);
    n = strlen(t);
    while (a < n && t[a] == '[') a++;
    while (n > a && t[n - 1] == ']') n--;
    memmove(t, t + a, n - a);
    t[n - a] = '\0';
    colon = strchr(t, ':');
    if (!colon) {
        if (ini_parse_uint(t, &st) && st < 32 && st <= 255) {
            *pos = (int)st;
            *size = 1;
            *disp = 0;
            ok = LT_TRUE;
        }
    } else {
        char *first = lt_strndup(t, (size_t)(colon - t));
        char *rest = lt_strdup(colon + 1);
        char *endtxt;
        char *plus, *minus;
        lt_str_trim(first);
        lt_str_trim(rest);
        endtxt = rest;
        plus = strchr(rest, '+');
        minus = NULL;
        if (!plus) {
            size_t k;
            for (k = strlen(rest); k > 0; k--)
                if (rest[k - 1] == '-') {
                    minus = rest + k - 1;
                    break;
                }
            if (minus && minus == rest) minus = NULL;
        }
        if (plus) {
            unsigned long dv = 0;
            char *after = plus + 1;
            *plus = '\0';
            lt_str_trim(after);
            if (ini_parse_uint(after, &dv) && dv <= 127) d = (int)dv;
        } else if (minus) {
            unsigned long dv = 0;
            char *after = minus + 1;
            *minus = '\0';
            lt_str_trim(after);
            if (ini_parse_uint(after, &dv) && dv <= 128) d = -(int)dv;
        }
        lt_str_trim(endtxt);
        if (ini_parse_uint(first, &st) && ini_parse_uint(endtxt, &en) && st < 32 && en < 32) {
            *pos = (int)st;
            *size = en >= st ? (int)(en - st + 1) : 1;
            *disp = d;
            ok = LT_TRUE;
        }
        lt_free(first);
        lt_free(rest);
    }
    lt_free(t);
    return ok;
}

/* ------------------------------------------------------------------ */
/* colours                                                             */
/* ------------------------------------------------------------------ */

static const struct { const char *name; lt_u32 rgb; } color_names[] = {
    {"white", 0xFFFFFFUL}, {"black", 0x000000UL}, {"red", 0xFF0000UL}, {"green", 0x00FF00UL},
    {"blue", 0x0000FFUL}, {"yellow", 0xFFFF00UL}, {"orange", 0xFFA500UL}, {"cyan", 0x00FFFFUL},
    {"magenta", 0xFF00FFUL}, {"gray", 0x808080UL}, {"grey", 0x808080UL},
    {"lightgray", 0xD3D3D3UL}, {"lightgrey", 0xD3D3D3UL}, {"darkgray", 0xA9A9A9UL},
    {"darkgrey", 0xA9A9A9UL}, {"lime", 0x00FF00UL}, {"maroon", 0x800000UL},
    {"navy", 0x000080UL}, {"olive", 0x808000UL}, {"purple", 0x800080UL},
    {"silver", 0xC0C0C0UL}, {"teal", 0x008080UL}
};

lt_bool lt_ini_parse_color(const char *s, lt_u32 *argb)
{
    char *t;
    size_t i, n;
    lt_bool ok = LT_FALSE;

    if (!s) return LT_FALSE;
    t = lt_strdup(s);
    lt_str_trim(t);
    n = strlen(t);
    if (n > 0 && t[0] == '#') {
        lt_u32 v = 0;
        int good = 1;
        for (i = 1; i < n; i++) {
            int h = hexval((unsigned char)t[i]);
            if (h < 0) {
                good = 0;
                break;
            }
            v = (v << 4) | (lt_u32)h;
        }
        if (good && n == 7) {
            if (argb) *argb = 0xFF000000UL | v;
            ok = LT_TRUE;
        } else if (good && n == 4) {
            lt_u32 r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
            if (argb) *argb = (lt_u32)0xFF000000UL | (r * 17U << 16) | (g * 17U << 8) | (b * 17U);
            ok = LT_TRUE;
        }
    } else {
        for (i = 0; i < LT_COUNTOF(color_names); i++) {
            if (lt_stricmp(t, color_names[i].name) == 0) {
                if (argb) *argb = 0xFF000000UL | color_names[i].rgb;
                ok = LT_TRUE;
                break;
            }
        }
    }
    lt_free(t);
    return ok;
}
