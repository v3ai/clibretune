/* tune_csv.c - tune CSV export / import (see tune_csv.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "tune_csv.h"

#define CSV_MAX_FIELDS 16
#define CSV_MAX_MSGS 50

/* ---- writing ---- */
static void put_field(lt_buf *b, const char *s)
{
    if (strchr(s, ',') != NULL || strchr(s, '"') != NULL) {
        lt_buf_putc(b, '"');
        for (; *s != '\0'; s++) {
            if (*s == '"') { lt_buf_putc(b, '"'); }
            lt_buf_putc(b, (unsigned char)*s);
        }
        lt_buf_putc(b, '"');
    } else {
        lt_buf_puts(b, s);
    }
}

static void put_num(lt_buf *b, double v)
{
    char t[40];
    lt_fmt_f64_shortest(t, sizeof(t), v);
    lt_buf_puts(b, t);
}

static void put_value(lt_buf *b, const lt_ini *ini, const lt_ini_const *c, const lt_pagesrc *src)
{
    if (c->type == LT_DT_STRING) {
        char *s = (char *)lt_calloc(c->size_bytes + 2, 1);
        size_t i;
        if (lt_ini_const_get_string(ini, c, src, s, c->size_bytes + 1) < 0) { s[0] = '\0'; }
        lt_buf_putc(b, '"');
        for (i = 0; s[i] != '\0'; i++) {
            if (s[i] == '"') { lt_buf_putc(b, '"'); }
            lt_buf_putc(b, (unsigned char)s[i]);
        }
        lt_buf_putc(b, '"');
        lt_free(s);
    } else if (c->shape.kind == LT_SH_SCALAR) {
        double v = 0.0;
        if (lt_ini_const_get(ini, c, src, 0, &v) < 0) { v = 0.0; }
        put_num(b, v);
    } else {
        size_t i, n = lt_shape_count(&c->shape);
        double *v = (double *)lt_calloc(n + 1, sizeof(double));
        if (lt_ini_const_get_array(ini, c, src, v, n) < 0) { memset(v, 0, n * sizeof(double)); }
        lt_buf_puts(b, "\"[");
        for (i = 0; i < n; i++) {
            if (i > 0) { lt_buf_putc(b, ','); }
            put_num(b, v[i]);
        }
        lt_buf_puts(b, "]\"");
        lt_free(v);
    }
}

char *lt_tune_csv_export(const lt_ini *ini, const lt_pagesrc *src, size_t *count)
{
    lt_buf b;
    size_t i, rows = 0;
    lt_buf_init(&b);
    lt_buf_puts(&b, "Name,Page,Offset,Shape,Value,Units,Min,Max,Scale,Translate,DataType,IsPcVariable");
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];
        char t[48];
        lt_buf_putc(&b, '\n');
        put_field(&b, c->name);
        lt_snprintf(t, sizeof(t), ",%d,%lu,", (int)c->page, (unsigned long)c->offset);
        lt_buf_puts(&b, t);
        if (c->shape.kind == LT_SH_SCALAR) { lt_buf_puts(&b, "scalar"); }
        else if (c->shape.kind == LT_SH_1D) {
            lt_snprintf(t, sizeof(t), "[%lu]", (unsigned long)c->shape.n);
            lt_buf_puts(&b, t);
        } else {
            lt_snprintf(t, sizeof(t), "[%lux%lu]", (unsigned long)c->shape.rows, (unsigned long)c->shape.cols);
            lt_buf_puts(&b, t);
        }
        lt_buf_putc(&b, ',');
        put_value(&b, ini, c, src);
        lt_buf_putc(&b, ',');
        put_field(&b, c->units != NULL ? c->units : "");
        lt_buf_putc(&b, ',');
        put_num(&b, c->min);
        lt_buf_putc(&b, ',');
        put_num(&b, c->max);
        lt_buf_putc(&b, ',');
        put_num(&b, c->scale);
        lt_buf_putc(&b, ',');
        put_num(&b, c->translate);
        lt_buf_putc(&b, ',');
        lt_buf_puts(&b, lt_dtype_name(c->type));
        lt_buf_puts(&b, c->is_pc_var ? ",true" : ",false");
        rows++;
    }
    if (count != NULL) { *count = rows; }
    return b.data != NULL ? lt_buf_take(&b) : lt_strdup("");
}

int lt_tune_csv_export_file(const lt_ini *ini, const lt_pagesrc *src, const char *path, size_t *count)
{
    char *text = lt_tune_csv_export(ini, src, count);
    int rc = lt_file_write(path, text, strlen(text));
    lt_free(text);
    if (rc < 0) { lt_err_set("Failed to write CSV file '%s'", path); return rc; }
    return 0;
}

/* ---- reading ---- */
size_t lt_csv_split(const char *line, size_t len, char **fields, size_t cap)
{
    size_t i = 0, n = 0;
    int quoted = 0;
    lt_buf cur;
    lt_buf_init(&cur);
    for (;;) {
        char ch = i < len ? line[i] : '\0';
        int end = i >= len;
        if (!end && ch == '"') {
            if (quoted && i + 1 < len && line[i + 1] == '"') { lt_buf_putc(&cur, '"'); i++; }
            else { quoted = !quoted; }
        } else if (end || (ch == ',' && !quoted)) {
            if (n < cap) {
                char *f = cur.data != NULL ? lt_buf_take(&cur) : lt_strdup("");
                lt_str_trim(f);
                fields[n++] = f;
            }
            lt_buf_clear(&cur);
            if (end) { break; }
        } else {
            lt_buf_putc(&cur, (unsigned char)ch);
        }
        i++;
    }
    lt_buf_free(&cur);
    return n;
}

void lt_csv_import_free(lt_csv_import *r)
{
    size_t i;
    if (r == NULL) { return; }
    for (i = 0; i < r->n_messages; i++) { lt_free(r->messages[i]); }
    lt_free(r->messages);
    memset(r, 0, sizeof(*r));
}

static void add_msg(lt_csv_import *r, const char *text)
{
    if (r == NULL || r->n_messages >= CSV_MAX_MSGS) { return; }
    r->messages = (char **)lt_realloc(r->messages, (r->n_messages + 1) * sizeof(char *));
    r->messages[r->n_messages++] = lt_strdup(text);
}

/* "Line N: <what> '<name>'" */
static void note_line(lt_csv_import *r, unsigned long line, const char *what, const char *name)
{
    char t[200];
    lt_snprintf(t, sizeof(t), "Line %lu: %s%s%s%s", line, what, name != NULL ? " '" : "",
                name != NULL ? name : "", name != NULL ? "'" : "");
    add_msg(r, t);
}

static void import_array(const lt_ini *ini, const lt_ini_const *c, lt_pagesrc *dst, const char *val,
                         lt_csv_import *r, unsigned long line)
{
    size_t n = lt_shape_count(&c->shape), k = 0;
    double *cur = (double *)lt_calloc(n + 1, sizeof(double));
    const char *p = val;
    if (lt_ini_const_get_array(ini, c, dst, cur, n) < 0) { memset(cur, 0, n * sizeof(double)); }
    while (*p == '[' || *p == ' ') { p++; }
    while (*p != '\0' && *p != ']' && k < n) {
        char tok[64];
        size_t tl = 0;
        double v;
        while (*p != '\0' && *p != ',' && *p != ']') {
            if (tl + 1 < sizeof(tok)) { tok[tl++] = *p; }
            p++;
        }
        tok[tl] = '\0';
        if (*p == ',') { p++; }
        if (lt_parse_double(tok, &v)) {
            if (c->range_resolved && c->max > c->min) {
                if (v < c->min) { v = c->min; } else if (v > c->max) { v = c->max; }
            }
            cur[k++] = v;
        }
    }
    if (k > 0) {
        if (lt_ini_const_set_array(ini, c, dst, cur, n) < 0) {
            note_line(r, line, "cannot write array", c->name);
        } else if (r != NULL) { r->imported++; }
    }
    lt_free(cur);
}

int lt_tune_csv_import(const lt_ini *ini, lt_pagesrc *dst, const char *text, size_t len,
                       lt_csv_import *r)
{
    size_t pos = 0;
    unsigned long line_no = 0;
    if (r != NULL) { memset(r, 0, sizeof(*r)); }
    if (ini == NULL || dst == NULL || text == NULL) { lt_err_set("no tune or definition"); return -1; }
    while (pos < len) {
        size_t e = pos, nf, i;
        char *f[CSV_MAX_FIELDS];
        const char *name, *val;
        const lt_ini_const *c = NULL;
        size_t k;
        while (e < len && text[e] != '\n') { e++; }
        line_no++;
        {
            size_t ll = e - pos;
            if (ll > 0 && text[pos + ll - 1] == '\r') { ll--; }
            if (ll == 0 || (line_no == 1 && (strncmp(text + pos, "Name,", 5) == 0 ||
                                             strncmp(text + pos, "\"Name\"", 6) == 0))) {
                pos = e + 1;
                continue;
            }
            {
                size_t a;
                int blank = 1;
                for (a = 0; a < ll; a++) { if (text[pos + a] != ' ' && text[pos + a] != '\t') { blank = 0; break; } }
                if (blank) { pos = e + 1; continue; }
            }
            nf = lt_csv_split(text + pos, ll, f, CSV_MAX_FIELDS);
        }
        pos = e + 1;
        if (nf >= 12) { name = f[0]; val = f[4]; }
        else if (nf >= 4) { name = f[0]; val = f[3]; }
        else {
            note_line(r, line_no, "too few fields", NULL);
            for (i = 0; i < nf; i++) { lt_free(f[i]); }
            continue;
        }
        for (k = 0; k < ini->n_constants; k++) {
            if (strcmp(ini->constants[k]->name, name) == 0) { c = ini->constants[k]; break; }
        }
        if (c == NULL) {
            if (r != NULL) { r->unknown++; }
        } else if (c->type == LT_DT_STRING) {
            if (lt_ini_const_set_string(ini, c, dst, val) < 0) {
                note_line(r, line_no, "cannot write", name);
            } else if (r != NULL) { r->imported++; }
        } else if (c->shape.kind != LT_SH_SCALAR) {
            import_array(ini, c, dst, val, r, line_no);
        } else {
            double v;
            if (!lt_parse_double(val, &v)) {
                note_line(r, line_no, "invalid value", val);
            } else {
                double cl = v;
                if (c->type != LT_DT_BITS && c->range_resolved && !lt_const_in_range(c, v)) {
                    cl = v < c->min ? c->min : c->max;
                    if (cl - v > 0.0001 || v - cl > 0.0001) {
                        char t[160];
                        if (r != NULL) { r->clamped++; }
                        lt_snprintf(t, sizeof(t), "Line %lu: value %g clamped to %g (range %g-%g)",
                                    line_no, v, cl, c->min, c->max);
                        add_msg(r, t);
                    }
                }
                if (lt_ini_const_set(ini, c, dst, 0, cl) < 0) {
                    note_line(r, line_no, "cannot write", name);
                } else if (r != NULL) { r->imported++; }
            }
        }
        for (i = 0; i < nf; i++) { lt_free(f[i]); }
    }
    return 0;
}

int lt_tune_csv_import_file(const lt_ini *ini, lt_pagesrc *dst, const char *path, lt_csv_import *r)
{
    size_t len = 0;
    char *text = lt_file_read(path, &len);
    int rc;
    if (text == NULL) {
        if (r != NULL) { memset(r, 0, sizeof(*r)); }
        lt_err_set("Failed to read CSV file '%s'", path);
        return -1;
    }
    rc = lt_tune_csv_import(ini, dst, text, len, r);
    lt_free(text);
    return rc;
}
