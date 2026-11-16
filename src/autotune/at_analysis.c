/* at_analysis.c - table anomaly detection, health scoring, zero-hit cell prediction
 * (spec 07 section 11).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "autotune.h"

/* UTF-8 sequences kept as octal escapes (source stays ASCII). */
#define U_SIGMA "\317\203"
#define U_ARROW "\342\206\222"

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ================================================================ anomaly */

void lt_anomaly_config_default(lt_anomaly_config *c)
{
    c->outlier_sigma = 2.0;
    c->gradient_threshold = 2.5;
    c->min_reasonable_ve = 5.0;
    c->max_reasonable_ve = 180.0;
    c->min_flat_region_size = 4;
    c->flat_tolerance = 0.1;
    c->monotonicity_load_floor_kpa = 30.0;
    c->monotonicity_min_rpm = 0.0;
}

typedef struct an_item {
    lt_anomaly a;
    size_t seq;
} an_item;

static void an_push(lt_vec *v, size_t row, size_t col, double value, double expected,
                    lt_anomaly_type type, double severity, const char *desc)
{
    an_item *it = (an_item *)lt_vec_push(v);
    it->a.row = row;
    it->a.col = col;
    it->a.value = value;
    it->a.expected = expected;
    it->a.type = type;
    it->a.severity = severity;
    lt_strlcpy(it->a.description, desc, sizeof it->a.description);
    it->seq = v->len - 1;
}

static int cmp_an(const void *pa, const void *pb)
{
    const an_item *a = (const an_item *)pa, *b = (const an_item *)pb;
    if (a->a.severity != b->a.severity) {
        return a->a.severity > b->a.severity ? -1 : 1;
    }
    return a->seq < b->seq ? -1 : (a->seq > b->seq ? 1 : 0);
}

static void an_outliers(const double *z, size_t rows, size_t cols, const double *xb,
                        const double *yb, const lt_anomaly_config *cfg, lt_vec *out)
{
    size_t r, c;
    long dr, dc;
    char d[160];
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            double px[8], py[8], pz[8], val = z[r * cols + c];
            size_t np = 0, i;
            double mx = 0, my = 0, mz = 0, sxx = 0, syy = 0, sxy = 0, sxz = 0, syz = 0, det;
            double expected, rstd = 0.0, cx, cy;
            for (dr = -1; dr <= 1; dr++) {
                for (dc = -1; dc <= 1; dc++) {
                    long rr = (long)r + dr, cc = (long)c + dc;
                    if ((dr == 0 && dc == 0) || rr < 0 || cc < 0 || rr >= (long)rows ||
                        cc >= (long)cols) {
                        continue;
                    }
                    px[np] = xb ? xb[cc] : (double)cc;
                    py[np] = yb ? yb[rr] : (double)rr;
                    pz[np] = z[(size_t)rr * cols + (size_t)cc];
                    np++;
                }
            }
            if (np <= 3) {
                continue;
            }
            for (i = 0; i < np; i++) {
                mx += px[i];
                my += py[i];
                mz += pz[i];
            }
            mx /= (double)np;
            my /= (double)np;
            mz /= (double)np;
            for (i = 0; i < np; i++) {
                double ax = px[i] - mx, ay = py[i] - my;
                sxx += ax * ax;
                syy += ay * ay;
                sxy += ax * ay;
                sxz += ax * pz[i];
                syz += ay * pz[i];
            }
            det = sxx * syy - sxy * sxy;
            cx = xb ? xb[c] : (double)c;
            cy = yb ? yb[r] : (double)r;
            if (fabs(det) > 1e-9) {
                double a = (syy * sxz - sxy * syz) / det;
                double b = (sxx * syz - sxy * sxz) / det;
                double cc0 = mz - a * mx - b * my;
                double ss = 0.0, res;
                double dof = (double)(np > 3 ? np - 3 : 1);
                expected = a * cx + b * cy + cc0;
                for (i = 0; i < np; i++) {
                    res = pz[i] - (a * px[i] + b * py[i] + cc0);
                    ss += res * res;
                }
                if (dof < 1.0) {
                    dof = 1.0;
                }
                rstd = sqrt(ss / dof);
            } else {
                expected = mz;
                rstd = 0.0;
            }
            if (rstd < 0.01) {
                double residual = fabs(val - expected);
                double gate = 5.0 * LT_MAX(cfg->outlier_sigma / 2.0, 0.0);
                if (residual > gate) {
                    lt_snprintf(d, sizeof d,
                                "Cell value %.1f deviates %.1f from a clean local plane (%.1f)",
                                val, residual, expected);
                    an_push(out, r, c, val, expected, LT_AN_OUTLIER,
                            clampd((residual - gate) / 45.0, 0.0, 1.0), d);
                }
            } else {
                double zs = fabs(val - expected) / rstd;
                if (zs > cfg->outlier_sigma) {
                    lt_snprintf(d, sizeof d, "Cell value %.1f is %.1f" U_SIGMA
                                " from local plane fit (%.1f)", val, zs, expected);
                    an_push(out, r, c, val, expected, LT_AN_OUTLIER,
                            LT_MIN((zs - cfg->outlier_sigma) / 3.0, 1.0), d);
                }
            }
        }
    }
}

static void an_monotonic(const double *z, size_t rows, size_t cols, const double *xb,
                         const double *yb, const lt_anomaly_config *cfg, lt_vec *out)
{
    size_t r, c;
    char d[160];
    if (!yb || rows < 2) {
        return;
    }
    for (c = 0; c < cols; c++) {
        if (xb && xb[c] < cfg->monotonicity_min_rpm) {
            continue;
        }
        for (r = 1; r < rows; r++) {
            double prev, curr, drop;
            if (yb[r - 1] < cfg->monotonicity_load_floor_kpa) {
                continue;
            }
            prev = z[(r - 1) * cols + c];
            curr = z[r * cols + c];
            if (yb[r] > yb[r - 1] && curr < prev * 0.8 && prev > 10.0) {
                drop = fabs((prev - curr) / prev * 100.0);
                lt_snprintf(d, sizeof d, "VE dropped %.0f%% (%.1f" U_ARROW "%.1f) with increasing load",
                            drop, prev, curr);
                an_push(out, r, c, curr, prev, LT_AN_MONOTONICITY, LT_MIN(drop / 50.0, 1.0), d);
            }
        }
    }
}

static void an_gradient(const double *z, size_t rows, size_t cols, const lt_anomaly_config *cfg,
                        lt_vec *out)
{
    size_t r, c, ng = 0;
    double sum = 0.0, avg, thr;
    char d[160];
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            if (c + 1 < cols) {
                sum += fabs(z[r * cols + c + 1] - z[r * cols + c]);
                ng++;
            }
            if (r + 1 < rows) {
                sum += fabs(z[(r + 1) * cols + c] - z[r * cols + c]);
                ng++;
            }
        }
    }
    if (ng == 0) {
        return;
    }
    avg = sum / (double)ng;
    if (avg < 0.01) {
        return;
    }
    thr = avg * cfg->gradient_threshold;
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            double v = z[r * cols + c], ml = 0.0, nv = v, diff;
            int have = 0;
            /* right, left, down, up; strict '>' keeps the first on ties */
            if (c + 1 < cols) {
                diff = fabs(z[r * cols + c + 1] - v);
                if (!have || diff > ml) { ml = diff; nv = z[r * cols + c + 1]; have = 1; }
            }
            if (c > 0) {
                diff = fabs(z[r * cols + c - 1] - v);
                if (!have || diff > ml) { ml = diff; nv = z[r * cols + c - 1]; have = 1; }
            }
            if (r + 1 < rows) {
                diff = fabs(z[(r + 1) * cols + c] - v);
                if (!have || diff > ml) { ml = diff; nv = z[(r + 1) * cols + c]; have = 1; }
            }
            if (r > 0) {
                diff = fabs(z[(r - 1) * cols + c] - v);
                if (!have || diff > ml) { ml = diff; nv = z[(r - 1) * cols + c]; have = 1; }
            }
            if (have && ml > thr) {
                lt_snprintf(d, sizeof d, "Sharp gradient: %.1f change vs %.1f average", ml, avg);
                an_push(out, r, c, v, nv, LT_AN_GRADIENT,
                        LT_MIN((ml / thr - 1.0) / 2.0, 1.0), d);
            }
        }
    }
}

static void an_unreasonable(const double *z, size_t rows, size_t cols,
                            const lt_anomaly_config *cfg, lt_vec *out)
{
    size_t r, c;
    char d[160];
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            double v = z[r * cols + c];
            if (v < cfg->min_reasonable_ve) {
                lt_snprintf(d, sizeof d, "VE %.1f below minimum reasonable value %.1f", v,
                            cfg->min_reasonable_ve);
                an_push(out, r, c, v, cfg->min_reasonable_ve, LT_AN_UNREASONABLE, 0.8, d);
            } else if (v > cfg->max_reasonable_ve) {
                lt_snprintf(d, sizeof d, "VE %.1f above maximum reasonable value %.1f", v,
                            cfg->max_reasonable_ve);
                an_push(out, r, c, v, cfg->max_reasonable_ve, LT_AN_UNREASONABLE, 0.9, d);
            }
        }
    }
}

#define FLAT_VISIT(n_) \
    if (!vis[n_] && fabs(z[n_] - seed) <= cfg->flat_tolerance) { \
        vis[n_] = 1; \
        stack[sp++] = (n_); \
    }

static void an_flat(const double *z, size_t rows, size_t cols, const lt_anomaly_config *cfg,
                    lt_vec *out)
{
    size_t total = rows * cols, r, c, k, sp;
    unsigned char *vis = (unsigned char *)lt_calloc(total ? total : 1, 1);
    size_t *stack = (size_t *)lt_malloc((total ? total : 1) * sizeof(size_t));
    size_t *region = (size_t *)lt_malloc((total ? total : 1) * sizeof(size_t));
    char d[160];
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            size_t start = r * cols + c, nreg = 0;
            double seed;
            if (vis[start]) {
                continue;
            }
            seed = z[start];
            sp = 0;
            stack[sp++] = start;
            vis[start] = 1;
            while (sp > 0) {
                size_t cur = stack[--sp], cr = cur / cols, cc = cur % cols, nb;
                region[nreg++] = cur;
                if (cr > 0) {
                    nb = cur - cols;
                    FLAT_VISIT(nb);
                }
                if (cr + 1 < rows) {
                    nb = cur + cols;
                    FLAT_VISIT(nb);
                }
                if (cc > 0) {
                    nb = cur - 1;
                    FLAT_VISIT(nb);
                }
                if (cc + 1 < cols) {
                    nb = cur + 1;
                    FLAT_VISIT(nb);
                }
            }
            if (nreg >= cfg->min_flat_region_size) {
                double sev = LT_MIN((double)nreg / (double)total, 0.8);
                lt_snprintf(d, sizeof d, "Part of %lu identical cells (value %.1f) - likely untuned",
                            (unsigned long)nreg, seed);
                for (k = 0; k < nreg; k++) {
                    an_push(out, region[k] / cols, region[k] % cols, seed, seed, LT_AN_FLAT, sev,
                            d);
                }
            }
        }
    }
    lt_free(vis);
    lt_free(stack);
    lt_free(region);
}

size_t lt_anomaly_detect(const double *z, size_t rows, size_t cols, const double *xbins,
                         const double *ybins, const lt_anomaly_config *c, lt_anomaly *out,
                         size_t cap)
{
    lt_vec v;
    an_item *items;
    size_t i, n, m = 0, w;
    unsigned char *seen;
    lt_anomaly_config dflt;
    if (!c) {
        lt_anomaly_config_default(&dflt);
        c = &dflt;
    }
    if (!z || rows == 0 || cols == 0) {
        return 0;
    }
    lt_vec_init(&v, sizeof(an_item));
    an_outliers(z, rows, cols, xbins, ybins, c, &v);
    an_monotonic(z, rows, cols, xbins, ybins, c, &v);
    an_gradient(z, rows, cols, c, &v);
    an_unreasonable(z, rows, cols, c, &v);
    an_flat(z, rows, cols, c, &v);
    items = (an_item *)v.data;
    n = v.len;
    if (n > 1) {
        qsort(items, n, sizeof(an_item), cmp_an);
    }
    seen = (unsigned char *)lt_calloc(rows * cols, 1);
    w = 0;
    for (i = 0; i < n; i++) {
        size_t cell = items[i].a.row * cols + items[i].a.col;
        if (seen[cell]) {
            continue;
        }
        seen[cell] = 1;
        if (out && w < cap) {
            out[w] = items[i].a;
        }
        w++;
    }
    m = out ? LT_MIN(w, cap) : w;
    lt_free(seen);
    lt_vec_free(&v);
    return m;
}

/* ================================================================ health */

static unsigned u32_of(double v)
{
    return v <= 0.0 ? 0U : (unsigned)v;
}

typedef struct hs_cfg {
    double idle_rpm_max, wot_rpm_min, idle_load_fraction, load_floor, min_rpm, wot_map;
} hs_cfg;

static size_t sat1(size_t v)
{
    return v > 0 ? v - 1 : 0;
}

static size_t first_ge(const double *b, size_t n, double thr, size_t dflt)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (b[i] >= thr) {
            return i;
        }
    }
    return dflt;
}

static void add_region(lt_health_report *rep, const char *name, lt_region_type type, size_t r0,
                       size_t r1, size_t c0, size_t c1)
{
    lt_region_health *rg;
    if (rep->n_regions >= 8) {
        return;
    }
    rg = &rep->regions[rep->n_regions++];
    memset(rg, 0, sizeof *rg);
    lt_strlcpy(rg->name, name, sizeof rg->name);
    rg->type = type;
    rg->row0 = r0;
    rg->row1 = r1;
    rg->col0 = c0;
    rg->col1 = c1;
    rg->cell_count = (r1 - r0 + 1) * (c1 - c0 + 1);
}

static void score_region(lt_region_health *rg, const double *z, const unsigned *hits, size_t rows,
                         size_t cols, const double *xb, const double *yb, const hs_cfg *cfg)
{
    size_t r, c, with_hits = 0, total = 0, nint = 0, mono = 0, mtot = 0;
    double sumd = 0.0, cov, sm, mo;
    size_t irows = rg->row1 > rg->row0 + 1 ? rg->row1 - (rg->row0 + 1) : 0;
    size_t icols = rg->col1 > rg->col0 + 1 ? rg->col1 - (rg->col0 + 1) : 0;
    unsigned overall;
    for (r = rg->row0; r <= rg->row1; r++) {
        for (c = rg->col0; c <= rg->col1; c++) {
            total++;
            if (hits && hits[r * cols + c] > 0) {
                with_hits++;
            }
        }
    }
    cov = total ? (double)u32_of((double)with_hits / (double)total * 100.0) : 0.0;
    if (irows == 0 || icols == 0) {
        sm = 100.0;
    } else {
        for (r = rg->row0 + 1; r + 1 <= rg->row1; r++) {
            for (c = rg->col0 + 1; c + 1 <= rg->col1; c++) {
                double v = z[r * cols + c];
                double up = r > 0 ? z[(r - 1) * cols + c] : v;
                double dn = r + 1 < rows ? z[(r + 1) * cols + c] : v;
                double lf = c > 0 ? z[r * cols + c - 1] : v;
                double rt = c + 1 < cols ? z[r * cols + c + 1] : v;
                sumd += fabs(up + dn + lf + rt - 4.0 * v);
                nint++;
            }
        }
        sm = nint ? (double)u32_of(clampd(1.0 - (sumd / (double)nint) / 10.0, 0.0, 1.0) * 100.0)
                  : 100.0;
    }
    for (c = rg->col0; c <= rg->col1; c++) {
        if (xb && xb[c] < cfg->min_rpm) {
            continue;
        }
        for (r = rg->row0; r < rg->row1; r++) {
            if (yb && yb[r] < cfg->load_floor) {
                continue;
            }
            mtot++;
            if (z[(r + 1) * cols + c] >= z[r * cols + c] * 0.95) {
                mono++;
            }
        }
    }
    mo = mtot ? (double)u32_of((double)mono / (double)mtot * 100.0) : 100.0;
    overall = u32_of(cov * 0.4 + sm * 0.35 + mo * 0.25);
    if (overall > 100U) {
        overall = 100U;
    }
    rg->score = overall;
    rg->coverage_score = (unsigned)cov;
    rg->smoothness_score = (unsigned)sm;
    rg->monotonicity_score = (unsigned)mo;
}

void lt_health_score(const double *z, const unsigned *hits, size_t rows, size_t cols,
                     const double *xbins, const double *ybins, lt_health_report *out)
{
    hs_cfg cfg;
    size_t idle_col_end, wot_col_start, idle_row_end, wot_row_start, p, i;
    double min_load, max_load, range, idle_thr;
    unsigned long wsum = 0, csum = 0;
    char msg[160];
    memset(out, 0, sizeof *out);
    out->grade = 'F';
    if (!z || rows == 0 || cols == 0 || !xbins || !ybins) {
        lt_strlcpy(out->recommendations[0], "No table data available",
                   sizeof out->recommendations[0]);
        out->n_recommendations = 1;
        return;
    }
    cfg.idle_rpm_max = 1500.0;
    cfg.wot_rpm_min = 4500.0;
    cfg.idle_load_fraction = 0.3;
    cfg.load_floor = 30.0;
    cfg.min_rpm = 0.0;
    cfg.wot_map = 90.0;

    p = first_ge(xbins, cols, cfg.idle_rpm_max, 1);
    if (p < 1) {
        p = 1;
    }
    idle_col_end = sat1(p);
    wot_col_start = first_ge(xbins, cols, cfg.wot_rpm_min, sat1(cols));
    min_load = ybins[0];
    max_load = ybins[rows - 1];
    range = max_load - min_load;
    idle_thr = min_load + range * cfg.idle_load_fraction;
    wot_row_start = first_ge(ybins, rows, cfg.wot_map, sat1(rows));
    p = first_ge(ybins, rows, idle_thr, 1);
    if (p < 1) {
        p = 1;
    }
    idle_row_end = sat1(p);

    if (idle_col_end < cols && idle_row_end < rows) {
        add_region(out, "Idle", LT_REG_IDLE, 0, LT_MIN(idle_row_end, rows - 1), 0,
                   LT_MIN(idle_col_end, cols - 1));
    }
    {
        size_t cs = LT_MIN(idle_col_end + 1, cols - 1);
        size_t ce = LT_MAX(sat1(wot_col_start), cs);
        size_t rs = LT_MIN(idle_row_end + 1, rows - 1);
        size_t re_ = wot_row_start > 0 ? wot_row_start - 1 : 0;
        if (ce >= cs && re_ >= rs) {
            add_region(out, "Cruise", LT_REG_CRUISE, rs, re_, cs, ce);
        }
    }
    if (wot_row_start < rows) {
        add_region(out, "WOT", LT_REG_WOT, wot_row_start, rows - 1, 0, cols - 1);
    }
    if (idle_row_end < rows && idle_col_end + 1 < cols) {
        add_region(out, "Low Load", LT_REG_TRANSITION, 0, LT_MIN(idle_row_end, rows - 1),
                   idle_col_end + 1, cols - 1);
    }
    {
        size_t ps = LT_MIN(idle_row_end + 1, rows - 1);
        size_t pe = sat1(wot_row_start);
        if (pe >= ps) {
            if (idle_col_end > 0) {
                add_region(out, "Part Throttle", LT_REG_PART_THROTTLE, ps, pe, 0, idle_col_end);
            }
            if (wot_col_start < cols) {
                add_region(out, "Part Throttle", LT_REG_PART_THROTTLE, ps, pe, wot_col_start,
                           cols - 1);
            }
        }
    }
    if (out->n_regions == 0) {
        add_region(out, "Full Table", LT_REG_CRUISE, 0, rows - 1, 0, cols - 1);
    }
    for (i = 0; i < out->n_regions; i++) {
        score_region(&out->regions[i], z, hits, rows, cols, xbins, ybins, &cfg);
        wsum += (unsigned long)out->regions[i].score * (unsigned long)out->regions[i].cell_count;
        csum += (unsigned long)out->regions[i].cell_count;
    }
    out->overall_score = csum ? (unsigned)(wsum / csum) : 0U;
    out->total_cells = rows * cols;
    if (hits) {
        for (i = 0; i < rows * cols; i++) {
            if (hits[i] > 0) {
                out->coverage_cells++;
            }
        }
    }
    out->coverage_percent = (double)out->coverage_cells / (double)out->total_cells * 100.0;
    out->grade = out->overall_score >= 90 ? 'A' : out->overall_score >= 80 ? 'B'
               : out->overall_score >= 70 ? 'C' : out->overall_score >= 60 ? 'D' : 'F';
    if (out->coverage_percent < 50.0) {
        lt_snprintf(msg, sizeof msg,
                    "Low data coverage (%.0f%%) - more driving needed to populate the VE table",
                    out->coverage_percent);
        lt_strlcpy(out->recommendations[out->n_recommendations++], msg,
                   sizeof out->recommendations[0]);
    }
    for (i = 0; i < out->n_regions && out->n_recommendations < 14; i++) {
        const lt_region_health *rg = &out->regions[i];
        if (rg->coverage_score < 30) {
            lt_snprintf(msg, sizeof msg,
                        "%s region has very low data coverage (%u) - focus on driving in this area",
                        rg->name, rg->coverage_score);
            lt_strlcpy(out->recommendations[out->n_recommendations++], msg,
                       sizeof out->recommendations[0]);
        }
        if (rg->smoothness_score < 40) {
            lt_snprintf(msg, sizeof msg,
                        "%s region has rough VE transitions - consider smoothing", rg->name);
            lt_strlcpy(out->recommendations[out->n_recommendations++], msg,
                       sizeof out->recommendations[0]);
        }
        if (rg->monotonicity_score < 50) {
            lt_snprintf(msg, sizeof msg,
                        "%s region has VE values that decrease with load - check for tuning errors",
                        rg->name);
            lt_strlcpy(out->recommendations[out->n_recommendations++], msg,
                       sizeof out->recommendations[0]);
        }
    }
}

/* ================================================================ predictor */

void lt_predict_config_default(lt_predict_config *c)
{
    c->min_confidence = 0.3;
    c->max_search_radius = 5;
    c->min_hit_count = 3;
    c->distance_decay = 2.0;
    c->physics_peak_torque_rpm = 4500.0;
    c->physics_min_ve = 10.0;
    c->physics_max_ve = 200.0;
}

typedef struct pr_ctx {
    const double *z;
    const unsigned *hits;
    size_t rows, cols;
    const double *xb, *yb;
    const lt_predict_config *cfg;
    double xrange, yrange;
} pr_ctx;

static lt_bool is_known(const pr_ctx *p, long r, long c)
{
    if (r < 0 || c < 0 || r >= (long)p->rows || c >= (long)p->cols) {
        return LT_FALSE;
    }
    return p->hits[(size_t)r * p->cols + (size_t)c] >= p->cfg->min_hit_count ? LT_TRUE : LT_FALSE;
}

static double axis_range(const double *b, size_t n)
{
    double last = 100.0, first = 0.0;
    if (b && n > 0) {
        first = b[0];
        last = b[n - 1];
    }
    return LT_MAX(last - first, 1e-6);
}

static double phys_dist(const pr_ctx *p, size_t r, size_t c, size_t r2, size_t c2)
{
    double dx, dy, d;
    if (p->xb) {
        dx = fabs(p->xb[c] - p->xb[c2]) / p->xrange;
    } else {
        dx = fabs((double)c - (double)c2);
    }
    if (p->yb) {
        dy = fabs(p->yb[r] - p->yb[r2]) / p->yrange;
    } else {
        dy = fabs((double)r - (double)r2);
    }
    d = sqrt(dx * dx + dy * dy);
    return LT_MAX(d, 0.001);
}

static int in_quadrant(int q, long dr, long dc)
{
    switch (q) {
    case 0: return dr < 0 && dc <= 0;    /* UL */
    case 1: return dr <= 0 && dc > 0;    /* UR */
    case 2: return dr >= 0 && dc < 0;    /* DL */
    default: return dr > 0 && dc >= 0;   /* DR */
    }
}

/* nearest known cell in a quadrant; returns 1 and sets (*rr,*cc) */
static int nearest_known(const pr_ctx *p, size_t r, size_t c, int q, size_t *rr, size_t *cc)
{
    long ring, dr, dc, n, i, j;
    long cdr[128], cdc[128], cd2[128];
    long R = (long)p->cfg->max_search_radius;
    for (ring = 1; ring <= R; ring++) {
        n = 0;
        for (dr = -ring; dr <= ring; dr++) {
            for (dc = -ring; dc <= ring; dc++) {
                if (!(labs(dr) == ring || labs(dc) == ring) || !in_quadrant(q, dr, dc)) {
                    continue;
                }
                if (n < 128) {
                    cdr[n] = dr;
                    cdc[n] = dc;
                    cd2[n] = dr * dr + dc * dc;
                    n++;
                }
            }
        }
        /* stable insertion sort by distance */
        for (i = 1; i < n; i++) {
            long a = cdr[i], b = cdc[i], d = cd2[i];
            j = i - 1;
            while (j >= 0 && cd2[j] > d) {
                cdr[j + 1] = cdr[j];
                cdc[j + 1] = cdc[j];
                cd2[j + 1] = cd2[j];
                j--;
            }
            cdr[j + 1] = a;
            cdc[j + 1] = b;
            cd2[j + 1] = d;
        }
        for (i = 0; i < n; i++) {
            if (is_known(p, (long)r + cdr[i], (long)c + cdc[i])) {
                *rr = (size_t)((long)r + cdr[i]);
                *cc = (size_t)((long)c + cdc[i]);
                return 1;
            }
        }
    }
    return 0;
}

static lt_bool pr_bilinear(const pr_ctx *p, size_t r, size_t c, lt_predicted_cell *o)
{
    size_t corners[4][2], nc = 0, q, i, k;
    double wsum = 0.0, vsum = 0.0, w, d, maxd = 0.0, rx, ry, mnd, cf, df;
    for (q = 0; q < 4; q++) {
        size_t rr, cc;
        int dup = 0;
        if (!nearest_known(p, r, c, (int)q, &rr, &cc)) {
            continue;
        }
        for (k = 0; k < nc; k++) {
            if (corners[k][0] == rr && corners[k][1] == cc) {
                dup = 1;
            }
        }
        if (!dup) {
            corners[nc][0] = rr;
            corners[nc][1] = cc;
            nc++;
        }
    }
    if (nc < 3) {
        return LT_FALSE;
    }
    for (i = 0; i < nc; i++) {
        d = phys_dist(p, r, c, corners[i][0], corners[i][1]);
        w = 1.0 / pow(d, p->cfg->distance_decay);
        wsum += w;
        vsum += w * p->z[corners[i][0] * p->cols + corners[i][1]];
        if (d > maxd) {
            maxd = d;
        }
    }
    if (wsum < 0.001) {
        return LT_FALSE;
    }
    o->predicted = clampd(vsum / wsum, 1.0, 200.0);
    cf = (double)nc / 4.0;
    rx = LT_MIN((double)p->cfg->max_search_radius / LT_MAX((double)p->cols - 1.0, 1.0), 1.0);
    ry = LT_MIN((double)p->cfg->max_search_radius / LT_MAX((double)p->rows - 1.0, 1.0), 1.0);
    mnd = LT_MAX(sqrt(rx * rx + ry * ry) * 1.5, 1e-6);
    df = LT_MAX(1.0 - maxd / mnd, 0.0);
    o->confidence = clampd(cf * 0.7 + df * 0.3, 0.0, 1.0);
    o->method = LT_PM_BILINEAR;
    o->neighbors = nc;
    return LT_TRUE;
}

static lt_bool pr_neighbor(const pr_ctx *p, size_t r, size_t c, lt_predicted_cell *o)
{
    long R = (long)p->cfg->max_search_radius, rr, cc;
    double wsum = 0.0, vsum = 0.0, w, d;
    size_t count = 0;
    for (rr = (long)r - R; rr <= (long)r + R; rr++) {
        for (cc = (long)c - R; cc <= (long)c + R; cc++) {
            if ((rr == (long)r && cc == (long)c) || !is_known(p, rr, cc)) {
                continue;
            }
            d = phys_dist(p, r, c, (size_t)rr, (size_t)cc);
            w = (1.0 / pow(d, p->cfg->distance_decay)) *
                sqrt((double)p->hits[(size_t)rr * p->cols + (size_t)cc]);
            wsum += w;
            vsum += w * p->z[(size_t)rr * p->cols + (size_t)cc];
            count++;
        }
    }
    if (count < 2 || wsum < 0.001) {
        return LT_FALSE;
    }
    o->predicted = clampd(vsum / wsum, 1.0, 200.0);
    o->confidence = LT_MIN((double)count / 8.0, 1.0) * 0.8;
    o->method = LT_PM_NEIGHBOR;
    o->neighbors = count;
    return LT_TRUE;
}

/* 1-D extrapolation along a line of known points (index, value), ascending index. */
static lt_bool extrap_line(const double *idx, const double *val, size_t n, double target,
                           double base_conf, lt_predicted_cell *o)
{
    size_t i;
    double slope, v, dist, ref, lo, hi;
    if (n < 2) {
        return LT_FALSE;
    }
    for (i = 0; i + 1 < n; i++) {
        if (idx[i] <= target && target <= idx[i + 1] && idx[i] != idx[i + 1]) {
            v = val[i] + (val[i + 1] - val[i]) * (target - idx[i]) / (idx[i + 1] - idx[i]);
            o->predicted = clampd(v, 1.0, 200.0);
            o->confidence = 0.75;
            o->method = LT_PM_ONE_D;
            o->neighbors = n;
            return LT_TRUE;
        }
    }
    if (target < idx[0]) {
        slope = (val[1] - val[0]) / (idx[1] - idx[0]);
        v = val[0] + slope * (target - idx[0]);
        dist = idx[0] - target;
        ref = val[0];
    } else if (target > idx[n - 1]) {
        slope = (val[n - 1] - val[n - 2]) / (idx[n - 1] - idx[n - 2]);
        v = val[n - 1] + slope * (target - idx[n - 1]);
        dist = target - idx[n - 1];
        ref = val[n - 1];
    } else {
        return LT_FALSE;
    }
    if (dist == 0.0 || dist > 3.0) {
        return LT_FALSE;
    }
    lo = LT_MAX(ref * 0.7, 1.0);
    hi = LT_MIN(ref * 1.3, 200.0);
    if (lo > hi) {
        hi = lo;
    }
    o->predicted = clampd(v, lo, hi);
    o->confidence = base_conf * (1.0 - (dist - 1.0) / 4.0);
    o->method = LT_PM_EXTRAPOLATE;
    o->neighbors = n;
    return LT_TRUE;
}

static lt_bool pr_linear(const pr_ctx *p, size_t r, size_t c, lt_predicted_cell *o)
{
    size_t n = LT_MAX(p->rows, p->cols), i, cnt;
    double *idx = (double *)lt_malloc(n * sizeof(double));
    double *val = (double *)lt_malloc(n * sizeof(double));
    lt_bool ok = LT_FALSE;
    cnt = 0;
    for (i = 0; i < p->cols; i++) {
        if (is_known(p, (long)r, (long)i)) {
            idx[cnt] = (double)i;
            val[cnt++] = p->z[r * p->cols + i];
        }
    }
    if (cnt >= 2) {
        ok = extrap_line(idx, val, cnt, (double)c, 0.4, o);
    }
    if (!ok) {
        cnt = 0;
        for (i = 0; i < p->rows; i++) {
            if (is_known(p, (long)i, (long)c)) {
                idx[cnt] = (double)i;
                val[cnt++] = p->z[i * p->cols + c];
            }
        }
        if (cnt >= 2) {
            ok = extrap_line(idx, val, cnt, (double)r, 0.35, o);
        }
    }
    lt_free(idx);
    lt_free(val);
    return ok;
}

static lt_bool pr_physics(const pr_ctx *p, size_t r, size_t c, lt_predicted_cell *o)
{
    double y0, ylast, yr, lf, peak, rpm, rf, ve;
    if (!p->yb || p->rows == 0) {
        return LT_FALSE;
    }
    y0 = p->yb[0];
    ylast = p->yb[p->rows - 1];
    yr = p->yb[r];
    lf = clampd((yr - y0) / LT_MAX(ylast - y0, 1.0), 0.0, 1.0);
    peak = LT_MAX(p->cfg->physics_peak_torque_rpm, 1.0);
    rpm = p->xb ? p->xb[c] : peak;
    if (rpm <= peak) {
        rf = 0.6 + 0.4 * clampd(rpm / peak, 0.0, 1.0);
    } else {
        rf = 1.0 - 0.4 * clampd((rpm - peak) / peak, 0.0, 1.0);
    }
    ve = (p->cfg->physics_min_ve + lf * LT_MAX(p->cfg->physics_max_ve - p->cfg->physics_min_ve, 1.0)) * rf;
    o->predicted = clampd(ve, p->cfg->physics_min_ve, p->cfg->physics_max_ve);
    o->confidence = 0.15;
    o->method = LT_PM_PHYSICS;
    o->neighbors = 0;
    return LT_TRUE;
}

typedef struct pr_item {
    lt_predicted_cell c;
    size_t seq;
} pr_item;

static int cmp_pr(const void *a, const void *b)
{
    const pr_item *x = (const pr_item *)a, *y = (const pr_item *)b;
    if (x->c.confidence != y->c.confidence) {
        return x->c.confidence > y->c.confidence ? -1 : 1;
    }
    return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

size_t lt_predict_cells(const double *z, const unsigned *hits, size_t rows, size_t cols,
                        const double *xbins, const double *ybins, const lt_predict_config *c,
                        lt_predicted_cell *out, size_t cap)
{
    pr_ctx p;
    lt_predict_config dflt;
    size_t r, cc, nknown = 0, n = 0, i, m;
    pr_item *items;
    if (!c) {
        lt_predict_config_default(&dflt);
        c = &dflt;
    }
    if (!z || !hits || rows == 0 || cols == 0) {
        return 0;
    }
    p.z = z;
    p.hits = hits;
    p.rows = rows;
    p.cols = cols;
    p.xb = xbins;
    p.yb = ybins;
    p.cfg = c;
    p.xrange = axis_range(xbins, cols);
    p.yrange = axis_range(ybins, rows);
    for (i = 0; i < rows * cols; i++) {
        if (hits[i] >= c->min_hit_count) {
            nknown++;
        }
    }
    if (nknown == 0) {
        return 0;
    }
    items = (pr_item *)lt_malloc(rows * cols * sizeof(pr_item));
    for (r = 0; r < rows; r++) {
        for (cc = 0; cc < cols; cc++) {
            lt_predicted_cell pc;
            if (hits[r * cols + cc] >= c->min_hit_count) {
                continue;
            }
            memset(&pc, 0, sizeof pc);
            pc.row = r;
            pc.col = cc;
            pc.current = z[r * cols + cc];
            if (!(pr_bilinear(&p, r, cc, &pc) && pc.confidence >= c->min_confidence) &&
                !(pr_neighbor(&p, r, cc, &pc) && pc.confidence >= c->min_confidence) &&
                !(pr_linear(&p, r, cc, &pc) && pc.confidence >= c->min_confidence) &&
                !(pr_physics(&p, r, cc, &pc) && pc.confidence >= c->min_confidence)) {
                continue;
            }
            items[n].c = pc;
            items[n].seq = n;
            n++;
        }
    }
    qsort(items, n, sizeof(pr_item), cmp_pr);
    m = out ? LT_MIN(n, cap) : 0;
    for (i = 0; i < m; i++) {
        out[i] = items[i].c;
    }
    lt_free(items);
    return n;
}
