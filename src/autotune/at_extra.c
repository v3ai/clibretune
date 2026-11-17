/* at_extra.c - AutoTune extras without UI (see at_extra.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "at_extra.h"

/* ================================================================ heat map views */

const char *lt_atx_map_name(int mode)
{
    switch (mode) {
    case LT_ATX_MAP_WEIGHT: return "Hit weighting";
    case LT_ATX_MAP_CHANGE: return "Change";
    case LT_ATX_MAP_COVERAGE: return "Coverage";
    default: return "Value";
    }
}

void lt_atx_map_maxima(const lt_at_rec *recs, size_t n, double *max_hits, double *max_weight,
                       double *max_change)
{
    size_t i;
    double h = 0.0, w = 0.0, c = 0.0, d;
    for (i = 0; i < n; i++) {
        if ((double)recs[i].hit_count > h) { h = (double)recs[i].hit_count; }
        if (recs[i].hit_weighting > w) { w = recs[i].hit_weighting; }
        d = fabs(recs[i].recommended_value - recs[i].beginning_value);
        if (d > c) { c = d; }
    }
    *max_hits = h; *max_weight = w; *max_change = c;
}

double lt_atx_map_fraction(int mode, const lt_at_rec *r, double max_hits, double max_weight,
                           double max_change)
{
    double f = 0.0;
    if (r == NULL) { return 0.0; }
    switch (mode) {
    case LT_ATX_MAP_WEIGHT:
        if (max_weight > 0.0) { f = r->hit_weighting / max_weight; }
        break;
    case LT_ATX_MAP_CHANGE:
        if (max_change > 0.0) { f = fabs(r->recommended_value - r->beginning_value) / max_change; }
        break;
    case LT_ATX_MAP_COVERAGE:
        if (max_hits > 0.0) { f = (double)r->hit_count / max_hits; }
        break;
    default:
        break;
    }
    return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
}

void lt_atx_trace_clear(lt_atx_trace *t) { t->n = 0; t->head = 0; }

size_t lt_atx_trace_count(const lt_atx_trace *t) { return t->n; }

void lt_atx_trace_push(lt_atx_trace *t, unsigned x, unsigned y)
{
    size_t last;
    if (t->n > 0) {
        last = (t->head + LT_ATX_TRACE_MAX - 1) % LT_ATX_TRACE_MAX;
        if (t->x[last] == x && t->y[last] == y) { return; }
    }
    t->x[t->head] = x; t->y[t->head] = y;
    t->head = (t->head + 1) % LT_ATX_TRACE_MAX;
    if (t->n < LT_ATX_TRACE_MAX) { t->n++; }
}

int lt_atx_trace_at(const lt_atx_trace *t, size_t i, unsigned *x, unsigned *y)
{
    size_t k;
    if (i >= t->n) { return 0; }
    k = (t->head + LT_ATX_TRACE_MAX - t->n + i) % LT_ATX_TRACE_MAX;
    *x = t->x[k]; *y = t->y[k];
    return 1;
}

/* ================================================================ load source */

const char *lt_atx_load_name(int src)
{
    switch (src) {
    case LT_ATX_LOAD_MAF: return "MAF";
    case LT_ATX_LOAD_TPS: return "TPS (Alpha-N)";
    default: return "MAP";
    }
}

static int has_ci(const char *s, const char *sub)
{
    size_t n = strlen(sub), i, j, m = strlen(s);
    for (i = 0; i + n <= m; i++) {
        for (j = 0; j < n; j++) {
            char a = s[i + j], b = sub[j];
            if (a >= 'A' && a <= 'Z') { a = (char)(a - 'A' + 'a'); }
            if (a != b) { break; }
        }
        if (j == n) { return 1; }
    }
    return 0;
}

int lt_atx_load_detect(const char *y)
{
    if (y == NULL || y[0] == '\0') { return LT_ATX_LOAD_MAP; }
    if (has_ci(y, "tps") || has_ci(y, "throttle")) { return LT_ATX_LOAD_TPS; }
    if (has_ci(y, "maf")) { return LT_ATX_LOAD_MAF; }
    return LT_ATX_LOAD_MAP;
}

int lt_atx_load_apply(lt_at_channels *ch, int src)
{
    const char *n = ch->map;
    if (src == LT_ATX_LOAD_MAF) { n = ch->maf; }
    else if (src == LT_ATX_LOAD_TPS) { n = ch->tps; }
    if (n[0] == '\0') { return 0; }
    lt_strlcpy(ch->load, n, sizeof(ch->load));
    return 1;
}

/* ================================================================ export */

void lt_atx_csv(lt_buf *out, const lt_at_rec *recs, size_t n, const double *xbins, size_t nx,
                const double *ybins, size_t ny)
{
    size_t i;
    lt_buf_puts(out, "cell_x,cell_y,x_value,y_value,beginning_value,recommended_value,change,"
                     "change_percent,hit_count,hit_weighting,target_afr\n");
    for (i = 0; i < n; i++) {
        const lt_at_rec *r = &recs[i];
        double d = r->recommended_value - r->beginning_value;
        double pct = r->beginning_value != 0.0 ? d / r->beginning_value * 100.0 : 0.0;
        double xv = r->cell_x < nx ? xbins[r->cell_x] : 0.0;
        double yv = r->cell_y < ny ? ybins[r->cell_y] : 0.0;
        lt_buf_printf(out, "%u,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%u,%.4f,%.3f\n", r->cell_x, r->cell_y,
                      xv, yv, r->beginning_value, r->recommended_value, d, pct, r->hit_count,
                      r->hit_weighting, r->target_afr);
    }
}

size_t lt_atx_proposed(const double *cur, size_t rows, size_t cols, const lt_at_rec *recs,
                       size_t n, double *out)
{
    size_t i, changed = 0;
    memcpy(out, cur, rows * cols * sizeof(double));
    for (i = 0; i < n; i++) {
        size_t k;
        if (recs[i].cell_y >= rows || recs[i].cell_x >= cols) { continue; }
        k = recs[i].cell_y * cols + recs[i].cell_x;
        if (fabs(recs[i].recommended_value - cur[k]) > 1e-9) { changed++; }
        out[k] = recs[i].recommended_value;
    }
    return changed;
}

static void json_str(lt_buf *b, const char *s)
{
    lt_buf_putc(b, '"');
    for (; s != NULL && *s != '\0'; s++) {
        if (*s == '"' || *s == '\\') { lt_buf_putc(b, '\\'); lt_buf_putc(b, *s); }
        else if ((unsigned char)*s < 32) { lt_buf_putc(b, ' '); }
        else { lt_buf_putc(b, *s); }
    }
    lt_buf_putc(b, '"');
}

static void json_grid(lt_buf *b, const double *z, size_t rows, size_t cols)
{
    size_t y, x;
    lt_buf_putc(b, '[');
    for (y = 0; y < rows; y++) {
        lt_buf_puts(b, y ? ",\n    [" : "\n    [");
        for (x = 0; x < cols; x++) { lt_buf_printf(b, x ? ", %.4f" : "%.4f", z[y * cols + x]); }
        lt_buf_putc(b, ']');
    }
    lt_buf_puts(b, "\n  ]");
}

size_t lt_atx_proposal_json(lt_buf *out, const char *table, const char *signature, const double *cur,
                            size_t rows, size_t cols, const lt_at_rec *recs, size_t n)
{
    double *prop = (double *)lt_calloc(rows * cols + 1, sizeof(double));
    size_t i, changed = lt_atx_proposed(cur, rows, cols, recs, n, prop);
    double largest = 0.0;
    for (i = 0; i < n; i++) {
        double d = recs[i].recommended_value - recs[i].beginning_value;
        if (fabs(d) > fabs(largest)) { largest = d; }
    }
    lt_buf_puts(out, "{\n  \"tableName\": "); json_str(out, table);
    lt_buf_puts(out, ",\n  \"definitionSignature\": "); json_str(out, signature);
    lt_buf_printf(out, ",\n  \"cellsChanged\": %lu,\n  \"largestDelta\": %.4f,\n  \"proposed\": ",
                  (unsigned long)changed, largest);
    json_grid(out, prop, rows, cols);
    lt_buf_puts(out, ",\n  \"current\": ");
    json_grid(out, cur, rows, cols);
    lt_buf_puts(out, ",\n  \"recommendations\": [");
    for (i = 0; i < n; i++) {
        lt_buf_printf(out, "%s\n    {\"cellX\": %u, \"cellY\": %u, \"beginningValue\": %.4f, "
                           "\"recommendedValue\": %.4f, \"hitCount\": %u, \"hitWeighting\": %.4f}",
                      i ? "," : "", recs[i].cell_x, recs[i].cell_y, recs[i].beginning_value,
                      recs[i].recommended_value, recs[i].hit_count, recs[i].hit_weighting);
    }
    lt_buf_puts(out, "\n  ]\n}\n");
    lt_free(prop);
    return changed;
}

/* ================================================================ delay test */

double lt_atx_clamp_step(double pct)
{
    if (!(pct >= LT_ATX_STEP_MIN_PCT)) { return LT_ATX_STEP_MIN_PCT; }
    return pct > LT_ATX_STEP_MAX_PCT ? LT_ATX_STEP_MAX_PCT : pct;
}

unsigned long lt_atx_clamp_hold(double ms)
{
    if (!(ms >= (double)LT_ATX_HOLD_MIN_MS)) { return LT_ATX_HOLD_MIN_MS; }
    return ms > (double)LT_ATX_HOLD_MAX_MS ? LT_ATX_HOLD_MAX_MS : (unsigned long)ms;
}

int lt_atx_clamp_repeats(double n)
{
    if (!(n >= 1.0)) { return 1; }
    return n > (double)LT_ATX_MAX_REPEATS ? LT_ATX_MAX_REPEATS : (int)n;
}

double lt_atx_step_value(double baseline, double pct)
{
    return baseline * (1.0 + lt_atx_clamp_step(pct) / 100.0);
}

void lt_atx_delays_init(lt_atx_delays *d)
{
    memset(d, 0, sizeof(*d));
    lt_delay_table_init(&d->table);
}

void lt_atx_delays_add(lt_atx_delays *d, double rpm, double load, double delay_ms)
{
    if (d->n >= LT_ATX_MAX_REPEATS) { return; }
    d->rpm[d->n] = rpm; d->load[d->n] = load; d->ms[d->n] = delay_ms;
    if (d->n == 0 || delay_ms < d->mn) { d->mn = delay_ms; }
    if (d->n == 0 || delay_ms > d->mx) { d->mx = delay_ms; }
    d->sum += delay_ms;
    d->n++;
    lt_delay_table_add(&d->table, rpm, load, delay_ms);
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

double lt_atx_delays_median(const lt_atx_delays *d)
{
    double *t;
    double m;
    if (d->n == 0) { return 0.0; }
    t = (double *)lt_malloc(d->n * sizeof(double));
    memcpy(t, d->ms, d->n * sizeof(double));
    qsort(t, d->n, sizeof(double), cmp_dbl);
    m = (d->n & 1u) ? t[d->n / 2] : 0.5 * (t[d->n / 2 - 1] + t[d->n / 2]);
    lt_free(t);
    return m;
}

int lt_atx_delay_model(const lt_atx_delays *d, lt_flow_fit *fit)
{
    memset(fit, 0, sizeof(*fit));
    if (d->n < 3) { return -1; }
    return lt_fit_flow_delay(d->rpm, d->load, d->ms, d->n, fit) ? 0 : -1;
}

void lt_atx_delay_model_json(lt_buf *out, const lt_flow_fit *fit)
{
    lt_buf_printf(out, "{\n  \"floorMs\": %.2f,\n  \"k\": %.6f,\n  \"anchorMs\": %.2f,\n"
                       "  \"rmsMs\": %.2f,\n  \"samples\": %lu\n}\n",
                  fit->floor_ms, fit->k, fit->anchor_ms, fit->rms_ms, (unsigned long)fit->samples);
}

/* ================================================================ analysis report */

void lt_atx_replay_report(lt_buf *out, const lt_replay_report *rep, size_t n_declared_filters)
{
    size_t i, rejected = 0, cells_cov = 0;
    unsigned long accepted;
    for (i = 0; i < rep->n_rejections; i++) { rejected += (size_t)rep->rejections[i].count; }
    accepted = rep->samples_all > (unsigned long)rejected ? rep->samples_all - (unsigned long)rejected : 0ul;
    lt_buf_printf(out, "Samples in log: %lu\nCounted: %lu (%.0f%%)\nDropped: %lu\n", rep->samples_all, accepted,
                  rep->samples_all ? 100.0 * (double)accepted / (double)rep->samples_all : 0.0,
                  (unsigned long)rejected);
    for (i = 0; i < rep->n_rejections && i < 8; i++) {
        lt_buf_printf(out, "  %s: %lu\n", rep->rejections[i].reason, rep->rejections[i].count);
    }
    if (rep->coverage != NULL) {
        for (i = 0; i < rep->nx * rep->ny; i++) { if (rep->coverage[i] > 0) { cells_cov++; } }
        lt_buf_printf(out, "Cells with samples: %lu of %lu\n", (unsigned long)cells_cov,
                      (unsigned long)(rep->nx * rep->ny));
    }
    lt_buf_printf(out, "Cells proposed: %lu\nINI analyze filters in use: %lu\n", (unsigned long)rep->ncells,
                  (unsigned long)n_declared_filters);
    if (rep->has_validation) {
        lt_buf_printf(out, "Validation on unseen samples (%lu folds, %lu scored): error %s by %.1f%%, "
                           "%.0f%% of cells worse\n",
                      rep->validation.folds, (unsigned long)rep->validation.scored,
                      rep->validation.gain_pct >= 0.0 ? "reduced" : "increased", fabs(rep->validation.gain_pct),
                      rep->validation.worsened_pct);
    } else {
        lt_buf_puts(out, "Validation: not enough data\n");
    }
}

static int col_of(const lt_log *log, const char *name)
{
    return name != NULL && name[0] != '\0' ? lt_log_channel_index(log, name) : -1;
}

int lt_atx_ae_report(lt_buf *out, const lt_log *log, const lt_at_channels *ch, double target_afr)
{
    int ct = col_of(log, ch->tps_rate), ca = col_of(log, ch->afr), cc = col_of(log, ch->clt);
    lt_ae_sample *s;
    lt_ae_config cfg;
    lt_ae_event *ev;
    lt_ae_bin_rec rec[8];
    size_t i, n, nev, nrec;
    if (ct < 0 || ca < 0 || log->n < 2) { return -1; }
    n = (size_t)log->n;
    s = (lt_ae_sample *)lt_calloc(n, sizeof(lt_ae_sample));
    for (i = 0; i < n; i++) {
        double a = LT_LOG_AT(log, i, ca);
        s[i].timestamp_ms = (unsigned long)(log->t[i] * 1000.0);
        s[i].tps_dot = LT_LOG_AT(log, i, ct);
        s[i].afr = ch->afr_is_lambda ? lt_at_normalise_to_afr(a) : a;
        s[i].afr_target = target_afr;
        s[i].clt = cc >= 0 ? LT_LOG_AT(log, i, cc) : 90.0;
    }
    lt_ae_config_default(&cfg);
    ev = (lt_ae_event *)lt_calloc(256, sizeof(lt_ae_event));
    nev = lt_ae_analyze(s, n, &cfg, ev, 256);
    lt_buf_printf(out, "Acceleration events: %lu\n", (unsigned long)nev);
    nrec = lt_ae_recommend(ev, nev, &cfg, rec, 8);
    for (i = 0; i < nrec; i++) {
        if (rec[i].has_hi) { lt_buf_printf(out, "  TPS rate %.0f-%.0f: ", rec[i].lo, rec[i].hi); }
        else { lt_buf_printf(out, "  TPS rate %.0f+: ", rec[i].lo); }
        if (rec[i].sufficient_data) {
            lt_buf_printf(out, "%lu events, residual %+.2f AFR, rate %.1f -> %.1f\n", (unsigned long)rec[i].events,
                          rec[i].median_residual_afr, rec[i].current_rate, rec[i].recommended_rate);
        } else {
            lt_buf_printf(out, "%lu events, %s\n", (unsigned long)rec[i].events,
                          rec[i].note[0] ? rec[i].note : "too few");
        }
    }
    lt_free(ev);
    lt_free(s);
    return (int)nev;
}
