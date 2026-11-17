/* dl_logcap.c - logger capture buffer, tooth/composite analysis and CSV text.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>
#include "logcap.h"

const char *const lt_comp_names[LT_COMP_CHANNELS] = {
    "Primary", "Secondary", "Sync", "Trigger", "Coil", "Injector"
};
static const char *const comp_fields[LT_COMP_CHANNELS] = {
    "priLevel", "secLevel", "sync", "trigger", "coil", "inj"
};

void lt_logcap_init(lt_logcap *c, const lt_ini_logger *lg)
{
    memset(c, 0, sizeof(*c));
    c->lg = lg;
    c->nf = lg != NULL ? lg->n_fields : 0;
    lt_buf_init(&c->last);
}

void lt_logcap_free(lt_logcap *c)
{
    lt_free(c->vals);
    lt_buf_free(&c->last);
    c->vals = NULL;
    c->n = 0;
    c->cap = 0;
}

void lt_logcap_clear(lt_logcap *c)
{
    c->n = 0;
    c->n_seen = 0;
    c->reads = 0;
    c->empty_reads = 0;
    c->note[0] = '\0';
    lt_buf_clear(&c->last);
}

int lt_logcap_field(const lt_logcap *c, const char *name)
{
    size_t i;
    if (c->lg == NULL) { return -1; }
    for (i = 0; i < c->lg->n_fields; i++) {
        if (c->lg->fields[i].name != NULL && strcmp(c->lg->fields[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

double lt_logcap_val(const lt_logcap *c, size_t rec, int field)
{
    if (field < 0 || rec >= c->n || (size_t)field >= c->nf) { return 0.0; }
    return c->vals[rec * c->nf + (size_t)field];
}

static void ensure(lt_logcap *c, size_t extra)
{
    size_t need, nc, bytes;
    if (c->nf == 0) { return; }
    need = c->n + extra;
    if (need <= c->cap) { return; }
    nc = c->cap > 0 ? c->cap : 256;
    while (nc < need) { nc *= 2; }
    if (!lt_mul_size(nc, c->nf * sizeof(double), &bytes)) { return; }
    c->vals = (double *)lt_realloc(c->vals, bytes);
    c->cap = nc;
}

size_t lt_logcap_feed(lt_logcap *c, const lt_u8 *payload, size_t len)
{
    size_t n, i, added = 0;
    const lt_ini_logger *lg = c->lg;
    c->reads++;
    if (lg == NULL || lg->record_len == 0 || c->nf == 0) { c->empty_reads++; return 0; }
    n = lt_ini_logger_record_count(lg, len);
    if (n == 0) { c->empty_reads++; return 0; }
    if (c->last.len == len && len > 0 && memcmp(c->last.data, payload, len) == 0) {
        c->empty_reads++;
        return 0;
    }
    lt_buf_clear(&c->last);
    lt_buf_append(&c->last, payload, len);
    ensure(c, n);
    for (i = 0; i < n; i++) {
        const lt_u8 *rec = payload + lg->header_len + i * lg->record_len;
        if (lt_ini_logger_is_empty(lg, rec, lg->record_len)) { continue; }
        if (c->n >= LT_LOGCAP_MAX || c->n >= c->cap) {
            size_t drop = 1024;     /* drop a block at once, not one record per record */
            if (drop > c->n) { drop = c->n; }
            memmove(c->vals, c->vals + drop * c->nf, (c->n - drop) * c->nf * sizeof(double));
            c->n -= drop;
        }
        (void)lt_ini_logger_decode(lg, rec, lg->record_len, c->vals + c->n * c->nf, NULL);
        c->n++;
        c->n_seen++;
        added++;
    }
    if (added == 0) { c->empty_reads++; } else { c->empty_reads = 0; }
    return added;
}

const lt_ini_logger *lt_logcap_pick(const lt_ini *ini, const char *kind, int fallback)
{
    size_t i;
    if (ini == NULL || ini->n_loggers == 0) { return NULL; }
    for (i = 0; i < ini->n_loggers; i++) {
        if (ini->loggers[i].kind != NULL && lt_stricmp(ini->loggers[i].kind, kind) == 0) {
            return &ini->loggers[i];
        }
    }
    return fallback ? &ini->loggers[0] : NULL;
}

/* ------------------------------------------------------------ tooth */
size_t lt_logcap_tooth_us(const lt_logcap *c, double *out, size_t max)
{
    int f_tt = lt_logcap_field(c, "toothTime");
    int f_t = lt_logcap_field(c, "refTime");
    int f_pri = lt_logcap_field(c, "priLevel");
    size_t i, n = 0;
    double prev = 0.0;
    int have_prev = 0, prev_lvl = 0;
    if (f_t < 0) { f_t = lt_logcap_field(c, "time"); }
    if (f_tt >= 0) {
        for (i = 0; i < c->n && n < max; i++) { out[n++] = lt_logcap_val(c, i, f_tt); }
        return n;
    }
    if (f_t < 0) { return 0; }
    for (i = 0; i < c->n && n < max; i++) {
        double t = lt_logcap_val(c, i, f_t);
        int lvl = f_pri >= 0 ? (lt_logcap_val(c, i, f_pri) != 0.0) : 1;
        if (f_pri >= 0) {
            int rising = lvl && !prev_lvl;
            prev_lvl = lvl;
            if (!rising) { continue; }
        }
        if (have_prev) { out[n++] = (t - prev) * 1000.0; }
        prev = t;
        have_prev = 1;
    }
    return n;
}

int lt_tooth_stats_calc(const double *us, size_t n, lt_tooth_stats *st)
{
    size_t i;
    double sum = 0.0, var = 0.0;
    memset(st, 0, sizeof(*st));
    st->missing_idx = -1;
    if (n < 2) { return -1; }
    st->min = us[0];
    st->max = us[0];
    for (i = 0; i < n; i++) {
        sum += us[i];
        if (us[i] < st->min) { st->min = us[i]; }
        if (us[i] > st->max) { st->max = us[i]; }
    }
    st->avg = sum / (double)n;
    for (i = 0; i < n; i++) { var += (us[i] - st->avg) * (us[i] - st->avg); }
    st->sd = sqrt(var / (double)n);
    st->variability = st->avg > 0.0 ? st->sd / st->avg * 100.0 : 0.0;
    for (i = 0; i < n; i++) {
        if (us[i] > st->avg * 1.8) {
            st->missing_n++;
            if (st->missing_idx < 0) { st->missing_idx = (long)i; }
        } else if (us[i] < st->avg * 0.5) {
            st->extra_n++;
        }
    }
    return 0;
}

double lt_tooth_rpm(const double *us, size_t n, int teeth_per_rev)
{
    size_t k, i;
    double tot = 0.0;
    if (teeth_per_rev < 1 || n < (size_t)teeth_per_rev) { return 0.0; }
    k = (size_t)teeth_per_rev;
    for (i = n - k; i < n; i++) { tot += us[i]; }
    return tot > 0.0 ? 60000000.0 / tot : 0.0;
}

int lt_tooth_class(double us, double avg)
{
    double dev;
    if (avg <= 0.0) { return 0; }
    dev = fabs(us - avg) / avg;
    return dev < 0.05 ? 0 : (dev < 0.15 ? 1 : 2);
}

void lt_tooth_csv(const double *us, size_t n, lt_buf *out)
{
    size_t i;
    lt_buf_puts(out, "Tooth Number,Time (us),Crank Angle (deg)\n");
    for (i = 0; i < n; i++) {
        char b[48];
        lt_fmt_double(b, sizeof(b), us[i], 3);
        lt_buf_printf(out, "%lu,%s,\n", (unsigned long)i, b);
    }
}

/* ------------------------------------------------------------ composite */
unsigned lt_logcap_comp_mask(const lt_logcap *c)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < LT_COMP_CHANNELS; i++) {
        if (lt_logcap_field(c, comp_fields[i]) >= 0) { m |= 1u << i; }
    }
    return m;
}

size_t lt_logcap_composite(const lt_logcap *c, lt_comp_sample *out, size_t max)
{
    int f[LT_COMP_CHANNELS], ft, k;
    size_t i, n = 0;
    double mult = 1000.0;
    for (k = 0; k < LT_COMP_CHANNELS; k++) { f[k] = lt_logcap_field(c, comp_fields[k]); }
    ft = lt_logcap_field(c, "time");
    if (ft < 0) { ft = lt_logcap_field(c, "refTime"); }
    if (ft < 0) {
        ft = lt_logcap_field(c, "toothTime");
        mult = 1.0;
    }
    if (ft < 0) { return 0; }
    for (i = 0; i < c->n && n < max; i++) {
        out[n].t_us = floor(lt_logcap_val(c, i, ft) * mult + 0.5);
        for (k = 0; k < LT_COMP_CHANNELS; k++) {
            out[n].lvl[k] = (f[k] >= 0 && lt_logcap_val(c, i, f[k]) != 0.0) ? 1 : 0;
        }
        n++;
    }
    return n;
}

int lt_comp_stats_calc(const lt_comp_sample *s, size_t n, lt_comp_stats *st)
{
    size_t i;
    double span;
    memset(st, 0, sizeof(*st));
    if (n < 2) { return -1; }
    span = s[n - 1].t_us - s[0].t_us;
    st->duration_ms = span / 1000.0;
    st->rate_hz = span > 0.0 ? (double)(n - 1) * 1000000.0 / span : 0.0;
    for (i = 0; i < n; i++) {
        if (s[i].lvl[2]) { st->sync_acquired = 1; }
        if (i == 0) { continue; }
        if (s[i].lvl[0] && !s[i - 1].lvl[0]) { st->pri_pulses++; }
        if (s[i].lvl[1] && !s[i - 1].lvl[1]) { st->sec_pulses++; }
        if (!s[i].lvl[2] && s[i - 1].lvl[2]) { st->sync_lost++; }
    }
    return 0;
}

void lt_comp_csv(const lt_comp_sample *s, size_t n, lt_buf *out)
{
    size_t i;
    lt_buf_puts(out, "Time (us),Primary,Secondary,Sync,Trigger,Coil,Injector\n");
    for (i = 0; i < n; i++) {
        lt_buf_printf(out, "%.0f,%d,%d,%d,%d,%d,%d\n", s[i].t_us, s[i].lvl[0], s[i].lvl[1],
                      s[i].lvl[2], s[i].lvl[3], s[i].lvl[4], s[i].lvl[5]);
    }
}
