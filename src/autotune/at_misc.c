/* at_misc.c - declared INI filters, preflight, transport-delay measurement, accel enrichment
 * (spec 07 sections 6, 7, 9, 10).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "autotune.h"

/* ================================================================ declared filters */

lt_bool lt_at_filter_rejects(const lt_ini_filter *f, lt_bool enabled, lt_bool have, double value)
{
    long bits, mask;
    if (!enabled || !have) {
        return LT_FALSE;
    }
    switch (f->op) {
    case LT_FOP_LT:
        return value < f->dflt;
    case LT_FOP_GT:
        return value > f->dflt;
    case LT_FOP_EQ:
        return fabs(value - f->dflt) < 1e-6;
    case LT_FOP_NE:
        return fabs(value - f->dflt) >= 1e-6;
    case LT_FOP_BITAND:
        bits = (long)lt_round(value);
        mask = (long)lt_round(f->dflt);
        return (mask != 0 && (bits & mask) != 0) ? LT_TRUE : LT_FALSE;
    default:
        return LT_FALSE;   /* LT_FOP_BITOR: not specified for rejection */
    }
}

int lt_at_first_rejecting(const lt_ini_filter *f, size_t n, const lt_expr_env *env)
{
    size_t i;
    double v = 0.0;
    lt_bool have;
    for (i = 0; i < n; i++) {
        have = LT_FALSE;
        if (env && env->get_var && f[i].channel && f[i].channel[0]) {
            have = env->get_var(env->ud, f[i].channel, &v) ? LT_TRUE : LT_FALSE;
        }
        if (lt_at_filter_rejects(&f[i], LT_TRUE, have, v)) {
            return (int)i;
        }
    }
    return -1;
}

/* ================================================================ preflight */

static void add_finding(lt_finding *out, size_t cap, size_t *n, lt_severity sev, const char *code,
                        const char *title, const char *detail, const char *cur, const char *sug)
{
    lt_finding *f;
    if (*n >= cap) {
        (*n)++;   /* keep counting so the caller can see truncation; clipped on return */
        return;
    }
    f = &out[*n];
    memset(f, 0, sizeof *f);
    f->sev = sev;
    lt_strlcpy(f->code, code, sizeof f->code);
    lt_strlcpy(f->title, title, sizeof f->title);
    lt_strlcpy(f->detail, detail, sizeof f->detail);
    if (cur) {
        lt_strlcpy(f->current, cur, sizeof f->current);
    }
    if (sug) {
        lt_strlcpy(f->suggested, sug, sizeof f->suggested);
    }
    (*n)++;
}

static void check_target(const lt_preflight_input *in, const lt_at_settings *s, lt_finding *out,
                         size_t cap, size_t *n)
{
    char d[600], c[64], g[64];
    size_t i, cnt = 0, total = in->t_rows * in->t_cols;
    double lo = 0.0, hi = 0.0, v;
    lt_bool looks_afr, looks_lambda;
    if (!in->target_table) {
        lt_snprintf(d, sizeof d,
                    "Every cell will be tuned to a flat %.1f AFR, including full load. If the "
                    "engine is meant to run richer under load, this asks AutoTune to remove "
                    "fuel exactly where it is most dangerous to do so.",
                    s->target_afr);
        lt_snprintf(c, sizeof c, "flat %.1f", s->target_afr);
        add_finding(out, cap, n, LT_SEV_BLOCKER, "target_table_missing", "No AFR target table", d,
                    c, "pick a target table");
        return;
    }
    if (in->t_rows != in->ve_rows || in->t_cols != in->ve_cols) {
        lt_snprintf(d, sizeof d,
                    "'%s' is %lux%lu but the VE table is %lux%lu. Cells cannot be matched "
                    "one-to-one, so the wrong target would be used for most cells.",
                    in->target_table, (unsigned long)in->t_rows, (unsigned long)in->t_cols,
                    (unsigned long)in->ve_rows, (unsigned long)in->ve_cols);
        lt_snprintf(c, sizeof c, "%lux%lu", (unsigned long)in->t_rows, (unsigned long)in->t_cols);
        lt_snprintf(g, sizeof g, "%lux%lu", (unsigned long)in->ve_rows, (unsigned long)in->ve_cols);
        add_finding(out, cap, n, LT_SEV_BLOCKER, "target_table_shape",
                    "AFR target table is the wrong shape", d, c, g);
        return;
    }
    for (i = 0; i < total && in->target_values; i++) {
        v = in->target_values[i];
        if (v > 0.0) {
            if (cnt == 0 || v < lo) {
                lo = v;
            }
            if (cnt == 0 || v > hi) {
                hi = v;
            }
            cnt++;
        }
    }
    if (cnt == 0) {
        lt_snprintf(d, sizeof d,
                    "Every value in '%s' is zero or negative, so there is no target to tune "
                    "towards.", in->target_table);
        add_finding(out, cap, n, LT_SEV_BLOCKER, "target_table_empty",
                    "AFR target table reads as all zeros", d, NULL, NULL);
        return;
    }
    looks_afr = (lo >= 8.0 && hi <= 22.0) ? LT_TRUE : LT_FALSE;
    looks_lambda = (lo >= 0.5 && hi <= 1.6) ? LT_TRUE : LT_FALSE;
    if (!looks_afr && !looks_lambda) {
        lt_snprintf(d, sizeof d,
                    "Values in '%s' are outside both the AFR range (8-22) and the lambda range "
                    "(0.5-1.6). The table may be the wrong one, or in other units.",
                    in->target_table);
        lt_snprintf(c, sizeof c, "%.2f..%.2f", lo, hi);
        add_finding(out, cap, n, LT_SEV_WARNING, "target_table_range",
                    "AFR target values look wrong", d, c, "8-22 AFR or 0.5-1.6 lambda");
    }
}

static void check_ve(const lt_preflight_input *in, lt_finding *out, size_t cap, size_t *n)
{
    size_t i, total = in->ve_rows * in->ve_cols;
    double hi = 0.0;
    lt_bool any_pos = LT_FALSE;
    char c[64];
    if (total == 0 || !in->ve_values) {
        add_finding(out, cap, n, LT_SEV_BLOCKER, "ve_table_unreadable",
                    "VE table could not be read",
                    "The VE table has no readable values, so there is nothing to tune.", NULL,
                    NULL);
        return;
    }
    for (i = 0; i < total; i++) {
        if (in->ve_values[i] > 0.0) {
            any_pos = LT_TRUE;
        }
        if (i == 0 || in->ve_values[i] > hi) {
            hi = in->ve_values[i];
        }
    }
    if (!any_pos) {
        add_finding(out, cap, n, LT_SEV_BLOCKER, "ve_table_zero", "VE table is all zeros",
                    "Corrections scale the existing value, so a table of zeros can never change.",
                    NULL, NULL);
        return;
    }
    if (hi > 255.0) {
        lt_snprintf(c, sizeof c, "max %.0f", hi);
        add_finding(out, cap, n, LT_SEV_WARNING, "ve_table_scale",
                    "VE values are larger than the table can hold",
                    "Some VE values exceed 255, the largest value a one-byte table cell stores. "
                    "The table may be in the wrong units.", c, "<= 255");
    }
}

static void check_filters(const lt_preflight_input *in, const lt_at_filters *f, lt_finding *out,
                          size_t cap, size_t *n)
{
    double sane_lo, sane_hi, suggest;
    const char *unit;
    char c[64], g[64], d[600];
    lt_bool boils;
    if (in->celsius) {
        sane_lo = 40.0;
        sane_hi = 100.0;
        unit = "C";
        suggest = 60.0;
    } else {
        sane_lo = 100.0;
        sane_hi = 210.0;
        unit = "F";
        suggest = 140.0;
    }
    if (f->min_clt < sane_lo || f->min_clt > sane_hi) {
        boils = (in->celsius && f->min_clt > 100.0) ? LT_TRUE : LT_FALSE;
        lt_snprintf(c, sizeof c, "%.0f %s", f->min_clt, unit);
        lt_snprintf(g, sizeof g, "%.0f %s", suggest, unit);
        lt_snprintf(d, sizeof d,
                    "The minimum coolant temperature filter is %.0f but this project works in "
                    "%s. %s",
                    f->min_clt, unit,
                    boils ? "That is above boiling, so no sample would ever be accepted."
                          : "Samples may be accepted too early or never.");
        add_finding(out, cap, n, boils ? LT_SEV_BLOCKER : LT_SEV_WARNING, "min_clt_units",
                    "Minimum coolant temperature looks like the wrong units", d, c, g);
    }
    if (f->min_rpm >= f->max_rpm) {
        lt_snprintf(c, sizeof c, "%.0f..%.0f", f->min_rpm, f->max_rpm);
        add_finding(out, cap, n, LT_SEV_BLOCKER, "rpm_window_empty", "RPM filter window is empty",
                    "The minimum RPM is not below the maximum RPM, so no sample can pass.", c,
                    "min below max");
    }
    if (f->max_tps_rate <= 0.0) {
        lt_snprintf(c, sizeof c, "%.1f", f->max_tps_rate);
        add_finding(out, cap, n, LT_SEV_WARNING, "tps_rate_inert",
                    "Transient filter is switched off",
                    "A maximum throttle rate of zero or less disables the transient filter, so "
                    "throttle movement is not excluded.", c, "10 %/s");
    }
}

static void check_authority(const lt_at_authority *a, lt_finding *out, size_t cap, size_t *n)
{
    char c[64];
    if (a->max_cell_value_change <= 0.0 || a->max_cell_percentage_change <= 0.0) {
        lt_snprintf(c, sizeof c, "%.1f / %.1f%%", a->max_cell_value_change,
                    a->max_cell_percentage_change);
        add_finding(out, cap, n, LT_SEV_BLOCKER, "authority_zero", "Authority limit is zero",
                    "With a zero change limit AutoTune is not allowed to move any cell.", c,
                    "10 / 20%");
    }
    if (a->min_cell_value > a->max_cell_value) {
        lt_snprintf(c, sizeof c, "%.0f..%.0f", a->min_cell_value, a->max_cell_value);
        add_finding(out, cap, n, LT_SEV_WARNING, "rails_reversed",
                    "Cell value limits are the wrong way round",
                    "The minimum cell value is above the maximum. The limits will be swapped.", c,
                    "min below max");
    }
}

size_t lt_preflight_check(const lt_preflight_input *in, const lt_at_settings *s,
                          const lt_at_filters *f, const lt_at_authority *a, lt_finding *out,
                          size_t cap)
{
    size_t n = 0, i, j, m;
    lt_finding *tmp;
    size_t tcap = 16;
    tmp = (lt_finding *)lt_malloc(tcap * sizeof *tmp);
    check_target(in, s, tmp, tcap, &n);
    check_ve(in, tmp, tcap, &n);
    check_filters(in, f, tmp, tcap, &n);
    check_authority(a, tmp, tcap, &n);
    if (s->lambda_delay_ms <= 0.0 && !s->lambda_delay_flow_scaled) {
        add_finding(tmp, tcap, &n, LT_SEV_WARNING, "delay_default_curve",
                    "Using the built-in RPM delay curve",
                    "No transport delay has been measured or configured, so a generic curve "
                    "(200 ms at idle falling to 50 ms) is assumed. A wrong delay attributes AFR "
                    "readings to the wrong cell.",
                    "200-50 ms (assumed)", "measure, or enable flow scaling");
    }
    if (in->will_write_to_ecu) {
        add_finding(tmp, tcap, &n, LT_SEV_INFO, "writes_live",
                    "Changes will be written to ECU RAM as they are accepted",
                    "Accepted recommendations are sent to the ECU immediately. Burn to keep them.",
                    NULL, NULL);
    } else {
        add_finding(tmp, tcap, &n, LT_SEV_INFO, "collect_only",
                    "Recommendations only - nothing will be written",
                    "AutoTune only collects recommendations. Apply them yourself when ready.",
                    NULL, NULL);
    }
    if (n > tcap) {
        n = tcap;   /* cannot happen with the fixed number of checks; defensive */
    }
    /* stable sort by severity */
    m = 0;
    for (j = 0; j < 3; j++) {
        for (i = 0; i < n; i++) {
            if ((int)tmp[i].sev == (int)j) {
                if (out && m < cap) {
                    out[m] = tmp[i];
                }
                m++;
            }
        }
    }
    lt_free(tmp);
    return out ? LT_MIN(m, cap) : m;
}

lt_bool lt_preflight_has_blocker(const lt_finding *f, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (f[i].sev == LT_SEV_BLOCKER) {
            return LT_TRUE;
        }
    }
    return LT_FALSE;
}

/* ================================================================ delay measurement */

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* median of v[0..n); sorts v in place; n>0 */
static double median_sorted(double *v, size_t n)
{
    qsort(v, n, sizeof(double), cmp_dbl);
    if (n % 2 == 1) {
        return v[n / 2];
    }
    return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

static double mad_of(const double *v, size_t n, double med)
{
    double *tmp = (double *)lt_malloc(n * sizeof(double));
    double r;
    size_t i;
    for (i = 0; i < n; i++) {
        tmp[i] = fabs(v[i] - med);
    }
    r = median_sorted(tmp, n);
    lt_free(tmp);
    return r;
}

const char *lt_delay_rejection_text(lt_delay_rejection r)
{
    switch (r) {
    case LT_DR_INSUFFICIENT_BASELINE:
        return "too few baseline samples";
    case LT_DR_UNSTABLE_BASELINE:
        return "baseline unstable - hold steadier";
    case LT_DR_NO_RESPONSE:
        return "no AFR response detected";
    case LT_DR_RESPONSE_PRECEDES_STEP:
        return "AFR already moving at the step";
    case LT_DR_RESPONSE_NOT_SETTLED:
        return "still moving at end of hold - use a longer hold";
    }
    return "unknown";
}

/* first run of >=2 consecutive samples below level; returns crossing time and edge time */
static lt_bool first_crossing(const lt_afr_sample *post, size_t n, double level, double *crossing,
                              unsigned long *edge_t)
{
    size_t i, run = 0;
    const lt_afr_sample *edge = NULL, *before = NULL, *prev = NULL;
    double frac;
    for (i = 0; i < n; i++) {
        const lt_afr_sample *s = &post[i];
        if (s->afr < level) {
            run++;
            if (run == 1) {
                edge = s;
                before = prev;
            }
            if (run >= 2) {
                if (before && before->afr > edge->afr && before->t_ms < edge->t_ms) {
                    frac = (before->afr - level) / (before->afr - edge->afr);
                    frac = frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac);
                    *crossing = (double)before->t_ms +
                                (double)(edge->t_ms - before->t_ms) * frac;
                } else {
                    *crossing = (double)edge->t_ms;
                }
                *edge_t = edge->t_ms;
                return LT_TRUE;
            }
        } else {
            run = 0;
            edge = NULL;
            before = NULL;
        }
        prev = s;
    }
    return LT_FALSE;
}

int lt_detect_delay(unsigned long anchor_ms, const lt_afr_sample *pre, size_t npre,
                    const lt_afr_sample *post, size_t npost, lt_delay_measurement *out,
                    lt_delay_rejection *why)
{
    double *v, baseline, scatter, threshold, leading, crossing, plateau, excursion, half_level,
        half_ms, first_t, last_t, tail_start;
    unsigned long edge_t = 0, dummy;
    size_t i, ntail, edge_idx, from;
    lt_delay_rejection r = LT_DR_NO_RESPONSE;
    int rc = LT_EDATA;

#define DD_FAIL(code) do { r = (code); goto fail_; } while (0)

    v = NULL;
    if (npre < 4) {
        DD_FAIL(LT_DR_INSUFFICIENT_BASELINE);
    }
    v = (double *)lt_malloc((npre > npost ? npre : npost) * sizeof(double) + sizeof(double));
    for (i = 0; i < npre; i++) {
        v[i] = pre[i].afr;
    }
    baseline = median_sorted(v, npre);
    scatter = mad_of(v, npre, baseline);
    if (scatter > 0.35) {
        DD_FAIL(LT_DR_UNSTABLE_BASELINE);
    }
    threshold = LT_MAX(3.0 * scatter, 0.15);
    if (!first_crossing(post, npost, baseline - threshold, &leading, &edge_t)) {
        DD_FAIL(LT_DR_NO_RESPONSE);
    }
    if (leading <= (double)anchor_ms) {
        DD_FAIL(LT_DR_RESPONSE_PRECEDES_STEP);
    }
    first_t = npost ? (double)post[0].t_ms : 0.0;
    last_t = npost ? (double)post[npost - 1].t_ms : 0.0;
    tail_start = last_t - (last_t - first_t) * 0.30;
    ntail = 0;
    for (i = 0; i < npost; i++) {
        if ((double)post[i].t_ms >= tail_start) {
            v[ntail++] = post[i].afr;
        }
    }
    if (ntail < 4) {
        DD_FAIL(LT_DR_RESPONSE_NOT_SETTLED);
    }
    plateau = median_sorted(v, ntail);
    if (mad_of(v, ntail, plateau) > 0.30) {
        DD_FAIL(LT_DR_RESPONSE_NOT_SETTLED);
    }
    excursion = baseline - plateau;
    if (excursion < threshold) {
        DD_FAIL(LT_DR_NO_RESPONSE);
    }
    half_level = baseline - excursion / 2.0;
    edge_idx = 0;
    for (i = 0; i < npost; i++) {
        if (post[i].t_ms >= edge_t) {
            edge_idx = i;
            break;
        }
    }
    from = 0;
    for (i = edge_idx; i > 0; i--) {
        if (post[i - 1].afr >= half_level) {
            from = i - 1;
            break;
        }
    }
    if (!first_crossing(post + from, npost - from, half_level, &crossing, &dummy)) {
        DD_FAIL(LT_DR_RESPONSE_NOT_SETTLED);
    }
    half_ms = crossing;
    if (half_ms <= (double)anchor_ms) {
        DD_FAIL(LT_DR_RESPONSE_PRECEDES_STEP);
    }
    if (out) {
        out->delay_ms = half_ms - (double)anchor_ms;
        out->excursion = excursion;
        out->baseline_afr = baseline;
        out->leading_edge_ms = leading - (double)anchor_ms;
    }
    lt_free(v);
    return 0;
fail_:
    if (v) {
        lt_free(v);
    }
    if (why) {
        *why = r;
    }
    lt_err_set("%s", lt_delay_rejection_text(r));
    return rc;
#undef DD_FAIL
}

/* ---- aggregate delay table ---- */

static size_t edge_bin(const double *edges, size_t n, double v)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (v < edges[i]) {
            return i;
        }
    }
    return n;
}

void lt_delay_table_init(lt_delay_table *t)
{
    memset(t, 0, sizeof *t);
}

void lt_delay_table_add(lt_delay_table *t, double rpm, double load, double delay_ms)
{
    static const double rpm_edges[5] = {1200.0, 2000.0, 3000.0, 4500.0, 6500.0};
    static const double load_edges[4] = {40.0, 60.0, 80.0, 100.0};
    size_t r = edge_bin(rpm_edges, 5, rpm);
    size_t l = edge_bin(load_edges, 4, load);
    lt_delay_cell *c = &t->cells[l][r];
    if (c->n == 0) {
        c->min_ms = c->max_ms = delay_ms;
    } else {
        c->min_ms = LT_MIN(c->min_ms, delay_ms);
        c->max_ms = LT_MAX(c->max_ms, delay_ms);
    }
    c->mean_ms = (c->mean_ms * (double)c->n + delay_ms) / (double)(c->n + 1);
    c->n++;
    c->range_ms = c->max_ms - c->min_ms;
}

lt_bool lt_fit_flow_delay(const double *rpm, const double *load, const double *ms, size_t n,
                          lt_flow_fit *out)
{
    size_t i, cnt = 0;
    double *x, *y, floor_ms, num, den, k, ss, best_ss = 0.0, d, best_floor = 0.0, best_k = 0.0;
    lt_bool have = LT_FALSE;
    x = (double *)lt_malloc((n + 1) * sizeof(double));
    y = (double *)lt_malloc((n + 1) * sizeof(double));
    for (i = 0; i < n; i++) {
        if (rpm[i] > 0.0 && load[i] > 0.0 && ms[i] > 0.0) {
            x[cnt] = 1.0 / (rpm[i] * load[i]);
            y[cnt] = ms[i];
            cnt++;
        }
    }
    if (cnt < 4) {
        lt_free(x);
        lt_free(y);
        return LT_FALSE;
    }
    for (floor_ms = 40.0; floor_ms <= 250.0; floor_ms += 5.0) {
        num = den = 0.0;
        for (i = 0; i < cnt; i++) {
            num += x[i] * (y[i] - floor_ms);
            den += x[i] * x[i];
        }
        if (den <= 0.0) {
            continue;
        }
        k = num / den;
        ss = 0.0;
        for (i = 0; i < cnt; i++) {
            d = floor_ms + k * x[i] - y[i];
            ss += d * d;
        }
        if (!have || ss < best_ss) {
            have = LT_TRUE;
            best_ss = ss;
            best_floor = floor_ms;
            best_k = k;
        }
    }
    lt_free(x);
    lt_free(y);
    if (!have) {
        return LT_FALSE;
    }
    if (out) {
        out->floor_ms = best_floor;
        out->k = best_k;
        out->anchor_ms = best_floor + best_k / (800.0 * 40.0);
        out->rms_ms = sqrt(best_ss / (double)cnt);
        out->samples = cnt;
    }
    return LT_TRUE;
}

/* ================================================================ accel enrichment */

void lt_ae_config_default(lt_ae_config *c)
{
    static const double bins[4] = {60.0, 160.0, 400.0, 600.0};
    static const double rates[4] = {10.0, 13.0, 17.0, 20.0};
    size_t i;
    memset(c, 0, sizeof *c);
    for (i = 0; i < 4; i++) {
        c->tae_bins[i] = bins[i];
        c->tae_rates[i] = rates[i];
    }
    c->nbins = 4;
    c->nrates = 4;
    c->ae_time_ms = 160.0;
    c->delay_ms = 800.0;
    c->event_floor = 30.0;
    c->min_clt = 70.0;
    c->min_events = 8;
    c->authority_pct = 5.0;
    c->rate_gain_pct_per_afr = 100.0 / 14.7;
    c->target_afr_fallback = 14.7;
}

static double afr_err(const lt_ae_sample *s, const lt_ae_config *c)
{
    return s->afr - (s->afr_target > 0.0 ? s->afr_target : c->target_afr_fallback);
}

size_t lt_ae_analyze(const lt_ae_sample *s, size_t n, const lt_ae_config *c, lt_ae_event *out,
                     size_t cap)
{
    double first_bin = c->nbins ? c->tae_bins[0] : 60.0;
    double settle = c->ae_time_ms + 500.0;
    size_t i = 1, j, k, peak_i, start, count = 0, nresp, nbase;
    double peak, peak_ts, start_ts, resp_lo, resp_hi, ts, *resp, *base, sum, med;
    if (n < 2) {
        return 0;
    }
    resp = (double *)lt_malloc(n * sizeof(double));
    base = (double *)lt_malloc(n * sizeof(double));
    while (i < n) {
        if (s[i].tps_dot < c->event_floor) {
            i++;
            continue;
        }
        start = i;
        j = i;
        peak = s[i].tps_dot;
        peak_i = i;
        while (j < n && s[j].tps_dot >= c->event_floor * 0.5) {
            if (s[j].tps_dot > peak) {
                peak = s[j].tps_dot;
                peak_i = j;
            }
            j++;
        }
        if (j == i) {
            j = i + 1;
        }
        if (s[peak_i].clt >= c->min_clt && peak >= first_bin) {
            peak_ts = (double)s[peak_i].timestamp_ms;
            start_ts = (double)s[start].timestamp_ms;
            resp_lo = peak_ts + c->delay_ms;
            resp_hi = resp_lo + settle;
            nresp = nbase = 0;
            for (k = 0; k < n; k++) {
                ts = (double)s[k].timestamp_ms;
                if (ts >= resp_lo && ts <= resp_hi) {
                    resp[nresp++] = afr_err(&s[k], c);
                } else if ((ts >= start_ts - 400.0 && ts < start_ts) ||
                           (ts > resp_hi && ts <= resp_hi + 400.0)) {
                    base[nbase++] = afr_err(&s[k], c);
                }
            }
            if (nresp >= 3 && nbase >= 3) {
                sum = 0.0;
                for (k = 0; k < nresp; k++) {
                    sum += resp[k];
                }
                med = median_sorted(base, nbase);
                if (out && count < cap) {
                    out[count].peak_tps_dot = peak;
                    out[count].residual_afr = sum / (double)nresp - med;
                }
                count++;
            }
        }
        i = j + 1;
    }
    lt_free(resp);
    lt_free(base);
    return out ? LT_MIN(count, cap) : count;
}

size_t lt_ae_recommend(const lt_ae_event *ev, size_t n, const lt_ae_config *c, lt_ae_bin_rec *out,
                       size_t cap)
{
    size_t b, i, cnt, m = 0;
    double *r, med, delta;
    if (!out) {
        return c->nbins;
    }
    r = (double *)lt_malloc((n + 1) * sizeof(double));
    for (b = 0; b < c->nbins && m < cap; b++) {
        lt_ae_bin_rec *o = &out[m++];
        cnt = 0;
        for (i = 0; i < n; i++) {
            /* bin_of: last index with peak >= bins[idx] */
            size_t k, bi = c->nbins;
            for (k = 0; k < c->nbins; k++) {
                if (ev[i].peak_tps_dot >= c->tae_bins[k]) {
                    bi = k;
                }
            }
            if (bi == b) {
                r[cnt++] = ev[i].residual_afr;
            }
        }
        memset(o, 0, sizeof *o);
        o->bin = b;
        o->lo = c->tae_bins[b];
        o->has_hi = (b + 1 < c->nbins) ? LT_TRUE : LT_FALSE;
        o->hi = o->has_hi ? c->tae_bins[b + 1] : 0.0;
        o->events = cnt;
        o->current_rate = b < c->nrates ? c->tae_rates[b] : 0.0;
        o->recommended_rate = o->current_rate;
        med = 0.0;
        if (cnt > 0) {
            med = median_sorted(r, cnt);
            o->has_median = LT_TRUE;
            o->median_residual_afr = med;
        }
        if (cnt < c->min_events) {
            o->sufficient_data = LT_FALSE;
            lt_snprintf(o->note, sizeof o->note, "insufficient data (need %lu) - held",
                        (unsigned long)c->min_events);
            continue;
        }
        o->sufficient_data = LT_TRUE;
        delta = med * c->rate_gain_pct_per_afr;
        delta = delta < -c->authority_pct ? -c->authority_pct
                                          : (delta > c->authority_pct ? c->authority_pct : delta);
        o->recommended_rate = LT_MAX(o->current_rate + delta, 0.0);
        if (delta > 0.3) {
            lt_snprintf(o->note, sizeof o->note, "richen (+%.1f); tip-ins run %+.2f AFR lean",
                        delta, med);
        } else if (delta < -0.3) {
            lt_snprintf(o->note, sizeof o->note, "lean (%.1f); tip-ins run %+.2f AFR rich", delta,
                        med);
        } else {
            lt_snprintf(o->note, sizeof o->note, "on target (%+.2f AFR); no change", med);
        }
    }
    lt_free(r);
    return m;
}
