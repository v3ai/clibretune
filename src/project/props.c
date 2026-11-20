/* props.c - Java .properties parser/writer (TunerStudio project.properties).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include "proj_priv.h"
#include "../plat/plat.h"

void lt_props_init(lt_props *p)
{
    memset(p, 0, sizeof *p);
}

void lt_props_free(lt_props *p)
{
    lt_proj_strv_free(p->keys, p->n);
    lt_proj_strv_free(p->vals, p->n);
    lt_proj_strv_free(p->header, p->n_header);
    lt_props_init(p);
}

static long props_find(const lt_props *p, const char *key)
{
    size_t i;
    for (i = 0; i < p->n; i++)
        if (strcmp(p->keys[i], key) == 0) return (long)i;
    return -1;
}

void lt_props_set(lt_props *p, const char *key, const char *val)
{
    long i = props_find(p, key);
    if (i >= 0) {
        lt_free(p->vals[i]);
        p->vals[i] = lt_strdup(val);
        return;
    }
    p->keys = (char **)lt_realloc(p->keys, (p->n + 1) * sizeof(char *));
    p->vals = (char **)lt_realloc(p->vals, (p->n + 1) * sizeof(char *));
    p->keys[p->n] = lt_strdup(key);
    p->vals[p->n] = lt_strdup(val);
    p->n++;
}

lt_bool lt_props_remove(lt_props *p, const char *key)
{
    long i = props_find(p, key);
    size_t k;
    if (i < 0) return LT_FALSE;
    lt_free(p->keys[i]);
    lt_free(p->vals[i]);
    for (k = (size_t)i + 1; k < p->n; k++) {
        p->keys[k - 1] = p->keys[k];
        p->vals[k - 1] = p->vals[k];
    }
    p->n--;
    return LT_TRUE;
}

const char *lt_props_get(const lt_props *p, const char *key)
{
    long i = props_find(p, key);
    return i < 0 ? NULL : p->vals[i];
}

lt_bool lt_props_get_i32(const lt_props *p, const char *key, int *out)
{
    const char *s = lt_props_get(p, key);
    char *end;
    long v;
    if (s == NULL || *s == '\0') return LT_FALSE;
    if (isspace((unsigned char)*s)) return LT_FALSE;
    errno = 0;
    v = strtol(s, &end, 10);
    if (*end != '\0' || errno != 0) return LT_FALSE;
    if (v < -2147483647L - 1L || v > 2147483647L) return LT_FALSE;
    *out = (int)v;
    return LT_TRUE;
}

lt_bool lt_props_get_f64(const lt_props *p, const char *key, double *out)
{
    const char *s = lt_props_get(p, key);
    char *end;
    double v;
    if (s == NULL || *s == '\0') return LT_FALSE;
    if (isspace((unsigned char)*s)) return LT_FALSE;
    v = strtod(s, &end);
    if (*end != '\0') return LT_FALSE;
    *out = v;
    return LT_TRUE;
}

lt_bool lt_props_get_bool(const lt_props *p, const char *key, lt_bool *out)
{
    const char *s = lt_props_get(p, key);
    if (s == NULL) return LT_FALSE;
    *out = (lt_stricmp(s, "true") == 0 || lt_stricmp(s, "yes") == 0 || strcmp(s, "1") == 0);
    return LT_TRUE;
}

/* ---- parsing ---- */
static int odd_trailing_backslashes(const char *s, size_t n)
{
    size_t k = 0;
    while (n > k && s[n - 1 - k] == '\\') k++;
    return (int)(k & 1);
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* unescape the first n bytes of s, appending to b */
static void unescape_to(lt_buf *b, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        char c = s[i++];
        if (c != '\\') {
            lt_buf_putc(b, (unsigned char)c);
            continue;
        }
        if (i >= n) break; /* lone trailing backslash dropped */
        c = s[i++];
        switch (c) {
        case 't': lt_buf_putc(b, '\t'); break;
        case 'n': lt_buf_putc(b, '\n'); break;
        case 'r': lt_buf_putc(b, '\r'); break;
        case 'f': lt_buf_putc(b, '\f'); break;
        case 'u': {
            unsigned long cp = 0;
            size_t k, cnt = 0;
            int ok = 1;
            for (k = 0; k < 4 && i < n; k++, i++) {
                int h = hexv((unsigned char)s[i]);
                cnt++;
                if (h < 0) ok = 0;
                else cp = (cp << 4) | (unsigned long)h;
            }
            if (ok && cnt > 0 && !(cp >= 0xD800 && cp <= 0xDFFF)) lt_proj_put_utf8(b, cp);
            break;
        }
        default: lt_buf_putc(b, (unsigned char)c); break;
        }
    }
}

static char *unescape_dup(const char *s, size_t n)
{
    lt_buf b;
    lt_buf_init(&b);
    unescape_to(&b, s, n);
    if (b.data == NULL) return lt_strdup("");
    return lt_buf_take(&b);
}

int lt_props_parse(lt_props *p, const char *text)
{
    const char *cur = text;
    int in_header = 1;
    char *ck = NULL;      /* continuation key */
    lt_buf cv;            /* continuation value */
    int pending = 0;

    lt_buf_init(&cv);
    while (*cur != '\0') {
        const char *eol = strchr(cur, '\n');
        size_t len = eol ? (size_t)(eol - cur) : strlen(cur);
        const char *line = cur;
        const char *t;
        size_t tl;
        cur = eol ? eol + 1 : cur + len;
        if (len > 0 && line[len - 1] == '\r') len--;

        if (pending) {
            int cont;
            size_t pl;
            t = line;
            tl = len;
            while (tl > 0 && (*t == ' ' || *t == '\t' || *t == '\f')) {
                t++;
                tl--;
            }
            cont = tl > 0 && odd_trailing_backslashes(t, tl);
            pl = cont ? tl - 1 : tl;
            unescape_to(&cv, t, pl);
            if (!cont) {
                lt_props_set(p, ck, cv.data ? cv.data : "");
                lt_free(ck);
                ck = NULL;
                lt_buf_clear(&cv);
                pending = 0;
            }
            continue;
        }

        /* trim */
        t = line;
        tl = len;
        while (tl > 0 && (*t == ' ' || *t == '\t' || *t == '\f')) {
            t++;
            tl--;
        }
        while (tl > 0 && (t[tl - 1] == ' ' || t[tl - 1] == '\t' || t[tl - 1] == '\f')) tl--;
        if (tl == 0) continue;
        if (*t == '#' || *t == '!') {
            if (in_header) {
                p->header = (char **)lt_realloc(p->header, (p->n_header + 1) * sizeof(char *));
                p->header[p->n_header++] = lt_strndup(t, tl);
            }
            continue;
        }
        in_header = 0;
        {
            size_t pos = tl, i;
            int prev_bs = 0;
            char *key;
            const char *v;
            size_t vl;
            int cont;
            for (i = 0; i < tl; i++) {
                char c = t[i];
                if ((c == '=' || c == ':') && !prev_bs) {
                    pos = i;
                    break;
                }
                prev_bs = (c == '\\' && !prev_bs);
            }
            if (pos == tl) {
                key = unescape_dup(t, tl);
                lt_props_set(p, key, "");
                lt_free(key);
                continue;
            }
            {
                size_t kl = pos;
                while (kl > 0 && (t[kl - 1] == ' ' || t[kl - 1] == '\t')) kl--;
                key = unescape_dup(t, kl);
            }
            v = t + pos + 1;
            vl = tl - pos - 1;
            while (vl > 0 && (*v == ' ' || *v == '\t' || *v == '\f')) {
                v++;
                vl--;
            }
            cont = vl > 0 && odd_trailing_backslashes(v, vl);
            if (cont) {
                ck = key;
                lt_buf_clear(&cv);
                unescape_to(&cv, v, vl - 1);
                pending = 1;
            } else {
                char *val = unescape_dup(v, vl);
                lt_props_set(p, key, val);
                lt_free(val);
                lt_free(key);
            }
        }
    }
    if (pending) {
        lt_props_set(p, ck, cv.data ? cv.data : "");
        lt_free(ck);
    }
    lt_buf_free(&cv);
    return LT_OK;
}

int lt_props_load(lt_props *p, const char *path)
{
    size_t n = 0;
    char *raw = lt_file_read(path, &n);
    char *text;
    int rc;
    if (raw == NULL) return LT_ENOENT;
    text = lt_proj_decode_latin1((const unsigned char *)raw, n);
    lt_free(raw);
    rc = lt_props_parse(p, text);
    lt_free(text);
    return rc;
}

/* ---- serialising ---- */
static void esc_key(lt_buf *b, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case ' ': lt_buf_puts(b, "\\ "); break;
        case '=': lt_buf_puts(b, "\\="); break;
        case ':': lt_buf_puts(b, "\\:"); break;
        case '\\': lt_buf_puts(b, "\\\\"); break;
        case '\t': lt_buf_puts(b, "\\t"); break;
        case '\n': lt_buf_puts(b, "\\n"); break;
        case '\r': lt_buf_puts(b, "\\r"); break;
        default: lt_buf_putc(b, (unsigned char)*s);
        }
    }
}

static void esc_val(lt_buf *b, const char *s)
{
    const char *start = s;
    for (; *s; s++) {
        switch (*s) {
        case '\\': lt_buf_puts(b, "\\\\"); break;
        case '\t': lt_buf_puts(b, "\\t"); break;
        case '\n': lt_buf_puts(b, "\\n"); break;
        case '\r': lt_buf_puts(b, "\\r"); break;
        case ' ':
            if (s == start) lt_buf_puts(b, "\\ ");
            else lt_buf_putc(b, ' ');
            break;
        default: lt_buf_putc(b, (unsigned char)*s);
        }
    }
}

char *lt_props_to_string(const lt_props *p)
{
    lt_buf b;
    size_t i;
    lt_buf_init(&b);
    for (i = 0; i < p->n_header; i++) {
        lt_buf_puts(&b, p->header[i]);
        lt_buf_putc(&b, '\n');
    }
    for (i = 0; i < p->n; i++) {
        esc_key(&b, p->keys[i]);
        lt_buf_putc(&b, '=');
        esc_val(&b, p->vals[i]);
        lt_buf_putc(&b, '\n');
    }
    if (b.data == NULL) return lt_strdup("");
    return lt_buf_take(&b);
}

int lt_props_save(const lt_props *p, const char *path)
{
    char *s = lt_props_to_string(p);
    int rc = lt_plat_write_atomic(path, s, strlen(s));
    lt_free(s);
    return rc == 0 ? LT_OK : LT_EIO;
}
