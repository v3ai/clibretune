/* basemap.c - base map generator (spec 08 section 4).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <string.h>
#include "basemap.h"

static double bm_round(double x)
{
    return x < 0.0 ? -floor(-x + 0.5) : floor(x + 0.5);
}

static double bm_clamp(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static double bm_max(double a, double b)
{
    return a > b ? a : b;
}

/* per-fuel property table, indexed by lt_fuel_type */
static const double fuel_stoich[5]  = { 14.7, 9.8, 9.0, 6.5, 15.7 };
static const double fuel_wot[5]     = { 12.5, 8.5, 7.8, 5.5, 13.5 };
static const double fuel_octane[5]  = { 93.0, 100.0, 105.0, 105.0, 105.0 };
static const double fuel_crank[5]   = { 1.0, 1.4, 1.6, 1.8, 0.8 };
static const double fuel_wue[5]     = { 1.0, 1.2, 1.3, 1.4, 0.9 };
static const double fuel_prime[5]   = { 3.0, 4.5, 5.0, 6.0, 2.0 };
static const double fuel_iac[5]     = { 70.0, 80.0, 80.0, 80.0, 70.0 };

static int fidx(lt_fuel_type f)
{
    int i = (int)f;
    return (i < 0 || i > 4) ? 0 : i;
}

void lt_engine_spec_default(lt_engine_spec *s)
{
    memset(s, 0, sizeof(*s));
    s->cylinder_count = 4;
    s->displacement_cc = 2000.0;
    s->injector_size_cc = 440.0;
    s->fuel = LT_FUEL_GASOLINE;
    s->aspiration = LT_ASP_NA;
    s->engine_stroke = LT_STROKE_FOUR;
    s->inj_mode = LT_INJ_SEQUENTIAL;
    s->ign_mode = LT_IGN_WASTED_SPARK;
    s->idle_rpm = 800;
    s->redline_rpm = 6500;
    s->has_boost = LT_FALSE;
    s->boost_target_kpa = 0.0;
    s->has_wot_afr = LT_FALSE;
    s->target_wot_afr = 0.0;
    s->has_octane = LT_FALSE;
    s->octane = 0.0;
    s->has_cr = LT_FALSE;
    s->compression_ratio = 0.0;
    s->has_chamber = LT_FALSE;
    s->chamber = LT_CH_QUENCH_TWO_VALVE;
}

double lt_fuel_stoich_afr(lt_fuel_type f)
{
    return fuel_stoich[fidx(f)];
}

double lt_engine_req_fuel(const lt_engine_spec *s)
{
    double cyl = (double)(s->cylinder_count > 0 ? s->cylinder_count : 1);
    double dpc = s->displacement_cc / cyl;
    double divider, req;

    switch (s->inj_mode) {
    case LT_INJ_BATCH:  divider = bm_max(cyl / 2.0, 1.0); break;
    case LT_INJ_SEQUENTIAL: divider = cyl; break;
    default: divider = 1.0; break;
    }
    req = (dpc * lt_fuel_stoich_afr(s->fuel) * 10.0) / (s->injector_size_cc * divider);
    if (!(req == req)) {
        return 0.1;   /* NaN (0cc / 0cc injector) */
    }
    return bm_clamp(req, 0.1, 25.5);
}

double lt_engine_safe_wot_afr(const lt_engine_spec *s)
{
    return s->has_wot_afr ? s->target_wot_afr : fuel_wot[fidx(s->fuel)];
}

double lt_engine_max_load_kpa(const lt_engine_spec *s)
{
    if (s->aspiration == LT_ASP_NA) {
        return 105.0;
    }
    return bm_max(s->has_boost ? s->boost_target_kpa : 200.0, 120.0);
}

double lt_engine_effective_octane(const lt_engine_spec *s)
{
    return s->has_octane ? s->octane : fuel_octane[fidx(s->fuel)];
}

double lt_engine_max_spark_advance(const lt_engine_spec *s)
{
    double adv = 34.0, cr;

    adv += bm_clamp((lt_engine_effective_octane(s) - 93.0) * 0.6, -8.0, 8.0);
    cr = s->has_cr ? s->compression_ratio : 10.0;
    adv += bm_clamp((10.0 - cr) * 1.5, -8.0, 6.0);
    if (s->has_chamber) {
        if (s->chamber == LT_CH_OPEN) {
            adv += 2.0;
        } else if (s->chamber == LT_CH_SWIRL_MULTI_VALVE) {
            adv -= 2.0;
        }
    }
    if (s->engine_stroke == LT_STROKE_TWO) {
        adv -= 6.0;
    }
    return bm_clamp(adv, 18.0, 42.0);
}

/* remove adjacent duplicates, pad with last+step, truncate to count */
static void finish_bins(double *tmp, size_t n, size_t count, double step, double *out)
{
    size_t w = 0, i;

    for (i = 0; i < n; i++) {
        if (w == 0 || tmp[i] != tmp[w - 1]) {
            tmp[w++] = tmp[i];
        }
    }
    while (w < count) {
        tmp[w] = tmp[w - 1] + step;
        w++;
    }
    for (i = 0; i < count; i++) {
        out[i] = tmp[i];
    }
}

void lt_basemap_rpm_bins(unsigned idle, unsigned redline, size_t count, double *out)
{
    double *tmp;
    double di = (double)idle, dr = (double)redline;
    size_t i;

    if (count == 0) {
        return;
    }
    if (count == 1) {
        out[0] = di;
        return;
    }
    tmp = (double *)lt_malloc((count + 1) * sizeof(double) * 2);
    for (i = 0; i < count; i++) {
        double t = (double)i / (double)(count - 1);
        double rpm = di + (dr - di) * pow(t, 1.3);
        double r = bm_round(rpm / 100.0) * 100.0;
        tmp[i] = bm_max(r, di);
    }
    tmp[0] = bm_round(di / 100.0) * 100.0;
    tmp[count - 1] = bm_round(dr / 100.0) * 100.0;
    finish_bins(tmp, count, count, 100.0, out);
    lt_free(tmp);
}

void lt_basemap_load_bins(double max_load, size_t count, double *out)
{
    double *tmp;
    size_t i;

    if (count == 0) {
        return;
    }
    if (count == 1) {
        out[0] = max_load;
        return;
    }
    tmp = (double *)lt_malloc((count + 1) * sizeof(double) * 2);
    for (i = 0; i < count; i++) {
        double t = (double)i / (double)(count - 1);
        tmp[i] = bm_round(15.0 + (max_load - 15.0) * t);
    }
    tmp[0] = 15.0;
    tmp[count - 1] = bm_round(max_load);
    finish_bins(tmp, count, count, 5.0, out);
    lt_free(tmp);
}

static void add_scalar(lt_basemap *m, const char *name, double v)
{
    lt_basemap_scalar *s;

    m->scalars = (lt_basemap_scalar *)lt_realloc(m->scalars,
                                                 (m->n_scalars + 1) * sizeof(*m->scalars));
    s = &m->scalars[m->n_scalars++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->value = v;
}

void lt_basemap_free(lt_basemap *m)
{
    if (m && m->scalars) {
        lt_free(m->scalars);
    }
    if (m) {
        m->scalars = NULL;
        m->n_scalars = 0;
    }
}

void lt_basemap_generate(const lt_engine_spec *spec, lt_basemap *out)
{
    static const double temps[8] = { -40, -20, 0, 20, 40, 60, 80, 100 };
    static const double crank[8] = { 400, 350, 280, 200, 150, 120, 100, 90 };
    static const double wue[6] = { 180, 165, 150, 135, 120, 110 };
    int fi = fidx(spec->fuel);
    double idle = (double)spec->idle_rpm, red = (double)spec->redline_rpm;
    double max_load = lt_engine_max_load_kpa(spec);
    double stoich = lt_fuel_stoich_afr(spec->fuel), wot = lt_engine_safe_wot_afr(spec);
    double peak = lt_engine_max_spark_advance(spec);
    double wot_pull = bm_clamp(peak - 12.0, 6.0, 16.0);
    double retard = bm_clamp(20.0 - (lt_engine_effective_octane(spec) - 90.0) * 0.4, 8.0, 22.0);
    int boosted = spec->aspiration != LT_ASP_NA;
    double inj = spec->injector_size_cc;
    int r, c, i;
    double inj_type, spark;

    memset(out, 0, sizeof(*out));
    lt_basemap_rpm_bins(spec->idle_rpm, spec->redline_rpm, LT_BASEMAP_N, out->rpm_bins);
    lt_basemap_load_bins(max_load, LT_BASEMAP_N, out->load_bins);

    for (r = 0; r < LT_BASEMAP_N; r++) {
        double load = out->load_bins[r];
        double ln = (max_load > 15.0) ? bm_clamp((load - 15.0) / (max_load - 15.0), 0.0, 1.0) : 0.0;
        for (c = 0; c < LT_BASEMAP_N; c++) {
            double rpm = out->rpm_bins[c];
            double rn = (red > idle) ? bm_clamp((rpm - idle) / (red - idle), 0.0, 1.0) : 0.0;
            double ve, rf, adv, afr, ramp;

            if (!boosted) {
                rf = 1.0 - (rn - 0.6) * (rn - 0.6) * 0.3;
                ve = 30.0 + 55.0 * ln * rf;
            } else {
                double bf = load > 101.0 ? 1.0 + bm_max(ln - 0.5, 0.0) * 0.3 : 1.0;
                rf = 1.0 - (rn - 0.55) * (rn - 0.55) * 0.25;
                ve = 28.0 + 62.0 * ln * rf * bf;
            }
            out->ve[r][c] = bm_round(bm_clamp(ve, 15.0, 120.0));

            ramp = rn / 0.55;
            if (ramp > 1.0) {
                ramp = 1.0;
            }
            adv = 14.0 + (peak - 14.0) * ramp - wot_pull * ln;
            if (boosted && load > 101.0 && max_load > 101.0) {
                adv -= (load - 101.0) / (max_load - 101.0) * retard;
            }
            if (rpm < idle * 1.2 && load < 50.0) {
                adv = bm_clamp(adv, 10.0, 20.0);
            }
            out->ign[r][c] = bm_round(bm_clamp(adv, 0.0, 45.0));

            if (ln > 0.85) {
                afr = wot;
            } else if (ln < 0.35 && rn < 0.3) {
                afr = stoich * 0.97;
            } else if (ln < 0.5) {
                afr = stoich * 1.02;
            } else {
                double blend = bm_clamp((ln - 0.5) / 0.35, 0.0, 1.0);
                afr = stoich + (wot - stoich) * blend;
            }
            out->afr[r][c] = bm_round(afr * 10.0) / 10.0;
        }
    }

    for (i = 0; i < 8; i++) {
        out->cranking[i].coolant_c = temps[i];
        out->cranking[i].value = crank[i] * fuel_crank[fi];
        out->warmup[i].coolant_c = temps[i];
        out->warmup[i].value = (i < 6) ? wue[i] * fuel_wue[fi] : 100.0;
    }
    out->prime_pulse_ms = bm_clamp(fuel_prime[fi] * (440.0 / bm_max(inj, 100.0)), 0.5, 15.0);
    out->req_fuel = lt_engine_req_fuel(spec);
    out->accel_tps_threshold = 10.0;
    out->accel_enrichment_pct = 30.0;
    out->accel_duration_cycles = 3;
    out->accel_taper_pct = 50.0;
    out->iac_cold_start_pct = fuel_iac[fi];
    out->iac_warm_idle_pct = 20.0;
    out->iac_warm_threshold_c = 75.0;

    switch (spec->inj_mode) {
    case LT_INJ_SIMULTANEOUS: inj_type = 0.0; break;
    case LT_INJ_SEQUENTIAL: inj_type = 1.0; break;
    case LT_INJ_BATCH: inj_type = 2.0; break;
    default: inj_type = 3.0; break;
    }
    switch (spec->ign_mode) {
    case LT_IGN_WASTED_SPARK: spark = 0.0; break;
    case LT_IGN_COIL_ON_PLUG: spark = 1.0; break;
    default: spark = 2.0; break;
    }
    add_scalar(out, "reqFuel", out->req_fuel);
    add_scalar(out, "injType", inj_type);
    add_scalar(out, "sparkMode", spark);
    add_scalar(out, "nCylinders", (double)spec->cylinder_count);
    add_scalar(out, "stoich", stoich);
}
