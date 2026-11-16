/* tune_bridge.c - page images <-> named constants (needs the INI), manifest
 * stamping, ECU memory glue and the layered variable environment.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "tune_priv.h"
#include "../proto/proto.h"

/* ---------------------------------------------------------------------- */
/* helpers                                                                 */
/* ---------------------------------------------------------------------- */
static size_t const_end(const lt_ini_const *c)
{
    if (c->type == LT_DT_BITS) {
        int pos = c->bit_pos < 0 ? 0 : c->bit_pos, size = c->bit_size < 0 ? 8 : c->bit_size;
        return (size_t)c->offset + (size_t)((pos + size + 7) / 8);
    }
    return (size_t)c->offset + c->size_bytes;
}

static size_t ini_page_size(const lt_ini *ini, int p)
{
    size_t i, mx = 0;
    if ((size_t)p < ini->proto.n_page_sizes && ini->proto.page_sizes[p] > 0)
        return (size_t)ini->proto.page_sizes[p];
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];
        size_t e;
        if (c->is_pc_var || (int)c->page != p) continue;
        e = const_end(c);
        if (e > mx) mx = e;
    }
    return mx;
}

static void ensure_page(lt_tune *t, const lt_ini *ini, int p)
{
    size_t sz;
    lt_u8 *z;
    if (p < 0 || p > 255 || t->pages[p]) return;
    sz = ini_page_size(ini, p);
    z = (lt_u8 *)lt_calloc(sz ? sz : 1, 1);
    lt_tune_set_page(t, p, z, sz);
    lt_free(z);
}

static int write_raw_elem(const lt_ini *ini, const lt_ini_const *c, lt_pagesrc *ps, size_t idx,
                          double display)
{
    double raw = display;
    if (c->type != LT_DT_BITS) {
        raw = (c->scale != 0.0) ? (display - c->translate) / c->scale : display;
    }
    return lt_ini_const_set_raw(ini, c, ps, idx, raw);
}

static int apply_one(const lt_ini *ini, const lt_ini_const *c, lt_pagesrc *ps,
                     const lt_tune_value *v)
{
    size_t i, cnt;
    if (c->type == LT_DT_STRING) {
        if (v->kind != LT_TV_STRING) return -1;
        return lt_ini_const_set_string(ini, c, ps, v->str ? v->str : "") == 0 ? 0 : -1;
    }
    if (c->type == LT_DT_BITS) {
        double d;
        switch (v->kind) {
        case LT_TV_STRING: {
            int idx = lt_ini_bit_index(c, v->str ? v->str : "");
            if (idx >= 0) d = (double)idx;
            else if (!lt_tune_parse_num(v->str ? v->str : "", v->str ? strlen(v->str) : 0, &d))
                return -1;
            break;
        }
        case LT_TV_BOOL: {   /* a quoted "true"/"false" label reads back as Bool */
            int idx = lt_ini_bit_index(c, v->boolean ? "true" : "false");
            d = idx >= 0 ? (double)idx : (v->boolean ? 1.0 : 0.0);
            break;
        }
        case LT_TV_ARRAY: if (v->n == 0) return -1; d = v->arr[0]; break;
        default: d = v->scalar; break;
        }
        return write_raw_elem(ini, c, ps, 0, lt_round(d)) == 0 ? 0 : -1;
    }
    cnt = lt_shape_count(&c->shape);
    switch (v->kind) {
    case LT_TV_SCALAR:
        return write_raw_elem(ini, c, ps, 0, v->scalar) == 0 ? 0 : -1;
    case LT_TV_BOOL:
        return write_raw_elem(ini, c, ps, 0, v->boolean ? 1.0 : 0.0) == 0 ? 0 : -1;
    case LT_TV_ARRAY: {
        int ok = 0;
        for (i = 0; i < v->n && i < cnt; i++)
            if (write_raw_elem(ini, c, ps, i, v->arr[i]) == 0) ok = 1;
        return ok ? 0 : -1;
    }
    default:
        return -1;
    }
}

int lt_tune_apply_to_pages(lt_tune *t, const lt_ini *ini)
{
    lt_pagesrc ps = lt_tune_pagesrc(t);
    size_t i;
    int n = 0;
    for (i = 0; i < t->constants.cap; i++) {
        const lt_ini_const *c;
        lt_tune_entry *e;
        if (t->constants.slots[i].used != 1) continue;
        c = lt_ini_find_const(ini, t->constants.slots[i].key);
        if (!c || c->is_pc_var) continue;
        e = (lt_tune_entry *)t->constants.slots[i].val;
        ensure_page(t, ini, c->page);
        if (apply_one(ini, c, &ps, &e->value) == 0) n++;
    }
    return n;
}

/* ---------------------------------------------------------------------- */
/* capture                                                                 */
/* ---------------------------------------------------------------------- */
static void set_attr_str(char **dst, const char *s)
{
    if (*dst) lt_free(*dst);
    *dst = lt_strdup(s);
}

static double tidy(const lt_ini_const *c, double d)
{
    if (c->type == LT_DT_F32 || c->type == LT_DT_F64) return d;
    if (fabs(d) < 1e6) return lt_round(d * 1e10) / 1e10;
    return d;
}

static void fill_attrs(lt_tune_entry *e, const lt_ini_const *c)
{
    char b[32];
    if (e->has_attrs || c->type == LT_DT_BITS || c->type == LT_DT_STRING) return;
    if (c->units && c->units[0] && c->units[0] != '{') set_attr_str(&e->attrs.units, c->units);
    if (c->digits >= 0) {
        lt_snprintf(b, sizeof b, "%d", c->digits);
        set_attr_str(&e->attrs.digits, b);
    }
    if (c->shape.kind == LT_SH_1D) {
        lt_snprintf(b, sizeof b, "%lu", (unsigned long)c->shape.n);
        set_attr_str(&e->attrs.rows, b);
        set_attr_str(&e->attrs.cols, "1");
    } else if (c->shape.kind == LT_SH_2D) {
        lt_snprintf(b, sizeof b, "%lu", (unsigned long)c->shape.rows);
        set_attr_str(&e->attrs.rows, b);
        lt_snprintf(b, sizeof b, "%lu", (unsigned long)c->shape.cols);
        set_attr_str(&e->attrs.cols, b);
    }
    e->has_attrs = (e->attrs.units || e->attrs.digits || e->attrs.rows || e->attrs.cols) ? LT_TRUE : LT_FALSE;
}

static lt_bool decode_const(const lt_ini *ini, const lt_ini_const *c, const lt_pagesrc *ps,
                            lt_tune_value *out)
{
    size_t i;
    lt_tune_value_init(out);
    if (c->type == LT_DT_STRING) {
        char *buf = (char *)lt_malloc(c->size_bytes + 2);
        if (lt_ini_const_get_string(ini, c, ps, buf, c->size_bytes + 1) != 0) {
            lt_free(buf);
            return LT_FALSE;
        }
        out->kind = LT_TV_STRING;
        out->str = buf;
        return LT_TRUE;
    }
    if (c->type == LT_DT_BITS) {
        double d;
        const char *lab;
        if (lt_ini_const_get(ini, c, ps, 0, &d) != 0) return LT_FALSE;
        lab = d >= 0 ? lt_ini_bit_option(c, (size_t)d) : NULL;
        if (lab && lt_ini_bit_index(c, lab) == (int)d) {   /* ambiguous labels stay numeric */
            out->kind = LT_TV_STRING;
            out->str = lt_strdup(lab);
        } else {
            out->scalar = d;
        }
        return LT_TRUE;
    }
    if (c->shape.kind == LT_SH_SCALAR) {
        double d;
        if (lt_ini_const_get(ini, c, ps, 0, &d) != 0) return LT_FALSE;
        out->scalar = tidy(c, d);
        return LT_TRUE;
    } else {
        size_t n = lt_shape_count(&c->shape);
        double *a = (double *)lt_malloc((n ? n : 1) * sizeof(double));
        int rc = lt_ini_const_get_array(ini, c, ps, a, n);
        if (rc < 0) {
            lt_free(a);
            return LT_FALSE;
        }
        for (i = 0; i < n; i++) a[i] = tidy(c, a[i]);
        out->kind = LT_TV_ARRAY;
        out->arr = a;
        out->n = n;
        return LT_TRUE;
    }
}

int lt_tune_capture_from_pages(lt_tune *t, const lt_ini *ini)
{
    lt_pagesrc ps = lt_tune_pagesrc(t);
    size_t i;
    int n = 0;
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];
        lt_tune_value v;
        lt_tune_entry *e;
        if (c->is_pc_var || !t->pages[c->page]) continue;
        if (!decode_const(ini, c, &ps, &v)) continue;
        lt_tune_set_constant(t, c->name, &v, c->page);
        lt_tune_value_clear(&v);
        e = (lt_tune_entry *)lt_map_get(&t->constants, c->name);
        fill_attrs(e, c);
        n++;
    }
    lt_tune_stamp_ini(t, ini, t->ini_metadata ? t->ini_metadata->hash : NULL);
    return n;
}

void lt_tune_stamp_ini(lt_tune *t, const lt_ini *ini, const char *ini_hash)
{
    size_t i;
    char *hash = lt_strdup(ini_hash ? ini_hash : "");
    const char *name = ini->source_path ? lt_path_base(ini->source_path) : "";
    if (t->ini_metadata) {
        lt_free(t->ini_metadata->signature);
        lt_free(t->ini_metadata->name);
        lt_free(t->ini_metadata->hash);
        lt_free(t->ini_metadata->spec_version);
        lt_free(t->ini_metadata->saved_at);
    } else {
        t->ini_metadata = (lt_ini_metadata *)lt_calloc(1, sizeof *t->ini_metadata);
    }
    t->ini_metadata->signature = lt_strdup(ini->signature ? ini->signature : "");
    t->ini_metadata->name = lt_strdup(name);
    t->ini_metadata->hash = hash;
    t->ini_metadata->spec_version = lt_strdup(ini->ini_spec_version ? ini->ini_spec_version : "");
    t->ini_metadata->saved_at = lt_tune_now_rfc3339();

    for (i = 0; i < t->n_manifest; i++) {
        lt_free(t->manifest[i].name);
        lt_free(t->manifest[i].type);
    }
    if (t->manifest) lt_free(t->manifest);
    t->manifest = (lt_manifest_entry *)lt_malloc((ini->n_constants + 1) * sizeof(lt_manifest_entry));
    t->n_manifest = 0;
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];
        lt_manifest_entry *m;
        if (c->is_pc_var) continue;
        m = &t->manifest[t->n_manifest++];
        m->name = lt_strdup(c->name);
        m->type = lt_strdup(lt_dtype_name(c->type));
        m->page = c->page;
        m->offset = c->offset;
        m->scale = c->scale;
        m->translate = c->translate;
    }
    t->has_manifest = LT_TRUE;
    if ((!t->signature || !t->signature[0]) && ini->signature) lt_tune_set_signature(t, ini->signature);
}

lt_tune *lt_tune_from_defaults(const lt_ini *ini)
{
    lt_tune *t = lt_tune_new(ini->signature ? ini->signature : "");
    lt_pagesrc ps;
    size_t i;
    int p;
    for (p = 0; p < (int)ini->n_pages && p < 256; p++) ensure_page(t, ini, p);
    ps = lt_tune_pagesrc(t);
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];
        double d;
        if (!lt_ini_default_value(ini, c->name, &d)) continue;
        if (c->is_pc_var) {
            lt_tune_value v;
            lt_tune_value_init(&v);
            v.scalar = d;
            lt_tune_set_pc_variable(t, c->name, &v);
            continue;
        }
        if (c->type == LT_DT_STRING) continue;
        ensure_page(t, ini, c->page);
        if (c->shape.kind == LT_SH_SCALAR || c->type == LT_DT_BITS) write_raw_elem(ini, c, &ps, 0, d);
    }
    lt_tune_capture_from_pages(t, ini);
    return t;
}

/* ---------------------------------------------------------------------- */
/* ECU memory glue                                                         */
/* ---------------------------------------------------------------------- */
int lt_tune_to_mem(const lt_tune *t, struct lt_ecu_mem *mem)
{
    size_t n = lt_ecu_mem_page_count(mem);
    int p, count = 0;
    for (p = 0; p < 256 && (size_t)p < n; p++) {
        if (!t->pages[p]) continue;
        lt_ecu_mem_load_page(mem, p, t->pages[p], t->page_len[p]);
        count++;
    }
    return count;
}

int lt_tune_from_mem(lt_tune *t, const struct lt_ecu_mem *mem)
{
    size_t n = lt_ecu_mem_page_count(mem);
    int p, count = 0;
    for (p = 0; p < 256 && (size_t)p < n; p++) {
        size_t len = 0;
        const lt_u8 *d = lt_ecu_mem_page(mem, p, &len);
        if (!d) continue;
        lt_tune_set_page(t, p, d, len);
        count++;
    }
    return count;
}

/* ---------------------------------------------------------------------- */
/* variable environment                                                    */
/* ---------------------------------------------------------------------- */
static lt_varenv *venv_of(void *ud, int with_ch)
{
    return with_ch ? (lt_varenv *)((char *)ud - offsetof(lt_varenv, env))
                   : (lt_varenv *)((char *)ud - offsetof(lt_varenv, consts));
}

static lt_bool tv_scalar(const lt_tune_value *v, double *out)
{
    switch (v->kind) {
    case LT_TV_SCALAR: *out = v->scalar; return LT_TRUE;
    case LT_TV_BOOL: *out = v->boolean ? 1.0 : 0.0; return LT_TRUE;
    case LT_TV_ARRAY: if (v->n) { *out = v->arr[0]; return LT_TRUE; } return LT_FALSE;
    default: return LT_FALSE;
    }
}

static lt_bool get_var_impl(lt_varenv *v, const char *name, double *out, int with_ch)
{
    const lt_tune_entry *e;
    const lt_ini_const *c;
    if (with_ch && v->rt && lt_ini_rt_get(v->rt, name, out)) return LT_TRUE;
    if (v->pcvars) {
        e = lt_tune_get_pc_variable(v->pcvars, name);
        if (e && tv_scalar(&e->value, out)) return LT_TRUE;
    }
    c = v->ini ? lt_ini_find_const(v->ini, name) : NULL;
    if (c && !c->is_pc_var && c->type != LT_DT_STRING &&
        lt_ini_const_get(v->ini, c, &v->pages, 0, out) == 0)
        return LT_TRUE;
    if (v->ini && lt_ini_default_value(v->ini, name, out)) return LT_TRUE;
    return LT_FALSE;
}

static lt_bool cb_get_var(void *ud, const char *n, double *o) { return get_var_impl(venv_of(ud, 1), n, o, 1); }
static lt_bool cb_get_var_c(void *ud, const char *n, double *o) { return get_var_impl(venv_of(ud, 0), n, o, 0); }

static const char *get_string_impl(lt_varenv *v, const char *name)
{
    const lt_ini_const *c;
    const lt_tune_entry *e;
    if (v->pcvars) {
        e = lt_tune_get_pc_variable(v->pcvars, name);
        if (e && e->value.kind == LT_TV_STRING && e->value.str) return e->value.str;
    }
    c = v->ini ? lt_ini_find_const(v->ini, name) : NULL;
    if (!c) return NULL;
    if (c->type == LT_DT_STRING) {
        if (lt_ini_const_get_string(v->ini, c, &v->pages, v->scratch, sizeof v->scratch) == 0)
            return v->scratch;
        return NULL;
    }
    if (c->type == LT_DT_BITS) {
        double d;
        const char *lab;
        if (lt_ini_const_get(v->ini, c, &v->pages, 0, &d) == 0 && d >= 0) {
            lab = lt_ini_bit_option(c, (size_t)d);
            if (lab) return lab;
        }
    }
    return NULL;
}

static const char *cb_get_string(void *ud, const char *n) { return get_string_impl(venv_of(ud, 1), n); }
static const char *cb_get_string_c(void *ud, const char *n) { return get_string_impl(venv_of(ud, 0), n); }

static const char *const *bit_opts_impl(lt_varenv *v, const char *name, size_t *n)
{
    const lt_ini_const *c = v->ini ? lt_ini_find_const(v->ini, name) : NULL;
    if (!c || c->type != LT_DT_BITS || !c->bit_options) return NULL;
    *n = c->n_bit_options;
    return (const char *const *)c->bit_options;
}

static const char *const *cb_bit_opts(void *ud, const char *n, size_t *k) { return bit_opts_impl(venv_of(ud, 1), n, k); }
static const char *const *cb_bit_opts_c(void *ud, const char *n, size_t *k) { return bit_opts_impl(venv_of(ud, 0), n, k); }

static lt_bool table_impl(lt_varenv *v, const char *file, double x, double *out)
{
    const lt_inc_table *t;
    if (!v->inc) return LT_FALSE;
    t = lt_inc_cache_get(v->inc, file);
    return t ? lt_inc_lookup(t, x, out) : LT_FALSE;
}

static lt_bool cb_table(void *ud, const char *f, double x, double *o) { return table_impl(venv_of(ud, 1), f, x, o); }
static lt_bool cb_table_c(void *ud, const char *f, double x, double *o) { return table_impl(venv_of(ud, 0), f, x, o); }

static lt_bool array_impl(lt_varenv *v, const char *name, double idx, double *out)
{
    const lt_ini_const *c = v->ini ? lt_ini_find_const(v->ini, name) : NULL;
    double *a;
    size_t n, i0, i1;
    double f;
    int rc;
    const lt_tune_entry *e;

    if (v->pcvars) {
        e = lt_tune_get_pc_variable(v->pcvars, name);
        if (e && e->value.kind == LT_TV_ARRAY && e->value.n) {
            a = e->value.arr;
            n = e->value.n;
            goto interp;
        }
    }
    if (!c || c->type == LT_DT_STRING || c->type == LT_DT_BITS || c->is_pc_var) return LT_FALSE;
    n = lt_shape_count(&c->shape);
    if (n == 0) return LT_FALSE;
    a = (double *)lt_malloc(n * sizeof(double));
    rc = lt_ini_const_get_array(v->ini, c, &v->pages, a, n);
    if (rc < 0) {
        lt_free(a);
        return LT_FALSE;
    }
    if (idx <= 0) *out = a[0];
    else if (idx >= (double)(n - 1)) *out = a[n - 1];
    else {
        i0 = (size_t)idx;
        i1 = i0 + 1;
        f = idx - (double)i0;
        *out = a[i0] + (a[i1] - a[i0]) * f;
    }
    lt_free(a);
    return LT_TRUE;
interp:
    if (idx <= 0) *out = a[0];
    else if (idx >= (double)(n - 1)) *out = a[n - 1];
    else {
        i0 = (size_t)idx;
        i1 = i0 + 1;
        f = idx - (double)i0;
        *out = a[i0] + (a[i1] - a[i0]) * f;
    }
    return LT_TRUE;
}

static lt_bool cb_array(void *ud, const char *n, double i, double *o) { return array_impl(venv_of(ud, 1), n, i, o); }
static lt_bool cb_array_c(void *ud, const char *n, double i, double *o) { return array_impl(venv_of(ud, 0), n, i, o); }
static lt_bool cb_online(void *ud) { return venv_of(ud, 1)->online; }
static lt_bool cb_online_c(void *ud) { return venv_of(ud, 0)->online; }
static const char *cb_dir(void *ud, int which) { LT_UNUSED(ud); LT_UNUSED(which); return ""; }

static double now_s(void)
{
    return (double)time(NULL);
}

static double cb_time(void *ud)
{
    lt_varenv *v = venv_of(ud, 1);
    return now_s() - v->t0_ms / 1000.0;
}

static double cb_time_c(void *ud)
{
    lt_varenv *v = venv_of(ud, 0);
    return now_s() - v->t0_ms / 1000.0;
}

void lt_varenv_init(lt_varenv *v, const lt_ini *ini, const lt_pagesrc *pages, lt_ini_rt *rt,
                    const lt_tune *pcvars)
{
    memset(v, 0, sizeof *v);
    v->ini = ini;
    if (pages) v->pages = *pages;
    v->rt = rt;
    v->pcvars = pcvars;
    v->t0_ms = now_s() * 1000.0;
    lt_expr_env_init(&v->env);
    lt_expr_env_init(&v->consts);
    v->env.ud = &v->env;
    v->env.get_var = cb_get_var;
    v->env.get_string = cb_get_string;
    v->env.get_bit_options = cb_bit_opts;
    v->env.table_lookup = cb_table;
    v->env.array_value = cb_array;
    v->env.is_online = cb_online;
    v->env.get_dir = cb_dir;
    v->env.time_now = cb_time;
    v->consts.ud = &v->consts;
    v->consts.get_var = cb_get_var_c;
    v->consts.get_string = cb_get_string_c;
    v->consts.get_bit_options = cb_bit_opts_c;
    v->consts.table_lookup = cb_table_c;
    v->consts.array_value = cb_array_c;
    v->consts.is_online = cb_online_c;
    v->consts.get_dir = cb_dir;
    v->consts.time_now = cb_time_c;
}
