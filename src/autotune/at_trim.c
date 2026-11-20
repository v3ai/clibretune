/* at_trim.c - Trim Table Analyze engine (see at_trim.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "at_trim.h"

typedef struct trim_cyl {
    lt_at_state *at;
    double *z;                      /* current trim values (trim units) */
    double *zm;                     /* the same as multiplier percent (engine reference) */
    unsigned long rail;             /* samples dropped at the raw sensor rail */
    int loaded;
} trim_cyl;

struct lt_trim_state {
    size_t ncyl, nx, ny;
    double *x, *y;
    lt_trim_units u;
    trim_cyl cyl[LT_TRIM_MAX_CYL];
    double *tx, *ty, *tz; size_t tnx, tny;      /* target table */
    double *vx, *vy, *vz; size_t vnx, vny;      /* VE factor table */
    lt_bool running;
};

/* ================================================================ units */

void lt_trim_units_detect(const lt_ini_const *z, lt_trim_units *u)
{
    u->offset = 0.0;
    u->scale = 1.0;
    if (z == NULL) { u->offset = 100.0; u->scale = 100.0; return; }
    if (z->min < 0.0) { u->offset = 100.0; u->scale = 100.0; }
    else if (z->max > 5.0) { u->scale = 100.0; }
}

double lt_trim_to_mult(const lt_trim_units *u, double v)
{
    return u->scale != 0.0 ? (v + u->offset) / u->scale : 1.0;
}

double lt_trim_from_mult(const lt_trim_units *u, double m)
{
    return m * u->scale - u->offset;
}

void lt_trim_config_default(lt_trim_config *c)
{
    memset(c, 0, sizeof(*c));
    lt_at_settings_default(&c->set);
    c->set.base_weight = 20.0;
    c->set.min_change = 0.2;
    lt_at_filters_default(&c->f);
    lt_at_authority_default(&c->auth);
    c->auth.max_cell_value_change = 5.0;
    c->auth.max_cell_percentage_change = 10.0;
    c->auth.min_cell_value = 75.0;
    c->auth.max_cell_value = 125.0;
    c->balance = LT_FALSE;
}

void lt_trim_config_clear(lt_trim_config *c)
{
    lt_at_filters_clear(&c->f);
}

/* ================================================================ state */

lt_trim_state *lt_trim_new(size_t ncyl)
{
    lt_trim_state *s = (lt_trim_state *)lt_calloc(1, sizeof(*s));
    size_t i;
    if (ncyl > LT_TRIM_MAX_CYL) { ncyl = LT_TRIM_MAX_CYL; }
    s->ncyl = ncyl;
    s->u.offset = 100.0;
    s->u.scale = 100.0;
    for (i = 0; i < ncyl; i++) {
        s->cyl[i].at = lt_at_new();
        lt_at_start(s->cyl[i].at);
    }
    return s;
}

static void free_arr(double **a) { lt_free(*a); *a = NULL; }

void lt_trim_free(lt_trim_state *s)
{
    size_t i;
    if (s == NULL) { return; }
    for (i = 0; i < s->ncyl; i++) {
        lt_at_free(s->cyl[i].at);
        free_arr(&s->cyl[i].z);
        free_arr(&s->cyl[i].zm);
    }
    free_arr(&s->x); free_arr(&s->y);
    free_arr(&s->tx); free_arr(&s->ty); free_arr(&s->tz);
    free_arr(&s->vx); free_arr(&s->vy); free_arr(&s->vz);
    lt_free(s);
}

size_t lt_trim_cylinders(const lt_trim_state *s) { return s->ncyl; }

static double *dup_arr(const double *a, size_t n)
{
    double *d = (double *)lt_malloc((n + 1) * sizeof(double));
    memcpy(d, a, n * sizeof(double));
    return d;
}

static void set_refs(lt_trim_state *s, size_t cyl)
{
    lt_at_reftables r;
    memset(&r, 0, sizeof(r));
    r.nx = s->nx;
    r.ny = s->ny;
    r.ve = s->cyl[cyl].zm;
    lt_at_set_reference_tables(s->cyl[cyl].at, &r);
}

int lt_trim_set_table(lt_trim_state *s, size_t cyl, const double *x, size_t nx, const double *y,
                      size_t ny, const double *z, const lt_trim_units *u)
{
    size_t i, n = nx * ny;
    trim_cyl *tc;
    if (cyl >= s->ncyl || x == NULL || y == NULL || z == NULL || nx == 0 || ny == 0) { return -1; }
    if (s->x == NULL || nx != s->nx || ny != s->ny) {
        free_arr(&s->x); free_arr(&s->y);
        s->x = dup_arr(x, nx);
        s->y = dup_arr(y, ny);
        s->nx = nx;
        s->ny = ny;
    } else {
        memcpy(s->x, x, nx * sizeof(double));
        memcpy(s->y, y, ny * sizeof(double));
    }
    if (u != NULL) { s->u = *u; }
    tc = &s->cyl[cyl];
    free_arr(&tc->z); free_arr(&tc->zm);
    tc->z = dup_arr(z, n);
    tc->zm = (double *)lt_malloc((n + 1) * sizeof(double));
    for (i = 0; i < n; i++) { tc->zm[i] = lt_trim_to_mult(&s->u, z[i]) * 100.0; }
    tc->loaded = 1;
    set_refs(s, cyl);
    return 0;
}

static void set_tab(double **xd, double **yd, double **zd, size_t *pnx, size_t *pny, const double *x,
                    size_t nx, const double *y, size_t ny, const double *z)
{
    free_arr(xd); free_arr(yd); free_arr(zd);
    *pnx = *pny = 0;
    if (z == NULL || x == NULL || y == NULL || nx == 0 || ny == 0) { return; }
    *xd = dup_arr(x, nx);
    *yd = dup_arr(y, ny);
    *zd = dup_arr(z, nx * ny);
    *pnx = nx;
    *pny = ny;
}

void lt_trim_set_target_table(lt_trim_state *s, const double *x, size_t nx, const double *y,
                              size_t ny, const double *z)
{
    set_tab(&s->tx, &s->ty, &s->tz, &s->tnx, &s->tny, x, nx, y, ny, z);
}

double lt_trim_target_at(const lt_trim_state *s, double rpm, double load, double fallback)
{
    double v;
    if (s->tz == NULL) { return fallback; }
    v = lt_at_table_lookup(s->tx, s->tnx, s->ty, s->tny, s->tz, rpm, load);
    return v > 0.1 ? lt_at_normalise_to_afr(v) : fallback;
}

void lt_trim_set_ve_factor_table(lt_trim_state *s, const double *x, size_t nx, const double *y,
                                 size_t ny, const double *z)
{
    set_tab(&s->vx, &s->vy, &s->vz, &s->vnx, &s->vny, x, nx, y, ny, z);
}

double lt_trim_ve_factor_at(const lt_trim_state *s, double rpm, double load)
{
    size_t ix, iy;
    double v;
    if (s->vz == NULL) { return 1.0; }
    ix = lt_tbl_nearest(s->vx, s->vnx, rpm);
    iy = lt_tbl_nearest(s->vy, s->vny, load);
    v = s->vz[iy * s->vnx + ix];
    return v > 0.1 ? v : 1.0;
}

void lt_trim_start(lt_trim_state *s) { s->running = LT_TRUE; }
void lt_trim_stop(lt_trim_state *s) { s->running = LT_FALSE; }
lt_bool lt_trim_is_running(const lt_trim_state *s) { return s->running; }

void lt_trim_reset_cyl(lt_trim_state *s, size_t cyl)
{
    if (cyl >= s->ncyl) { return; }
    lt_at_start(s->cyl[cyl].at);
    s->cyl[cyl].rail = 0;
}

void lt_trim_reset(lt_trim_state *s)
{
    size_t i;
    for (i = 0; i < s->ncyl; i++) { lt_trim_reset_cyl(s, i); }
}

lt_at_state *lt_trim_engine(lt_trim_state *s, size_t cyl)
{
    return cyl < s->ncyl ? s->cyl[cyl].at : NULL;
}

unsigned long lt_trim_samples(const lt_trim_state *s, size_t cyl)
{
    return cyl < s->ncyl ? lt_at_total_samples(s->cyl[cyl].at) : 0UL;
}

size_t lt_trim_rejections(const lt_trim_state *s, size_t cyl, lt_at_reject *out, size_t cap)
{
    size_t n, i, j;
    if (cyl >= s->ncyl || cap == 0) { return 0; }
    n = lt_at_rejections(s->cyl[cyl].at, out, cap);
    if (n > cap) { n = cap; }
    if (s->cyl[cyl].rail > 0) {
        for (i = 0; i < n; i++) {
            if (strcmp(out[i].reason, "afr at sensor rail") == 0) { out[i].count += s->cyl[cyl].rail; break; }
        }
        if (i == n) {
            if (n == cap) { n--; }
            out[n].reason = "afr at sensor rail";
            out[n].count = s->cyl[cyl].rail;
            n++;
        }
        for (i = 1; i < n; i++) {
            lt_at_reject t = out[i];
            j = i;
            while (j > 0 && out[j - 1].count < t.count) { out[j] = out[j - 1]; j--; }
            out[j] = t;
        }
    }
    return n;
}

void lt_trim_add(lt_trim_state *s, size_t cyl, const lt_ve_point *pt, double target,
                 double ve_factor, double ref_afr, const lt_trim_config *c)
{
    lt_ve_point q;
    lt_at_settings set;
    double factor;
    if (!s->running || cyl >= s->ncyl || !s->cyl[cyl].loaded) { return; }
    if (!(pt->afr >= LT_AT_AFR_RAIL_LOW && pt->afr <= LT_AT_AFR_RAIL_HIGH)) {
        s->cyl[cyl].rail++;
        q = *pt;
        lt_at_push_sample(s->cyl[cyl].at, &q);   /* keeps the delay buffer continuous */
        return;
    }
    if (ref_afr > 0.1) {
        factor = pt->afr / lt_at_normalise_to_afr(ref_afr);
    } else {
        target = target > 0.1 ? lt_at_normalise_to_afr(target)
                              : lt_trim_target_at(s, pt->rpm, pt->load, c->set.target_afr);
        if (ve_factor <= 0.0) { ve_factor = 1.0; }
        factor = target > 0.1 ? pt->afr / target / ve_factor : 1.0;
    }
    q = *pt;
    q.afr = LT_AT_STOICH_AFR * factor;
    set = c->set;
    set.target_afr = LT_AT_STOICH_AFR;
    lt_at_add_data_point(s->cyl[cyl].at, &q, s->x, s->nx, s->y, s->ny, &set, &c->f, &c->auth);
}

size_t lt_trim_recommendations(const lt_trim_state *s, size_t cyl, const lt_trim_config *c,
                               lt_at_rec *out, size_t cap)
{
    size_t n, i, k, m, cells;
    lt_at_rec *mine, *other;
    if (cyl >= s->ncyl || out == NULL || cap == 0) { return 0; }
    cells = s->nx * s->ny + 1;
    mine = (lt_at_rec *)lt_calloc(cells, sizeof(lt_at_rec));
    n = lt_at_recommendations(s->cyl[cyl].at, mine, cells);
    if (n > cells) { n = cells; }
    if (c != NULL && c->balance && s->ncyl > 1) {
        double *sb = (double *)lt_calloc(cells, sizeof(double)), *sr = (double *)lt_calloc(cells, sizeof(double));
        unsigned *cnt = (unsigned *)lt_calloc(cells, sizeof(unsigned));
        other = (lt_at_rec *)lt_calloc(cells, sizeof(lt_at_rec));
        for (k = 0; k < s->ncyl; k++) {
            if (!s->cyl[k].loaded) { continue; }
            m = lt_at_recommendations(s->cyl[k].at, other, cells);
            if (m > cells) { m = cells; }
            for (i = 0; i < m; i++) {
                size_t idx = (size_t)other[i].cell_y * s->nx + other[i].cell_x;
                if (idx >= cells) { continue; }
                sb[idx] += other[i].beginning_value;
                sr[idx] += other[i].recommended_value;
                cnt[idx]++;
            }
        }
        for (i = 0; i < n; i++) {
            size_t idx = (size_t)mine[i].cell_y * s->nx + mine[i].cell_x;
            if (idx < cells && cnt[idx] >= 2 && sr[idx] > 0.0) {
                mine[i].recommended_value *= sb[idx] / sr[idx];
            }
        }
        lt_free(sb); lt_free(sr); lt_free(cnt); lt_free(other);
    }
    for (i = 0; i < n && i < cap; i++) {
        out[i] = mine[i];
        out[i].beginning_value = lt_trim_from_mult(&s->u, mine[i].beginning_value / 100.0);
        out[i].recommended_value = lt_trim_from_mult(&s->u, mine[i].recommended_value / 100.0);
    }
    lt_free(mine);
    return n < cap ? n : cap;
}

/* ================================================================ replay */

static int col(const lt_log *log, const char *name)
{
    return (name != NULL && name[0] != '\0') ? lt_log_channel_index(log, name) : -1;
}

static double at(const lt_log *log, long r, int c, double dflt)
{
    double v;
    if (c < 0) { return dflt; }
    v = LT_LOG_AT(log, r, c);
    return lt_isfinite(v) ? v : dflt;
}

long lt_trim_replay(lt_trim_state *s, size_t cyl, const lt_log *log, const lt_at_channels *ch,
                    const char *ref_channel, const lt_trim_config *c)
{
    int crpm, cload, cafr, cve, cclt, ctps, ctr, cfc, cae, cref;
    long i;
    lt_bool was;
    unsigned long before;
    if (log == NULL || cyl >= s->ncyl || !s->cyl[cyl].loaded) { lt_err_set("no trim table for this cylinder"); return LT_EINVAL; }
    crpm = col(log, ch->rpm); cload = col(log, ch->load); cafr = col(log, ch->afr);
    if (crpm < 0 || cload < 0 || cafr < 0) {
        lt_err_set("the log has no rpm, load or lambda/AFR channel");
        return LT_ENOENT;
    }
    cve = col(log, ch->ve); cclt = col(log, ch->clt); ctps = col(log, ch->tps);
    ctr = col(log, ch->tps_rate); cfc = col(log, ch->fuel_cut); cae = col(log, ch->accel_enrich);
    cref = col(log, ref_channel);
    lt_trim_reset_cyl(s, cyl);
    was = s->running;
    s->running = LT_TRUE;
    before = lt_trim_samples(s, cyl);
    for (i = 0; i < log->n; i++) {
        lt_ve_point p;
        double ms = log->t[i] * 1000.0, ref = 0.0, a;
        lt_ve_point_default(&p);
        p.timestamp_ms = ms > 0.0 ? (unsigned long)(ms + 0.5) : 0UL;
        p.rpm = at(log, i, crpm, 0.0);
        p.load = at(log, i, cload, 0.0);
        p.map = p.load;
        a = at(log, i, cafr, 0.0);
        p.afr = ch->afr_is_lambda ? lt_at_normalise_to_afr(a) : a;
        p.ve = at(log, i, cve, 0.0);
        p.clt = at(log, i, cclt, 90.0);
        p.tps = at(log, i, ctps, 0.0);
        p.tps_rate = at(log, i, ctr, 0.0);
        if (cfc >= 0) { p.fuel_cut_active = at(log, i, cfc, 0.0) > 0.5 ? 1 : 0; }
        if (cae >= 0) { p.accel_enrich_active = at(log, i, cae, 0.0) > 0.5 ? 1 : 0; }
        if (cref >= 0) { ref = lt_at_normalise_to_afr(at(log, i, cref, 0.0)); }
        lt_trim_add(s, cyl, &p, 0.0, lt_trim_ve_factor_at(s, p.rpm, p.load), ref, c);
    }
    s->running = was;
    return (long)(lt_trim_samples(s, cyl) - before);
}

/* ================================================================ INI discovery */

static lt_bool has_ci(const char *s, const char *w)
{
    size_t i, n, m;
    if (s == NULL) { return LT_FALSE; }
    n = strlen(s);
    m = strlen(w);
    for (i = 0; i + m <= n; i++) {
        if (lt_strnicmp(s + i, w, m) == 0) { return LT_TRUE; }
    }
    return LT_FALSE;
}

static int last_number(const char *s)
{
    const char *p, *end = NULL;
    int v = 0;
    if (s == NULL) { return -1; }
    for (p = s; *p != '\0'; p++) {
        if (isdigit((unsigned char)*p)) { end = p; }
    }
    if (end == NULL) { return -1; }
    p = end;
    while (p > s && isdigit((unsigned char)p[-1])) { p--; }
    for (; p <= end && v < 1000; p++) { v = v * 10 + (*p - '0'); }
    return v;
}

static lt_bool is_fuel_trim(const lt_ini_table *t)
{
    static const char *const bad[] = { "ign", "spark", "timing", "long term", "short term", "ltft",
                                       "stft", "knock", "bank", "etb", "throttle", "boost", "idle", NULL };
    size_t i;
    if (!(has_ci(t->name, "trim") || has_ci(t->title, "trim")) || t->y_const == NULL) { return LT_FALSE; }
    if (!(has_ci(t->name, "fuel") || has_ci(t->title, "fuel") || has_ci(t->name, "inj") ||
          has_ci(t->title, "inj"))) {
        return LT_FALSE;
    }
    for (i = 0; bad[i] != NULL; i++) {
        if (has_ci(t->name, bad[i]) || has_ci(t->title, bad[i])) { return LT_FALSE; }
    }
    return LT_TRUE;
}

size_t lt_trim_find_tables(const lt_ini *ini, lt_trim_table_ref *out, size_t cap)
{
    size_t i, j, n = 0;
    if (ini == NULL) { return 0; }
    for (i = 0; i < ini->n_tables && n < cap; i++) {
        const lt_ini_table *t = ini->tables[i];
        int cyl;
        lt_bool dup = LT_FALSE;
        if (!is_fuel_trim(t)) { continue; }
        cyl = last_number(t->name);
        if (cyl <= 0) { cyl = last_number(t->title); }
        if (cyl <= 0 || cyl > LT_TRIM_MAX_CYL) { continue; }
        for (j = 0; j < n; j++) { if (out[j].cylinder == cyl) { dup = LT_TRUE; } }
        if (dup) { continue; }
        out[n].table = t->name;
        out[n].cylinder = cyl;
        n++;
    }
    for (i = 1; i < n; i++) {
        lt_trim_table_ref r = out[i];
        j = i;
        while (j > 0 && out[j - 1].cylinder > r.cylinder) { out[j] = out[j - 1]; j--; }
        out[j] = r;
    }
    return n;
}

size_t lt_trim_lambda_channels(const lt_ini *ini, const char **out, size_t cap)
{
    static const char *const bad[] = { "target", "trim", "blend", "error", "raw", "time", "axis",
                                       "correction", "fault", "valid", "heater", "temp", "gauge", NULL };
    size_t i, k, n = 0;
    if (ini == NULL) { return 0; }
    for (i = 0; i < ini->n_channels && n < cap; i++) {
        const char *nm = ini->channels[i]->name;
        lt_bool ok;
        if (nm == NULL || !(has_ci(nm, "lambda") || has_ci(nm, "afr") || has_ci(nm, "ego"))) { continue; }
        ok = LT_TRUE;
        for (k = 0; bad[k] != NULL; k++) { if (has_ci(nm, bad[k])) { ok = LT_FALSE; break; } }
        if (has_ci(nm, "ego") && !has_ci(nm, "lambda") && !has_ci(nm, "afr")) { ok = LT_FALSE; }
        if (ok) { out[n++] = nm; }
    }
    return n;
}
