/* json.c - tiny JSON DOM: RFC 8259 parser and serde_json compatible writer.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "json.h"

#define JSON_MAX_DEPTH 200

typedef struct jparser {
    const char *s;
    size_t len, pos;
    int depth;
    char msg[128];
    int failed;
} jparser;

static lt_json *j_new(lt_json_type t)
{
    lt_json *j = (lt_json *)lt_calloc(1, sizeof(lt_json));
    j->type = t;
    return j;
}

lt_json *lt_json_null(void) { return j_new(LT_JSON_NULL); }
lt_json *lt_json_bool(int b)
{
    lt_json *j = j_new(LT_JSON_BOOL);
    j->boolean = b ? 1 : 0;
    return j;
}
lt_json *lt_json_num(double v)
{
    lt_json *j = j_new(LT_JSON_NUM);
    j->num = v;
    return j;
}
lt_json *lt_json_str(const char *s)
{
    lt_json *j;
    if (s == NULL) return j_new(LT_JSON_NULL);
    j = j_new(LT_JSON_STR);
    j->str = lt_strdup(s);
    return j;
}
lt_json *lt_json_arr(void) { return j_new(LT_JSON_ARR); }
lt_json *lt_json_obj(void) { return j_new(LT_JSON_OBJ); }

void lt_json_free(lt_json *j)
{
    size_t i;
    if (j == NULL) return;
    for (i = 0; i < j->n; i++) {
        lt_json_free(j->items[i]);
        if (j->keys != NULL) lt_free(j->keys[i]);
    }
    lt_free(j->items);
    lt_free(j->keys);
    lt_free(j->str);
    lt_free(j);
}

static void j_grow(lt_json *j)
{
    if (j->n == j->cap) {
        j->cap = j->cap ? j->cap * 2 : 4;
        j->items = (lt_json **)lt_realloc(j->items, j->cap * sizeof(lt_json *));
        if (j->type == LT_JSON_OBJ)
            j->keys = (char **)lt_realloc(j->keys, j->cap * sizeof(char *));
    }
}

void lt_json_arr_push(lt_json *arr, lt_json *val)
{
    if (arr == NULL || arr->type != LT_JSON_ARR || val == NULL) {
        lt_json_free(val);
        return;
    }
    j_grow(arr);
    arr->items[arr->n++] = val;
}

void lt_json_obj_set(lt_json *obj, const char *key, lt_json *val)
{
    size_t i;
    if (obj == NULL || obj->type != LT_JSON_OBJ || key == NULL || val == NULL) {
        lt_json_free(val);
        return;
    }
    for (i = 0; i < obj->n; i++) {
        if (strcmp(obj->keys[i], key) == 0) {
            lt_json_free(obj->items[i]);
            obj->items[i] = val;
            return;
        }
    }
    j_grow(obj);
    obj->keys[obj->n] = lt_strdup(key);
    obj->items[obj->n] = val;
    obj->n++;
}

const lt_json *lt_json_get(const lt_json *obj, const char *key)
{
    size_t i;
    if (obj == NULL || obj->type != LT_JSON_OBJ || key == NULL) return NULL;
    for (i = 0; i < obj->n; i++)
        if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
    return NULL;
}

const char *lt_json_get_str(const lt_json *obj, const char *key, const char *dflt)
{
    const lt_json *v = lt_json_get(obj, key);
    return (v != NULL && v->type == LT_JSON_STR) ? v->str : dflt;
}
double lt_json_get_num(const lt_json *obj, const char *key, double dflt)
{
    const lt_json *v = lt_json_get(obj, key);
    return (v != NULL && v->type == LT_JSON_NUM) ? v->num : dflt;
}
int lt_json_get_bool(const lt_json *obj, const char *key, int dflt)
{
    const lt_json *v = lt_json_get(obj, key);
    return (v != NULL && v->type == LT_JSON_BOOL) ? v->boolean : dflt;
}
const lt_json *lt_json_at(const lt_json *arr, size_t i)
{
    if (arr == NULL || arr->type != LT_JSON_ARR || i >= arr->n) return NULL;
    return arr->items[i];
}

/* ------------------------------------------------------------------ */
/* parser                                                              */
/* ------------------------------------------------------------------ */
static void jp_fail(jparser *p, const char *what)
{
    if (!p->failed) {
        p->failed = 1;
        lt_snprintf(p->msg, sizeof p->msg, "%s at byte %lu", what, (unsigned long)p->pos);
    }
}

static void jp_ws(jparser *p)
{
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static void put_utf8(lt_buf *b, unsigned long cp)
{
    if (cp < 0x80) {
        lt_buf_putc(b, (int)cp);
    } else if (cp < 0x800) {
        lt_buf_putc(b, (int)(0xC0 | (cp >> 6)));
        lt_buf_putc(b, (int)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000UL) {
        lt_buf_putc(b, (int)(0xE0 | (cp >> 12)));
        lt_buf_putc(b, (int)(0x80 | ((cp >> 6) & 0x3F)));
        lt_buf_putc(b, (int)(0x80 | (cp & 0x3F)));
    } else {
        lt_buf_putc(b, (int)(0xF0 | (cp >> 18)));
        lt_buf_putc(b, (int)(0x80 | ((cp >> 12) & 0x3F)));
        lt_buf_putc(b, (int)(0x80 | ((cp >> 6) & 0x3F)));
        lt_buf_putc(b, (int)(0x80 | (cp & 0x3F)));
    }
}

static int hex4(const char *s, unsigned long *out)
{
    int i;
    unsigned long v = 0;
    for (i = 0; i < 4; i++) {
        int c = (unsigned char)s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* parses a string literal at p->pos (on the opening quote); returns malloc'd */
static char *jp_string(jparser *p)
{
    lt_buf b;
    lt_buf_init(&b);
    p->pos++; /* opening quote */
    for (;;) {
        unsigned char c;
        if (p->pos >= p->len) {
            jp_fail(p, "unterminated string");
            lt_buf_free(&b);
            return NULL;
        }
        c = (unsigned char)p->s[p->pos++];
        if (c == '"') break;
        if (c < 0x20) {
            p->pos--;
            jp_fail(p, "control character in string");
            lt_buf_free(&b);
            return NULL;
        }
        if (c == '\\') {
            char e;
            if (p->pos >= p->len) {
                jp_fail(p, "unterminated escape");
                lt_buf_free(&b);
                return NULL;
            }
            e = p->s[p->pos++];
            switch (e) {
            case '"': lt_buf_putc(&b, '"'); break;
            case '\\': lt_buf_putc(&b, '\\'); break;
            case '/': lt_buf_putc(&b, '/'); break;
            case 'b': lt_buf_putc(&b, '\b'); break;
            case 'f': lt_buf_putc(&b, '\f'); break;
            case 'n': lt_buf_putc(&b, '\n'); break;
            case 'r': lt_buf_putc(&b, '\r'); break;
            case 't': lt_buf_putc(&b, '\t'); break;
            case 'u': {
                unsigned long cp, lo;
                if (p->pos + 4 > p->len || !hex4(p->s + p->pos, &cp)) {
                    jp_fail(p, "bad \\u escape");
                    lt_buf_free(&b);
                    return NULL;
                }
                p->pos += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (p->pos + 6 <= p->len && p->s[p->pos] == '\\' &&
                        p->s[p->pos + 1] == 'u' && hex4(p->s + p->pos + 2, &lo) &&
                        lo >= 0xDC00 && lo <= 0xDFFF) {
                        p->pos += 6;
                        cp = 0x10000UL + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                put_utf8(&b, cp);
                break;
            }
            default:
                p->pos--;
                jp_fail(p, "bad escape");
                lt_buf_free(&b);
                return NULL;
            }
        } else {
            lt_buf_putc(&b, c);
        }
    }
    if (b.data == NULL) return lt_strdup("");
    return lt_buf_take(&b);
}

static lt_json *jp_value(jparser *p);

static lt_json *jp_number(jparser *p)
{
    size_t st = p->pos;
    char tmp[64];
    size_t n;
    char *end;
    double v;
    if (p->pos < p->len && p->s[p->pos] == '-') p->pos++;
    if (p->pos >= p->len) {
        jp_fail(p, "bad number");
        return NULL;
    }
    if (p->s[p->pos] == '0') {
        p->pos++;
    } else if (p->s[p->pos] >= '1' && p->s[p->pos] <= '9') {
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    } else {
        jp_fail(p, "bad number");
        return NULL;
    }
    if (p->pos < p->len && p->s[p->pos] == '.') {
        p->pos++;
        if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9') {
            jp_fail(p, "bad number");
            return NULL;
        }
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) p->pos++;
        if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9') {
            jp_fail(p, "bad number");
            return NULL;
        }
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    }
    n = p->pos - st;
    if (n >= sizeof tmp) {
        jp_fail(p, "number too long");
        return NULL;
    }
    memcpy(tmp, p->s + st, n);
    tmp[n] = '\0';
    v = strtod(tmp, &end);
    return lt_json_num(v);
}

static int jp_lit(jparser *p, const char *lit)
{
    size_t n = strlen(lit);
    if (p->pos + n <= p->len && memcmp(p->s + p->pos, lit, n) == 0) {
        p->pos += n;
        return 1;
    }
    return 0;
}

static lt_json *jp_value(jparser *p)
{
    lt_json *r = NULL;
    char c;
    jp_ws(p);
    if (p->pos >= p->len) {
        jp_fail(p, "unexpected end of input");
        return NULL;
    }
    if (p->depth >= JSON_MAX_DEPTH) {
        jp_fail(p, "nesting too deep");
        return NULL;
    }
    c = p->s[p->pos];
    if (c == '{') {
        p->depth++;
        p->pos++;
        r = lt_json_obj();
        jp_ws(p);
        if (p->pos < p->len && p->s[p->pos] == '}') {
            p->pos++;
        } else {
            for (;;) {
                char *key;
                lt_json *val;
                jp_ws(p);
                if (p->pos >= p->len || p->s[p->pos] != '"') {
                    jp_fail(p, "expected object key");
                    break;
                }
                key = jp_string(p);
                if (key == NULL) break;
                jp_ws(p);
                if (p->pos >= p->len || p->s[p->pos] != ':') {
                    lt_free(key);
                    jp_fail(p, "expected ':'");
                    break;
                }
                p->pos++;
                val = jp_value(p);
                if (val == NULL) {
                    lt_free(key);
                    break;
                }
                lt_json_obj_set(r, key, val);
                lt_free(key);
                jp_ws(p);
                if (p->pos < p->len && p->s[p->pos] == ',') {
                    p->pos++;
                    continue;
                }
                if (p->pos < p->len && p->s[p->pos] == '}') {
                    p->pos++;
                    break;
                }
                jp_fail(p, "expected ',' or '}'");
                break;
            }
        }
        p->depth--;
    } else if (c == '[') {
        p->depth++;
        p->pos++;
        r = lt_json_arr();
        jp_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ']') {
            p->pos++;
        } else {
            for (;;) {
                lt_json *val = jp_value(p);
                if (val == NULL) break;
                lt_json_arr_push(r, val);
                jp_ws(p);
                if (p->pos < p->len && p->s[p->pos] == ',') {
                    p->pos++;
                    continue;
                }
                if (p->pos < p->len && p->s[p->pos] == ']') {
                    p->pos++;
                    break;
                }
                jp_fail(p, "expected ',' or ']'");
                break;
            }
        }
        p->depth--;
    } else if (c == '"') {
        char *s = jp_string(p);
        if (s != NULL) {
            r = j_new(LT_JSON_STR);
            r->str = s;
        }
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        r = jp_number(p);
    } else if (c == 't') {
        if (jp_lit(p, "true")) r = lt_json_bool(1);
        else jp_fail(p, "bad literal");
    } else if (c == 'f') {
        if (jp_lit(p, "false")) r = lt_json_bool(0);
        else jp_fail(p, "bad literal");
    } else if (c == 'n') {
        if (jp_lit(p, "null")) r = lt_json_null();
        else jp_fail(p, "bad literal");
    } else {
        jp_fail(p, "unexpected character");
    }
    if (p->failed) {
        lt_json_free(r);
        return NULL;
    }
    return r;
}

lt_json *lt_json_parse(const char *text, size_t len, char *err, size_t errsz)
{
    jparser p;
    lt_json *v;
    memset(&p, 0, sizeof p);
    p.s = text;
    p.len = len;
    if (text == NULL) {
        if (err != NULL && errsz > 0) lt_strlcpy(err, "no input", errsz);
        return NULL;
    }
    /* tolerate a UTF-8 BOM */
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        p.pos = 3;
    v = jp_value(&p);
    if (v != NULL) {
        jp_ws(&p);
        if (p.pos != p.len) {
            jp_fail(&p, "trailing characters");
            lt_json_free(v);
            v = NULL;
        }
    }
    if (v == NULL && err != NULL && errsz > 0) lt_strlcpy(err, p.msg, errsz);
    return v;
}

/* ------------------------------------------------------------------ */
/* writer                                                              */
/* ------------------------------------------------------------------ */
void lt_json_put_string(lt_buf *out, const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    lt_buf_putc(out, '"');
    for (; *p; p++) {
        switch (*p) {
        case '"': lt_buf_puts(out, "\\\""); break;
        case '\\': lt_buf_puts(out, "\\\\"); break;
        case '\n': lt_buf_puts(out, "\\n"); break;
        case '\r': lt_buf_puts(out, "\\r"); break;
        case '\t': lt_buf_puts(out, "\\t"); break;
        case '\b': lt_buf_puts(out, "\\b"); break;
        case '\f': lt_buf_puts(out, "\\f"); break;
        default:
            if (*p < 0x20) {
                char t[8];
                lt_snprintf(t, sizeof t, "\\u%04x", (unsigned)*p);
                lt_buf_puts(out, t);
            } else {
                lt_buf_putc(out, *p);
            }
        }
    }
    lt_buf_putc(out, '"');
}

static void put_number(lt_buf *out, double v)
{
    char t[40];
    int prec;
    if (!lt_isfinite(v)) {
        lt_buf_puts(out, "null");
        return;
    }
    if (v == floor(v) && fabs(v) < 1e15) {
        lt_snprintf(t, sizeof t, "%.0f", v);
        if (strcmp(t, "-0") == 0) lt_strlcpy(t, "0", sizeof t);
        lt_buf_puts(out, t);
        return;
    }
    for (prec = 15; prec <= 17; prec++) {
        lt_snprintf(t, sizeof t, "%.*g", prec, v);
        if (strtod(t, NULL) == v) break;
    }
    lt_buf_puts(out, t);
}

static void put_indent(lt_buf *out, int depth)
{
    int i;
    for (i = 0; i < depth; i++) lt_buf_puts(out, "  ");
}

static void write_rec(const lt_json *j, int pretty, int depth, lt_buf *out)
{
    size_t i;
    switch (j->type) {
    case LT_JSON_NULL: lt_buf_puts(out, "null"); break;
    case LT_JSON_BOOL: lt_buf_puts(out, j->boolean ? "true" : "false"); break;
    case LT_JSON_NUM: put_number(out, j->num); break;
    case LT_JSON_STR: lt_json_put_string(out, j->str); break;
    case LT_JSON_ARR:
        if (j->n == 0) {
            lt_buf_puts(out, "[]");
            break;
        }
        lt_buf_putc(out, '[');
        for (i = 0; i < j->n; i++) {
            if (i > 0) lt_buf_putc(out, ',');
            if (pretty) {
                lt_buf_putc(out, '\n');
                put_indent(out, depth + 1);
            }
            write_rec(j->items[i], pretty, depth + 1, out);
        }
        if (pretty) {
            lt_buf_putc(out, '\n');
            put_indent(out, depth);
        }
        lt_buf_putc(out, ']');
        break;
    case LT_JSON_OBJ:
        if (j->n == 0) {
            lt_buf_puts(out, "{}");
            break;
        }
        lt_buf_putc(out, '{');
        for (i = 0; i < j->n; i++) {
            if (i > 0) lt_buf_putc(out, ',');
            if (pretty) {
                lt_buf_putc(out, '\n');
                put_indent(out, depth + 1);
            }
            lt_json_put_string(out, j->keys[i]);
            lt_buf_puts(out, pretty ? ": " : ":");
            write_rec(j->items[i], pretty, depth + 1, out);
        }
        if (pretty) {
            lt_buf_putc(out, '\n');
            put_indent(out, depth);
        }
        lt_buf_putc(out, '}');
        break;
    }
}

void lt_json_write(const lt_json *j, int pretty, lt_buf *out)
{
    if (j == NULL) {
        lt_buf_puts(out, "null");
        return;
    }
    write_rec(j, pretty, 0, out);
}

char *lt_json_to_string(const lt_json *j, int pretty)
{
    lt_buf b;
    char *s;
    lt_buf_init(&b);
    lt_json_write(j, pretty, &b);
    if (b.data == NULL) return lt_strdup("");
    s = lt_buf_take(&b);
    return s;
}

lt_json *lt_json_load(const char *path, char *err, size_t errsz)
{
    size_t len = 0;
    char *data = lt_file_read(path, &len);
    lt_json *j;
    if (data == NULL) {
        if (err != NULL && errsz > 0) lt_strlcpy(err, lt_err_get(), errsz);
        return NULL;
    }
    j = lt_json_parse(data, len, err, errsz);
    lt_free(data);
    return j;
}

int lt_json_save(const lt_json *j, const char *path, int pretty)
{
    char *s = lt_json_to_string(j, pretty);
    int rc = lt_file_write(path, s, strlen(s));
    lt_free(s);
    return rc == 0 ? LT_OK : LT_EIO;
}
