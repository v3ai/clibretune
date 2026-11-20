/* at_wue.c - WUE Analyze engine (see at_wue.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "at_wue.h"

#define WUE_REASONS 16
#define WUE_RATE_WINDOW_MS 3000UL
#define WUE_NEUTRAL_TOL 0.005

struct lt_wue_state {
    lt_at_state *buf;               /* VE engine used as the history / transport-delay buffer */
    size_t n;
    double clt[LT_WUE_MAX_BINS], cur[LT_WUE_MAX_BINS];
    unsigned char lock[LT_WUE_MAX_BINS];
    double offset, unity;
    double offx[LT_WUE_MAX_BINS], offv[LT_WUE_MAX_BINS];
    size_t noff;
    double *tx, *ty, *tz;
    size_t tnx, tny;
    double wsum[LT_WUE_MAX_BINS], fsum[LT_WUE_MAX_BINS], asum[LT_WUE_MAX_BINS], tsum[LT_WUE_MAX_BINS];
    unsigned hits[LT_WUE_MAX_BINS];
    double warm_w, warm_f;
    unsigned long warm_n, accepted;
    char rname[WUE_REASONS][48];
    unsigned long rcount[WUE_REASONS];
    size_t nrej;
    lt_bool running, started;       /* started: an engine start was seen in this session */
    int engine;                     /* 0 unknown, 1 stopped/cranking, 2 running */
    unsigned long run_start_ms;
};

/* ================================================================ lookups */

double lt_at_curve_lookup(const double *x, const double *y, size_t n, double v)
{
    size_t i;
    double t;
    if (n == 0 || x == NULL || y == NULL) { return 0.0; }
    if (n == 1 || v <= x[0]) { return y[0]; }
    if (v >= x[n - 1]) { return y[n - 1]; }
    for (i = 0; i + 1 < n; i++) {
        if (v >= x[i] && v <= x[i + 1]) {
            t = x[i + 1] > x[i] ? (v - x[i]) / (x[i + 1] - x[i]) : 0.0;
            return y[i] + (y[i + 1] - y[i]) * t;
        }
    }
    return y[n - 1];
}

/* index i and fraction t so that v sits between bins i and i+1 (clamped) */
static void axis_pos(const double *b, size_t n, double v, size_t *i, double *t)
{
    size_t k;
    *i = 0;
    *t = 0.0;
    if (n < 2 || v <= b[0]) { return; }
    if (v >= b[n - 1]) { *i = n - 2; *t = 1.0; return; }
    for (k = 0; k + 1 < n; k++) {
        if (v >= b[k] && v <= b[k + 1]) {
            *i = k;
            *t = b[k + 1] > b[k] ? (v - b[k]) / (b[k + 1] - b[k]) : 0.0;
            return;
        }
    }
}

double lt_at_table_lookup(const double *x, size_t nx, const double *y, size_t ny, const double *z,
                          double px, double py)
{
    size_t ix, iy, ix1, iy1;
    double tx, ty, a, b;
    if (z == NULL || nx == 0 || ny == 0) { return 0.0; }
    axis_pos(x, nx, px, &ix, &tx);
    axis_pos(y, ny, py, &iy, &ty);
    ix1 = nx > 1 ? ix + 1 : ix;
    iy1 = ny > 1 ? iy + 1 : iy;
    a = z[iy * nx + ix] + (z[iy * nx + ix1] - z[iy * nx + ix]) * tx;
    b = z[iy1 * nx + ix] + (z[iy1 * nx + ix1] - z[iy1 * nx + ix]) * tx;
    return a + (b - a) * ty;
}

/* ================================================================ config */

void lt_wue_config_default(lt_wue_config *c)
{
    memset(c, 0, sizeof(*c));
    lt_at_filters_default(&c->f);
    c->f.min_rpm = 500.0;
    c->f.max_rpm = 4000.0;
    c->f.min_clt = -1000.0;           /* coolant is the axis here, not a filter */
    c->f.max_tps_rate = 20.0;
    lt_at_authority_default(&c->auth);
    c->auth.max_cell_value_change = 1e9;   /* off: the percentage limit applies */
    c->auth.max_cell_percentage_change = 10.0;
    c->auth.min_cell_value = 0.0;
    c->auth.max_cell_value = 1e9;
    c->target_afr = LT_AT_STOICH_AFR;
    c->lambda_delay_ms = 0.0;
    c->cranking_rpm = 400.0;
    c->after_start_s = 30.0;
    c->max_tps = 40.0;
    c->min_clt_rate = 0.0;
    c->max_clt_rate = 2.0;
    c->min_samples = 20;
    c->base_weight = 40.0;
    c->min_change = 0.0;
    c->split_hits = LT_TRUE;
    c->relative_to_warm = LT_TRUE;
    c->monotonic = LT_TRUE;
    c->never_below_unity = LT_TRUE;
    c->warm_clt = LT_WUE_WARM_AUTO;
}

void lt_wue_config_clear(lt_wue_config *c)
{
    lt_at_filters_clear(&c->f);
}

void lt_wue_point_default(lt_wue_point *p)
{
    memset(p, 0, sizeof(*p));
    p->afr = LT_AT_STOICH_AFR;
    p->run_s = -1.0;
    p->accel_enrich_active = -1;
    p->fuel_cut_active = -1;
    p->cranking = -1;
    p->after_start = -1;
}

const char *lt_wue_status_name(int status)
{
    switch (status) {
    case LT_WUE_FEW: return "few samples";
    case LT_WUE_REC: return "recommended";
    case LT_WUE_WARM: return "warm (unity)";
    case LT_WUE_LOCKED: return "locked";
    default: return "no data";
    }
}

/* ================================================================ state */

lt_wue_state *lt_wue_new(void)
{
    lt_wue_state *s = (lt_wue_state *)lt_calloc(1, sizeof(*s));
    s->buf = lt_at_new();
    s->unity = 1.0;
    return s;
}

static void free_target(lt_wue_state *s)
{
    lt_free(s->tx); lt_free(s->ty); lt_free(s->tz);
    s->tx = s->ty = s->tz = NULL;
    s->tnx = s->tny = 0;
}

void lt_wue_free(lt_wue_state *s)
{
    if (s == NULL) { return; }
    free_target(s);
    lt_at_free(s->buf);
    lt_free(s);
}

static void clear_data(lt_wue_state *s)
{
    memset(s->wsum, 0, sizeof(s->wsum));
    memset(s->fsum, 0, sizeof(s->fsum));
    memset(s->asum, 0, sizeof(s->asum));
    memset(s->tsum, 0, sizeof(s->tsum));
    memset(s->hits, 0, sizeof(s->hits));
    s->warm_w = s->warm_f = 0.0;
    s->warm_n = s->accepted = 0;
    s->nrej = 0;
    s->engine = 0;
    s->started = LT_FALSE;
    s->run_start_ms = 0;
    lt_at_free(s->buf);
    s->buf = lt_at_new();
}

int lt_wue_set_curve(lt_wue_state *s, const double *clt, const double *vals, size_t n,
                     double offset, double unity)
{
    size_t i;
    if (n < 2 || clt == NULL || vals == NULL) { s->n = 0; return -1; }
    if (n > LT_WUE_MAX_BINS) { n = LT_WUE_MAX_BINS; }
    if (n != s->n) { memset(s->lock, 0, sizeof(s->lock)); }
    s->n = n;
    for (i = 0; i < n; i++) { s->clt[i] = clt[i]; s->cur[i] = vals[i]; }
    s->offset = offset;
    s->unity = unity > 0.0 ? unity : 1.0;
    clear_data(s);
    return 0;
}

void lt_wue_update_values(lt_wue_state *s, const double *vals, size_t n)
{
    size_t i;
    for (i = 0; i < n && i < s->n; i++) { s->cur[i] = vals[i]; }
}

size_t lt_wue_bin_count(const lt_wue_state *s) { return s->n; }

double lt_wue_unity_value(const lt_wue_state *s) { return s->unity - s->offset; }

void lt_wue_set_afr_offset(lt_wue_state *s, const double *clt, const double *off, size_t n)
{
    size_t i;
    if (n > LT_WUE_MAX_BINS) { n = LT_WUE_MAX_BINS; }
    s->noff = (clt != NULL && off != NULL) ? n : 0;
    for (i = 0; i < s->noff; i++) { s->offx[i] = clt[i]; s->offv[i] = off[i]; }
}

double lt_wue_afr_offset_at(const lt_wue_state *s, double clt)
{
    return s->noff > 0 ? lt_at_curve_lookup(s->offx, s->offv, s->noff, clt) : 0.0;
}

static double *dup_arr(const double *a, size_t n)
{
    double *d = (double *)lt_malloc((n + 1) * sizeof(double));
    memcpy(d, a, n * sizeof(double));
    return d;
}

void lt_wue_set_target_table(lt_wue_state *s, const double *x, size_t nx, const double *y,
                             size_t ny, const double *z)
{
    free_target(s);
    if (z == NULL || x == NULL || y == NULL || nx == 0 || ny == 0) { return; }
    s->tx = dup_arr(x, nx);
    s->ty = dup_arr(y, ny);
    s->tz = dup_arr(z, nx * ny);
    s->tnx = nx;
    s->tny = ny;
}

double lt_wue_target_at(const lt_wue_state *s, double rpm, double load, double fallback)
{
    double v;
    if (s->tz == NULL) { return fallback; }
    v = lt_at_table_lookup(s->tx, s->tnx, s->ty, s->tny, s->tz, rpm, load);
    return v > 0.1 ? lt_at_normalise_to_afr(v) : fallback;
}

void lt_wue_start(lt_wue_state *s)
{
    clear_data(s);
    s->running = LT_TRUE;
}

void lt_wue_stop(lt_wue_state *s) { s->running = LT_FALSE; }

lt_bool lt_wue_is_running(const lt_wue_state *s) { return s->running; }

void lt_wue_lock(lt_wue_state *s, size_t bin, lt_bool on)
{
    if (bin < LT_WUE_MAX_BINS) { s->lock[bin] = on ? 1 : 0; }
}

lt_bool lt_wue_is_locked(const lt_wue_state *s, size_t bin)
{
    return (bin < LT_WUE_MAX_BINS && s->lock[bin]) ? LT_TRUE : LT_FALSE;
}

unsigned long lt_wue_accepted(const lt_wue_state *s) { return s->accepted; }

static void count_reason(lt_wue_state *s, const char *reason)
{
    size_t i;
    for (i = 0; i < s->nrej; i++) {
        if (strcmp(s->rname[i], reason) == 0) { s->rcount[i]++; return; }
    }
    if (s->nrej < WUE_REASONS) {
        lt_strlcpy(s->rname[s->nrej], reason, sizeof(s->rname[0]));
        s->rcount[s->nrej] = 1;
        s->nrej++;
    }
}

size_t lt_wue_rejections(const lt_wue_state *s, lt_at_reject *out, size_t cap)
{
    size_t i, j, n = 0;
    for (i = 0; i < s->nrej && n < cap; i++) {
        out[n].reason = s->rname[i];
        out[n].count = s->rcount[i];
        n++;
    }
    for (i = 1; i < n; i++) {           /* insertion sort: count desc, reason asc */
        lt_at_reject t = out[i];
        j = i;
        while (j > 0 && (out[j - 1].count < t.count ||
                         (out[j - 1].count == t.count && strcmp(out[j - 1].reason, t.reason) > 0))) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = t;
    }
    return n;
}

static double to_mult(const lt_wue_state *s, double v) { return (v + s->offset) / s->unity; }
static double from_mult(const lt_wue_state *s, double m) { return m * s->unity - s->offset; }

size_t lt_wue_warm_index(const lt_wue_state *s, const lt_wue_config *c)
{
    size_t i;
    if (s->n == 0) { return 0; }
    if (c->warm_clt > LT_WUE_WARM_AUTO + 1.0) {
        for (i = 0; i < s->n; i++) {
            if (s->clt[i] >= c->warm_clt) { return i; }
        }
        return s->n;   /* nothing warm enough: no bin is pinned */
    }
    /* auto: the neutral tail of the curve, at least the last bin */
    i = s->n;
    while (i > 0 && fabs(to_mult(s, s->cur[i - 1]) - 1.0) <= WUE_NEUTRAL_TOL) { i--; }
    return i >= s->n ? s->n - 1 : i;
}

static double warm_clt_of(const lt_wue_state *s, const lt_wue_config *c)
{
    size_t w = lt_wue_warm_index(s, c);
    return w < s->n ? s->clt[w] : 1e300;
}

double lt_wue_warm_factor(const lt_wue_state *s, const lt_wue_config *c, unsigned long *n)
{
    if (n != NULL) { *n = s->warm_n; }
    if (s->warm_n < (unsigned long)(c->min_samples > 0 ? c->min_samples : 1) || s->warm_w <= 0.0) {
        return 1.0;
    }
    return s->warm_f / s->warm_w;
}

/* the sample as a VE point for the shared buffer and filters: load carries the throttle (the
 * steady-state check then watches rpm and throttle), map the target AFR and maf the EGO factor */
static void to_vp(const lt_wue_point *p, double target, lt_ve_point *vp)
{
    lt_ve_point_default(vp);
    vp->rpm = p->rpm;
    vp->load = p->tps;
    vp->map = target;
    vp->maf = p->ego;
    vp->afr = p->afr;
    vp->ve = p->load;
    vp->clt = p->clt;
    vp->tps = p->tps;
    vp->tps_rate = p->tps_rate;
    vp->accel_enrich_active = p->accel_enrich_active;
    vp->fuel_cut_active = p->fuel_cut_active;
    vp->timestamp_ms = p->t_ms;
}

static void add_to_bin(lt_wue_state *s, size_t i, double w, double factor, double afr, double target)
{
    if (w <= 0.0 || i >= s->n || s->lock[i]) { return; }
    s->wsum[i] += w;
    s->fsum[i] += w * factor;
    s->asum[i] += w * afr;
    s->tsum[i] += w * target;
    s->hits[i]++;
}

void lt_wue_add(lt_wue_state *s, const lt_wue_point *p, const lt_wue_config *c)
{
    lt_ve_point vp, old, hist;
    const char *why = NULL;
    double target, factor, ego, rate = 0.0, t, delay;
    lt_bool have_rate, crank;
    size_t i;
    unsigned long age;
    if (!s->running || s->n < 2) { return; }
    /* engine start tracking for the after-start filter */
    crank = (p->cranking == 1 || p->rpm < c->cranking_rpm) ? LT_TRUE : LT_FALSE;
    if (crank) {
        s->engine = 1;
    } else if (s->engine != 2) {
        s->started = s->engine == 1 ? LT_TRUE : LT_FALSE;   /* a start seen, or already running */
        s->engine = 2;
        s->run_start_ms = p->t_ms;
    }
    target = p->target_afr > 0.1 ? lt_at_normalise_to_afr(p->target_afr)
                                 : lt_wue_target_at(s, p->rpm, p->load, c->target_afr);
    to_vp(p, target, &vp);
    delay = c->lambda_delay_ms > 0.0 ? c->lambda_delay_ms : (double)lt_at_default_delay_ms(p->rpm);
    lt_at_set_buffer_max_age_ms(s->buf, (unsigned long)delay + WUE_RATE_WINDOW_MS + 2000UL);
    have_rate = lt_at_find_delayed(s->buf, p->t_ms, WUE_RATE_WINDOW_MS, &old);
    if (have_rate && p->t_ms > old.timestamp_ms + 500UL) {
        rate = (p->clt - old.clt) / ((double)(p->t_ms - old.timestamp_ms) / 1000.0);
    } else {
        have_rate = LT_FALSE;
    }
    lt_at_push_sample(s->buf, &vp);

    age = (s->started && p->t_ms >= s->run_start_ms) ? p->t_ms - s->run_start_ms : 0xFFFFFFFFUL;
    if (p->reject != NULL) { why = p->reject; }
    else if (crank) { why = "cranking"; }
    else if (p->after_start == 1 || (p->run_s >= 0.0 && p->run_s < c->after_start_s) ||
             (p->run_s < 0.0 && (double)age < c->after_start_s * 1000.0)) {
        why = "after-start enrichment";
    }
    else if (c->max_tps > 0.0 && p->tps > c->max_tps) { why = "throttle above max"; }
    else if (have_rate && c->max_clt_rate > 0.0 && fabs(rate) > c->max_clt_rate) { why = "coolant changing too fast"; }
    else if (have_rate && c->min_clt_rate > 0.0 && rate < c->min_clt_rate) { why = "coolant not rising"; }
    else if (target < 0.1) { why = "no target"; }
    else { (void)lt_at_classify(s->buf, &vp, &c->f, &why); }
    if (why != NULL) { count_reason(s, why); return; }

    /* transport delay: the operating point the measured AFR belongs to */
    if (!lt_at_find_delayed(s->buf, p->t_ms, (unsigned long)delay, &hist)) { hist = vp; }
    target = hist.map + lt_wue_afr_offset_at(s, hist.clt);
    if (target < 0.1) { count_reason(s, "no target"); return; }
    ego = hist.maf > 0.1 ? hist.maf : 1.0;
    factor = p->afr / target * ego;
    s->accepted++;
    if (hist.clt >= warm_clt_of(s, c)) {
        s->warm_w += 1.0;
        s->warm_f += factor;
        s->warm_n++;
    }
    if (c->split_hits) {
        axis_pos(s->clt, s->n, hist.clt, &i, &t);
        add_to_bin(s, i, 1.0 - t, factor, p->afr, target);
        add_to_bin(s, i + 1, t, factor, p->afr, target);
    } else {
        add_to_bin(s, lt_tbl_nearest(s->clt, s->n, hist.clt), 1.0, factor, p->afr, target);
    }
}

size_t lt_wue_bins(const lt_wue_state *s, const lt_wue_config *c, lt_wue_bin *out, size_t cap)
{
    size_t i, n = s->n < cap ? s->n : cap, widx = lt_wue_warm_index(s, c);
    double warm = c->relative_to_warm ? lt_wue_warm_factor(s, c, NULL) : 1.0;
    for (i = 0; i < n; i++) {
        lt_wue_bin *b = &out[i];
        double cur = s->cur[i], rec = cur;
        memset(b, 0, sizeof(*b));
        b->clt = s->clt[i];
        b->current = cur;
        b->hits = s->hits[i];
        b->weight = s->wsum[i];
        b->factor = s->wsum[i] > 0.0 ? s->fsum[i] / s->wsum[i] / warm : 1.0;
        if (s->wsum[i] > 0.0) {
            b->afr_mean = s->asum[i] / s->wsum[i];
            b->target_mean = s->tsum[i] / s->wsum[i];
        }
        if (s->lock[i]) {
            b->status = LT_WUE_LOCKED;
        } else if (i >= widx) {
            b->status = LT_WUE_WARM;
            rec = lt_at_apply_authority_limits(cur, from_mult(s, 1.0), &c->auth);
        } else if (s->hits[i] < c->min_samples || s->wsum[i] <= 0.0) {
            b->status = s->hits[i] > 0 ? LT_WUE_FEW : LT_WUE_NO_DATA;
        } else {
            double conf = c->base_weight > 0.0 ? s->wsum[i] / c->base_weight : 1.0, m;
            if (conf > 1.0) { conf = 1.0; }
            m = to_mult(s, cur) * (1.0 + (b->factor - 1.0) * conf);
            if (c->never_below_unity && m < 1.0) { m = 1.0; }
            rec = lt_at_apply_authority_limits(cur, from_mult(s, m), &c->auth);
            if (fabs(rec - cur) < c->min_change) { rec = cur; }
            b->status = LT_WUE_REC;
        }
        b->recommended = rec;
    }
    if (c->monotonic && n > 1) {
        /* walk from the warm end: a colder bin keeps at least the enrichment of the next one */
        i = (widx < n ? widx : n - 1);
        while (i > 0) {
            i--;
            if (out[i].status == LT_WUE_REC && out[i].recommended < out[i + 1].recommended) {
                double hi = lt_at_apply_authority_limits(out[i].current, 1e12, &c->auth);
                double want = out[i + 1].recommended < hi ? out[i + 1].recommended : hi;
                if (want > out[i].recommended) { out[i].recommended = want; }
            }
        }
    }
    return s->n;
}

/* ================================================================ glue */

static lt_bool has_ch(const lt_ini *ini, const char *name)
{
    return (ini != NULL && name != NULL && name[0] != '\0' &&
            lt_map_get(&ini->channel_idx, name) != NULL) ? LT_TRUE : LT_FALSE;
}

static void pick(const lt_ini *ini, char *dst, size_t cap, const char *const *cands)
{
    size_t i;
    dst[0] = '\0';
    for (i = 0; cands[i] != NULL; i++) {
        if (has_ch(ini, cands[i])) { lt_strlcpy(dst, cands[i], cap); return; }
    }
}

static lt_bool lambda_name(const char *n)
{
    size_t i, len = strlen(n);
    for (i = 0; i + 6 <= len; i++) {
        if (lt_strnicmp(n + i, "lambda", 6) == 0) { return LT_TRUE; }
    }
    return LT_FALSE;
}

void lt_wue_channels_from_ini(const lt_ini *ini, lt_wue_channels *out)
{
    static const char *const c_crank[] = { "isCranking", "cranking", "crank", NULL };
    static const char *const c_eng[] = { "engine", "engineStatus", NULL };
    static const char *const c_run[] = { "running_timeSinceCrankingInSecs", "timeSinceCranking",
                                         "timeSinceStart", NULL };
    static const char *const c_ego[] = { "egoCorrection", "egoCorrection1", "afrCorrection", NULL };
    memset(out, 0, sizeof(*out));
    lt_at_channels_from_ini(ini, &out->at);
    pick(ini, out->cranking, sizeof(out->cranking), c_crank);
    pick(ini, out->engine, sizeof(out->engine), c_eng);
    pick(ini, out->run_s, sizeof(out->run_s), c_run);
    pick(ini, out->ego, sizeof(out->ego), c_ego);
    if (ini == NULL) { return; }
    if (has_ch(ini, ini->wue_analyze.coolant_channel)) {
        lt_strlcpy(out->at.clt, ini->wue_analyze.coolant_channel, sizeof(out->at.clt));
    }
    if (has_ch(ini, ini->wue_analyze.lambda_channel)) {
        lt_strlcpy(out->at.afr, ini->wue_analyze.lambda_channel, sizeof(out->at.afr));
        out->at.afr_is_lambda = lambda_name(out->at.afr);
    }
    if (has_ch(ini, ini->wue_analyze.ego_channel)) {
        lt_strlcpy(out->ego, ini->wue_analyze.ego_channel, sizeof(out->ego));
    } else if (has_ch(ini, ini->ve_analyze.ego_channel)) {
        lt_strlcpy(out->ego, ini->ve_analyze.ego_channel, sizeof(out->ego));
    }
}

/* EGO correction as a multiplier: percent (100 = none) or a ratio (1 = none) */
static double ego_mult(double v)
{
    if (v > 10.0) { return v / 100.0; }
    return v > 0.1 ? v : 1.0;
}

typedef lt_bool (*wue_get_fn)(const void *src, const char *name, double *v);

static void build_point(const void *src, wue_get_fn get, const lt_wue_channels *ch,
                        unsigned long t_ms, lt_wue_point *p)
{
    double v;
    lt_wue_point_default(p);
    p->t_ms = t_ms;
    p->clt = 90.0;
    if (get(src, ch->at.rpm, &v)) { p->rpm = v; }
    if (get(src, ch->at.load, &v)) { p->load = v; }
    if (get(src, ch->at.clt, &v)) { p->clt = v; }
    if (get(src, ch->at.afr, &v)) { p->afr = ch->at.afr_is_lambda ? lt_at_normalise_to_afr(v) : v; }
    if (get(src, ch->at.tps, &v)) { p->tps = v; }
    if (get(src, ch->at.tps_rate, &v)) { p->tps_rate = v; }
    if (get(src, ch->at.fuel_cut, &v)) { p->fuel_cut_active = v > 0.5 ? 1 : 0; }
    if (get(src, ch->at.accel_enrich, &v)) { p->accel_enrich_active = v > 0.5 ? 1 : 0; }
    if (get(src, ch->ego, &v)) { p->ego = ego_mult(v); }
    if (get(src, ch->cranking, &v)) { p->cranking = v > 0.5 ? 1 : 0; }
    if (get(src, ch->run_s, &v)) { p->run_s = v; }
    if (get(src, ch->engine, &v)) {      /* MegaSquirt engine bits: 2 cranking, 4 after-start */
        long bits = (long)lt_round(v);
        if (p->cranking < 0) { p->cranking = (bits & 2L) ? 1 : 0; }
        p->after_start = (bits & 4L) ? 1 : 0;
    }
}

static lt_bool rt_get(const void *src, const char *name, double *v)
{
    return (name != NULL && name[0] != '\0' && lt_ini_rt_get((const lt_ini_rt *)src, name, v))
               ? LT_TRUE : LT_FALSE;
}

lt_bool lt_wue_sample_from_rt(const lt_ini_rt *rt, const lt_wue_channels *ch, unsigned long now_ms,
                              lt_wue_point *out)
{
    double v;
    if (rt == NULL || !rt_get(rt, ch->at.clt, &v) || !rt_get(rt, ch->at.afr, &v)) { return LT_FALSE; }
    build_point(rt, rt_get, ch, now_ms, out);
    return LT_TRUE;
}

typedef struct log_row { const lt_log *log; long row; } log_row;

static lt_bool log_get(const void *src, const char *name, double *v)
{
    const log_row *r = (const log_row *)src;
    int c;
    if (name == NULL || name[0] == '\0') { return LT_FALSE; }
    c = lt_log_channel_index(r->log, name);
    if (c < 0) { return LT_FALSE; }
    *v = LT_LOG_AT(r->log, r->row, c);
    return lt_isfinite(*v);
}

long lt_wue_replay(lt_wue_state *s, const lt_log *log, const lt_wue_channels *ch,
                   const lt_wue_config *c, const lt_ini_filter *flt, size_t nflt,
                   const unsigned char *enabled)
{
    long i;
    size_t k;
    log_row r;
    lt_wue_point p;
    double v, ms;
    if (log == NULL || s->n < 2) { lt_err_set("no curve or log"); return LT_EINVAL; }
    if (lt_log_channel_index(log, ch->at.clt) < 0 || lt_log_channel_index(log, ch->at.afr) < 0) {
        lt_err_set("the log has no coolant or AFR/lambda channel");
        return LT_ENOENT;
    }
    lt_wue_start(s);
    r.log = log;
    for (i = 0; i < log->n; i++) {
        r.row = i;
        ms = log->t[i] * 1000.0;
        build_point(&r, log_get, ch, ms > 0.0 ? (unsigned long)(ms + 0.5) : 0UL, &p);
        for (k = 0; k < nflt && flt != NULL; k++) {
            if (enabled != NULL && !enabled[k]) { continue; }
            if (log_get(&r, flt[k].channel, &v) && lt_at_filter_rejects(&flt[k], LT_TRUE, LT_TRUE, v)) {
                p.reject = flt[k].display != NULL ? flt[k].display : flt[k].name;
                break;
            }
        }
        lt_wue_add(s, &p, c);
    }
    lt_wue_stop(s);
    return (long)s->accepted;
}

void lt_wue_units_for(const lt_ini_const *z, double percent_offset, double *offset, double *unity)
{
    *offset = 0.0;
    *unity = 1.0;
    if (percent_offset > 0.5) { *offset = percent_offset; *unity = percent_offset; return; }
    if (z == NULL) { return; }
    if ((z->units != NULL && strchr(z->units, '%') != NULL) || z->max > 20.0) { *unity = 100.0; }
}
