/* at_glue.c - AutoTune glue to the INI, realtime and table-view modules
 * (spec 07 sections 4 and 5).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include "autotune.h"

static lt_bool has_channel(const lt_ini *ini, const char *name)
{
    return (ini && name && lt_map_get(&ini->channel_idx, name) != NULL) ? LT_TRUE : LT_FALSE;
}

/* first candidate present in the INI; dst empty if none */
static void pick(const lt_ini *ini, char *dst, size_t cap, const char *const *cands)
{
    size_t i;
    dst[0] = '\0';
    for (i = 0; cands[i]; i++) {
        if (has_channel(ini, cands[i])) {
            lt_strlcpy(dst, cands[i], cap);
            return;
        }
    }
}

static lt_bool name_has_lambda(const char *n)
{
    size_t i, len = strlen(n);
    for (i = 0; i + 6 <= len; i++) {
        if (lt_strnicmp(n + i, "lambda", 6) == 0) {
            return LT_TRUE;
        }
    }
    return LT_FALSE;
}

void lt_at_channels_from_ini(const lt_ini *ini, lt_at_channels *out)
{
    static const char *const c_rpm[] = {"RPMValue", "rpm", "RPM", "rpmValue", NULL};
    static const char *const c_load[] = {"fuelload", "fuelLoad", "MAPValue", "map", "MAP",
                                         "load", NULL};
    static const char *const c_afr[] = {"afr", "AFR", "afr1", "AFR1", "lambda", "Lambda",
                                        "lambda1", "afrValue", NULL};
    static const char *const c_ve[] = {"veCurr", "veCurr1", "VE", "VE1", "ve", NULL};
    static const char *const c_clt[] = {"coolant", "CLT", "clt", "coolantTemp", NULL};
    static const char *const c_tps[] = {"TPSValue", "tps", "TPS", NULL};
    static const char *const c_tpsr[] = {"tpsDOT", "TPSdot", "tpsdot", "TPSDOT", "deltaTps",
                                         NULL};
    static const char *const c_map[] = {"MAPValue", "map", "MAP", NULL};
    static const char *const c_maf[] = {"MAFValue", "maf", "MAF", NULL};
    static const char *const c_fc[] = {"fuelCut", "dfcoActive", "dfco", "DFCO", NULL};
    static const char *const c_ae[] = {"tpsaccaen", "accelEnrich", "AccelEnrich", NULL};
    const char *lam = NULL;
    memset(out, 0, sizeof *out);
    pick(ini, out->rpm, sizeof out->rpm, c_rpm);
    pick(ini, out->load, sizeof out->load, c_load);
    pick(ini, out->ve, sizeof out->ve, c_ve);
    pick(ini, out->clt, sizeof out->clt, c_clt);
    pick(ini, out->tps, sizeof out->tps, c_tps);
    pick(ini, out->tps_rate, sizeof out->tps_rate, c_tpsr);
    pick(ini, out->map, sizeof out->map, c_map);
    pick(ini, out->maf, sizeof out->maf, c_maf);
    pick(ini, out->fuel_cut, sizeof out->fuel_cut, c_fc);
    pick(ini, out->accel_enrich, sizeof out->accel_enrich, c_ae);
    if (ini && ini->ve_analyze.lambda_channel && has_channel(ini, ini->ve_analyze.lambda_channel)) {
        lam = ini->ve_analyze.lambda_channel;
        lt_strlcpy(out->afr, lam, sizeof out->afr);
        out->afr_is_lambda = LT_TRUE;
    } else {
        pick(ini, out->afr, sizeof out->afr, c_afr);
        out->afr_is_lambda = name_has_lambda(out->afr);
    }
}

static lt_bool rt_get(const lt_ini_rt *rt, const char *name, double *v)
{
    return (name && name[0] && lt_ini_rt_get(rt, name, v)) ? LT_TRUE : LT_FALSE;
}

lt_bool lt_at_sample_from_rt(const lt_ini_rt *rt, const lt_at_channels *ch, unsigned long now_ms,
                             lt_ve_point *out)
{
    double v;
    lt_ve_point p;
    lt_ve_point_default(&p);
    if (!rt_get(rt, ch->rpm, &p.rpm) || !rt_get(rt, ch->afr, &p.afr) ||
        !rt_get(rt, ch->load, &p.load)) {
        return LT_FALSE;
    }
    if (ch->afr_is_lambda) {
        p.afr = lt_at_normalise_to_afr(p.afr);
    }
    p.map = p.load;
    p.clt = 90.0;
    if (rt_get(rt, ch->ve, &v)) p.ve = v;
    if (rt_get(rt, ch->clt, &v)) p.clt = v;
    if (rt_get(rt, ch->tps, &v)) p.tps = v;
    if (rt_get(rt, ch->tps_rate, &v)) p.tps_rate = v;
    if (rt_get(rt, ch->map, &v)) p.map = v;
    if (rt_get(rt, ch->maf, &v)) p.maf = v;
    if (rt_get(rt, ch->fuel_cut, &v)) p.fuel_cut_active = v > 0.5 ? 1 : 0;
    if (rt_get(rt, ch->accel_enrich, &v)) p.accel_enrich_active = v > 0.5 ? 1 : 0;
    p.timestamp_ms = now_ms;
    *out = p;
    return LT_TRUE;
}

void lt_at_reftables_from_views(const lt_tblview *ve, const lt_tblview *target,
                                lt_at_reftables *out)
{
    memset(out, 0, sizeof *out);
    if (!ve) {
        return;
    }
    out->nx = ve->cols;
    out->ny = ve->rows;
    out->ve = ve->z;
    if (target && target->rows == ve->rows && target->cols == ve->cols) {
        out->target_afr = target->z;
    }
}

/* ---------------------------------------------------------------- fuel guard */

static char refusal_buf[640];

static const char *what_for_role(lt_table_role r)
{
    switch (r) {
    case LT_ROLE_IGNITION:
        return "an ignition table";
    case LT_ROLE_AFR_TARGET:
        return "the AFR target table";
    case LT_ROLE_WARMUP:
        return "a warm-up enrichment table";
    default:
        return "not a fuel table";
    }
}

static const char *refusal_for(const lt_ini *ini, const lt_ini_table *t, const char *name)
{
    const char *what, *label;
    size_t i;
    if (!t) {
        lt_snprintf(refusal_buf, sizeof refusal_buf, "%s is not a table in this definition.",
                    name ? name : "(null)");
        return refusal_buf;
    }
    switch (t->role) {
    case LT_ROLE_VE:
        return NULL;
    case LT_ROLE_OTHER:
        for (i = 0; i < ini->n_tables; i++) {
            if (ini->tables[i]->role == LT_ROLE_VE) {
                break;
            }
        }
        if (i >= ini->n_tables) {
            return NULL;   /* INI labels nothing: allow */
        }
        what = "not a fuel table";
        break;
    default:
        what = what_for_role(t->role);
        break;
    }
    label = (t->title && t->title[0]) ? t->title : (t->name ? t->name : name);
    lt_snprintf(refusal_buf, sizeof refusal_buf,
                "%s is %s. AutoTune scales a table by measured/target AFR, which is only "
                "meaningful for a fuel table - on this one it would corrupt the values.",
                label, what);
    return refusal_buf;
}

const char *lt_fuel_tune_refusal(const lt_ini *ini, const char *table_name)
{
    const lt_ini_table *t = (ini && table_name) ? (const lt_ini_table *)lt_map_get(&ini->table_idx,
                                                                                    table_name)
                                                : NULL;
    return refusal_for(ini, t, table_name);
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

size_t lt_fuel_tunable_tables(const lt_ini *ini, const char **names, size_t cap)
{
    size_t i, n = 0, m;
    const char **all;
    if (!ini || ini->n_tables == 0) {
        return 0;
    }
    all = (const char **)lt_malloc(ini->n_tables * sizeof(char *));
    for (i = 0; i < ini->n_tables; i++) {
        const lt_ini_table *t = ini->tables[i];
        if (t->y_const && t->name && refusal_for(ini, t, t->name) == NULL) {
            all[n++] = t->name;
        }
    }
    qsort(all, n, sizeof(char *), cmp_names);
    if (names) {
        m = LT_MIN(n, cap);
        for (i = 0; i < m; i++) {
            names[i] = all[i];
        }
    }
    lt_free((void *)all);
    return n;
}

/* ---------------------------------------------------------------- proposals */

size_t lt_at_apply_to_view(const lt_at_state *s, lt_tblview *view)
{
    size_t n = lt_at_recommendations(s, NULL, 0), i, changed = 0;
    lt_at_rec *recs;
    lt_bool pushed = LT_FALSE;
    if (n == 0) {
        return 0;
    }
    recs = (lt_at_rec *)lt_malloc(n * sizeof *recs);
    lt_at_recommendations(s, recs, n);
    for (i = 0; i < n; i++) {
        size_t r = recs[i].cell_y, c = recs[i].cell_x;
        if (r >= view->rows || c >= view->cols) {
            continue;
        }
        if (fabs(recs[i].recommended_value - view->z[r * view->cols + c]) <= DBL_EPSILON) {
            continue;
        }
        if (!pushed) {
            lt_tblview_push_undo(view);
            pushed = LT_TRUE;
        }
        lt_tblview_set(view, r, c, recs[i].recommended_value);
        changed++;
    }
    lt_free(recs);
    return changed;
}

size_t lt_at_proposal_summary(const lt_at_rec *recs, size_t n, const double *z, size_t nx,
                              size_t ny, double *largest)
{
    size_t i, changed = 0;
    double big = 0.0, d;
    for (i = 0; i < n; i++) {
        if (recs[i].cell_x >= nx || recs[i].cell_y >= ny) {
            continue;
        }
        d = recs[i].recommended_value - z[(size_t)recs[i].cell_y * nx + recs[i].cell_x];
        if (fabs(d) > DBL_EPSILON) {
            changed++;
            if (fabs(d) > fabs(big)) {
                big = d;
            }
        }
    }
    if (largest) {
        *largest = big;
    }
    return changed;
}
