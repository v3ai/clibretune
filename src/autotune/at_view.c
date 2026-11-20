/* at_view.c - view helpers of the AutoTune tab without UI (see at_extra.h): heat map zoom and
 * pan arithmetic, the dyno overlay on the tuned table and the per-sample verdicts of a replay.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "at_extra.h"

/* ================================================================ zoom and pan */

int lt_atx_zoom_clamp(int pct)
{
    return LT_CLAMP(pct, LT_ATX_ZOOM_MIN, LT_ATX_ZOOM_MAX);
}

int lt_atx_zoom_step(int pct, int notches)
{
    double z = (double)lt_atx_zoom_clamp(pct);
    int i, n = notches < 0 ? -notches : notches, out;
    if (n > 40) { n = 40; }
    for (i = 0; i < n; i++) {
        z = notches > 0 ? z * 1.25 : z / 1.25;
        if (z > (double)LT_ATX_ZOOM_MAX) { z = (double)LT_ATX_ZOOM_MAX; }
        if (z < (double)LT_ATX_ZOOM_MIN) { z = (double)LT_ATX_ZOOM_MIN; }
    }
    out = (int)lt_round(z);
    if (out != pct && out >= 94 && out <= 106) { out = 100; }
    /* a step always moves while there is room (rounding could otherwise stall it) */
    if (notches > 0 && out <= pct && pct < LT_ATX_ZOOM_MAX) { out = pct + 1; }
    if (notches < 0 && out >= pct && pct > LT_ATX_ZOOM_MIN) { out = pct - 1; }
    return lt_atx_zoom_clamp(out);
}

int lt_atx_zoom_fit(int view_w, int view_h, int cols, int rows, int base_cw, int base_ch)
{
    long zw = 100L, zh = 100L;
    if (view_w <= 0 || view_h <= 0 || cols <= 0 || rows <= 0 || base_cw <= 0 || base_ch <= 0) {
        return LT_ATX_ZOOM_DEF;
    }
    if ((long)cols * base_cw > (long)view_w) { zw = (long)view_w * 100L / ((long)cols * base_cw); }
    if ((long)rows * base_ch > (long)view_h) { zh = (long)view_h * 100L / ((long)rows * base_ch); }
    return lt_atx_zoom_clamp((int)(zw < zh ? zw : zh));
}

int lt_atx_zoom_px(int base_px, int pct, int min_px)
{
    long v = (long)base_px * (long)lt_atx_zoom_clamp(pct) / 100L;
    if (v < (long)min_px) { v = (long)min_px; }
    if (v > 32767L) { v = 32767L; }
    return (int)v;
}

int lt_atx_zoom_font(int base_px, int pct)
{
    long v = ((long)base_px * (long)lt_atx_zoom_clamp(pct) + 50L) / 100L;
    return (int)LT_CLAMP(v, 8L, 22L);
}

int lt_atx_pan_clamp(int scroll, int content, int view)
{
    int mx = content - view;
    if (mx < 0) { mx = 0; }
    return LT_CLAMP(scroll, 0, mx);
}

int lt_atx_zoom_anchor(int scroll, int anchor, int old_cell, int new_cell, int count, int view)
{
    double pos, cells;
    long content;
    if (old_cell <= 0 || new_cell <= 0 || count <= 0) { return 0; }
    cells = ((double)scroll + (double)anchor) / (double)old_cell;      /* cell units under the anchor */
    pos = cells * (double)new_cell - (double)anchor;
    content = (long)count * (long)new_cell;
    if (content > 32767L * 1024L) { content = 32767L * 1024L; }
    if (pos < 0.0) { pos = 0.0; }
    if (pos > (double)content) { pos = (double)content; }
    return lt_atx_pan_clamp((int)lt_round(pos), (int)content, view);
}

int lt_atx_label_stride(int cell_px, int min_px)
{
    if (cell_px <= 0) { return 1; }
    if (cell_px >= min_px) { return 1; }
    return (min_px + cell_px - 1) / cell_px;
}

/* ================================================================ dyno overlay */

const char *lt_atx_dyno_name(int what)
{
    switch (what) {
    case LT_ATX_DYNO_TQ: return "Torque";
    case LT_ATX_DYNO_AFR: return "AFR";
    default: return "Power";
    }
}

void lt_atx_dyno_index(const lt_dyno_cell *cells, size_t n, size_t rows, size_t cols, int *idx)
{
    size_t i, k = rows * cols;
    for (i = 0; i < k; i++) { idx[i] = -1; }
    for (i = 0; i < n && i < 32767u; i++) {
        if (cells[i].row < rows && cells[i].col < cols) { idx[cells[i].row * cols + cells[i].col] = (int)i; }
    }
}

lt_bool lt_atx_dyno_value(const lt_dyno_cell *c, int what, double *v)
{
    if (c == NULL) { return LT_FALSE; }
    if (what == LT_ATX_DYNO_TQ && c->has_tq) { *v = c->tq_val; return LT_TRUE; }
    if (what == LT_ATX_DYNO_AFR && c->has_afr) { *v = c->afr; return LT_TRUE; }
    if (what == LT_ATX_DYNO_HP && c->has_hp) { *v = c->hp; return LT_TRUE; }
    return LT_FALSE;
}

lt_bool lt_atx_dyno_range(const lt_dyno_cell *cells, size_t n, int what, double *lo, double *hi)
{
    size_t i;
    lt_bool any = LT_FALSE;
    double v = 0.0;
    *lo = 0.0; *hi = 0.0;
    for (i = 0; i < n; i++) {
        if (!lt_atx_dyno_value(&cells[i], what, &v)) { continue; }
        if (!any || v < *lo) { *lo = v; }
        if (!any || v > *hi) { *hi = v; }
        any = LT_TRUE;
    }
    return any;
}

void lt_atx_dyno_label(const lt_dyno_cell *c, int what, char *out, size_t cap)
{
    double v = 0.0;
    if (cap == 0) { return; }
    out[0] = '\0';
    if (!lt_atx_dyno_value(c, what, &v)) { return; }
    if (what == LT_ATX_DYNO_AFR) { lt_snprintf(out, cap, "%.1f", v); }
    else { lt_snprintf(out, cap, what == LT_ATX_DYNO_TQ ? "%.0f ft" : "%.0f hp", v); }
}

/* ================================================================ verdicts */

void lt_atx_verdicts_init(lt_atx_verdicts *v)
{
    memset(v, 0, sizeof(*v));
}

void lt_atx_verdicts_free(lt_atx_verdicts *v)
{
    lt_free(v->rows);
    lt_atx_verdicts_init(v);
}

static size_t vd_nearest(const double *b, size_t n, double x)
{
    size_t i, best = 0;
    double bd;
    if (n == 0 || b == NULL) { return 0; }
    bd = fabs(b[0] - x);
    for (i = 1; i < n; i++) {
        double d = fabs(b[i] - x);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static int vd_col(const lt_log *log, const char *name)
{
    return name != NULL && name[0] != '\0' ? lt_log_channel_index(log, name) : -1;
}

/* reason slot for the string (adds it); the last slot collects the overflow */
static int vd_slot(lt_atx_verdicts *v, const char *r)
{
    size_t i;
    for (i = 0; i < v->nreason; i++) {
        if (v->reason[i] == r || strcmp(v->reason[i], r) == 0) { return (int)i; }
    }
    if (v->nreason + 1 < LT_ATX_VREASONS) {
        v->reason[v->nreason] = r;
        return (int)v->nreason++;
    }
    v->reason[LT_ATX_VREASONS - 1] = "other";
    v->nreason = LT_ATX_VREASONS;
    return LT_ATX_VREASONS - 1;
}

int lt_atx_verdicts_build(lt_atx_verdicts *v, const lt_replay_report *rep, const lt_log *log,
                          const lt_at_channels *ch, const double *xbins, size_t nx,
                          const double *ybins, size_t ny)
{
    size_t i, j, n;
    int crpm, cload, cafr, map[LT_ATX_VREASONS];
    lt_atx_verdicts_free(v);
    if (rep == NULL || log == NULL || rep->verdicts == NULL) { lt_err_set("no verdicts"); return -1; }
    n = rep->n_verdicts;
    if (log->n >= 0 && (size_t)log->n < n) { n = (size_t)log->n; }
    crpm = vd_col(log, ch->rpm);
    cload = vd_col(log, ch->load);
    cafr = vd_col(log, ch->afr);
    v->rows = (lt_atx_vrow *)lt_calloc(n + 1, sizeof(lt_atx_vrow));
    v->n = n;
    for (i = 0; i < n; i++) {
        lt_atx_vrow *r = &v->rows[i];
        r->t = log->t != NULL ? log->t[i] : 0.0;
        r->rpm = crpm >= 0 ? LT_LOG_AT(log, i, crpm) : 0.0;
        r->load = cload >= 0 ? LT_LOG_AT(log, i, cload) : 0.0;
        r->afr = cafr >= 0 ? LT_LOG_AT(log, i, cafr) : 0.0;
        if (ch->afr_is_lambda) { r->afr = lt_at_normalise_to_afr(r->afr); }
        r->cx = (unsigned)vd_nearest(xbins, nx, r->rpm);
        r->cy = (unsigned)vd_nearest(ybins, ny, r->load);
        if (rep->verdicts[i] == NULL) {
            r->reason = -1;
            v->counted++;
        } else {
            r->reason = vd_slot(v, rep->verdicts[i]);
            v->count[r->reason]++;
        }
    }
    /* order the reasons by count (desc), then name, and renumber the rows */
    for (i = 0; i < v->nreason; i++) { map[i] = (int)i; }
    for (i = 1; i < v->nreason; i++) {
        for (j = i; j > 0; j--) {
            int a = map[j - 1], b = map[j], sw;
            sw = v->count[b] > v->count[a] ||
                 (v->count[b] == v->count[a] && strcmp(v->reason[b], v->reason[a]) < 0);
            if (!sw) { break; }
            map[j - 1] = b; map[j] = a;
        }
    }
    {
        const char *rs[LT_ATX_VREASONS];
        unsigned long cs[LT_ATX_VREASONS];
        int inv[LT_ATX_VREASONS];
        for (i = 0; i < v->nreason; i++) {
            rs[i] = v->reason[map[i]];
            cs[i] = v->count[map[i]];
            inv[map[i]] = (int)i;
        }
        for (i = 0; i < v->nreason; i++) { v->reason[i] = rs[i]; v->count[i] = cs[i]; }
        for (i = 0; i < n; i++) {
            if (v->rows[i].reason >= 0) { v->rows[i].reason = inv[v->rows[i].reason]; }
        }
    }
    return 0;
}

lt_bool lt_atx_verdict_match(const lt_atx_verdicts *v, size_t i, const lt_atx_vfilter *f)
{
    const lt_atx_vrow *r;
    if (i >= v->n) { return LT_FALSE; }
    r = &v->rows[i];
    if (f->cx >= 0 && r->cx != (unsigned)f->cx) { return LT_FALSE; }
    if (f->cy >= 0 && r->cy != (unsigned)f->cy) { return LT_FALSE; }
    switch (f->kind) {
    case LT_ATX_VF_COUNTED: return r->reason < 0 ? LT_TRUE : LT_FALSE;
    case LT_ATX_VF_DROPPED: return r->reason >= 0 ? LT_TRUE : LT_FALSE;
    case LT_ATX_VF_REASON: return r->reason == f->reason ? LT_TRUE : LT_FALSE;
    default: return LT_TRUE;
    }
}

size_t lt_atx_verdicts_select(const lt_atx_verdicts *v, const lt_atx_vfilter *f, size_t *idx,
                              size_t cap)
{
    size_t i, k = 0;
    for (i = 0; i < v->n; i++) {
        if (!lt_atx_verdict_match(v, i, f)) { continue; }
        if (idx != NULL && k < cap) { idx[k] = i; }
        k++;
    }
    return k;
}

int lt_atx_verdict_dominant(const lt_atx_verdicts *v, size_t from, size_t to)
{
    unsigned long tally[LT_ATX_VREASONS + 1];
    size_t i;
    int best = -2;
    unsigned long bn = 0;
    if (to > v->n) { to = v->n; }
    if (from >= to) { return -2; }
    memset(tally, 0, sizeof(tally));
    for (i = from; i < to; i++) {
        int r = v->rows[i].reason;
        tally[r < 0 ? 0 : (size_t)r + 1]++;
    }
    for (i = 0; i <= LT_ATX_VREASONS; i++) {
        if (tally[i] > bn) { bn = tally[i]; best = (int)i - 1; }
    }
    return best;
}

size_t lt_atx_verdict_row_at(size_t n, int w, int x)
{
    double f;
    size_t r;
    if (n == 0 || w <= 0) { return 0; }
    if (x < 0) { x = 0; }
    if (x >= w) { x = w - 1; }
    f = (double)x * (double)n / (double)w;
    r = (size_t)f;
    return r < n ? r : n - 1;
}
