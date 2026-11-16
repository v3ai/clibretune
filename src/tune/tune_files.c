/* tune_files.c - TunerStudio .table (spec 04 section 5) and .tuneView (section 6) files.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tune_priv.h"

/* ====================================================================== */
/* .table                                                                  */
/* ====================================================================== */
static void table_zero(lt_table_file *t)
{
    memset(t, 0, sizeof *t);
}

void lt_table_file_clear(lt_table_file *t)
{
    if (t->x) lt_free(t->x);
    if (t->y) lt_free(t->y);
    if (t->z) lt_free(t->z);
    table_zero(t);
}

static lt_bool parse_size(const char *s, size_t *out)
{
    unsigned long v = 0;
    size_t i = 0;
    if (!s) return LT_FALSE;
    while (s[i] == ' ') i++;
    if (!s[i]) return LT_FALSE;
    for (; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return LT_FALSE;
        v = v * 10 + (unsigned long)(s[i] - '0');
        if (v > 100000000UL) return LT_FALSE;
    }
    *out = (size_t)v;
    return LT_TRUE;
}

static int is_wsc(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* tokens of s that parse as numbers appended to vec of doubles; returns count appended */
static size_t read_floats(const char *s, size_t n, lt_vec *out)
{
    size_t i = 0, cnt = 0;
    while (i < n) {
        size_t st;
        double d;
        while (i < n && is_wsc(s[i])) i++;
        if (i >= n) break;
        st = i;
        while (i < n && !is_wsc(s[i])) i++;
        if (lt_tune_parse_num(s + st, i - st, &d)) {
            *(double *)lt_vec_push(out) = d;
            cnt++;
        }
    }
    return cnt;
}

static double *vec_take(lt_vec *v)
{
    double *a = (double *)lt_malloc((v->len ? v->len : 1) * sizeof(double));
    if (v->len) memcpy(a, v->data, v->len * sizeof(double));
    return a;
}

int lt_table_file_parse(const char *xml, size_t len, lt_table_file *out)
{
    char err[128];
    lt_xml_node *root;
    const lt_xml_node *td, *tb, *xa, *ya, *za;
    size_t cols = 0, rows = 0;
    lt_vec xs, ys, zs;
    size_t nz_rows = 0, i;
    const char *zt;
    int rc = 0;

    table_zero(out);
    root = lt_xml_parse(xml, len, LT_XML_STRIP_NS, err, sizeof err);
    if (!root) {
        lt_err_set("%s", err);
        return LT_EDATA;
    }
    td = strcmp(root->name, "tableData") == 0 ? root : lt_xml_find(root, "tableData");
    if (!td) {
        lt_xml_free(root);
        lt_err_set("not a TunerStudio table file (missing <tableData>)");
        return LT_EDATA;
    }
    tb = lt_xml_find(td, "table");
    if (!tb || !parse_size(lt_xml_attr_get(tb, "cols"), &cols)) {
        lt_xml_free(root);
        lt_err_set("malformed table file: missing <table cols=...>");
        return LT_EDATA;
    }
    if (!parse_size(lt_xml_attr_get(tb, "rows"), &rows)) {
        lt_xml_free(root);
        lt_err_set("malformed table file: missing <table rows=...>");
        return LT_EDATA;
    }
    xa = lt_xml_find(tb, "xAxis");
    ya = lt_xml_find(tb, "yAxis");
    za = lt_xml_find(tb, "zValues");
    lt_vec_init(&xs, sizeof(double));
    lt_vec_init(&ys, sizeof(double));
    lt_vec_init(&zs, sizeof(double));
    if (xa && xa->text) read_floats(xa->text, strlen(xa->text), &xs);
    if (ya && ya->text) read_floats(ya->text, strlen(ya->text), &ys);
    if (xs.len != cols) {
        lt_err_set("xAxis has %lu values, table declares %lu cols", (unsigned long)xs.len, (unsigned long)cols);
        rc = LT_EDATA;
    } else if (ys.len != rows) {
        lt_err_set("yAxis has %lu values, table declares %lu rows", (unsigned long)ys.len, (unsigned long)rows);
        rc = LT_EDATA;
    } else {
        int ok = 1;
        zt = (za && za->text) ? za->text : "";
        i = 0;
        while (zt[i]) {
            size_t st = i, got;
            while (zt[i] && zt[i] != '\n') i++;
            got = read_floats(zt + st, i - st, &zs);
            if (got > 0) {
                nz_rows++;
                if (got != cols) ok = 0;
            }
            if (zt[i]) i++;
        }
        if (!ok || nz_rows != rows) {
            lt_err_set("zValues is not a %lux%lu grid", (unsigned long)cols, (unsigned long)rows);
            rc = LT_EDATA;
        }
    }
    if (rc == 0) {
        out->cols = cols;
        out->rows = rows;
        out->x = vec_take(&xs);
        out->y = vec_take(&ys);
        out->z = vec_take(&zs);
    }
    lt_vec_free(&xs);
    lt_vec_free(&ys);
    lt_vec_free(&zs);
    lt_xml_free(root);
    return rc;
}

static void put_esc(lt_buf *b, const char *s)
{
    char *e = lt_xml_escape(s ? s : "");
    lt_buf_puts(b, e);
    lt_free(e);
}

char *lt_table_file_write(const char *x_name, const char *y_name, const double *x, size_t nx,
                          const double *y, size_t ny, const double *z)
{
    lt_buf b;
    size_t i, j;
    char n[420];

    lt_buf_init(&b);
    lt_buf_puts(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n");
    lt_buf_puts(&b, "<tableData xmlns=\"http://www.EFIAnalytics.com/:table\">\n");
    lt_buf_printf(&b, "    <bibliography author=\"LibreTune\" company=\"LibreTune, open source\" "
                      "writeDate=\"epoch %lu UTC\"/>\n", (unsigned long)time(NULL));
    lt_buf_puts(&b, "    <versionInfo fileFormat=\"1.0\"/>\n");
    lt_buf_printf(&b, "    <table cols=\"%lu\" rows=\"%lu\">\n", (unsigned long)nx, (unsigned long)ny);
    lt_buf_printf(&b, "        <xAxis cols=\"%lu\" name=\"", (unsigned long)nx);
    put_esc(&b, x_name);
    lt_buf_puts(&b, "\">");
    for (i = 0; i < nx; i++) {
        lt_tune_fmt_num(n, sizeof n, x[i], 1);
        lt_buf_puts(&b, n);
        lt_buf_putc(&b, '\n');
    }
    lt_buf_puts(&b, "</xAxis>\n");
    lt_buf_puts(&b, "        <yAxis name=\"");
    put_esc(&b, y_name);
    lt_buf_printf(&b, "\" rows=\"%lu\">", (unsigned long)ny);
    for (i = 0; i < ny; i++) {
        lt_tune_fmt_num(n, sizeof n, y[i], 1);
        lt_buf_puts(&b, n);
        lt_buf_putc(&b, '\n');
    }
    lt_buf_puts(&b, "</yAxis>\n");
    lt_buf_printf(&b, "        <zValues cols=\"%lu\" rows=\"%lu\">", (unsigned long)nx, (unsigned long)ny);
    for (i = 0; i < ny; i++) {
        for (j = 0; j < nx; j++) {
            if (j) lt_buf_putc(&b, ' ');
            lt_tune_fmt_num(n, sizeof n, z[i * nx + j], 1);
            lt_buf_puts(&b, n);
        }
        lt_buf_putc(&b, '\n');
    }
    lt_buf_puts(&b, "</zValues>\n");
    lt_buf_puts(&b, "    </table>\n</tableData>\n");
    return lt_buf_take(&b);
}

/* ====================================================================== */
/* kvmap                                                                   */
/* ====================================================================== */
void lt_kvmap_set(lt_kvmap *m, const char *k, const char *v)
{
    size_t lo = 0, hi = m->n, i;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(m->a[mid].k, k);
        if (c == 0) {
            lt_free(m->a[mid].v);
            m->a[mid].v = lt_strdup(v ? v : "");
            return;
        }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    m->a = (lt_kv *)lt_realloc(m->a, (m->n + 1) * sizeof(lt_kv));
    for (i = m->n; i > lo; i--) m->a[i] = m->a[i - 1];
    m->a[lo].k = lt_strdup(k);
    m->a[lo].v = lt_strdup(v ? v : "");
    m->n++;
}

const char *lt_kvmap_get(const lt_kvmap *m, const char *k)
{
    size_t lo = 0, hi = m->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(m->a[mid].k, k);
        if (c == 0) return m->a[mid].v;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

void lt_kvmap_clear(lt_kvmap *m)
{
    size_t i;
    for (i = 0; i < m->n; i++) {
        lt_free(m->a[i].k);
        lt_free(m->a[i].v);
    }
    if (m->a) lt_free(m->a);
    m->a = NULL;
    m->n = 0;
}

/* ====================================================================== */
/* .tuneView                                                               */
/* ====================================================================== */
void lt_tune_view_clear(lt_tune_view *v)
{
    size_t i, j;
    lt_kvmap_clear(&v->bibliography);
    lt_kvmap_clear(&v->version_info);
    lt_kvmap_clear(&v->tuning_view_attrs);
    if (v->preview_image) lt_free(v->preview_image);
    for (i = 0; i < v->ncomps; i++) {
        lt_tune_comp *c = &v->comps[i];
        for (j = 0; j < c->n; j++) {
            lt_free(c->props[j].name);
            lt_free(c->props[j].ts_type);
            lt_free(c->props[j].text);
            lt_kvmap_clear(&c->props[j].attrs);
        }
        if (c->props) lt_free(c->props);
        lt_free(c->comp_type);
    }
    if (v->comps) lt_free(v->comps);
    memset(v, 0, sizeof *v);
}

static void attrs_to_map(const lt_xml_node *n, lt_kvmap *m)
{
    size_t i;
    for (i = 0; i < n->n_attrs; i++) lt_kvmap_set(m, n->attrs[i].name, n->attrs[i].value);
}

static lt_tune_prop *add_prop(lt_tune_comp *c)
{
    lt_tune_prop *p;
    c->props = (lt_tune_prop *)lt_realloc(c->props, (c->n + 1) * sizeof(lt_tune_prop));
    p = &c->props[c->n++];
    memset(p, 0, sizeof *p);
    return p;
}

static void parse_comp(lt_tune_view *v, const lt_xml_node *cn)
{
    lt_tune_comp *c;
    const char *ty = lt_xml_attr_get(cn, "type");
    size_t i, j;

    v->comps = (lt_tune_comp *)lt_realloc(v->comps, (v->ncomps + 1) * sizeof(lt_tune_comp));
    c = &v->comps[v->ncomps++];
    memset(c, 0, sizeof *c);
    c->comp_type = lt_strdup(ty ? ty : "");
    for (i = 0; i < cn->n_kids; i++) {
        const lt_xml_node *pn = cn->kids[i];
        lt_tune_prop *p = add_prop(c);
        p->name = lt_strdup(pn->name);
        p->text = lt_strdup(pn->text ? pn->text : "");
        for (j = 0; j < pn->n_attrs; j++) {
            if (strcmp(pn->attrs[j].name, "type") == 0)
                p->ts_type = lt_strdup(pn->attrs[j].value);
            else
                lt_kvmap_set(&p->attrs, pn->attrs[j].name, pn->attrs[j].value);
        }
    }
}

int lt_tune_view_parse(const char *xml, size_t len, lt_tune_view *out)
{
    char err[128];
    lt_xml_node *root;
    size_t i, j;

    memset(out, 0, sizeof *out);
    root = lt_xml_parse(xml, len, LT_XML_STRIP_NS | LT_XML_NORMALIZE, err, sizeof err);
    if (!root) {
        lt_err_set("%s", err);
        return LT_EDATA;
    }
    if (strcmp(root->name, "tuneView") == 0) {
        for (i = 0; i < root->n_kids; i++) {
            const lt_xml_node *k = root->kids[i];
            if (strcmp(k->name, "bibliography") == 0) {
                lt_kvmap_clear(&out->bibliography);
                attrs_to_map(k, &out->bibliography);
            } else if (strcmp(k->name, "versionInfo") == 0) {
                lt_kvmap_clear(&out->version_info);
                attrs_to_map(k, &out->version_info);
            } else if (strcmp(k->name, "previewImage") == 0) {
                const char *s = k->text ? k->text : "";
                char *p = (char *)lt_malloc(strlen(s) + 1);
                size_t o = 0;
                for (j = 0; s[j]; j++)
                    if (!is_wsc(s[j])) p[o++] = s[j];
                p[o] = '\0';
                if (out->preview_image) lt_free(out->preview_image);
                out->preview_image = p;
            } else if (strcmp(k->name, "tuningView") == 0) {
                size_t m;
                lt_kvmap_clear(&out->tuning_view_attrs);
                attrs_to_map(k, &out->tuning_view_attrs);
                for (m = 0; m < k->n_kids; m++)
                    if (strcmp(k->kids[m]->name, "tuneComp") == 0) parse_comp(out, k->kids[m]);
            }
        }
    }
    lt_xml_free(root);
    if (out->bibliography.n == 0 && out->version_info.n == 0 && out->ncomps == 0 &&
        !out->preview_image) {
        lt_tune_view_clear(out);
        lt_err_set("Invalid tuneView document: no recognized tuneView content");
        return LT_EDATA;
    }
    return 0;
}

static void put_attr_tv(lt_buf *b, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': lt_buf_puts(b, "&amp;"); break;
        case '<': lt_buf_puts(b, "&lt;"); break;
        case '>': lt_buf_puts(b, "&gt;"); break;
        case '"': lt_buf_puts(b, "&quot;"); break;
        case '\r': lt_buf_puts(b, "&#13;"); break;
        case '\n': lt_buf_puts(b, "&#10;"); break;
        default: lt_buf_putc(b, *s); break;
        }
    }
}

static void put_text_tv(lt_buf *b, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': lt_buf_puts(b, "&amp;"); break;
        case '<': lt_buf_puts(b, "&lt;"); break;
        case '>': lt_buf_puts(b, "&gt;"); break;
        case '\r': lt_buf_puts(b, "&#13;"); break;
        default: lt_buf_putc(b, *s); break;
        }
    }
}

static void put_kv(lt_buf *b, const lt_kvmap *m)
{
    size_t i;
    for (i = 0; i < m->n; i++) {
        lt_buf_putc(b, ' ');
        lt_buf_puts(b, m->a[i].k);
        lt_buf_puts(b, "=\"");
        put_attr_tv(b, m->a[i].v);
        lt_buf_putc(b, '"');
    }
}

char *lt_tune_view_write(const lt_tune_view *v)
{
    lt_buf b;
    size_t i, j;

    lt_buf_init(&b);
    lt_buf_puts(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n");
    lt_buf_puts(&b, "<tuneView xmlns=\"http://www.EFIAnalytics.com/:tuneView\">\n");
    if (v->bibliography.n) {
        lt_buf_puts(&b, "<bibliography");
        put_kv(&b, &v->bibliography);
        lt_buf_puts(&b, "/>\n");
    }
    if (v->version_info.n) {
        lt_buf_puts(&b, "<versionInfo");
        put_kv(&b, &v->version_info);
        lt_buf_puts(&b, "/>\n");
    }
    if (v->preview_image) {
        lt_buf_puts(&b, "<previewImage>");
        lt_buf_puts(&b, v->preview_image);
        lt_buf_puts(&b, "</previewImage>\n");
    }
    lt_buf_puts(&b, "<tuningView");
    put_kv(&b, &v->tuning_view_attrs);
    lt_buf_puts(&b, ">\n");
    for (i = 0; i < v->ncomps; i++) {
        const lt_tune_comp *c = &v->comps[i];
        lt_buf_puts(&b, "<tuneComp type=\"");
        put_attr_tv(&b, c->comp_type);
        lt_buf_puts(&b, "\">\n");
        for (j = 0; j < c->n; j++) {
            const lt_tune_prop *p = &c->props[j];
            lt_buf_putc(&b, '<');
            lt_buf_puts(&b, p->name);
            if (p->ts_type) {
                lt_buf_puts(&b, " type=\"");
                put_attr_tv(&b, p->ts_type);
                lt_buf_putc(&b, '"');
            }
            put_kv(&b, &p->attrs);
            if (p->text && p->text[0]) {
                lt_buf_putc(&b, '>');
                put_text_tv(&b, p->text);
                lt_buf_puts(&b, "</");
                lt_buf_puts(&b, p->name);
                lt_buf_puts(&b, ">\n");
            } else {
                lt_buf_puts(&b, "/>\n");
            }
        }
        lt_buf_puts(&b, "</tuneComp>\n");
    }
    lt_buf_puts(&b, "</tuningView>\n</tuneView>\n");
    return lt_buf_take(&b);
}
