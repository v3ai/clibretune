/* dl_ana.c - channel statistics, key tracking, alert rules. SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dl_ana.h"

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int is_fin(double v) { return v == v && v - v == 0.0; }

static double pctile(const double *sorted, long n, int p)
{
    long idx = (long)ceil((double)p * (double)n / 100.0) - 1;
    if (idx < 0) { idx = 0; }
    if (idx >= n) { idx = n - 1; }
    return sorted[idx];
}

int lt_stats_compute(const double *v, long n, lt_stats *out)
{
    double *s, sum = 0.0, var = 0.0;
    long i, m = 0;
    memset(out, 0, sizeof(*out));
    if (v == NULL || n <= 0) { return -1; }
    s = (double *)lt_malloc((size_t)n * sizeof(double));
    for (i = 0; i < n; i++) {
        if (is_fin(v[i])) { s[m++] = v[i]; sum += v[i]; }
    }
    if (m == 0) { lt_free(s); return -1; }
    qsort(s, (size_t)m, sizeof(double), cmp_dbl);
    out->n = m;
    out->mean = sum / (double)m;
    for (i = 0; i < m; i++) { var += (s[i] - out->mean) * (s[i] - out->mean); }
    out->stddev = sqrt(var / (double)m);
    out->median = (m % 2 == 0) ? (s[m / 2 - 1] + s[m / 2]) / 2.0 : s[m / 2];
    out->vmin = s[0];
    out->vmax = s[m - 1];
    out->p25 = pctile(s, m, 25);
    out->p75 = pctile(s, m, 75);
    out->iqr = out->p75 - out->p25;
    lt_free(s);
    return 0;
}

int lt_stats_log_channel(const lt_log *l, int ch, long i0, long i1, lt_stats *out)
{
    double *tmp;
    long i, m;
    int rc;
    memset(out, 0, sizeof(*out));
    if (l == NULL || ch < 0 || ch >= l->n_ch || l->n <= 0) { return -1; }
    if (i0 < 0) { i0 = 0; }
    if (i1 >= l->n) { i1 = l->n - 1; }
    if (i1 < i0) { return -1; }
    m = i1 - i0 + 1;
    tmp = (double *)lt_malloc((size_t)m * sizeof(double));
    for (i = 0; i < m; i++) { tmp[i] = LT_LOG_AT(l, i0 + i, ch); }
    rc = lt_stats_compute(tmp, m, out);
    lt_free(tmp);
    return rc;
}

void lt_keytrack_init(lt_keytrack *k) { memset(k, 0, sizeof(*k)); }

int lt_keytrack_update(lt_keytrack *k, double rpm, double now_ms, double threshold_rpm,
                       double timeout_s)
{
    int above = rpm >= threshold_rpm;
    if (!k->on) {
        if (above) { k->on = 1; k->pending = 0; return 1; }
        return 0;
    }
    if (above) { k->pending = 0; return 0; }
    if (!k->pending) { k->pending = 1; k->off_start_ms = now_ms; return 0; }
    if (now_ms - k->off_start_ms >= timeout_s * 1000.0) {
        k->on = 0; k->pending = 0;
        return -1;
    }
    return 0;
}

int lt_alert_rule_check(lt_alert_rule *r, double value)
{
    int hit;
    if (!r->enabled || !is_fin(value)) { r->active = 0; return 0; }
    hit = r->above ? value > r->limit : value < r->limit;
    if (hit && !r->active) { r->active = 1; return 1; }
    if (!hit) { r->active = 0; }
    return 0;
}

int lt_alert_large_change(double old_v, double new_v, double abs_thr, double pct_thr, double *delta)
{
    double d = new_v - old_v, ad = fabs(d), pct;
    if (delta != NULL) { *delta = d; }
    if (!is_fin(d) || ad == 0.0) { return 0; }
    if (ad >= abs_thr) { return 1; }
    if (old_v != 0.0) {
        pct = ad / fabs(old_v) * 100.0;
        if (pct >= pct_thr) { return 1; }
    }
    return 0;
}
