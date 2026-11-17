/* at_replay.c - replay a datalog through the AutoTune engine (spec 07 section 8).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "autotune.h"

#define RP_BLOCK_MS 10000UL
#define RP_FOLDS 5
#define RP_STEADY_MS 800.0
#define RP_LOOKBACK 512

void lt_replay_config_default(lt_replay_config *c)
{
    memset(c, 0, sizeof *c);
    lt_at_settings_default(&c->settings);
    lt_at_filters_default(&c->filters);
    lt_at_authority_default(&c->authority);
    c->lambda_match_strict = LT_TRUE;
    c->validate = LT_TRUE;
}

static int col_of(const lt_log *log, const char *name)
{
    int i;
    if (!name || !name[0]) {
        return -1;
    }
    for (i = 0; i < log->n_ch; i++) {
        if (log->names[i] && strcmp(log->names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static size_t nearest_bin(const double *b, size_t n, double v)
{
    size_t i, best = 0;
    double bd, d;
    if (n == 0) {
        return 0;
    }
    bd = fabs(b[0] - v);
    for (i = 1; i < n; i++) {
        d = fabs(b[i] - v);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

static unsigned long to_ms(double t_sec)
{
    double ms = lt_round(t_sec * 1000.0);
    if (lt_isnan(ms) || ms <= 0.0) {
        return 0UL;
    }
    if (ms >= 4294967295.0) {
        return 4294967295UL;
    }
    return (unsigned long)ms;
}

typedef struct rp_ctx {
    long n;
    lt_ve_point *pts;
    double *afr;            /* normalised measured AFR per sample */
    size_t nx, ny;
    const double *xbins, *ybins;
    const lt_at_reftables *ref;
    const lt_replay_config *cfg;
} rp_ctx;

static lt_at_state *run(const rp_ctx *c, int fold, const char **verdicts, unsigned *coverage,
                        double *afr_sum, unsigned *afr_n)
{
    lt_at_state *s = lt_at_new();
    long i;
    const char *reason;
    lt_at_set_reference_tables(s, c->ref);
    lt_at_set_strict_lambda_match(s, c->cfg->lambda_match_strict);
    lt_at_start(s);
    for (i = 0; i < c->n; i++) {
        const lt_ve_point *p = &c->pts[i];
        if (fold >= 0 && (int)((p->timestamp_ms / RP_BLOCK_MS) % RP_FOLDS) == fold) {
            if (verdicts) {
                verdicts[i] = "not in this fold";
            }
            continue;
        }
        if (lt_at_classify(s, p, &c->cfg->filters, &reason) == 0) {
            size_t cx = nearest_bin(c->xbins, c->nx, p->rpm);
            size_t cy = nearest_bin(c->ybins, c->ny, p->load);
            if (coverage && cx < c->nx && cy < c->ny) {
                coverage[cy * c->nx + cx]++;
            }
            if (afr_sum && cx < c->nx && cy < c->ny) {
                afr_sum[cy * c->nx + cx] += c->afr[i];
                afr_n[cy * c->nx + cx]++;
            }
            if (verdicts) {
                verdicts[i] = NULL;
            }
        } else if (verdicts) {
            verdicts[i] = reason;
        }
        lt_at_add_data_point(s, p, c->xbins, c->nx, c->ybins, c->ny, &c->cfg->settings,
                             &c->cfg->filters, &c->cfg->authority);
    }
    return s;
}

static void steady_mask(const rp_ctx *c, unsigned char *mask)
{
    long i, j, lo;
    for (i = 0; i < c->n; i++) {
        const lt_ve_point *p = &c->pts[i];
        double start;
        int steady = 1, reached = 0;
        mask[i] = 0;
        if (p->rpm < 1.0 || !(c->afr[i] >= LT_AT_AFR_RAIL_LOW && c->afr[i] <= LT_AT_AFR_RAIL_HIGH)) {
            continue;
        }
        start = (double)p->timestamp_ms - RP_STEADY_MS;
        lo = i - RP_LOOKBACK;
        if (lo < 0) {
            lo = 0;
        }
        for (j = i; j >= lo; j--) {
            const lt_ve_point *q = &c->pts[j];
            if ((double)q->timestamp_ms < start) {
                reached = 1;
                break;
            }
            if (fabs(q->rpm - p->rpm) > LT_AT_STEADY_RPM_TOLERANCE ||
                fabs(q->load - p->load) > LT_AT_STEADY_LOAD_TOLERANCE || q->fuel_cut_active == 1) {
                steady = 0;
                break;
            }
        }
        mask[i] = (unsigned char)(steady && reached);
    }
}

static lt_bool validate(const rp_ctx *c, lt_validation *out)
{
    unsigned char *mask;
    int fold;
    long i;
    double before = 0.0, after = 0.0;
    unsigned long scored = 0, worse = 0;
    size_t ncell = c->nx * c->ny;
    if (c->n == 0 || ncell == 0) {
        return LT_FALSE;
    }
    mask = (unsigned char *)lt_malloc((size_t)c->n);
    steady_mask(c, mask);
    for (fold = 0; fold < RP_FOLDS; fold++) {
        lt_at_state *s = run(c, fold, NULL, NULL, NULL, NULL);
        size_t nrec = lt_at_recommendations(s, NULL, 0);
        lt_at_rec *recs = (lt_at_rec *)lt_malloc((nrec + 1) * sizeof *recs);
        double *cur = (double *)lt_calloc(ncell, sizeof(double));
        double *prop = (double *)lt_calloc(ncell, sizeof(double));
        unsigned char *has = (unsigned char *)lt_calloc(ncell, 1);
        size_t k;
        lt_at_recommendations(s, recs, nrec);
        for (k = 0; k < nrec; k++) {
            size_t idx = (size_t)recs[k].cell_y * c->nx + recs[k].cell_x;
            if (recs[k].cell_x < c->nx && recs[k].cell_y < c->ny) {
                cur[idx] = recs[k].beginning_value;
                prop[idx] = recs[k].recommended_value;
                has[idx] = 1;
            }
        }
        for (i = 0; i < c->n; i++) {
            const lt_ve_point *p = &c->pts[i];
            size_t cx, cy, idx;
            double target, e0, e1, v;
            if ((int)((p->timestamp_ms / RP_BLOCK_MS) % RP_FOLDS) != fold || !mask[i]) {
                continue;
            }
            cx = nearest_bin(c->xbins, c->nx, p->rpm);
            cy = nearest_bin(c->ybins, c->ny, p->load);
            idx = cy * c->nx + cx;
            if (!has[idx] || cur[idx] <= 0.0 || prop[idx] <= 0.0) {
                continue;
            }
            target = c->cfg->settings.target_afr;
            if (c->ref && c->ref->target_afr && cx < c->ref->nx && cy < c->ref->ny) {
                v = c->ref->target_afr[cy * c->ref->nx + cx];
                if (v > 0.1) {
                    target = lt_at_normalise_to_afr(v);
                }
            }
            e0 = fabs(c->afr[i] - target);
            e1 = fabs(c->afr[i] * cur[idx] / prop[idx] - target);
            before += e0;
            after += e1;
            scored++;
            if (e1 > e0 + 1e-9) {
                worse++;
            }
        }
        lt_free(recs);
        lt_free(cur);
        lt_free(prop);
        lt_free(has);
        lt_at_free(s);
    }
    lt_free(mask);
    if (scored == 0 || !(before > 0.0)) {
        return LT_FALSE;
    }
    out->gain_pct = 100.0 * (before - after) / before;
    out->worsened_pct = 100.0 * (double)worse / (double)scored;
    out->scored = (size_t)scored;
    out->folds = RP_FOLDS;
    return LT_TRUE;
}

int lt_replay(const lt_log *log, const lt_at_channels *ch, const double *xbins, size_t nx,
              const double *ybins, size_t ny, const lt_at_reftables *ref,
              const lt_replay_config *cfg, lt_replay_report *out)
{
    int c_rpm, c_load, c_afr, c_ve, c_clt, c_tps, c_tpsr, c_fc, c_ae, c_map, c_maf;
    rp_ctx c;
    long i;
    lt_at_state *s;
    size_t k, ncell = nx * ny, nrec, nrej;
    lt_at_rec *recs;
    double *afr_sum;
    unsigned *afr_n;
    lt_at_reject *rej;

    memset(out, 0, sizeof *out);
    out->nx = nx;
    out->ny = ny;
    c_rpm = col_of(log, ch->rpm);
    c_load = col_of(log, ch->load);
    c_afr = col_of(log, ch->afr);
    if (c_rpm < 0 || c_load < 0 || c_afr < 0) {
        lt_err_set("log lacks a required channel (%s%s%s)", c_rpm < 0 ? "rpm " : "",
                   c_load < 0 ? "load " : "", c_afr < 0 ? "afr" : "");
        return LT_ENOENT;
    }
    c_ve = col_of(log, ch->ve);
    c_clt = col_of(log, ch->clt);
    c_tps = col_of(log, ch->tps);
    c_tpsr = col_of(log, ch->tps_rate);
    c_fc = col_of(log, ch->fuel_cut);
    c_ae = col_of(log, ch->accel_enrich);
    c_map = col_of(log, ch->map);
    c_maf = col_of(log, ch->maf);

    c.n = log->n;
    c.pts = (lt_ve_point *)lt_malloc((size_t)(c.n + 1) * sizeof(lt_ve_point));
    c.afr = (double *)lt_malloc((size_t)(c.n + 1) * sizeof(double));
    c.nx = nx;
    c.ny = ny;
    c.xbins = xbins;
    c.ybins = ybins;
    c.ref = ref;
    c.cfg = cfg;
    for (i = 0; i < c.n; i++) {
        lt_ve_point *p = &c.pts[i];
        double a = LT_LOG_AT(log, i, c_afr);
        lt_ve_point_default(p);
        if (ch->afr_is_lambda) {
            a = lt_at_normalise_to_afr(a);
        }
        c.afr[i] = a;
        p->rpm = LT_LOG_AT(log, i, c_rpm);
        p->load = LT_LOG_AT(log, i, c_load);
        p->map = p->load;
        p->afr = a;
        p->ve = c_ve >= 0 ? LT_LOG_AT(log, i, c_ve) : 0.0;
        p->clt = c_clt >= 0 ? LT_LOG_AT(log, i, c_clt) : 90.0;
        p->tps = c_tps >= 0 ? LT_LOG_AT(log, i, c_tps) : 0.0;
        p->tps_rate = c_tpsr >= 0 ? LT_LOG_AT(log, i, c_tpsr) : 0.0;
        if (c_map >= 0) {
            p->map = LT_LOG_AT(log, i, c_map);
        }
        if (c_maf >= 0) {
            p->maf = LT_LOG_AT(log, i, c_maf);
        }
        if (c_fc >= 0) {
            p->fuel_cut_active = LT_LOG_AT(log, i, c_fc) > 0.5 ? 1 : 0;
        }
        if (c_ae >= 0) {
            p->accel_enrich_active = LT_LOG_AT(log, i, c_ae) > 0.5 ? 1 : 0;
        }
        p->timestamp_ms = to_ms(log->t[i]);
    }
    if (c.n == 0) {
        lt_free(c.pts);
        lt_free(c.afr);
        return 0;
    }

    out->verdicts = (const char **)lt_calloc((size_t)c.n, sizeof(char *));
    out->n_verdicts = (size_t)c.n;
    out->coverage = (unsigned *)lt_calloc(ncell ? ncell : 1, sizeof(unsigned));
    afr_sum = (double *)lt_calloc(ncell ? ncell : 1, sizeof(double));
    afr_n = (unsigned *)lt_calloc(ncell ? ncell : 1, sizeof(unsigned));
    s = run(&c, -1, out->verdicts, out->coverage, afr_sum, afr_n);

    out->samples_all = lt_at_total_samples(s);
    nrec = lt_at_recommendations(s, NULL, 0);
    recs = (lt_at_rec *)lt_malloc((nrec + 1) * sizeof *recs);
    lt_at_recommendations(s, recs, nrec);
    out->cells = (lt_cell_result *)lt_calloc(nrec + 1, sizeof(lt_cell_result));
    out->ncells = nrec;
    for (k = 0; k < nrec; k++) {
        lt_cell_result *r = &out->cells[k];
        size_t idx = (size_t)recs[k].cell_y * nx + recs[k].cell_x;
        r->x = recs[k].cell_x;
        r->y = recs[k].cell_y;
        r->rpm = r->x < nx ? xbins[r->x] : 0.0;
        r->load = r->y < ny ? ybins[r->y] : 0.0;
        r->current_ve = recs[k].beginning_value;
        r->proposed_ve = recs[k].recommended_value;
        r->delta = r->proposed_ve - r->current_ve;
        r->hits = recs[k].hit_count;
        r->weight = recs[k].hit_weighting;
        r->confidence = cfg->settings.base_weight > 0.0
                            ? LT_MIN(r->weight / cfg->settings.base_weight, 1.0)
                            : 1.0;
        r->target_afr = recs[k].target_afr;
        r->mean_afr = (idx < ncell && afr_n[idx] > 0) ? afr_sum[idx] / (double)afr_n[idx] : 0.0;
    }
    nrej = lt_at_rejections(s, NULL, 0);
    rej = (lt_at_reject *)lt_malloc((nrej + 1) * sizeof *rej);
    lt_at_rejections(s, rej, nrej);
    out->rejections = rej;
    out->n_rejections = nrej;
    lt_free(recs);
    lt_free(afr_sum);
    lt_free(afr_n);
    lt_at_free(s);

    if (cfg->validate) {
        out->has_validation = validate(&c, &out->validation);
    }
    lt_free(c.pts);
    lt_free(c.afr);
    return 0;
}

void lt_replay_report_free(lt_replay_report *r)
{
    if (!r) {
        return;
    }
    if (r->cells) {
        lt_free(r->cells);
    }
    if (r->rejections) {
        lt_free(r->rejections);
    }
    if (r->verdicts) {
        lt_free((void *)r->verdicts);
    }
    if (r->coverage) {
        lt_free(r->coverage);
    }
    memset(r, 0, sizeof *r);
}
