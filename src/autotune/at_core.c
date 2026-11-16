/* at_core.c - AutoTune engine: settings, filters, add_data_point (spec 07 sections 1-3).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include "autotune.h"

/* ---------------------------------------------------------------- helpers */

/* Rust `as u64`: truncate toward zero, saturate, NaN/negative -> 0. */
static unsigned long f2u(double v)
{
    if (lt_isnan(v) || v <= 0.0) {
        return 0UL;
    }
    if (v >= 4294967295.0) {
        return 4294967295UL;
    }
    return (unsigned long)v;
}

static unsigned long sat_sub(unsigned long a, unsigned long b)
{
    return a > b ? a - b : 0UL;
}

/* clamp that never misbehaves: NaN value passes through, reversed bounds are ordered. */
static double clampd(double v, double lo, double hi)
{
    double t;
    if (lt_isnan(v)) {
        return v;
    }
    if (lt_isnan(lo) || lt_isnan(hi)) {
        return v;
    }
    if (lo > hi) {
        t = lo;
        lo = hi;
        hi = t;
    }
    return v < lo ? lo : (v > hi ? hi : v);
}

double lt_at_normalise_to_afr(double v)
{
    return v < LT_AT_LAMBDA_THRESHOLD ? v * LT_AT_STOICH_AFR : v;
}

/* ---------------------------------------------------------------- defaults */

void lt_at_settings_default(lt_at_settings *s)
{
    s->target_afr = 14.7;
    s->update_rate_ms = 100UL;
    s->lambda_delay_ms = 0.0;
    s->lambda_delay_flow_scaled = LT_FALSE;
    s->lambda_delay_floor_ms = 120.0;
    s->hit_weighting = LT_HW_UNIFORM;
    s->base_weight = 20.0;
    s->min_change = 1.0;
}

void lt_at_authority_default(lt_at_authority *a)
{
    a->max_cell_value_change = 10.0;
    a->max_cell_percentage_change = 20.0;
    a->min_cell_value = 0.0;
    a->max_cell_value = 255.0;
}

void lt_at_filters_default(lt_at_filters *f)
{
    f->min_rpm = 1000.0;
    f->max_rpm = 7000.0;
    f->min_y_axis = NULL;
    f->max_y_axis = NULL;
    f->min_clt = 160.0;
    f->custom_filter = NULL;
    f->max_tps_rate = 50.0;
    f->exclude_accel_enrich = LT_TRUE;
    f->min_steady_ms = 0UL;
}

void lt_at_filters_clear(lt_at_filters *f)
{
    if (f->min_y_axis) {
        lt_free(f->min_y_axis);
    }
    if (f->max_y_axis) {
        lt_free(f->max_y_axis);
    }
    if (f->custom_filter) {
        lt_free(f->custom_filter);
    }
    f->min_y_axis = NULL;
    f->max_y_axis = NULL;
    f->custom_filter = NULL;
}

static void set_str(char **dst, const char *src)
{
    char *n = src ? lt_strdup(src) : NULL;
    if (*dst) {
        lt_free(*dst);
    }
    *dst = n;
}

void lt_at_filters_set_y_range(lt_at_filters *f, const char *min_y, const char *max_y)
{
    set_str(&f->min_y_axis, min_y);
    set_str(&f->max_y_axis, max_y);
}

void lt_at_filters_set_custom(lt_at_filters *f, const char *expr)
{
    set_str(&f->custom_filter, expr);
}

void lt_at_filters_adopt_ini(lt_at_filters *f, const lt_ini *ini)
{
    size_t i;
    if (!ini || f->min_clt != 160.0) {
        return;
    }
    for (i = 0; i < ini->ve_analyze.n_filters; i++) {
        const lt_ini_filter *fl = &ini->ve_analyze.filters[i];
        if (fl->name && strcmp(fl->name, "minCltFilter") == 0) {
            f->min_clt = fl->dflt;
            return;
        }
    }
}

void lt_ve_point_default(lt_ve_point *p)
{
    memset(p, 0, sizeof *p);
    p->afr = 14.7;
    p->accel_enrich_active = -1;
    p->fuel_cut_active = -1;
}

/* ---------------------------------------------------------------- authority */

double lt_at_clamp_to_rails(const lt_at_authority *a, double v)
{
    double lo = LT_MIN(a->min_cell_value, a->max_cell_value);
    double hi = LT_MAX(a->min_cell_value, a->max_cell_value);
    return clampd(v, lo, hi);
}

double lt_at_apply_authority_limits(double beginning, double recommended, const lt_at_authority *a)
{
    double delta = recommended - beginning;
    double max_abs = fabs(a->max_cell_value_change);
    double cd, max_pct, fd;
    cd = lt_isfinite(max_abs) ? clampd(delta, -max_abs, max_abs) : delta;
    max_pct = fabs(beginning * (a->max_cell_percentage_change / 100.0));
    fd = lt_isfinite(max_pct) ? clampd(cd, -max_pct, max_pct) : cd;
    return lt_at_clamp_to_rails(a, beginning + fd);
}

/* ---------------------------------------------------------------- bins */

static lt_bool within_axis(double v, const double *bins, size_t n)
{
    double lo, lo_next, hi, hi_prev, lo_margin, hi_margin;
    if (n == 0) {
        return LT_FALSE;
    }
    if (n < 2) {
        return LT_TRUE;
    }
    if (bins[0] <= bins[n - 1]) {
        lo = bins[0];
        lo_next = bins[1];
        hi = bins[n - 1];
        hi_prev = bins[n - 2];
    } else {
        lo = bins[n - 1];
        lo_next = bins[n - 2];
        hi = bins[0];
        hi_prev = bins[1];
    }
    lo_margin = fabs(lo_next - lo) / 2.0;
    hi_margin = fabs(hi - hi_prev) / 2.0;
    return (v >= lo - lo_margin && v <= hi + hi_margin) ? LT_TRUE : LT_FALSE;
}

int lt_at_find_bin(double v, const double *bins, size_t n)
{
    size_t i, best;
    double bd, d;
    if (n == 0 || !bins) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (fabs(bins[i] - v) < 0.1) {
            return (int)i;
        }
    }
    if (!within_axis(v, bins, n)) {
        return -1;
    }
    best = 0;
    bd = fabs(bins[0] - v);
    for (i = 1; i < n; i++) {
        d = fabs(bins[i] - v);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return (int)best;
}

/* ---------------------------------------------------------------- weights */

static double axis_weight(double value, size_t idx, const double *bins, size_t n)
{
    double centre, d, neighbour, span, w;
    if (idx >= n) {
        return 1.0;
    }
    centre = bins[idx];
    d = value - centre;
    if (fabs(d) < DBL_EPSILON) {
        return 1.0;
    }
    if (d > 0.0) {
        if (idx + 1 >= n) {
            return 1.0;
        }
        neighbour = bins[idx + 1];
    } else {
        if (idx < 1) {
            return 1.0;
        }
        neighbour = bins[idx - 1];
    }
    span = fabs(neighbour - centre);
    if (span < DBL_EPSILON) {
        return 1.0;
    }
    w = 1.0 - fabs(d) / span;
    return clampd(w, 0.0, 1.0);
}

double lt_at_hit_weight(lt_hit_weighting mode, double rpm, double load, size_t cx, size_t cy,
                        const double *xbins, size_t nx, const double *ybins, size_t ny)
{
    double ax, ay, p;
    if (mode == LT_HW_UNIFORM) {
        return 1.0;
    }
    ax = axis_weight(rpm, cx, xbins, nx);
    ay = axis_weight(load, cy, ybins, ny);
    switch (mode) {
    case LT_HW_CELL_PROXIMITY:
        return ax * ay;
    case LT_HW_CELL_PROXIMITY_SQUARED:
        p = ax * ay;
        return p * p;
    case LT_HW_CELL_CENTRE_ONLY:
        return (ax >= LT_AT_CENTRE_ONLY_MIN_WEIGHT && ay >= LT_AT_CENTRE_ONLY_MIN_WEIGHT) ? 1.0
                                                                                           : 0.0;
    default:
        return 1.0;
    }
}

/* ---------------------------------------------------------------- delay curve */

unsigned long lt_at_default_delay_ms(double rpm)
{
    double r = clampd(rpm, 800.0, 6000.0);
    double ratio = (r - 800.0) / 5200.0;
    return f2u(200.0 - ratio * 150.0);
}

static unsigned long configured_or_curve_delay(const lt_at_settings *s, double rpm)
{
    if (s->lambda_delay_ms > 0.0) {
        return f2u(s->lambda_delay_ms);
    }
    return lt_at_default_delay_ms(rpm);
}

void lt_at_flow_scaled_delay_table(const double *ve, const double *xbins, size_t nx,
                                   const double *ybins, size_t ny, double idle_delay_ms,
                                   double floor_ms, double *out)
{
    size_t i, j, ax = 0, ay = 0;
    double bd, d, anchor_ve, anchor_flow, idle, k, v, flow, ar, al;
    if (nx == 0 || ny == 0) {
        return;
    }
    bd = fabs(xbins[0] - 800.0);
    for (i = 1; i < nx; i++) {
        d = fabs(xbins[i] - 800.0);
        if (d < bd) {
            bd = d;
            ax = i;
        }
    }
    bd = fabs(ybins[0] - 40.0);
    for (j = 1; j < ny; j++) {
        d = fabs(ybins[j] - 40.0);
        if (d < bd) {
            bd = d;
            ay = j;
        }
    }
    anchor_ve = ve ? ve[ay * nx + ax] : 50.0;
    ar = LT_MAX(xbins[ax], 1.0);
    al = LT_MAX(ybins[ay], 1.0);
    anchor_flow = ar * al * LT_MAX(anchor_ve, 1.0);
    idle = LT_MAX(idle_delay_ms, floor_ms);
    k = (idle - floor_ms) * anchor_flow;
    for (j = 0; j < ny; j++) {
        for (i = 0; i < nx; i++) {
            v = ve ? ve[j * nx + i] : anchor_ve;
            flow = LT_MAX(xbins[i], 1.0) * LT_MAX(ybins[j], 1.0) * LT_MAX(v, 1.0);
            out[j * nx + i] = clampd(floor_ms + k / flow, floor_ms, idle);
        }
    }
}

/* ---------------------------------------------------------------- state */

typedef struct at_rec {
    lt_at_rec r;
    double raw_cma;
} at_rec;

typedef struct at_reason {
    const char *reason;
    unsigned long count;
} at_reason;

struct lt_at_state {
    lt_bool running;
    char *table_name;
    lt_vec locked;         /* unsigned[2] pairs, unique */
    lt_vec recs;           /* at_rec */
    lt_ve_point *buf;      /* ring-ish buffer: live range [head, head+cnt) */
    size_t buf_head, buf_cnt, buf_cap;
    unsigned long buffer_max_age_ms;
    size_t ref_nx, ref_ny;
    double *ref_delay, *ref_target, *ref_ve;
    lt_bool strict;
    unsigned long total_samples;
    lt_vec reasons;        /* at_reason */
};

lt_at_state *lt_at_new(void)
{
    lt_at_state *s = (lt_at_state *)lt_calloc(1, sizeof *s);
    lt_vec_init(&s->locked, 2 * sizeof(unsigned));
    lt_vec_init(&s->recs, sizeof(at_rec));
    lt_vec_init(&s->reasons, sizeof(at_reason));
    s->buffer_max_age_ms = 500UL;
    s->strict = LT_TRUE;
    return s;
}

static void free_ref(lt_at_state *s)
{
    if (s->ref_delay) {
        lt_free(s->ref_delay);
    }
    if (s->ref_target) {
        lt_free(s->ref_target);
    }
    if (s->ref_ve) {
        lt_free(s->ref_ve);
    }
    s->ref_delay = s->ref_target = s->ref_ve = NULL;
    s->ref_nx = s->ref_ny = 0;
}

void lt_at_free(lt_at_state *s)
{
    if (!s) {
        return;
    }
    free_ref(s);
    if (s->table_name) {
        lt_free(s->table_name);
    }
    if (s->buf) {
        lt_free(s->buf);
    }
    lt_vec_free(&s->locked);
    lt_vec_free(&s->recs);
    lt_vec_free(&s->reasons);
    lt_free(s);
}

void lt_at_start(lt_at_state *s)
{
    s->running = LT_TRUE;
    lt_vec_clear(&s->recs);
    lt_vec_clear(&s->reasons);
    s->buf_head = s->buf_cnt = 0;
    s->total_samples = 0UL;
}

void lt_at_stop(lt_at_state *s)
{
    s->running = LT_FALSE;
}

lt_bool lt_at_is_running(const lt_at_state *s)
{
    return s->running;
}

void lt_at_set_table_name(lt_at_state *s, const char *name)
{
    set_str(&s->table_name, name);
}

const char *lt_at_table_name(const lt_at_state *s)
{
    return s->table_name;
}

static double *copy_grid(const double *src, size_t n)
{
    double *d;
    if (!src || n == 0) {
        return NULL;
    }
    d = (double *)lt_malloc(n * sizeof(double));
    memcpy(d, src, n * sizeof(double));
    return d;
}

void lt_at_set_reference_tables(lt_at_state *s, const lt_at_reftables *r)
{
    size_t n;
    free_ref(s);
    if (!r) {
        return;
    }
    n = r->nx * r->ny;
    s->ref_nx = r->nx;
    s->ref_ny = r->ny;
    s->ref_delay = copy_grid(r->lambda_delay, n);
    s->ref_target = copy_grid(r->target_afr, n);
    s->ref_ve = copy_grid(r->ve, n);
}

void lt_at_set_strict_lambda_match(lt_at_state *s, lt_bool on)
{
    s->strict = on ? LT_TRUE : LT_FALSE;
}

static long find_lock(const lt_at_state *s, unsigned x, unsigned y)
{
    size_t i;
    for (i = 0; i < s->locked.len; i++) {
        const unsigned *p = (const unsigned *)lt_vec_at(&s->locked, i);
        if (p[0] == x && p[1] == y) {
            return (long)i;
        }
    }
    return -1;
}

lt_bool lt_at_is_cell_locked(const lt_at_state *s, unsigned x, unsigned y)
{
    return find_lock(s, x, y) >= 0 ? LT_TRUE : LT_FALSE;
}

void lt_at_lock_cell(lt_at_state *s, unsigned x, unsigned y)
{
    unsigned *p;
    if (find_lock(s, x, y) >= 0) {
        return;
    }
    p = (unsigned *)lt_vec_push(&s->locked);
    p[0] = x;
    p[1] = y;
}

void lt_at_unlock_cell(lt_at_state *s, unsigned x, unsigned y)
{
    long i = find_lock(s, x, y);
    if (i >= 0) {
        lt_vec_remove(&s->locked, (size_t)i);
    }
}

unsigned long lt_at_total_samples(const lt_at_state *s)
{
    return s->total_samples;
}

/* ---------------------------------------------------------------- buffer */

static void buf_push(lt_at_state *s, const lt_ve_point *p)
{
    if (s->buf_head + s->buf_cnt >= s->buf_cap) {
        if (s->buf_head > 0 && s->buf_head >= s->buf_cap / 2) {
            memmove(s->buf, s->buf + s->buf_head, s->buf_cnt * sizeof(lt_ve_point));
            s->buf_head = 0;
        } else {
            s->buf_cap = s->buf_cap ? s->buf_cap * 2 : 64;
            s->buf = (lt_ve_point *)lt_realloc(s->buf, s->buf_cap * sizeof(lt_ve_point));
        }
    }
    s->buf[s->buf_head + s->buf_cnt] = *p;
    s->buf_cnt++;
}

static void buf_prune(lt_at_state *s, unsigned long now)
{
    unsigned long cutoff = sat_sub(now, s->buffer_max_age_ms);
    while (s->buf_cnt > 0 && s->buf[s->buf_head].timestamp_ms < cutoff) {
        s->buf_head++;
        s->buf_cnt--;
    }
}

unsigned long lt_at_required_buffer_ms(const lt_at_state *s, const lt_at_settings *set,
                                       const lt_at_filters *f)
{
    double table_max = 0.0, configured;
    unsigned long for_delay, steady;
    size_t i, n;
    n = s->ref_nx * s->ref_ny;
    if (s->ref_delay) {
        for (i = 0; i < n; i++) {
            if (s->ref_delay[i] > table_max) {
                table_max = s->ref_delay[i];
            }
        }
    }
    configured = LT_MAX(LT_MAX(set->lambda_delay_ms, 0.0), table_max);
    if (configured <= 0.0) {
        for_delay = 500UL;
    } else {
        for_delay = f2u(configured) + 500UL;
        if (for_delay < 500UL) {
            for_delay = 500UL;
        }
    }
    steady = f->min_steady_ms + 500UL;
    return for_delay > steady ? for_delay : steady;
}

static lt_bool is_steady(const lt_at_state *s, const lt_ve_point *pt, const lt_at_filters *f)
{
    unsigned long window_start;
    size_t i;
    if (f->min_steady_ms == 0UL) {
        return LT_TRUE;
    }
    window_start = sat_sub(pt->timestamp_ms, f->min_steady_ms);
    for (i = s->buf_cnt; i > 0; i--) {
        const lt_ve_point *p = &s->buf[s->buf_head + i - 1];
        if (p->timestamp_ms < window_start) {
            return LT_TRUE;   /* reached back far enough */
        }
        if (fabs(p->rpm - pt->rpm) > LT_AT_STEADY_RPM_TOLERANCE ||
            fabs(p->load - pt->load) > LT_AT_STEADY_LOAD_TOLERANCE || p->fuel_cut_active == 1) {
            return LT_FALSE;
        }
    }
    return LT_FALSE;
}

/* ---------------------------------------------------------------- custom filter */

typedef struct cf {
    const char *p;
    const lt_ve_point *pt;
    int err;
} cf;

static void cf_ws(cf *c)
{
    while (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r') {
        c->p++;
    }
}

static int cf_match(cf *c, const char *tok)
{
    size_t n = strlen(tok);
    cf_ws(c);
    if (strncmp(c->p, tok, n) == 0) {
        c->p += n;
        return 1;
    }
    return 0;
}

static double cf_or(cf *c);

static double cf_ident(cf *c, const char *name)
{
    const lt_ve_point *p = c->pt;
    if (strcmp(name, "true") == 0) return 1.0;
    if (strcmp(name, "false") == 0) return 0.0;
    if (strcmp(name, "rpm") == 0) return p->rpm;
    if (strcmp(name, "map") == 0) return p->map;
    if (strcmp(name, "maf") == 0) return p->maf;
    if (strcmp(name, "load") == 0) return p->load;
    if (strcmp(name, "afr") == 0) return p->afr;
    if (strcmp(name, "ve") == 0) return p->ve;
    if (strcmp(name, "clt") == 0) return p->clt;
    if (strcmp(name, "tps") == 0) return p->tps;
    if (strcmp(name, "tps_rate") == 0) return p->tps_rate;
    if (strcmp(name, "accel_enrich") == 0 || strcmp(name, "accel_enrich_active") == 0) {
        return p->accel_enrich_active == 1 ? 1.0 : 0.0;
    }
    c->err = 1;
    return 0.0;
}

static double cf_primary(cf *c)
{
    double v;
    char name[40];
    size_t n;
    char *end;
    cf_ws(c);
    if (c->err) {
        return 0.0;
    }
    if (*c->p == '(') {
        c->p++;
        v = cf_or(c);
        if (!cf_match(c, ")")) {
            c->err = 1;
        }
        return v;
    }
    if ((*c->p >= '0' && *c->p <= '9') || *c->p == '.') {
        v = strtod(c->p, &end);
        if (end == c->p) {
            c->err = 1;
            return 0.0;
        }
        c->p = end;
        return v;
    }
    if ((*c->p >= 'a' && *c->p <= 'z') || (*c->p >= 'A' && *c->p <= 'Z') || *c->p == '_') {
        n = 0;
        while ((*c->p >= 'a' && *c->p <= 'z') || (*c->p >= 'A' && *c->p <= 'Z') ||
               (*c->p >= '0' && *c->p <= '9') || *c->p == '_') {
            if (n < sizeof name - 1) {
                name[n++] = *c->p;
            }
            c->p++;
        }
        name[n] = '\0';
        return cf_ident(c, name);
    }
    c->err = 1;
    return 0.0;
}

static double cf_unary(cf *c)
{
    cf_ws(c);
    if (*c->p == '!' && c->p[1] != '=') {
        c->p++;
        return cf_unary(c) == 0.0 ? 1.0 : 0.0;
    }
    if (*c->p == '-') {
        c->p++;
        return -cf_unary(c);
    }
    if (*c->p == '+') {
        c->p++;
        return cf_unary(c);
    }
    return cf_primary(c);
}

static double cf_mul(cf *c)
{
    double a = cf_unary(c), b;
    for (;;) {
        cf_ws(c);
        if (*c->p == '*') {
            c->p++;
            a *= cf_unary(c);
        } else if (*c->p == '/') {
            c->p++;
            b = cf_unary(c);
            if (b == 0.0) {
                c->err = 1;
                return 0.0;
            }
            a /= b;
        } else if (*c->p == '%') {
            c->p++;
            b = cf_unary(c);
            if (b == 0.0) {
                c->err = 1;
                return 0.0;
            }
            a = fmod(a, b);
        } else {
            return a;
        }
    }
}

static double cf_add(cf *c)
{
    double a = cf_mul(c);
    for (;;) {
        cf_ws(c);
        if (*c->p == '+') {
            c->p++;
            a += cf_mul(c);
        } else if (*c->p == '-') {
            c->p++;
            a -= cf_mul(c);
        } else {
            return a;
        }
    }
}

static double cf_cmp(cf *c)
{
    double a = cf_add(c), b;
    for (;;) {
        if (cf_match(c, "<=")) {
            b = cf_add(c);
            a = a <= b ? 1.0 : 0.0;
        } else if (cf_match(c, ">=")) {
            b = cf_add(c);
            a = a >= b ? 1.0 : 0.0;
        } else if (cf_match(c, "==")) {
            b = cf_add(c);
            a = a == b ? 1.0 : 0.0;
        } else if (cf_match(c, "!=")) {
            b = cf_add(c);
            a = a != b ? 1.0 : 0.0;
        } else if (cf_match(c, "<")) {
            b = cf_add(c);
            a = a < b ? 1.0 : 0.0;
        } else if (cf_match(c, ">")) {
            b = cf_add(c);
            a = a > b ? 1.0 : 0.0;
        } else {
            return a;
        }
    }
}

static double cf_and(cf *c)
{
    double a = cf_cmp(c), b;
    while (cf_match(c, "&&")) {
        b = cf_cmp(c);
        a = (a != 0.0 && b != 0.0) ? 1.0 : 0.0;
    }
    return a;
}

static double cf_or(cf *c)
{
    double a = cf_and(c), b;
    while (cf_match(c, "||")) {
        b = cf_and(c);
        a = (a != 0.0 || b != 0.0) ? 1.0 : 0.0;
    }
    return a;
}

/* LT_TRUE if the expression accepts the sample; parse/eval errors reject. */
static lt_bool custom_accepts(const char *expr, const lt_ve_point *pt)
{
    cf c;
    double v;
    c.p = expr;
    c.pt = pt;
    c.err = 0;
    v = cf_or(&c);
    cf_ws(&c);
    if (c.err || *c.p != '\0') {
        return LT_FALSE;
    }
    return v != 0.0 ? LT_TRUE : LT_FALSE;
}

/* ---------------------------------------------------------------- classify */

static const char *filter_reason(const lt_ve_point *pt, const lt_at_filters *f)
{
    double bound;
    if (pt->rpm < f->min_rpm || pt->rpm > f->max_rpm) {
        return "rpm out of range";
    }
    if (pt->clt < f->min_clt) {
        return "clt below min_clt";
    }
    if (f->min_y_axis && lt_parse_double(f->min_y_axis, &bound) && pt->load < bound) {
        return "load below min_y_axis";
    }
    if (f->max_y_axis && lt_parse_double(f->max_y_axis, &bound) && pt->load > bound) {
        return "load above max_y_axis";
    }
    if (pt->fuel_cut_active == 1) {
        return "overrun fuel cut";
    }
    if (!(pt->afr >= LT_AT_AFR_RAIL_LOW && pt->afr <= LT_AT_AFR_RAIL_HIGH)) {
        return "afr at sensor rail";
    }
    if (fabs(pt->tps_rate) > f->max_tps_rate) {
        return "tps_rate above max_tps_rate";
    }
    if (f->exclude_accel_enrich && pt->accel_enrich_active == 1) {
        return "accel enrichment active";
    }
    if (f->custom_filter) {
        char tmp[512];
        lt_strlcpy(tmp, f->custom_filter, sizeof tmp);
        if (lt_str_trim(tmp)[0] != '\0' && !custom_accepts(f->custom_filter, pt)) {
            return "custom_filter";
        }
    }
    return NULL;
}

int lt_at_classify(const lt_at_state *s, const lt_ve_point *pt, const lt_at_filters *f,
                   const char **reason)
{
    const char *r;
    if (!is_steady(s, pt, f)) {
        r = "rpm/load not steady";
    } else {
        r = filter_reason(pt, f);
    }
    if (reason) {
        *reason = r;
    }
    return r ? 1 : 0;
}

static void count_reason(lt_at_state *s, const char *reason)
{
    size_t i;
    at_reason *a;
    for (i = 0; i < s->reasons.len; i++) {
        a = (at_reason *)lt_vec_at(&s->reasons, i);
        if (strcmp(a->reason, reason) == 0) {
            a->count++;
            return;
        }
    }
    a = (at_reason *)lt_vec_push(&s->reasons);
    a->reason = reason;
    a->count = 1UL;
}

/* ---------------------------------------------------------------- add_data_point */

static const lt_ve_point *find_delayed(const lt_at_state *s, unsigned long now,
                                       unsigned long delay)
{
    unsigned long target = sat_sub(now, delay), best_diff = 0, diff, gap = 0, tol;
    const lt_ve_point *best = NULL, *p;
    size_t i;
    if (s->buf_cnt == 0) {
        return NULL;
    }
    for (i = 0; i < s->buf_cnt; i++) {
        p = &s->buf[s->buf_head + i];
        diff = p->timestamp_ms > target ? p->timestamp_ms - target : target - p->timestamp_ms;
        if (!best || diff < best_diff) {
            best = p;
            best_diff = diff;
        }
    }
    if (s->buf_cnt >= 2) {
        unsigned long first = s->buf[s->buf_head].timestamp_ms;
        unsigned long last = s->buf[s->buf_head + s->buf_cnt - 1].timestamp_ms;
        if (last >= first) {
            gap = (last - first) / (unsigned long)(s->buf_cnt - 1);
        }
    }
    tol = gap ? LT_MAX(gap * 6UL / 10UL, 50UL) : 50UL;
    return best_diff <= tol ? best : NULL;
}

static at_rec *find_rec(const lt_at_state *s, unsigned x, unsigned y)
{
    size_t i;
    for (i = 0; i < s->recs.len; i++) {
        at_rec *r = (at_rec *)lt_vec_at(&s->recs, i);
        if (r->r.cell_x == x && r->r.cell_y == y) {
            return r;
        }
    }
    return NULL;
}

void lt_at_add_data_point(lt_at_state *s, const lt_ve_point *pt, const double *xbins, size_t nx,
                          const double *ybins, size_t ny, const lt_at_settings *set,
                          const lt_at_filters *f, const lt_at_authority *auth)
{
    const char *reason = NULL;
    int cx, cy, xi, yi;
    unsigned long delay;
    const lt_ve_point *hist;
    lt_ve_point cur;
    double cell_ve, target, required, w, w_total, confidence, ramped, clamped, v;
    at_rec *rec;
    size_t idx;

    if (!s->running) {
        return;
    }
    s->buffer_max_age_ms = lt_at_required_buffer_ms(s, set, f);
    buf_push(s, pt);
    buf_prune(s, pt->timestamp_ms);

    if (lt_at_classify(s, pt, f, &reason)) {
        count_reason(s, reason);
        return;
    }
    s->total_samples++;

    cx = lt_at_find_bin(pt->rpm, xbins, nx);
    cy = lt_at_find_bin(pt->load, ybins, ny);
    delay = 0;
    {
        lt_bool have = LT_FALSE;
        if (cx >= 0 && cy >= 0 && s->ref_delay && (size_t)cx < s->ref_nx && (size_t)cy < s->ref_ny) {
            v = s->ref_delay[(size_t)cy * s->ref_nx + (size_t)cx];
            if (v > 0.1) {
                delay = f2u(v);
                have = LT_TRUE;
            }
        }
        if (!have) {
            delay = configured_or_curve_delay(set, pt->rpm);
        }
    }
    hist = NULL;
    if (delay > 0 && pt->timestamp_ms > delay) {
        hist = find_delayed(s, pt->timestamp_ms, delay);
    }
    if (!hist) {
        if (s->strict) {
            return;
        }
        cur = *pt;
        hist = &cur;
    }
    xi = lt_at_find_bin(hist->rpm, xbins, nx);
    yi = lt_at_find_bin(hist->load, ybins, ny);
    if (xi < 0 || yi < 0) {
        return;
    }
    cell_ve = hist->ve;
    if (s->ref_ve && (size_t)xi < s->ref_nx && (size_t)yi < s->ref_ny) {
        v = s->ref_ve[(size_t)yi * s->ref_nx + (size_t)xi];
        if (v > 0.1) {
            cell_ve = v;
        }
    }
    if (lt_at_is_cell_locked(s, (unsigned)xi, (unsigned)yi)) {
        return;
    }
    target = set->target_afr;
    if (s->ref_target && (size_t)xi < s->ref_nx && (size_t)yi < s->ref_ny) {
        v = s->ref_target[(size_t)yi * s->ref_nx + (size_t)xi];
        if (v > 0.1) {
            target = lt_at_normalise_to_afr(v);
        }
    }
    if (pt->afr < 0.1 || target < 0.1) {
        required = cell_ve;
    } else {
        required = cell_ve * (pt->afr / target);
    }
    rec = find_rec(s, (unsigned)xi, (unsigned)yi);
    if (!rec) {
        rec = (at_rec *)lt_vec_push(&s->recs);
        rec->r.cell_x = (unsigned)xi;
        rec->r.cell_y = (unsigned)yi;
        rec->r.beginning_value = cell_ve;
        rec->r.recommended_value = cell_ve;
        rec->r.hit_count = 0;
        rec->r.hit_weighting = 0.0;
        rec->r.target_afr = target;
        rec->r.hit_percentage = 0.0;
        rec->raw_cma = cell_ve;
    }
    rec->r.hit_count++;
    w = lt_at_hit_weight(set->hit_weighting, hist->rpm, hist->load, (size_t)xi, (size_t)yi, xbins,
                         nx, ybins, ny);
    rec->r.hit_weighting += w;
    w_total = LT_MAX(rec->r.hit_weighting, DBL_MIN);
    rec->raw_cma += (required - rec->raw_cma) * (w / w_total);
    confidence = set->base_weight > 0.0 ? LT_MIN(rec->r.hit_weighting / set->base_weight, 1.0)
                                        : 1.0;
    ramped = rec->r.beginning_value + (rec->raw_cma - rec->r.beginning_value) * confidence;
    clamped = lt_at_apply_authority_limits(rec->r.beginning_value, ramped, auth);
    if (fabs(clamped - rec->r.beginning_value) < set->min_change) {
        clamped = rec->r.beginning_value;
    }
    rec->r.recommended_value = clamped;
    rec->r.target_afr = target;
    idx = s->total_samples;
    rec->r.hit_percentage = idx > 0 ? (double)rec->r.hit_count / (double)idx * 100.0 : 0.0;
}

/* ---------------------------------------------------------------- outputs */

static int cmp_rec(const void *a, const void *b)
{
    const lt_at_rec *x = (const lt_at_rec *)a, *y = (const lt_at_rec *)b;
    if (x->cell_y != y->cell_y) {
        return x->cell_y < y->cell_y ? -1 : 1;
    }
    if (x->cell_x != y->cell_x) {
        return x->cell_x < y->cell_x ? -1 : 1;
    }
    return 0;
}

size_t lt_at_recommendations(const lt_at_state *s, lt_at_rec *out, size_t cap)
{
    size_t n = s->recs.len, i, m;
    lt_at_rec *tmp;
    if (!out) {
        return n;
    }
    if (n == 0) {
        return 0;
    }
    tmp = (lt_at_rec *)lt_malloc(n * sizeof *tmp);
    for (i = 0; i < n; i++) {
        tmp[i] = ((const at_rec *)lt_vec_at(&s->recs, i))->r;
    }
    qsort(tmp, n, sizeof *tmp, cmp_rec);
    m = LT_MIN(n, cap);
    memcpy(out, tmp, m * sizeof *tmp);
    lt_free(tmp);
    return n;
}

static int cmp_reject(const void *a, const void *b)
{
    const lt_at_reject *x = (const lt_at_reject *)a, *y = (const lt_at_reject *)b;
    if (x->count != y->count) {
        return x->count > y->count ? -1 : 1;
    }
    return strcmp(x->reason, y->reason);
}

size_t lt_at_rejections(const lt_at_state *s, lt_at_reject *out, size_t cap)
{
    size_t n = s->reasons.len, i, m;
    lt_at_reject *tmp;
    if (!out) {
        return n;
    }
    if (n == 0) {
        return 0;
    }
    tmp = (lt_at_reject *)lt_malloc(n * sizeof *tmp);
    for (i = 0; i < n; i++) {
        const at_reason *r = (const at_reason *)lt_vec_at(&s->reasons, i);
        tmp[i].reason = r->reason;
        tmp[i].count = r->count;
    }
    qsort(tmp, n, sizeof *tmp, cmp_reject);
    m = LT_MIN(n, cap);
    memcpy(out, tmp, m * sizeof *tmp);
    lt_free(tmp);
    return n;
}

/* Test/diagnostic helpers (additive; declared in autotune.h). */
size_t lt_at_buffer_count(const lt_at_state *s)
{
    return s->buf_cnt;
}

unsigned long lt_at_buffer_oldest_ms(const lt_at_state *s)
{
    return s->buf_cnt ? s->buf[s->buf_head].timestamp_ms : 0UL;
}

unsigned long lt_at_buffer_max_age_ms(const lt_at_state *s)
{
    return s->buffer_max_age_ms;
}

unsigned long lt_at_mean_sample_gap_ms(const lt_at_state *s)
{
    unsigned long first, last;
    if (s->buf_cnt < 2) {
        return 0UL;
    }
    first = s->buf[s->buf_head].timestamp_ms;
    last = s->buf[s->buf_head + s->buf_cnt - 1].timestamp_ms;
    if (last < first) {
        return 0UL;
    }
    return (last - first) / (unsigned long)(s->buf_cnt - 1);
}

lt_bool lt_at_find_delayed(const lt_at_state *s, unsigned long now_ms, unsigned long delay_ms,
                           lt_ve_point *out)
{
    const lt_ve_point *p = find_delayed(s, now_ms, delay_ms);
    if (!p) {
        return LT_FALSE;
    }
    if (out) {
        *out = *p;
    }
    return LT_TRUE;
}

unsigned long lt_at_configured_or_curve_delay_ms(const lt_at_settings *set, double rpm)
{
    return configured_or_curve_delay(set, rpm);
}

void lt_at_push_sample(lt_at_state *s, const lt_ve_point *pt)
{
    buf_push(s, pt);
    buf_prune(s, pt->timestamp_ms);
}

void lt_at_set_buffer_max_age_ms(lt_at_state *s, unsigned long ms)
{
    s->buffer_max_age_ms = ms;
}
