/* calib.c - sensor calibration curves: solutions, AFR correction, linear
 * wideband, Steinhart-Hart thermistor fit. Spec: docs/spec/09-project.md sec. 6.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>
#include "proj_priv.h"

double lt_calib_adc_to_volts(int adc)
{
    return (double)adc * 5.0 / 1023.0;
}

/* ---------------------------------------------------------------- */
/* solution evaluation                                               */
/* ---------------------------------------------------------------- */
typedef struct calib_ctx {
    double adc;
    const lt_inc_table *tbl;
} calib_ctx;

static lt_bool ctx_get_var(void *ud, const char *name, double *out)
{
    calib_ctx *c = (calib_ctx *)ud;
    if (strcmp(name, "adcValue") == 0) {
        *out = c->adc;
        return LT_TRUE;
    }
    return LT_FALSE;
}

static lt_bool ctx_table(void *ud, const char *file, double x, double *out)
{
    calib_ctx *c = (calib_ctx *)ud;
    LT_UNUSED(file);
    if (c->tbl == NULL) return LT_FALSE;
    return lt_inc_lookup(c->tbl, x, out);
}

int lt_calib_evaluate_solution(const char *expr, lt_inc_cache *cache, double out[1024])
{
    calib_ctx ctx;
    lt_expr_env env;
    lt_expr *e;
    char err[160], *file = NULL;
    const char *t;
    int adc;

    ctx.adc = 0;
    ctx.tbl = NULL;
    t = strstr(expr, "table(");
    if (t != NULL) {
        const char *q1 = strchr(t, '"');
        const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (q1 != NULL && q2 != NULL) {
            file = lt_strndup(q1 + 1, (size_t)(q2 - q1 - 1));
            if (cache != NULL) ctx.tbl = lt_inc_cache_get(cache, file);
            if (ctx.tbl == NULL) {
                lt_err_set("preset needs the lookup file '%s', which is not available", file);
                lt_free(file);
                return LT_ENOENT;
            }
            lt_free(file);
        }
    }
    e = lt_expr_compile(expr, err, sizeof err);
    if (e == NULL) {
        lt_err_set("calibration formula parse failed: %s (in `%s`)", err, expr);
        return LT_EDATA;
    }
    lt_expr_env_init(&env);
    env.ud = &ctx;
    env.get_var = ctx_get_var;
    env.table_lookup = ctx_table;
    for (adc = 0; adc < LT_CALIB_ADC_COUNTS; adc++) {
        double v = 0;
        ctx.adc = (double)adc;
        if (lt_expr_eval_f64(e, &env, &v) != 0) {
            lt_err_set("calibration formula failed: %s (at adcValue=%d)", lt_err_get(), adc);
            lt_expr_free(e);
            return LT_EDATA;
        }
        out[adc] = v;
    }
    lt_expr_free(e);
    return LT_OK;
}

int lt_calib_solution_curve(const lt_ini_ref_table *t, const char *label, lt_inc_cache *cache,
                            double out[1024])
{
    size_t i;
    for (i = 0; i < t->n_solutions; i++) {
        const lt_ini_solution *s = &t->solutions[i];
        if (s->label != NULL && lt_stricmp(s->label, label) == 0) {
            if (s->expression == NULL) {
                lt_err_set("this preset uses the '%s' generator and has no formula to evaluate",
                           s->generator ? s->generator : "");
                return LT_EUNSUPPORTED;
            }
            return lt_calib_evaluate_solution(s->expression, cache, out);
        }
    }
    lt_err_set("no solution named '%s'", label);
    return LT_ENOENT;
}

/* ---------------------------------------------------------------- */
/* AFR correction                                                    */
/* ---------------------------------------------------------------- */
int lt_afr_corr_from_points(lt_afr_corr *c, const double *pts, int n)
{
    memset(c, 0, sizeof *c);
    c->gain = 1.0;
    if (n == 1) {
        c->npoints = 1;
        c->pts[0][0] = pts[0];
        c->pts[0][1] = pts[1];
        c->offset = pts[1] - pts[0];
        return LT_OK;
    }
    if (n == 2) {
        double m1 = pts[0], e1 = pts[1], m2 = pts[2], e2 = pts[3], g;
        if (fabs(m1 - m2) < 1e-6) {
            lt_err_set("the two measured readings must differ (both were %.2f AFR) - a gain "
                       "cannot be solved from one point", m1);
            return LT_EDATA;
        }
        g = (e2 - e1) / (m2 - m1);
        if (g <= 0) {
            lt_err_set("these points give a gain of %.3f, which would reverse the curve - check "
                       "the expected values are paired with the right measured readings", g);
            return LT_EDATA;
        }
        c->npoints = 2;
        c->gain = g;
        c->offset = e1 - g * m1;
        c->pts[0][0] = m1;
        c->pts[0][1] = e1;
        c->pts[1][0] = m2;
        c->pts[1][1] = e2;
        return LT_OK;
    }
    lt_err_set("a correction takes one or two points, got %d", n);
    return LT_EDATA;
}

void lt_afr_corr_apply(const lt_afr_corr *c, double *curve, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) curve[i] = c->gain * curve[i] + c->offset;
}

void lt_afr_corr_describe(const lt_afr_corr *c, char *buf, size_t cap)
{
    if (c->npoints == 1)
        lt_snprintf(buf, cap, "corrected %+.2f AFR against a reference (%.2f read as %.2f)",
                    c->offset, c->pts[0][1], c->pts[0][0]);
    else
        lt_snprintf(buf, cap, "corrected against a reference (gain %.4f, offset %+.2f)", c->gain,
                    c->offset);
}

/* ---------------------------------------------------------------- */
/* linear wideband                                                   */
/* ---------------------------------------------------------------- */
int lt_linear_wb_new(lt_linear_wb *w, double v1, double afr1, double v2, double afr2)
{
    memset(w, 0, sizeof *w);
    if (fabs(v1 - v2) < 1e-9) {
        lt_err_set("the two calibration points must be at different voltages (both were %.3f V)",
                   v1);
        return LT_EDATA;
    }
    w->v1 = v1;
    w->afr1 = afr1;
    w->v2 = v2;
    w->afr2 = afr2;
    w->slope = (afr2 - afr1) / (v2 - v1);
    w->intercept = afr1 - w->slope * v1;
    return LT_OK;
}

double lt_linear_wb_afr(const lt_linear_wb *w, double volts)
{
    return w->intercept + w->slope * volts;
}

void lt_linear_wb_curve(const lt_linear_wb *w, double out[1024])
{
    int adc;
    for (adc = 0; adc < LT_CALIB_ADC_COUNTS; adc++)
        out[adc] = lt_linear_wb_afr(w, lt_calib_adc_to_volts(adc));
}

void lt_linear_wb_describe(const lt_linear_wb *w, char *buf, size_t cap)
{
    lt_snprintf(buf, cap, "%.2f-%.2f AFR over 0-%.0f V (%.4f AFR/V)", lt_linear_wb_afr(w, 0.0),
                lt_linear_wb_afr(w, 5.0), 5.0, w->slope);
}

/* ---------------------------------------------------------------- */
/* thermistor                                                        */
/* ---------------------------------------------------------------- */
int lt_therm_fit_points(lt_therm_fit *f, double bias_resistor, const double temp_c[3],
                        const double ohms[3])
{
    double l[3], y[3], g2, g3, denom;
    int i;
    memset(f, 0, sizeof *f);
    for (i = 0; i < 3; i++) {
        if (!(ohms[i] > 0)) {
            lt_err_set("thermistor resistance must be positive (point %d was %g ohm)", i, ohms[i]);
            return LT_EDATA;
        }
        l[i] = log(ohms[i]);
        y[i] = 1.0 / (temp_c[i] + 273.15);
    }
    g2 = (y[1] - y[0]) / (l[1] - l[0]);
    g3 = (y[2] - y[0]) / (l[2] - l[0]);
    denom = (l[2] - l[1]) * (l[0] + l[1] + l[2]);
    if (!lt_isfinite(g2) || !lt_isfinite(g3) || !lt_isfinite(denom) || fabs(denom) < 1e-12) {
        lt_err_set("the three thermistor points are collinear in log-resistance and cannot be "
                   "fitted");
        return LT_EDATA;
    }
    f->c = (g3 - g2) / denom;
    f->b = g2 - f->c * (l[0] * l[0] + l[0] * l[1] + l[1] * l[1]);
    f->a = y[0] - (f->b + l[0] * l[0] * f->c) * l[0];
    f->bias_resistor = bias_resistor;
    return LT_OK;
}

int lt_therm_fit_option(lt_therm_fit *f, const lt_ini_therm_opt *o)
{
    return lt_therm_fit_points(f, o->bias_resistor, o->t, o->r);
}

double lt_therm_temp_at_resistance(const lt_therm_fit *f, double ohms)
{
    double ln = log(ohms > 1e-6 ? ohms : 1e-6);
    double inv = f->a + f->b * ln + f->c * ln * ln * ln;
    return 1.0 / inv - 273.15;
}

static double temp_at_adc_d(const lt_therm_fit *f, double adc)
{
    double ratio = (adc * 5.0 / 1023.0) / 5.0, r;
    if (ratio < 1e-6) ratio = 1e-6;
    if (ratio > 1.0 - 1e-6) ratio = 1.0 - 1e-6;
    r = f->bias_resistor * ratio / (1.0 - ratio);
    return lt_therm_temp_at_resistance(f, r);
}

double lt_therm_temp_at_adc(const lt_therm_fit *f, int adc)
{
    return temp_at_adc_d(f, (double)adc);
}

void lt_therm_sample_at_bins(const lt_therm_fit *f, const double adc_bins[32], double out[32])
{
    int i;
    for (i = 0; i < 32; i++) out[i] = temp_at_adc_d(f, adc_bins[i]);
}
