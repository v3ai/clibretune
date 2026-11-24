/* agent_tools.c - the assistant's tool catalogue, executor and proposal queue.
 * Read tools inspect the open definition and tune; propose tools only queue proposals
 * (validated against the INI, clamped to the AutoTune authority limits) that the user
 * accepts in the UI.  The MCP server exposes the read tools only.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "agent.h"
#include "../tableops/tableops.h"
#include "../autotune/autotune.h"
#include "../datalog/datalog.h"
#include "../plat/plat.h"
#include "../project/project.h"

/* ---- catalogue ---- */
static const lt_ag_tool ag_catalog[] = {
    { "list_tables",
      "List all editable tables with their role (Ve / Ignition / AfrTarget / ...) and "
      "dimensions. Call first to discover names.",
      "{\"type\":\"object\",\"properties\":{}}", 0, 0 },
    { "read_table",
      "Read the current values, axis bins and units of one table.",
      "{\"type\":\"object\",\"required\":[\"table_name\"],\"properties\":"
      "{\"table_name\":{\"type\":\"string\"}}}", 0, 0 },
    { "read_constant",
      "Read a single constant's current value, min/max, units and options (for bits-type "
      "feature toggles).",
      "{\"type\":\"object\",\"required\":[\"name\"],\"properties\":{\"name\":{\"type\":\"string\"}}}",
      0, 0 },
    { "list_features",
      "List feature-toggle (bits) constants with their current setting and available options.",
      "{\"type\":\"object\",\"properties\":{}}", 0, 0 },
    { "tune_health_check",
      "Health score of a table: value statistics, axis monotonicity and suspect cells that "
      "differ strongly from their neighbors.",
      "{\"type\":\"object\",\"required\":[\"table_name\"],\"properties\":"
      "{\"table_name\":{\"type\":\"string\"}}}", 0, 0 },
    { "get_realtime_snapshot",
      "Read the ECU's current sensor values (RPM, MAP, TPS, CLT, AFR, ...) as one snapshot. "
      "Requires a live connection.",
      "{\"type\":\"object\",\"properties\":{}}", 0, 0 },
    { "query_datalog",
      "Query recorded datalogs from the project's datalogs folder (newest when 'log' is "
      "omitted). mode 'summary': per-channel min/max/mean/last over up to 50 channels; "
      "'tail': the last rows (up to 50) of the requested channels.",
      "{\"type\":\"object\",\"properties\":{\"log\":{\"type\":\"string\"},\"channels\":"
      "{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"mode\":{\"type\":\"string\","
      "\"enum\":[\"summary\",\"tail\"]}}}", 0, 0 },
    { "propose_table_edit",
      "Propose changing one cell of a table. The change is staged for explicit user approval "
      "and never applied automatically.",
      "{\"type\":\"object\",\"required\":[\"table_name\",\"x_index\",\"y_index\",\"new_value\"],"
      "\"properties\":{\"table_name\":{\"type\":\"string\"},\"x_index\":{\"type\":\"integer\","
      "\"description\":\"column (0-based)\"},\"y_index\":{\"type\":\"integer\","
      "\"description\":\"row (0-based)\"},\"new_value\":{\"type\":\"number\"},"
      "\"reason\":{\"type\":\"string\"}}}", LT_AG_TIER_TUNE, 1 },
    { "propose_bulk_operation",
      "Propose a bulk operation (scale / offset / smooth / interpolate / set_equal) over a set "
      "of cells. Staged for user approval.",
      "{\"type\":\"object\",\"required\":[\"table_name\",\"operation\",\"cells\"],\"properties\":"
      "{\"table_name\":{\"type\":\"string\"},\"operation\":{\"type\":\"string\",\"enum\":"
      "[\"scale\",\"offset\",\"smooth\",\"interpolate\",\"set_equal\"]},\"cells\":{\"type\":"
      "\"array\",\"items\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"},\"maxItems\":2},"
      "\"description\":\"[x_index, y_index] pairs\"},\"value\":{\"type\":\"number\","
      "\"description\":\"factor, offset or value for scale / offset / set_equal\"},"
      "\"reason\":{\"type\":\"string\"}}}", LT_AG_TIER_TUNE, 1 },
    { "propose_constant_change",
      "Propose changing a single constant or feature toggle. Staged for user approval; "
      "dangerous constants (pin assignments, limits) are flagged.",
      "{\"type\":\"object\",\"required\":[\"name\",\"value\"],\"properties\":{\"name\":"
      "{\"type\":\"string\"},\"value\":{\"type\":\"number\"},\"reason\":{\"type\":\"string\"}}}",
      LT_AG_TIER_CONFIG, 1 }
};

size_t lt_ag_tool_count(void) { return LT_COUNTOF(ag_catalog); }
const lt_ag_tool *lt_ag_tool_at(size_t i) { return i < LT_COUNTOF(ag_catalog) ? &ag_catalog[i] : NULL; }

const lt_ag_tool *lt_ag_tool_find(const char *name)
{
    size_t i;
    if (name == NULL) { return NULL; }
    for (i = 0; i < LT_COUNTOF(ag_catalog); i++) {
        if (strcmp(ag_catalog[i].name, name) == 0) { return &ag_catalog[i]; }
    }
    return NULL;
}

int lt_ag_tool_allowed(int tier, const char *name)
{
    const lt_ag_tool *t = lt_ag_tool_find(name);
    return t != NULL && tier >= t->min_tier;
}

lt_json *lt_ag_catalogue(int tier, int read_only, int openai)
{
    lt_json *arr = lt_json_arr();
    size_t i;
    for (i = 0; i < LT_COUNTOF(ag_catalog); i++) {
        lt_json *o, *schema;
        const lt_ag_tool *t = &ag_catalog[i];
        char err[64];
        if (tier < t->min_tier || (read_only && t->write)) { continue; }
        schema = lt_json_parse(t->schema, strlen(t->schema), err, sizeof(err));
        if (schema == NULL) { continue; }
        o = lt_json_obj();
        lt_json_obj_set(o, "name", lt_json_str(t->name));
        lt_json_obj_set(o, "description", lt_json_str(t->desc));
        if (openai) {
            lt_json *f = lt_json_obj(), *w = lt_json_obj();
            lt_json_obj_set(f, "name", lt_json_str(t->name));
            lt_json_obj_set(f, "description", lt_json_str(t->desc));
            lt_json_obj_set(f, "parameters", schema);
            lt_json_obj_set(w, "type", lt_json_str("function"));
            lt_json_obj_set(w, "function", f);
            lt_json_free(o);
            o = w;
        } else {
            lt_json_obj_set(o, "inputSchema", schema);
        }
        lt_json_arr_push(arr, o);
    }
    return arr;
}

/* ---- proposal queue ---- */
void lt_ag_props_init(lt_ag_props *p) { memset(p, 0, sizeof(*p)); }

void lt_ag_props_clear(lt_ag_props *p)
{
    size_t i;
    for (i = 0; i < p->n; i++) { lt_free(p->v[i].cells); }
    p->n = 0;
}

void lt_ag_props_free(lt_ag_props *p)
{
    lt_ag_props_clear(p);
    lt_free(p->v);
    p->v = NULL;
    p->cap = 0;
}

static lt_ag_prop *props_add(lt_ag_props *p)
{
    lt_ag_prop *r;
    if (p->n == p->cap) {
        p->cap = p->cap == 0 ? 8 : p->cap * 2;
        p->v = (lt_ag_prop *)lt_realloc(p->v, p->cap * sizeof(lt_ag_prop));
    }
    r = &p->v[p->n++];
    memset(r, 0, sizeof(*r));
    return r;
}

size_t lt_ag_props_pending(const lt_ag_props *p)
{
    size_t i, n = 0;
    for (i = 0; i < p->n; i++) { if (p->v[i].state == LT_AG_PENDING && !p->v[i].applied) { n++; } }
    return n;
}

size_t lt_ag_props_accepted(const lt_ag_props *p)
{
    size_t i, n = 0;
    for (i = 0; i < p->n; i++) {
        if (p->v[i].state == LT_AG_ACCEPTED && p->v[i].valid && !p->v[i].applied) { n++; }
    }
    return n;
}

void lt_ag_prop_describe(const lt_ag_prop *p, char *out, size_t cap)
{
    char a[32], b[32];
    if (p->kind == LT_AG_P_CELL) {
        lt_fmt_double(a, sizeof(a), p->old_value, 3);
        lt_fmt_double(b, sizeof(b), p->param, 3);
        lt_snprintf(out, cap, "%s [col %lu, row %lu]: %s -> %s", p->name, (unsigned long)p->x,
                    (unsigned long)p->y, a, b);
    } else if (p->kind == LT_AG_P_BULK) {
        lt_fmt_double(a, sizeof(a), p->param, 3);
        lt_snprintf(out, cap, "%s: %s %s over %lu cells", p->name, p->op, a,
                    (unsigned long)p->n_cells);
    } else {
        lt_fmt_double(a, sizeof(a), p->old_value, 3);
        lt_fmt_double(b, sizeof(b), p->param, 3);
        lt_snprintf(out, cap, "constant %s: %s -> %s", p->name, a, b);
    }
}

/* ---- helpers ---- */
static double fin(double v) { return lt_isfinite(v) ? v : 0.0; }
static void jset_n(lt_json *o, const char *k, double v) { lt_json_obj_set(o, k, lt_json_num(fin(v))); }
static void jset_s(lt_json *o, const char *k, const char *v) { lt_json_obj_set(o, k, lt_json_str(v != NULL ? v : "")); }

static int fail(lt_buf *out, const char *fmt, ...)
{
    char msg[256];
    lt_json *o = lt_json_obj();
    va_list ap;
    va_start(ap, fmt);
    lt_vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    jset_s(o, "error", msg);
    lt_json_write(o, 0, out);
    lt_json_free(o);
    return -1;
}

static int done(lt_buf *out, lt_json *o)
{
    lt_json_write(o, 0, out);
    lt_json_free(o);
    return 0;
}

static const char *role_name(lt_table_role r)
{
    switch (r) {
    case LT_ROLE_VE: return "Ve";
    case LT_ROLE_IGNITION: return "Ignition";
    case LT_ROLE_AFR_TARGET: return "AfrTarget";
    case LT_ROLE_WARMUP: return "Warmup";
    default: return "Other";
    }
}

static lt_json *num_arr(const double *v, size_t n)
{
    lt_json *a = lt_json_arr();
    size_t i;
    for (i = 0; i < n; i++) { lt_json_arr_push(a, lt_json_num(fin(v[i]))); }
    return a;
}

static const char *arg_str(const lt_json *args, const char *k)
{
    const char *s = lt_json_get_str(args, k, "");
    return s;
}

/* ---- read tools ---- */
static int t_list_tables(const lt_ag_env *env, lt_buf *out)
{
    lt_json *o = lt_json_obj(), *a = lt_json_arr();
    size_t i;
    for (i = 0; i < env->ini->n_tables; i++) {
        const lt_ini_table *t = env->ini->tables[i];
        lt_json *e = lt_json_obj();
        jset_s(e, "name", t->name);
        jset_s(e, "title", t->title);
        jset_s(e, "role", role_name(t->role));
        jset_n(e, "cols", (double)t->x_size);
        jset_n(e, "rows", (double)t->y_size);
        lt_json_arr_push(a, e);
    }
    lt_json_obj_set(o, "tables", a);
    return done(out, o);
}

static int load_view(const lt_ag_env *env, const char *name, lt_tblview *v,
                     const lt_ini_table **tp, lt_buf *out)
{
    const lt_ini_table *t = lt_ini_find_table(env->ini, name);
    int rc;
    lt_tblview_init(v);
    if (t == NULL) { return fail(out, "no table named '%s' (call list_tables)", name); }
    rc = lt_tblview_load_table(v, env->ini, t, &env->src);
    if (rc < 0) {
        lt_tblview_free(v);
        return fail(out, "cannot read table '%s': %s", name, lt_err_get());
    }
    *tp = t;
    return 0;
}

static int t_read_table(const lt_ag_env *env, const lt_json *args, lt_buf *out)
{
    lt_tblview v;
    const lt_ini_table *t = NULL;
    lt_json *o, *z;
    const lt_ini_const *zc;
    size_t r;
    if (load_view(env, arg_str(args, "table_name"), &v, &t, out) != 0) { return -1; }
    o = lt_json_obj();
    jset_s(o, "name", t->name);
    jset_s(o, "title", t->title);
    jset_s(o, "role", role_name(t->role));
    jset_n(o, "rows", (double)v.rows);
    jset_n(o, "cols", (double)v.cols);
    zc = lt_ini_find_const(env->ini, t->z_const);
    jset_s(o, "units", zc != NULL ? zc->units : "");
    lt_json_obj_set(o, "x_bins", num_arr(v.x, v.cols));
    if (v.y != NULL) { lt_json_obj_set(o, "y_bins", num_arr(v.y, v.rows)); }
    z = lt_json_arr();
    for (r = 0; r < v.rows; r++) { lt_json_arr_push(z, num_arr(v.z + r * v.cols, v.cols)); }
    lt_json_obj_set(o, "values_by_row", z);
    lt_tblview_free(&v);
    return done(out, o);
}

static int t_read_constant(const lt_ag_env *env, const lt_json *args, lt_buf *out)
{
    const char *name = arg_str(args, "name");
    const lt_ini_const *c = lt_ini_find_const(env->ini, name);
    lt_json *o;
    double d = 0.0;
    size_t cnt;
    if (c == NULL) { return fail(out, "no constant named '%s'", name); }
    o = lt_json_obj();
    jset_s(o, "name", c->name);
    jset_s(o, "units", c->units);
    if (c->type == LT_DT_STRING) {
        char s[128];
        s[0] = '\0';
        (void)lt_ini_const_get_string(env->ini, c, &env->src, s, sizeof(s));
        jset_s(o, "value", s);
        return done(out, o);
    }
    cnt = lt_shape_count(&c->shape);
    if (cnt > 1) {
        double *buf = (double *)lt_malloc((cnt + 1) * sizeof(double));
        int n = lt_ini_const_get_array(env->ini, c, &env->src, buf, cnt);
        if (n < 0) {
            lt_free(buf);
            lt_json_free(o);
            return fail(out, "cannot read '%s': %s", name, lt_err_get());
        }
        lt_json_obj_set(o, "values", num_arr(buf, (size_t)(n > 256 ? 256 : n)));
        jset_n(o, "count", (double)n);
        lt_free(buf);
    } else {
        if (lt_ini_const_get(env->ini, c, &env->src, 0, &d) < 0) {
            lt_json_free(o);
            return fail(out, "cannot read '%s': %s", name, lt_err_get());
        }
        jset_n(o, "value", d);
    }
    if (c->type == LT_DT_BITS) {
        lt_json *opts = lt_json_arr();
        size_t i;
        const char *cur = lt_ini_bit_option(c, (size_t)(d < 0 ? 0 : d));
        for (i = 0; i < c->n_bit_options; i++) {
            if (c->bit_options[i] != NULL) { lt_json_arr_push(opts, lt_json_str(c->bit_options[i])); }
        }
        lt_json_obj_set(o, "options", opts);
        if (cur != NULL) { jset_s(o, "option", cur); }
    } else if (c->range_resolved) {
        jset_n(o, "min", c->min);
        jset_n(o, "max", c->max);
    }
    return done(out, o);
}

static int t_list_features(const lt_ag_env *env, lt_buf *out)
{
    lt_json *o = lt_json_obj(), *a = lt_json_arr();
    size_t i;
    for (i = 0; i < env->ini->n_constants && lt_json_at(a, 199) == NULL; i++) {
        const lt_ini_const *c = env->ini->constants[i];
        lt_json *e, *opts;
        double d = 0.0;
        size_t k;
        const char *cur;
        if (c->type != LT_DT_BITS || c->n_bit_options < 2) { continue; }
        if (lt_ini_const_get(env->ini, c, &env->src, 0, &d) < 0) { continue; }
        e = lt_json_obj();
        jset_s(e, "name", c->name);
        cur = lt_ini_bit_option(c, (size_t)(d < 0 ? 0 : d));
        jset_s(e, "current", cur);
        opts = lt_json_arr();
        for (k = 0; k < c->n_bit_options; k++) {
            if (c->bit_options[k] != NULL) { lt_json_arr_push(opts, lt_json_str(c->bit_options[k])); }
        }
        lt_json_obj_set(e, "options", opts);
        lt_json_arr_push(a, e);
    }
    lt_json_obj_set(o, "features", a);
    return done(out, o);
}

static int t_health(const lt_ag_env *env, const lt_json *args, lt_buf *out)
{
    lt_tblview v;
    const lt_ini_table *t = NULL;
    lt_json *o, *sus;
    size_t x, y, n, bad = 0, nsus = 0;
    double sum = 0.0, sq = 0.0, lo, hi, mean, sd, score;
    int mono_x = 1, mono_y = 1;
    if (load_view(env, arg_str(args, "table_name"), &v, &t, out) != 0) { return -1; }
    n = v.rows * v.cols;
    lo = hi = v.z[0];
    for (x = 0; x < n; x++) {
        sum += v.z[x];
        sq += v.z[x] * v.z[x];
        if (v.z[x] < lo) { lo = v.z[x]; }
        if (v.z[x] > hi) { hi = v.z[x]; }
    }
    mean = sum / (double)n;
    sd = sqrt(fabs(sq / (double)n - mean * mean));
    for (x = 1; x < v.cols; x++) { if (v.x[x] <= v.x[x - 1]) { mono_x = 0; } }
    if (v.y != NULL) { for (y = 1; y < v.rows; y++) { if (v.y[y] <= v.y[y - 1]) { mono_y = 0; } } }
    sus = lt_json_arr();
    for (y = 0; y < v.rows; y++) {
        for (x = 0; x < v.cols; x++) {
            double nb = 0.0, c = v.z[y * v.cols + x], dev;
            int k = 0;
            if (x > 0) { nb += v.z[y * v.cols + x - 1]; k++; }
            if (x + 1 < v.cols) { nb += v.z[y * v.cols + x + 1]; k++; }
            if (y > 0) { nb += v.z[(y - 1) * v.cols + x]; k++; }
            if (y + 1 < v.rows) { nb += v.z[(y + 1) * v.cols + x]; k++; }
            if (k == 0) { continue; }
            nb /= (double)k;
            dev = fabs(c - nb);
            if (dev > 0.25 * fabs(nb) && dev > 0.05 * (hi - lo)) {
                bad++;
                if (nsus < 10) {
                    lt_json *e = lt_json_obj();
                    jset_n(e, "x_index", (double)x);
                    jset_n(e, "y_index", (double)y);
                    jset_n(e, "value", c);
                    jset_n(e, "neighbour_mean", nb);
                    lt_json_arr_push(sus, e);
                    nsus++;
                }
            }
        }
    }
    score = 100.0 - 400.0 * (double)bad / (double)n - (mono_x ? 0.0 : 20.0) - (mono_y ? 0.0 : 20.0);
    if (score < 0.0) { score = 0.0; }
    o = lt_json_obj();
    jset_s(o, "name", t->name);
    jset_n(o, "health_score", lt_round(score));
    jset_n(o, "cells", (double)n);
    jset_n(o, "min", lo);
    jset_n(o, "max", hi);
    jset_n(o, "mean", mean);
    jset_n(o, "std_dev", sd);
    lt_json_obj_set(o, "x_axis_increasing", lt_json_bool(mono_x));
    lt_json_obj_set(o, "y_axis_increasing", lt_json_bool(mono_y));
    jset_n(o, "suspect_cell_count", (double)bad);
    lt_json_obj_set(o, "suspect_cells", sus);
    jset_s(o, "note", "score is computed from the table values only (no datalog coverage)");
    lt_tblview_free(&v);
    return done(out, o);
}

static int t_realtime(const lt_ag_env *env, lt_buf *out)
{
    lt_json *o, *ch;
    size_t i, k = 0;
    if (env->rt == NULL) { return fail(out, "no live ECU connection"); }
    o = lt_json_obj();
    ch = lt_json_obj();
    for (i = 0; i < env->ini->n_channels && k < 300; i++) {
        const lt_ini_channel *c = env->ini->channels[i];
        if (!lt_ini_rt_valid(env->rt, c->index)) { continue; }
        jset_n(ch, c->name, lt_ini_rt_value(env->rt, c->index));
        k++;
    }
    lt_json_obj_set(o, "channels", ch);
    return done(out, o);
}

static int safe_name(const char *s)
{
    return s[0] != '\0' && strchr(s, '/') == NULL && strchr(s, '\\') == NULL &&
           strstr(s, "..") == NULL;
}

static int t_datalog(const lt_ag_env *env, const lt_json *args, lt_buf *out)
{
    const char *want = arg_str(args, "log"), *mode = lt_json_get_str(args, "mode", "summary");
    const lt_json *chj = lt_json_get(args, "channels");
    char *path = NULL, **names = NULL;
    int n, i, pick = -1, c;
    lt_log log;
    lt_json *o, *res = NULL;
    int *sel;
    int nsel = 0;
    if (env->log_dir == NULL) { return fail(out, "no datalogs folder (open a project)"); }
    if (want[0] != '\0') {
        if (!safe_name(want)) { return fail(out, "log must be a plain file name"); }
        path = lt_path_join(env->log_dir, want);
    } else {
        n = lt_plat_list_dir(env->log_dir, &names);
        for (i = 0; i < n; i++) {
            if (lt_str_ends(names[i], ".mlg") || lt_str_ends(names[i], ".csv")) { pick = i; }
        }
        if (pick < 0) {
            if (n > 0) { lt_plat_free_list(names, n); }
            return fail(out, "no datalog entries recorded (start logging first)");
        }
        path = lt_path_join(env->log_dir, names[pick]);
        lt_plat_free_list(names, n);
    }
    lt_log_init(&log);
    if (lt_log_read(path, &log) < 0) {
        int rc = fail(out, "cannot read log: %s", lt_err_get());
        lt_log_free(&log);
        lt_free(path);
        return rc;
    }
    lt_free(path);
    sel = (int *)lt_malloc(((size_t)log.n_ch + 1) * sizeof(int));
    if (chj != NULL && chj->type == LT_JSON_ARR) {
        size_t k;
        for (k = 0; k < chj->n; k++) {
            const lt_json *e = lt_json_at(chj, k);
            int idx = (e != NULL && e->type == LT_JSON_STR) ? lt_log_channel_index(&log, e->str) : -1;
            if (idx >= 0 && nsel < log.n_ch) { sel[nsel++] = idx; }
        }
    } else {
        for (c = 0; c < log.n_ch && nsel < 50; c++) { sel[nsel++] = c; }
    }
    o = lt_json_obj();
    jset_n(o, "rows", (double)log.n);
    jset_n(o, "duration_s", lt_log_duration(&log));
    if (strcmp(mode, "tail") == 0) {
        long from = log.n > 50 ? log.n - 50 : 0, r;
        lt_json *cols = lt_json_arr(), *rows = lt_json_arr();
        if (nsel > 12 && chj == NULL) { nsel = 12; }
        for (c = 0; c < nsel; c++) { lt_json_arr_push(cols, lt_json_str(log.names[sel[c]])); }
        for (r = from; r < log.n; r++) {
            lt_json *row = lt_json_arr();
            lt_json_arr_push(row, lt_json_num(fin(log.t[r])));
            for (c = 0; c < nsel; c++) { lt_json_arr_push(row, lt_json_num(fin(LT_LOG_AT(&log, r, sel[c])))); }
            lt_json_arr_push(rows, row);
        }
        lt_json_obj_set(o, "columns_after_time", cols);
        lt_json_obj_set(o, "rows_data", rows);
    } else {
        res = lt_json_obj();
        for (c = 0; c < nsel; c++) {
            lt_json *s = lt_json_obj();
            double mn = 0.0, mx = 0.0, sum = 0.0, last = 0.0;
            long r;
            for (r = 0; r < log.n; r++) {
                double v = LT_LOG_AT(&log, r, sel[c]);
                if (r == 0 || v < mn) { mn = v; }
                if (r == 0 || v > mx) { mx = v; }
                sum += v;
                last = v;
            }
            jset_n(s, "min", mn); jset_n(s, "max", mx);
            jset_n(s, "mean", log.n > 0 ? sum / (double)log.n : 0.0);
            jset_n(s, "last", last);
            lt_json_obj_set(res, log.names[sel[c]], s);
        }
        lt_json_obj_set(o, "channels", res);
    }
    lt_free(sel);
    lt_log_free(&log);
    return done(out, o);
}

/* ---- propose tools ---- */
static int dangerous_name(const char *n)
{
    static const char *const bad[] = { "pin", "invert", "trigger", "limit", "rev", "crank",
                                       "inj", "spark", "output", "port", "dwell", "mode" };
    char low[64];
    size_t i, k;
    for (i = 0; i < sizeof(low) - 1 && n[i] != 0; i++) {
        low[i] = (n[i] >= 'A' && n[i] <= 'Z') ? (char)(n[i] - 'A' + 'a') : n[i];
    }
    low[i] = '\0';
    for (k = 0; k < LT_COUNTOF(bad); k++) { if (strstr(low, bad[k]) != NULL) { return 1; } }
    return 0;
}

static void authority_for(const lt_tblview *v, const lt_ini *ini, lt_at_authority *a)
{
    double lo = 0.0, hi = 0.0;
    lt_at_authority_default(a);
    lt_tblview_z_range(v, ini, &lo, &hi);
    if (hi > lo) { a->min_cell_value = lo; a->max_cell_value = hi; }
    else { a->min_cell_value = -1e9; a->max_cell_value = 1e9; }
}

static void set_reason(lt_ag_prop *p, const lt_json *args)
{
    lt_strlcpy(p->reason, lt_json_get_str(args, "reason", ""), sizeof(p->reason));
}

static int t_prop_cell(const lt_ag_env *env, const lt_json *args, lt_ag_props *props, lt_buf *out)
{
    lt_tblview v;
    const lt_ini_table *t = NULL;
    lt_ag_prop *p;
    lt_at_authority a;
    double want, cur, got;
    double xi = lt_json_get_num(args, "x_index", -1.0), yi = lt_json_get_num(args, "y_index", -1.0);
    lt_json *o;
    if (lt_json_get(args, "new_value") == NULL) { return fail(out, "new_value is required"); }
    want = lt_json_get_num(args, "new_value", 0.0);
    if (!lt_isfinite(want)) { return fail(out, "new_value must be a finite number"); }
    if (load_view(env, arg_str(args, "table_name"), &v, &t, out) != 0) { return -1; }
    if (xi < 0.0 || yi < 0.0 || xi >= (double)v.cols || yi >= (double)v.rows) {
        int rc = fail(out, "cell (%d, %d) is outside the %lu x %lu table", (int)xi, (int)yi,
                      (unsigned long)v.cols, (unsigned long)v.rows);
        lt_tblview_free(&v);
        return rc;
    }
    cur = lt_tblview_get(&v, (size_t)yi, (size_t)xi);
    authority_for(&v, env->ini, &a);
    got = lt_at_apply_authority_limits(cur, want, &a);
    got = lt_at_clamp_to_rails(&a, got);
    p = props_add(props);
    p->kind = LT_AG_P_CELL;
    lt_strlcpy(p->name, t->name, sizeof(p->name));
    p->x = (size_t)xi; p->y = (size_t)yi;
    p->old_value = cur;
    p->param = got;
    p->valid = 1;
    if (got != want) {
        p->was_clamped = 1;
        p->clamped_from = want;
        lt_snprintf(p->note, sizeof(p->note), "clamped to the authority limits (requested %.3f)", want);
    }
    set_reason(p, args);
    lt_tblview_free(&v);
    o = lt_json_obj();
    jset_s(o, "status", "staged for user approval");
    jset_n(o, "value_that_would_apply", got);
    if (p->was_clamped) { jset_s(o, "note", p->note); }
    return done(out, o);
}

static int t_prop_bulk(const lt_ag_env *env, const lt_json *args, lt_ag_props *props, lt_buf *out)
{
    static const char *const ops[] = { "scale", "offset", "smooth", "interpolate", "set_equal" };
    lt_tblview v;
    const lt_ini_table *t = NULL;
    lt_ag_prop *p;
    const char *op = arg_str(args, "operation");
    const lt_json *cells = lt_json_get(args, "cells");
    size_t i, k, n;
    int known = 0;
    double val = lt_json_get_num(args, "value", lt_json_get_num(args, "factor", 0.0));
    lt_json *o;
    for (k = 0; k < LT_COUNTOF(ops); k++) { if (strcmp(op, ops[k]) == 0) { known = 1; } }
    if (!known) { return fail(out, "operation must be scale, offset, smooth, interpolate or set_equal"); }
    if (cells == NULL || cells->type != LT_JSON_ARR || cells->n == 0 || cells->n > 4096) {
        return fail(out, "cells must be a non-empty array of [x_index, y_index]");
    }
    if (!lt_isfinite(val)) { return fail(out, "value must be finite"); }
    if (strcmp(op, "scale") == 0 && (val <= 0.0 || val > 3.0)) {
        return fail(out, "scale factor must be within (0, 3]");
    }
    if (strcmp(op, "interpolate") == 0 && cells->n < 4) {
        return fail(out, "interpolate needs at least 4 cells");
    }
    if (load_view(env, arg_str(args, "table_name"), &v, &t, out) != 0) { return -1; }
    n = cells->n;
    p = props_add(props);
    p->cells = (size_t *)lt_malloc(2 * n * sizeof(size_t));
    for (i = 0; i < n; i++) {
        const lt_json *c = lt_json_at(cells, i);
        double cx, cy;
        const lt_json *jx = c != NULL ? lt_json_at(c, 0) : NULL, *jy = c != NULL ? lt_json_at(c, 1) : NULL;
        cx = (jx != NULL && jx->type == LT_JSON_NUM) ? jx->num : -1.0;
        cy = (jy != NULL && jy->type == LT_JSON_NUM) ? jy->num : -1.0;
        if (cx < 0.0 || cy < 0.0 || cx >= (double)v.cols || cy >= (double)v.rows) {
            int rc = fail(out, "cell %lu is outside the %lu x %lu table", (unsigned long)i,
                          (unsigned long)v.cols, (unsigned long)v.rows);
            lt_free(p->cells);
            p->cells = NULL;
            props->n--;
            lt_tblview_free(&v);
            return rc;
        }
        p->cells[2 * i] = (size_t)cx;
        p->cells[2 * i + 1] = (size_t)cy;
    }
    p->kind = LT_AG_P_BULK;
    p->n_cells = n;
    lt_strlcpy(p->name, t->name, sizeof(p->name));
    lt_strlcpy(p->op, op, sizeof(p->op));
    p->param = val;
    p->valid = 1;
    lt_strlcpy(p->note, "each cell is clamped to the authority limits when applied", sizeof(p->note));
    set_reason(p, args);
    lt_tblview_free(&v);
    o = lt_json_obj();
    jset_s(o, "status", "staged for user approval");
    return done(out, o);
}

static int t_prop_const(const lt_ag_env *env, const lt_json *args, lt_ag_props *props, lt_buf *out)
{
    const char *name = arg_str(args, "name");
    const lt_ini_const *c = lt_ini_find_const(env->ini, name);
    lt_ag_prop *p;
    double want, cur = 0.0;
    lt_json *o;
    if (c == NULL) { return fail(out, "no constant named '%s'", name); }
    if (lt_json_get(args, "value") == NULL) { return fail(out, "value is required"); }
    want = lt_json_get_num(args, "value", 0.0);
    if (!lt_isfinite(want)) { return fail(out, "value must be a finite number"); }
    if (c->type == LT_DT_STRING || lt_shape_count(&c->shape) > 1) {
        return fail(out, "only scalar and bits constants can be proposed");
    }
    (void)lt_ini_const_get(env->ini, c, &env->src, 0, &cur);
    p = props_add(props);
    p->kind = LT_AG_P_CONST;
    lt_strlcpy(p->name, c->name, sizeof(p->name));
    p->old_value = cur;
    p->param = want;
    p->dangerous = dangerous_name(c->name);
    p->valid = 1;
    if (c->type == LT_DT_BITS) {
        if (want < 0.0 || want != floor(want) || (size_t)want >= c->n_bit_options ||
            lt_ini_bit_option(c, (size_t)want) == NULL) {
            p->valid = 0;
            lt_snprintf(p->note, sizeof(p->note), "%.0f is not one of the options of %s", want, c->name);
        }
    } else if (c->range_resolved && !lt_const_in_range(c, want)) {
        p->valid = 0;
        lt_snprintf(p->note, sizeof(p->note), "outside the declared range %.3f .. %.3f", c->min, c->max);
    }
    if (p->dangerous && p->valid) {
        lt_strlcpy(p->note, "affects pins, triggers or limits: confirm carefully", sizeof(p->note));
    }
    set_reason(p, args);
    o = lt_json_obj();
    if (p->valid) { jset_s(o, "status", "staged for user approval"); }
    else { jset_s(o, "status", "rejected by validation"); jset_s(o, "note", p->note); }
    return done(out, o);
}

int lt_ag_tool_run(const lt_ag_env *env, int tier, const char *name, const lt_json *args,
                   lt_ag_props *props, lt_buf *out)
{
    const lt_ag_tool *t = lt_ag_tool_find(name);
    lt_json *empty = NULL;
    int rc;
    if (t == NULL) { return fail(out, "unknown tool '%s'", name != NULL ? name : ""); }
    if (tier < t->min_tier) {
        return fail(out, "tool '%s' is not permitted at the %s capability tier", name, lt_ag_tier_name(tier));
    }
    if (t->write && props == NULL) {
        return fail(out, "tool '%s' is not available here: this interface is read-only", name);
    }
    if (env == NULL || env->ini == NULL) {
        return fail(out, "no ECU definition is loaded; open a project first");
    }
    if (args == NULL || args->type != LT_JSON_OBJ) { empty = lt_json_obj(); args = empty; }
    if (strcmp(name, "list_tables") == 0) { rc = t_list_tables(env, out); }
    else if (strcmp(name, "read_table") == 0) { rc = t_read_table(env, args, out); }
    else if (strcmp(name, "read_constant") == 0) { rc = t_read_constant(env, args, out); }
    else if (strcmp(name, "list_features") == 0) { rc = t_list_features(env, out); }
    else if (strcmp(name, "tune_health_check") == 0) { rc = t_health(env, args, out); }
    else if (strcmp(name, "get_realtime_snapshot") == 0) { rc = t_realtime(env, out); }
    else if (strcmp(name, "query_datalog") == 0) { rc = t_datalog(env, args, out); }
    else if (strcmp(name, "propose_table_edit") == 0) { rc = t_prop_cell(env, args, props, out); }
    else if (strcmp(name, "propose_bulk_operation") == 0) { rc = t_prop_bulk(env, args, props, out); }
    else if (strcmp(name, "propose_constant_change") == 0) { rc = t_prop_const(env, args, props, out); }
    else { rc = fail(out, "unknown tool '%s'", name); }
    lt_json_free(empty);
    return rc;
}

/* ---- applying accepted proposals ---- */
static int apply_table(lt_ag_prop *p, const lt_ini *ini, lt_pagesrc *dst)
{
    lt_tblview v;
    const lt_ini_table *t = lt_ini_find_table(ini, p->name);
    lt_at_authority a;
    double *old;
    size_t i, n;
    int rc;
    lt_tcell *sel;
    lt_tblview_init(&v);
    if (t == NULL) { return LT_ENOENT; }
    rc = lt_tblview_load_table(&v, ini, t, dst);
    if (rc < 0) { return rc; }
    authority_for(&v, ini, &a);
    if (p->kind == LT_AG_P_CELL) {
        if (p->x >= v.cols || p->y >= v.rows) { lt_tblview_free(&v); return LT_ERANGE; }
        lt_tblview_set(&v, p->y, p->x, p->param);
    } else {
        n = p->n_cells;
        sel = (lt_tcell *)lt_malloc(n * sizeof(lt_tcell));
        old = (double *)lt_malloc(v.rows * v.cols * sizeof(double));
        memcpy(old, v.z, v.rows * v.cols * sizeof(double));
        for (i = 0; i < n; i++) {
            sel[i].x = p->cells[2 * i];
            sel[i].y = p->cells[2 * i + 1];
            if (sel[i].x >= v.cols || sel[i].y >= v.rows) {
                lt_free(sel); lt_free(old); lt_tblview_free(&v);
                return LT_ERANGE;
            }
        }
        if (strcmp(p->op, "scale") == 0) { lt_tblview_scale(&v, sel, n, p->param); }
        else if (strcmp(p->op, "offset") == 0) { lt_tblview_offset(&v, sel, n, p->param); }
        else if (strcmp(p->op, "set_equal") == 0) { lt_tblview_set_equal(&v, sel, n, p->param); }
        else if (strcmp(p->op, "smooth") == 0) { lt_tblview_smooth(&v, sel, n, p->param > 0.0 ? p->param : 1.0); }
        else { lt_tblview_interp2d(&v, sel, n); }
        for (i = 0; i < n; i++) {
            size_t k = sel[i].y * v.cols + sel[i].x;
            double nv = lt_at_apply_authority_limits(old[k], v.z[k], &a);
            v.z[k] = lt_at_clamp_to_rails(&a, nv);
        }
        lt_free(sel);
        lt_free(old);
    }
    rc = lt_tblview_commit(&v, ini, dst);
    lt_tblview_free(&v);
    return rc < 0 ? rc : 0;
}

int lt_ag_props_apply(lt_ag_props *p, const lt_ini *ini, lt_pagesrc *dst)
{
    size_t i;
    int n = 0, rc;
    for (i = 0; i < p->n; i++) {
        lt_ag_prop *q = &p->v[i];
        if (q->state != LT_AG_ACCEPTED || !q->valid || q->applied) { continue; }
        if (q->kind == LT_AG_P_CONST) { rc = lt_ini_set(ini, dst, q->name, q->param); }
        else { rc = apply_table(q, ini, dst); }
        if (rc < 0) { return rc; }
        q->applied = 1;
        n++;
    }
    return n;
}
