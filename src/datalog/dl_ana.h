/* dl_ana.h - log analysis helpers: channel statistics, key-on/key-off tracking for smart
 * recording, alert rules (channel thresholds and large table changes).  Pure logic, no UI.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_DL_ANA_H
#define LT_DL_ANA_H

#include "datalog.h"

/* ---- statistics (spec: LoggerStatsPanel) ---- */
typedef struct lt_stats {
    long n;                                  /* finite samples used */
    double mean, median, stddev;             /* stddev: population */
    double vmin, vmax, p25, p75, iqr;        /* percentile = sorted[ceil(p*n)-1] */
} lt_stats;
/* NaN/inf values are skipped. Returns 0, or -1 when there is no finite value (out zeroed). */
int lt_stats_compute(const double *v, long n, lt_stats *out);
/* Statistics of log channel `ch` over samples [i0, i1] (inclusive, clamped). */
int lt_stats_log_channel(const lt_log *l, int ch, long i0, long i1, lt_stats *out);

/* ---- smart recording: key on/off from RPM ---- */
typedef struct lt_keytrack {
    int on;                  /* current key state */
    int pending;             /* below threshold, timing the off timeout */
    double off_start_ms;
} lt_keytrack;
#define LT_KEY_RPM_MIN 50
#define LT_KEY_RPM_MAX 500
#define LT_KEY_TIMEOUT_MIN 1
#define LT_KEY_TIMEOUT_MAX 10
void lt_keytrack_init(lt_keytrack *k);
/* Returns +1 on key-on, -1 on key-off (after the timeout), else 0. */
int  lt_keytrack_update(lt_keytrack *k, double rpm, double now_ms, double threshold_rpm,
                        double timeout_s);

/* ---- alert rules ---- */
typedef struct lt_alert_rule {
    char channel[48];
    int  above;              /* 1: trigger when value > limit, 0: when value < limit */
    double limit;
    int  enabled;
    int  active;             /* runtime: currently triggered (edge detection) */
} lt_alert_rule;
/* Returns 1 on the rising edge (newly triggered), else 0; updates rule->active. NaN never triggers. */
int  lt_alert_rule_check(lt_alert_rule *r, double value);
/* Table edit guard: nonzero when |new-old| >= abs_thr or the percent change (from old) >= pct_thr.
 * *delta receives the signed change when non-NULL. */
int  lt_alert_large_change(double old_v, double new_v, double abs_thr, double pct_thr, double *delta);

#endif
