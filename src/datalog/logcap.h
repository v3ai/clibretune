/* logcap.h - capture buffer and analysis for the INI [LoggerDefinition] loggers (tooth
 * logger, composite logger): decoding blocks against the declared record layout, derived
 * tooth intervals, statistics and CSV text.  Pure logic, no I/O.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_LOGCAP_H
#define LT_LOGCAP_H

#include "../lt.h"
#include "../ini/ini.h"

/* the capture keeps the most recent records only (a long capture exceeds any chart) */
#define LT_LOGCAP_MAX 20000

typedef struct lt_logcap {
    const lt_ini_logger *lg;
    size_t nf;                 /* fields per record */
    double *vals;              /* n * nf decoded values */
    size_t n, cap;
    unsigned long n_seen;       /* records seen over the whole capture */
    unsigned long reads, empty_reads;
    lt_buf last;               /* previous payload (identical re-reads are "not ready") */
    char note[160];
} lt_logcap;

void lt_logcap_init(lt_logcap *c, const lt_ini_logger *lg);
void lt_logcap_free(lt_logcap *c);
void lt_logcap_clear(lt_logcap *c);
/* Feed one read reply.  Zero-filled records and a payload identical to the previous one are
 * skipped (the ECU serves the same buffer until it refills).  Returns records added. */
size_t lt_logcap_feed(lt_logcap *c, const lt_u8 *payload, size_t len);
int    lt_logcap_field(const lt_logcap *c, const char *name);       /* index or -1 */
double lt_logcap_val(const lt_logcap *c, size_t rec, int field);
/* the logger to use: first of kind `kind` (case-insensitive), else (fallback) the first one */
const lt_ini_logger *lt_logcap_pick(const lt_ini *ini, const char *kind, int fallback);

/* ---- tooth view ---- */
/* Per-tooth intervals in microseconds.  Uses toothTime when declared; otherwise differences of
 * the running timestamp (time/refTime, ms), and for a composite log only rising edges of the
 * primary level count as teeth.  Returns the number written (<= max). */
size_t lt_logcap_tooth_us(const lt_logcap *c, double *out, size_t max);

typedef struct lt_tooth_stats {
    double avg, sd, min, max, variability;   /* variability = sd / avg in percent */
    long   missing_idx;                      /* first interval > 1.8 x avg, -1 none */
    size_t missing_n;                        /* intervals > 1.8 x avg */
    size_t extra_n;                          /* intervals < 0.5 x avg (extra / noise teeth) */
} lt_tooth_stats;
/* returns 0, or -1 with fewer than 2 intervals */
int    lt_tooth_stats_calc(const double *us, size_t n, lt_tooth_stats *st);
/* rpm from the last `teeth_per_rev` intervals, 0 when not enough data */
double lt_tooth_rpm(const double *us, size_t n, int teeth_per_rev);
/* colour class of one interval: 0 normal (<5% off the average), 1 warning (<15%), 2 problem */
int    lt_tooth_class(double us, double avg);
void   lt_tooth_csv(const double *us, size_t n, lt_buf *out);

/* ---- composite view ---- */
#define LT_COMP_CHANNELS 6
typedef struct lt_comp_sample {
    double t_us;
    int    lvl[LT_COMP_CHANNELS];   /* primary, secondary, sync, trigger, coil, injector */
} lt_comp_sample;
extern const char *const lt_comp_names[LT_COMP_CHANNELS];
size_t lt_logcap_composite(const lt_logcap *c, lt_comp_sample *out, size_t max);
/* bit mask of channels the logger declares (bit i = lt_comp_names[i]) */
unsigned lt_logcap_comp_mask(const lt_logcap *c);

typedef struct lt_comp_stats {
    double duration_ms, rate_hz;
    unsigned long pri_pulses, sec_pulses, sync_lost;
    int sync_acquired;
} lt_comp_stats;
int  lt_comp_stats_calc(const lt_comp_sample *s, size_t n, lt_comp_stats *st);
void lt_comp_csv(const lt_comp_sample *s, size_t n, lt_buf *out);

#endif
