/* dash_view.c - runtime gauge state: smoothing, peak hold, spikes, zones with hysteresis,
 * history rings, layout and hit testing. Painting is done by painters supplied through
 * lt_dash_set_painters (GUI layer); a plain fallback is drawn without them.
 * Spec: docs/spec/05-dash-gauges.md sections 5.1, 5.2, 5.4.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>
#include "dash_priv.h"
#include "../tableops/tableops.h"

/* ================================================================== */
/* pure helpers                                                         */
/* ================================================================== */

double lt_gauge_ema_step(double prev, double target, double dt_ms)
{
    double alpha;
    if (!(dt_ms > 0.0)) return prev;
    if (dt_ms > 100.0) dt_ms = 100.0;
    alpha = 1.0 - exp(-dt_ms / 120.0);
    return prev + (target - prev) * alpha;
}

void lt_gauge_resolve_arc(const lt_dash_gauge *g, lt_gauge_arc *out)
{
    double start = (double)g->sweep_begin_degree;
    double req = (double)g->sweep_angle;
    double face = (g->face_angle < 45) ? 360.0 : (double)LT_MIN(g->face_angle, 360);
    double sweep = LT_MAX(1.0, LT_MIN(req, face));
    double mid = g->counter_clockwise ? start - sweep / 2.0 : start + sweep / 2.0;
    out->start_deg = start;
    out->sweep_deg = sweep;
    out->face_deg = face;
    out->face_start_deg = mid - face / 2.0;
    out->ccw = g->counter_clockwise ? LT_TRUE : LT_FALSE;
    out->full_circle = face >= 360.0;
}

lt_bool lt_gauge_is_spike(double raw, double smoothed, double min, double max)
{
    double range = max - min;
    if (!(range > 0.0)) return LT_FALSE;
    return fabs(raw - smoothed) * 20.0 > 3.0 * range;   /* 15% of range, exact at the boundary */
}

double lt_gauge_seed_peak(const lt_dash_gauge *g, double fallback, double min, double max)
{
    if (!g->peak_hold) return fallback;
    return LT_CLAMP(g->history_value, min, max);
}

double lt_gauge_peak_step(lt_peak_state *s, double target, double now_ms, int history_delay)
{
    if (!s->started) {
        s->started = LT_TRUE;
        s->clock_ms = now_ms;
    }
    if (target > s->peak) {
        s->peak = target;
        s->clock_ms = now_ms;
    } else if (history_delay > 0 && now_ms - s->clock_ms >= (double)history_delay) {
        s->peak = target;
        s->clock_ms = now_ms;
    }
    return s->peak;
}

int lt_gauge_stagger_slot(const char *channel, int *counter)
{
    if (channel && channel[0]) {
        lt_u32 h = 5381UL;
        long sh;
        const unsigned char *p = (const unsigned char *)channel;
        for (; *p; p++) h = ((h * 33UL) + (lt_u32)*p) & LT_U32_MASK;
        sh = (h >= 0x80000000UL) ? -(long)(0xFFFFFFFFUL - h) - 1L : (long)h;
        if (sh < 0) sh = -sh;
        return (int)(sh % 16L);
    }
    if (counter) {
        int s = *counter % 16;
        (*counter)++;
        return s;
    }
    return 0;
}

void lt_dash_ring_init(lt_dash_ring *r, int cap)
{
    r->d = (double *)lt_calloc((size_t)(cap > 0 ? cap : 1), sizeof(double));
    r->cap = cap > 0 ? cap : 1;
    r->head = 0;
    r->n = 0;
}

void lt_dash_ring_free(lt_dash_ring *r)
{
    lt_free(r->d);
    memset(r, 0, sizeof(*r));
}

void lt_dash_ring_push(lt_dash_ring *r, double v)
{
    if (r->d == NULL) return;
    r->d[r->head] = v;
    r->head = (r->head + 1) % r->cap;
    if (r->n < r->cap) r->n++;
}

double lt_dash_ring_get(const lt_dash_ring *r, int i)
{
    int start;
    if (r->d == NULL || i < 0 || i >= r->n) return 0.0;
    start = (r->head - r->n + r->cap * 2) % r->cap;
    return r->d[(start + i) % r->cap];
}

/* ================================================================== */
/* zones                                                                */
/* ================================================================== */

void lt_dash_limits_from_gauge(const lt_dash_gauge *g, lt_dash_limits *o)
{
    o->min = g->min;
    o->max = g->max;
    o->has_lw = g->has_low_warning;
    o->has_hw = g->has_high_warning;
    o->has_lc = g->has_low_critical;
    o->has_hc = g->has_high_critical;
    o->lw = g->low_warning;
    o->hw = g->high_warning;
    o->lc = g->low_critical;
    o->hc = g->high_critical;
}

int lt_dash_zone(const lt_dash_limits *l, double h, double v, int *prev_zone)
{
    int prev = prev_zone ? *prev_zone : 0;
    int zone = 0;
    double hc = (prev == 2) ? h : 0.0;
    double hw = (prev >= 1) ? h : 0.0;

    if ((l->has_hc && v >= l->hc - hc) || (l->has_lc && v <= l->lc + hc)) zone = 2;
    else if ((l->has_hw && v >= l->hw - hw) || (l->has_lw && v <= l->lw + hw)) zone = 1;
    if (prev_zone) *prev_zone = zone;
    return zone;
}

lt_color lt_dash_value_color(const lt_dash_gauge *g, double v, int *prev_zone)
{
    lt_dash_limits l;
    int z;
    lt_dash_limits_from_gauge(g, &l);
    z = lt_dash_zone(&l, g->has_hysteresis ? g->hysteresis : 0.0, v, prev_zone);
    return z == 2 ? g->critical_color : (z == 1 ? g->warn_color : g->font_color);
}

lt_bool lt_dash_gstate_spiking(const lt_dash_gstate *s, double now_ms)
{
    return s->last_spike_ms >= 0.0 && now_ms - s->last_spike_ms <= LT_DASH_SPIKE_MS;
}

/* ================================================================== */
/* painters                                                             */
/* ================================================================== */

static lt_dash_painters g_painters;
static lt_bool g_have_painters = LT_FALSE;

void lt_dash_set_painters(const lt_dash_painters *p)
{
    if (p) {
        g_painters = *p;
        g_have_painters = LT_TRUE;
    } else {
        memset(&g_painters, 0, sizeof(g_painters));
        g_have_painters = LT_FALSE;
    }
}

static void fallback_gauge(lt_gfx *g, lt_rect r, const lt_dash_gauge *cfg, const lt_dash_gstate *st)
{
    char buf[96];
    double range = st->lim.max - st->lim.min;
    double pct = range > 0.0 ? (st->display - st->lim.min) / range : 0.0;
    int z = st->zone;
    lt_color vc = z == 2 ? cfg->critical_color : (z == 1 ? cfg->warn_color : cfg->font_color);
    int bw;

    pct = lt_isnan(pct) ? 0.0 : LT_CLAMP(pct, 0.0, 1.0);
    lt_gfx_fill_rect(g, r, cfg->back_color);
    lt_gfx_rect(g, r, cfg->trim_color);
    if (r.w < 8 || r.h < 8) return;
    bw = (int)((double)(r.w - 4) * pct);
    if (bw > 0) lt_gfx_fill_rect(g, lt_rect_make(r.x + 2, r.y + r.h - 5, bw, 3), vc);
    if (cfg->title && cfg->title[0] && cfg->title[0] != '{')
        lt_gfx_text(g, r.x + 3, r.y + 3, cfg->title, cfg->trim_color, 1);
    lt_snprintf(buf, sizeof buf, "%.*f %s", cfg->value_digits < 0 ? 0 : (cfg->value_digits > 12 ? 12 : cfg->value_digits),
                st->display, cfg->units && cfg->units[0] != '{' ? cfg->units : "");
    lt_gfx_text(g, r.x + 3, r.y + 3 + lt_gfx_text_height(1) + 2, buf, vc, 1);
}

static void fallback_indicator(lt_gfx *g, lt_rect r, const lt_dash_indicator *in, lt_bool on)
{
    lt_color bg = on ? in->on_background_color : in->off_background_color;
    lt_color fg = on ? in->on_text_color : in->off_text_color;
    const char *t = on ? in->on_text : in->off_text;
    if (LT_COLOR_A(bg) == 0) bg = LT_RGB(40, 40, 40);
    lt_gfx_fill_rect(g, r, bg);
    lt_gfx_rect(g, r, LT_RGB(100, 100, 100));
    if (t && t[0] && r.w > 8 && r.h > 8)
        lt_gfx_text(g, r.x + 3, r.y + (r.h - lt_gfx_text_height(1)) / 2, t, fg, 1);
}

static void paint_gauge(lt_gfx *g, lt_rect r, const lt_dash_gauge *cfg, const lt_dash_gstate *st,
                        double now_ms)
{
    if (g_have_painters && g_painters.gauge) g_painters.gauge(g_painters.ud, g, r, cfg, st, now_ms);
    else fallback_gauge(g, r, cfg, st);
}

static void paint_indicator(lt_gfx *g, lt_rect r, const lt_dash_indicator *in, lt_bool on)
{
    if (g_have_painters && g_painters.indicator) g_painters.indicator(g_painters.ud, g, r, in, on);
    else fallback_indicator(g, r, in, on);
}

void lt_dash_render_gauge(lt_gfx *g, lt_rect r, const lt_dash_gauge *cfg, double value)
{
    lt_dash_gstate st;
    memset(&st, 0, sizeof(st));
    st.is_gauge = LT_TRUE;
    st.enabled = LT_TRUE;
    st.has_value = LT_TRUE;
    st.raw = st.display = value;
    st.last_spike_ms = -1.0;
    lt_dash_limits_from_gauge(cfg, &st.lim);
    st.peak.peak = lt_gauge_seed_peak(cfg, value, cfg->min, cfg->max);
    lt_dash_value_color(cfg, value, &st.zone);
    paint_gauge(g, r, cfg, &st, 0.0);
}

void lt_dash_render_indicator(lt_gfx *g, lt_rect r, const lt_dash_indicator *ind, lt_bool on)
{
    paint_indicator(g, r, ind, on);
}

/* ================================================================== */
/* view                                                                 */
/* ================================================================== */

struct lt_dash_view {
    const lt_dash_file *d;
    size_t n;
    lt_dash_gstate *st;
    lt_rect *rects;
    lt_expr_cache cache;
    int hz;
    double now_ms;
    int cell_auto;                 /* terminal UI: lay the components out as a grid */
};

static void series_setup(lt_dash_gstate *s, const lt_dash_gauge *g)
{
    int i, n = 0;
    if (g->painter != LT_GP_LINE_GRAPH && g->painter != LT_GP_MULTI_TREND) return;
    s->series_ch[0] = lt_strdup(g->output_channel ? g->output_channel : "");
    lt_dash_ring_init(&s->ring[0], LT_DASH_HIST_LEN);
    n = 1;
    if (g->painter == LT_GP_MULTI_TREND) {
        for (i = 2; i <= LT_DASH_MAX_SERIES; i++) {
            char key[40];
            const char *c;
            lt_snprintf(key, sizeof key, "lt_series%d_channel", i);
            c = lt_dash_extras_get(&g->extras, key);
            if (c && c[0]) {
                s->series_ch[n] = lt_strdup(c);
                lt_dash_ring_init(&s->ring[n], LT_DASH_HIST_LEN);
                n++;
            }
        }
    }
    s->n_series = n;
}

lt_dash_view *lt_dash_view_new(const lt_dash_file *d)
{
    lt_dash_view *v = (lt_dash_view *)lt_calloc(1, sizeof(*v));
    size_t i;
    int counter = 0;

    v->d = d;
    v->n = d->cluster.n_comps;
    v->hz = 30;
    v->st = (lt_dash_gstate *)lt_calloc(v->n + 1, sizeof(lt_dash_gstate));
    v->rects = (lt_rect *)lt_calloc(v->n + 1, sizeof(lt_rect));
    lt_expr_cache_init(&v->cache);
    for (i = 0; i < v->n; i++) {
        const lt_dash_comp *c = &d->cluster.comps[i];
        lt_dash_gstate *s = &v->st[i];
        s->enabled = LT_TRUE;
        s->last_spike_ms = -1.0;
        s->last_frame = -1000000000L;
        s->last_hist_ms = -1e30;
        if (c->gauge) {
            s->is_gauge = LT_TRUE;
            s->slot = lt_gauge_stagger_slot(c->gauge->output_channel, &counter);
            lt_dash_limits_from_gauge(c->gauge, &s->lim);
            series_setup(s, c->gauge);
        } else {
            s->slot = lt_gauge_stagger_slot(c->indicator ? c->indicator->output_channel : NULL,
                                            &counter);
        }
    }
    return v;
}

void lt_dash_view_free(lt_dash_view *v)
{
    size_t i;
    int k;
    if (v == NULL) return;
    for (i = 0; i < v->n; i++) {
        for (k = 0; k < LT_DASH_MAX_SERIES; k++) {
            lt_free(v->st[i].series_ch[k]);
            lt_dash_ring_free(&v->st[i].ring[k]);
        }
    }
    lt_free(v->st);
    lt_free(v->rects);
    lt_expr_cache_free(&v->cache);
    lt_free(v);
}

void lt_dash_view_set_redraw_hz(lt_dash_view *v, int hz)
{
    static const int ok[] = { 10, 15, 20, 25, 30, 60 };
    size_t i;
    int best = 30, bd = 1000;
    for (i = 0; i < LT_COUNTOF(ok); i++) {
        int d = hz > ok[i] ? hz - ok[i] : ok[i] - hz;
        if (d < bd) {
            bd = d;
            best = ok[i];
        }
    }
    v->hz = best;
}

size_t lt_dash_view_count(const lt_dash_view *v)
{
    return v->n;
}

const lt_dash_gstate *lt_dash_view_state(const lt_dash_view *v, size_t index)
{
    return index < v->n ? &v->st[index] : NULL;
}

lt_bool lt_dash_view_due(lt_dash_view *v, size_t index, double now_ms)
{
    double interval, phase;
    long frame;
    lt_dash_gstate *s;
    if (index >= v->n) return LT_FALSE;
    s = &v->st[index];
    interval = 1000.0 / (double)v->hz;
    phase = (double)s->slot * interval / 16.0;
    frame = (long)floor((now_ms - phase) / interval);
    if (frame == s->last_frame) return LT_FALSE;
    s->last_frame = frame;
    return LT_TRUE;
}

static lt_bool get_var(const lt_expr_env *env, const char *name, double *out)
{
    double d;
    if (env == NULL || env->get_var == NULL || name == NULL || name[0] == '\0') return LT_FALSE;
    if (!env->get_var(env->ud, name, &d) || !lt_isfinite(d)) return LT_FALSE;
    *out = d;
    return LT_TRUE;
}

static double resolve_vp(const char *vp, const lt_expr_env *env, double fallback)
{
    double d;
    if (vp && vp[0] && get_var(env, vp, &d)) return d;
    return fallback;
}

static void update_gauge(lt_dash_view *v, lt_dash_gstate *s, const lt_dash_gauge *g, double now,
                         const lt_expr_env *env)
{
    double raw, target, dt;
    lt_bool first = !s->has_value;
    lt_uconv uc;
    int k;

    lt_dash_limits_from_gauge(g, &s->lim);
    s->lim.min = resolve_vp(g->min_vp, env, g->min);
    s->lim.max = resolve_vp(g->max_vp, env, g->max);
    if (g->low_warning_vp && g->low_warning_vp[0]) {
        double d;
        if (get_var(env, g->low_warning_vp, &d)) { s->lim.has_lw = LT_TRUE; s->lim.lw = d; }
    }
    if (g->high_warning_vp && g->high_warning_vp[0]) {
        double d;
        if (get_var(env, g->high_warning_vp, &d)) { s->lim.has_hw = LT_TRUE; s->lim.hw = d; }
    }
    if (g->low_critical_vp && g->low_critical_vp[0]) {
        double d;
        if (get_var(env, g->low_critical_vp, &d)) { s->lim.has_lc = LT_TRUE; s->lim.lc = d; }
    }
    if (g->high_critical_vp && g->high_critical_vp[0]) {
        double d;
        if (get_var(env, g->high_critical_vp, &d)) { s->lim.has_hc = LT_TRUE; s->lim.hc = d; }
    }

    if (!get_var(env, g->output_channel, &raw)) raw = g->value;   /* unknown channel: demo value */
    /* unit preferences: gauge units "C"/"kPa"/"AFR"... are shown in the preferred unit */
    lt_uconv_make(&uc, g->units);
    if (uc.kind != s->unit_kind || (uc.kind != LT_UK_NONE && uc.dst != s->unit_dst)) {
        s->has_value = LT_FALSE;     /* smoothed/peak values are in the old unit */
        first = LT_TRUE;
        s->unit_kind = uc.kind;
        s->unit_dst = uc.dst;
    }
    s->unit_digits = lt_uconv_digits(&uc, 0);
    s->unit_label = lt_uconv_active(&uc) ? lt_unit_name(uc.kind, uc.dst) : NULL;
    if (lt_uconv_active(&uc)) {
        raw = lt_uconv_to_disp(&uc, raw);
        s->lim.min = lt_uconv_to_disp(&uc, s->lim.min);
        s->lim.max = lt_uconv_to_disp(&uc, s->lim.max);
        s->lim.lw = lt_uconv_to_disp(&uc, s->lim.lw);
        s->lim.hw = lt_uconv_to_disp(&uc, s->lim.hw);
        s->lim.lc = lt_uconv_to_disp(&uc, s->lim.lc);
        s->lim.hc = lt_uconv_to_disp(&uc, s->lim.hc);
    }
    s->raw = raw;
    target = raw;
    if (g->peg_limits && s->lim.max > s->lim.min) target = LT_CLAMP(target, s->lim.min, s->lim.max);

    if (first) {
        s->display = target;
        s->has_value = LT_TRUE;
        s->peak.peak = lt_gauge_seed_peak(g, s->display, s->lim.min, s->lim.max);
        s->peak.started = LT_FALSE;
    } else {
        dt = now - s->last_ms;
        s->display = lt_gauge_ema_step(s->display, target, dt);
        if (lt_gauge_is_spike(raw, s->display, s->lim.min, s->lim.max)) s->last_spike_ms = now;
    }
    s->last_ms = now;
    if (g->peak_hold) lt_gauge_peak_step(&s->peak, s->display, now, g->history_delay);
    lt_dash_zone(&s->lim, g->has_hysteresis ? g->hysteresis : 0.0, s->display, &s->zone);

    if (s->n_series > 0) {
        for (k = 0; k < s->n_series; k++) {
            double sv;
            if (k == 0) sv = raw;
            else if (get_var(env, s->series_ch[k], &sv)) { /* live */ }
            else sv = s->series_latest[k];
            s->series_latest[k] = sv;
        }
        if (s->ring[0].n == 0 || now - s->last_hist_ms >= 100.0) {
            for (k = 0; k < s->n_series; k++) lt_dash_ring_push(&s->ring[k], s->series_latest[k]);
            s->last_hist_ms = now;
        }
    }
    (void)v;
}

static lt_bool indicator_on(lt_dash_view *v, const lt_dash_indicator *in, const lt_expr_env *env)
{
    const char *ex = lt_dash_extras_get(&in->extras, "lt_expression");
    double d;
    if (ex && ex[0]) {
        const lt_expr *e = lt_expr_cache_get(&v->cache, ex);
        lt_bool b = LT_FALSE;
        if (e == NULL || env == NULL || lt_expr_eval_bool(e, env, &b) != 0) return LT_FALSE;
        return b;
    }
    if (get_var(env, in->output_channel, &d)) return d != 0.0;
    return in->value != 0.0;
}

void lt_dash_view_update(lt_dash_view *v, double now_ms, const lt_expr_env *env)
{
    size_t i;
    v->now_ms = now_ms;
    for (i = 0; i < v->n; i++) {
        const lt_dash_comp *c = &v->d->cluster.comps[i];
        lt_dash_gstate *s = &v->st[i];
        if (c->gauge) {
            s->enabled = lt_expr_cache_test(&v->cache, c->gauge->enabled_condition, env);
            update_gauge(v, s, c->gauge, now_ms, env);
        } else if (c->indicator) {
            s->enabled = lt_expr_cache_test(&v->cache, c->indicator->enabled_condition, env);
            s->on = indicator_on(v, c->indicator, env);
        }
    }
}

static int iround(double x)
{
    if (lt_isnan(x)) return 0;
    if (x > 1.0e9) return 1000000000;
    if (x < -1.0e9) return -1000000000;
    return (int)lt_round(x);
}


/* ---- terminal (cell) layout --------------------------------------------- */
/* gauges become a grid of panels that fills the area (columns chosen for the best panel shape),
 * indicators fixed width pills below it, the lit ones first; what does not fit is counted in a
 * "+N more" pill. Sets v->rects (w = 0 for hidden components) and returns the hidden count
 * of indicators; *more_r is the rect of the note (w = 0 when there is none). */
static int ind_label_cols(const lt_dash_indicator *in, lt_bool on)
{
    const char *t = on ? in->on_text : in->off_text;
    int n = 0;
    if (t == NULL) { return 0; }
    while (lt_utf8_next(&t) != 0) { n++; }
    return n;
}

static int cell_layout(lt_dash_view *v, lt_rect cr, lt_rect *more_r)
{
    const lt_dash_cluster *cl = &v->d->cluster;
    size_t i, j, ng = 0, ni = 0, *gi, *ii;
    int W = cr.w / 8, H = cr.h / 16, x0 = cr.x / 8, y0 = cr.y / 16;
    int gh, ind_rows = 0, per = 1, pw = 10, shown_i = 0, hidden = 0, longest = 0, k, cols = 1, rws = 1;
    int shown_g = 0;
    double best = -1.0;
    more_r->x = more_r->y = more_r->w = more_r->h = 0;
    gi = (size_t *)lt_malloc(sizeof(size_t) * (v->n + 1));
    ii = (size_t *)lt_malloc(sizeof(size_t) * (v->n + 1));
    for (i = 0; i < v->n; i++) {
        lt_rect z;
        z.x = z.y = z.w = z.h = 0;
        v->rects[i] = z;
        if (!v->st[i].enabled) { continue; }
        if (cl->comps[i].gauge) {
            /* reading order: row bands of the file's layout, then left to right */
            double key = floor(cl->comps[i].gauge->relative_y * 6.0) * 10.0 + cl->comps[i].gauge->relative_x;
            size_t p = ng;
            while (p > 0) {
                const lt_dash_gauge *o = cl->comps[gi[p - 1]].gauge;
                double ok = floor(o->relative_y * 6.0) * 10.0 + o->relative_x;
                if (ok <= key) { break; }
                gi[p] = gi[p - 1];
                p--;
            }
            gi[p] = i;
            ng++;
        } else {
            ii[ni++] = i;
        }
    }
    /* lit indicators first (stable) */
    {
        size_t *tmp = (size_t *)lt_malloc(sizeof(size_t) * (ni + 1));
        size_t n2 = 0;
        for (j = 0; j < ni; j++) { if (v->st[ii[j]].on) { tmp[n2++] = ii[j]; } }
        for (j = 0; j < ni; j++) { if (!v->st[ii[j]].on) { tmp[n2++] = ii[j]; } }
        for (j = 0; j < ni; j++) { ii[j] = tmp[j]; }
        lt_free(tmp);
    }
    if (W < 4 || H < 2) { lt_free(gi); lt_free(ii); return 0; }
    /* indicator block */
    if (ni > 0) {
        int max_rows, need;
        for (j = 0; j < ni; j++) {
            const lt_dash_indicator *in = cl->comps[ii[j]].indicator;
            int a = ind_label_cols(in, 1), b = ind_label_cols(in, 0);
            if (a > longest) { longest = a; }
            if (b > longest) { longest = b; }
        }
        pw = longest + 4;
        if (pw < 11) { pw = 11; }
        if (pw > 18) { pw = 18; }
        if (pw > W) { pw = W; }
        per = (W + 1 + (pw + 1) / 3) / (pw + 1);      /* pills may shrink to two thirds */
        if (per < 1) { per = 1; }
        pw = (W - (per - 1)) / per;          /* stretch to fill the row */
        need = (int)((ni + (size_t)per - 1) / (size_t)per);
        max_rows = ng > 0 ? H / 5 : H;
        if (max_rows < 1) { max_rows = 1; }
        ind_rows = need < max_rows ? need : max_rows;
        shown_i = (int)ni;
        if (need > max_rows) {
            shown_i = per * ind_rows - 1;        /* the last slot carries the note */
            if (shown_i < 0) { shown_i = 0; }
            hidden = (int)ni - shown_i;
        }
    }
    gh = H - ind_rows;
    if (ng > 0 && gh < 3) { gh = H; ind_rows = 0; shown_i = 0; hidden = (int)ni; }
    /* gauge grid: best panel shape */
    if (ng > 0) {
        for (k = 1; k <= (int)ng && k <= W / 10 + 1; k++) {
            int rr = ((int)ng + k - 1) / k;
            double pwid = (double)W / (double)k, ph = (double)gh / (double)rr;
            double sc = pwid / 26.0 < ph / 9.0 ? pwid / 26.0 : ph / 9.0;
            if (ph < 5.0) { sc *= 0.35; }
            if (pwid < 14.0) { sc *= 0.5; }
            if (sc > best) { best = sc; cols = k; rws = rr; }
        }
        if ((double)gh / (double)rws < 5.0 && gh >= 5) {
            rws = gh / 5;
            cols = ((int)ng + rws - 1) / rws;
            if (cols > W / 10 + 1) { cols = W / 10 + 1; }
        }
        shown_g = (int)ng < cols * rws ? (int)ng : cols * rws;
        rws = (shown_g + cols - 1) / cols;
        for (k = 0; k < shown_g; k++) {
            int row = k / cols, col = k % cols, cnt = shown_g - row * cols;
            int ya, yb, xa, xb;
            if (cnt > cols) { cnt = cols; }
            ya = gh * row / rws;
            yb = gh * (row + 1) / rws;
            xa = W * col / cnt;
            xb = W * (col + 1) / cnt - (col < cnt - 1 ? 1 : 0);
            v->rects[gi[k]] = lt_rect_make((x0 + xa) * 8, (y0 + ya) * 16, (xb - xa) * 8, (yb - ya) * 16);
        }
    }
    /* indicator pills */
    for (k = 0; k < shown_i; k++) {
        int row = k / per, col = k % per;
        v->rects[ii[k]] = lt_rect_make((x0 + col * (pw + 1)) * 8, (y0 + gh + row) * 16, pw * 8, 16);
    }
    if (hidden > 0) {
        int last = shown_i;
        *more_r = lt_rect_make((x0 + (last % per) * (pw + 1)) * 8, (y0 + gh + last / per) * 16, pw * 8, 16);
    }
    lt_free(gi);
    lt_free(ii);
    return hidden;
}

void lt_dash_view_set_cell_auto(lt_dash_view *v, int on)
{
    v->cell_auto = on;
}

void lt_dash_view_render(lt_dash_view *v, lt_gfx *g, lt_rect area, const lt_expr_env *env)
{
    const lt_dash_cluster *cl = &v->d->cluster;
    lt_rect cr = area, prev;
    size_t i;
    lt_bool show;

    if (cl->force_aspect && cl->force_aspect_width > 0.0 && cl->force_aspect_height > 0.0 &&
        area.w > 0 && area.h > 0) {
        double want = cl->force_aspect_width / cl->force_aspect_height;
        double have = (double)area.w / (double)area.h;
        if (have > want) {
            cr.h = area.h;
            cr.w = iround((double)area.h * want);
        } else {
            cr.w = area.w;
            cr.h = iround((double)area.w / want);
        }
        cr.x = area.x + (area.w - cr.w) / 2;
        cr.y = area.y + (area.h - cr.h) / 2;
        lt_gfx_fill_rect(g, area, LT_RGB(0, 0, 0));
    }
    prev = lt_gfx_push_clip(g, area);
    lt_gfx_fill_rect(g, cr, cl->background_color | 0xFF000000UL);
    show = lt_expr_cache_test(&v->cache, cl->enabled_condition, env);
    if (v->cell_auto && lt_gfx_cell_mode(g)) {
        lt_rect more = { 0, 0, 0, 0 };
        int hidden;
        char note[40];
        for (i = 0; i < v->n; i++) {
            const lt_dash_comp *c = &cl->comps[i];
            v->st[i].enabled = lt_expr_cache_test(&v->cache, c->gauge ? c->gauge->enabled_condition
                                                                      : c->indicator->enabled_condition, env);
        }
        hidden = show ? cell_layout(v, area, &more) : 0;
        if (!show) {
            more.x = more.y = more.w = more.h = 0;
            for (i = 0; i < v->n; i++) { v->rects[i].w = v->rects[i].h = 0; }
        }
        for (i = 0; i < v->n; i++) {
            const lt_dash_comp *c = &cl->comps[i];
            if (!show || v->rects[i].w <= 0) { continue; }
            if (c->gauge) paint_gauge(g, v->rects[i], c->gauge, &v->st[i], v->now_ms);
            else paint_indicator(g, v->rects[i], c->indicator, v->st[i].on);
        }
        if (hidden > 0 && more.w > 0) {
            lt_snprintf(note, sizeof note, " +%d more", hidden);
            lt_gfx_fill_rect(g, more, LT_RGB(26, 29, 35));
            lt_gfx_cell_text(g, more.x, more.y, note, LT_RGB(150, 160, 180), 0, 0);
        }
        lt_gfx_pop_clip(g, prev);
        return;
    }
    for (i = 0; i < v->n; i++) {
        const lt_dash_comp *c = &cl->comps[i];
        lt_dash_gstate *s = &v->st[i];
        double rx, ry, rw, rh;
        int shortest = 0;
        lt_rect r;

        if (c->gauge) {
            rx = c->gauge->relative_x; ry = c->gauge->relative_y;
            rw = c->gauge->relative_width; rh = c->gauge->relative_height;
            shortest = c->gauge->shortest_size;
            s->enabled = lt_expr_cache_test(&v->cache, c->gauge->enabled_condition, env);
        } else {
            rx = c->indicator->relative_x; ry = c->indicator->relative_y;
            rw = c->indicator->relative_width; rh = c->indicator->relative_height;
            s->enabled = lt_expr_cache_test(&v->cache, c->indicator->enabled_condition, env);
        }
        r.x = cr.x + iround(rx * (double)cr.w);
        r.y = cr.y + iround(ry * (double)cr.h);
        r.w = iround(rw * (double)cr.w);
        r.h = iround(rh * (double)cr.h);
        if (shortest > 0) {
            if (r.w < shortest) r.w = LT_MIN(shortest, cr.w);
            if (r.h < shortest) r.h = LT_MIN(shortest, cr.h);
        }
        if (r.w < 1) r.w = 1;
        if (r.h < 1) r.h = 1;
        v->rects[i] = r;
        if (!show || !s->enabled) continue;
        if (c->gauge) paint_gauge(g, r, c->gauge, s, v->now_ms);
        else paint_indicator(g, r, c->indicator, s->on);
    }
    lt_gfx_pop_clip(g, prev);
}

int lt_dash_view_hit(const lt_dash_view *v, int x, int y)
{
    size_t i;
    for (i = v->n; i > 0; i--) {
        if (v->st[i - 1].enabled && v->rects[i - 1].w > 0 &&
            lt_rect_contains(v->rects[i - 1], x, y))
            return (int)(i - 1);
    }
    return -1;
}

lt_rect lt_dash_view_component_rect(const lt_dash_view *v, size_t index)
{
    lt_rect z;
    z.x = z.y = z.w = z.h = 0;
    return index < v->n ? v->rects[index] : z;
}
