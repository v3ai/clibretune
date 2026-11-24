/* sim_engine.c - deterministic integer engine physics, VE model, och codec.
 * Spec: docs/spec/08-sim-basemap-tableops.md sections 6, 6.1, 6.2.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "sim.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define SIM_SEED 0x4F545531UL
#define SIM_STEP_MS 50UL

typedef struct sim_vectx {
    double *rpm, *load, *ve;
    size_t nx, ny;
} sim_vectx;

struct lt_simengine {
    lt_u32 rng;
    int mode;
    unsigned long time_ms, acc_ms, state_start_ms, steps;
    int secl;
    int target_rpm, rpm, rpm_accel, target_tps, tps, tps_reading;
    int coolant_dc, intake_dc, map_kpa, battery_dv, advance_deg;
    sim_vectx ve;
};

/* ---------------------------------------------------------------- RNG */
static lt_u32 rng_next(lt_simengine *e)
{
    lt_u32 x = e->rng & LT_U32_MASK;

    x ^= (x << 13) & LT_U32_MASK;
    x ^= x >> 17;
    x ^= (x << 5) & LT_U32_MASK;
    e->rng = x & LT_U32_MASK;
    return e->rng;
}

/* lo + next() % (hi - lo), hi exclusive */
static int rng_range(lt_simengine *e, int lo, int hi)
{
    lt_u32 r = rng_next(e);

    return lo + (int)(r % (lt_u32)(hi - lo));
}

static int iclamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

static int slew(int cur, int tgt, int rate)
{
    int delta = tgt - cur;
    int step = delta * rate / 100;

    if (step == 0 && delta != 0) {
        step = delta > 0 ? 1 : -1;
    }
    return cur + step;
}

static int map_value(int x, int imin, int imax, int omin, int omax)
{
    return (x - imin) * (omax - omin) / (imax - imin) + omin;
}

/* ------------------------------------------------------------ states */
static void transition(lt_simengine *e, lt_sim_mode m)
{
    int a, b;

    e->mode = (int)m;
    e->state_start_ms = e->time_ms;
    switch (m) {
    case LT_SIM_STARTUP:
        e->target_rpm = 900; e->target_tps = 7; e->rpm_accel = 500;
        break;
    case LT_SIM_WARMUP_IDLE:
        e->target_rpm = 850; e->target_tps = 5; e->rpm_accel = 100;
        break;
    case LT_SIM_IDLE:
        a = rng_range(e, -50, 50);
        e->target_rpm = 700 + a; e->target_tps = 2; e->rpm_accel = 50;
        break;
    case LT_SIM_LIGHT_LOAD:
        a = rng_range(e, -300, 300);
        b = rng_range(e, -5, 10);
        e->target_rpm = 2500 + a; e->target_tps = 20 + b; e->rpm_accel = 200;
        break;
    case LT_SIM_ACCELERATION:
        a = rng_range(e, -500, 500);
        b = rng_range(e, 10, 40);
        e->target_rpm = 5000 + a; e->target_tps = 50 + b; e->rpm_accel = 1000;
        break;
    case LT_SIM_HIGH_RPM:
        a = rng_range(e, 100, 500);
        b = rng_range(e, 0, 20);
        e->target_rpm = 6800 - a; e->target_tps = 100 - b; e->rpm_accel = 500;
        break;
    case LT_SIM_DECELERATION:
        a = rng_range(e, 0, 150);
        e->target_rpm = 900 + a; e->target_tps = 2; e->rpm_accel = -800;
        break;
    case LT_SIM_WOT:
    default:
        e->target_rpm = 6800; e->target_tps = 100; e->rpm_accel = 1500;
        break;
    }
}

static void state_machine(lt_simengine *e)
{
    unsigned long in_state = e->time_ms - e->state_start_ms;
    int r;

    switch ((lt_sim_mode)e->mode) {
    case LT_SIM_STARTUP:
        if (in_state > 1000UL && e->rpm > 350) {
            transition(e, LT_SIM_WARMUP_IDLE);
        }
        break;
    case LT_SIM_WARMUP_IDLE:
        if (e->coolant_dc > 600) {
            transition(e, LT_SIM_IDLE);
        }
        break;
    case LT_SIM_IDLE:
        if (in_state > 5000UL) {
            r = rng_range(e, 0, 100);
            if (r < 30) {
                transition(e, LT_SIM_LIGHT_LOAD);
            } else if (r < 35) {
                transition(e, LT_SIM_ACCELERATION);
            }
        }
        break;
    case LT_SIM_LIGHT_LOAD:
        if (in_state > 5000UL) {
            r = rng_range(e, 0, 100);
            if (r < 40) {
                transition(e, LT_SIM_ACCELERATION);
            } else if (r < 70) {
                transition(e, LT_SIM_DECELERATION);
            } else {
                transition(e, LT_SIM_IDLE);
            }
        }
        break;
    case LT_SIM_ACCELERATION:
        if (e->rpm > 5000) {
            transition(e, LT_SIM_HIGH_RPM);
        } else if (in_state > 3000UL) {
            r = rng_range(e, 0, 100);
            if (r < 30) {
                transition(e, LT_SIM_LIGHT_LOAD);
            }
        }
        break;
    case LT_SIM_HIGH_RPM:
        if (in_state > 2000UL) {
            transition(e, LT_SIM_DECELERATION);
        }
        break;
    case LT_SIM_DECELERATION:
        if (e->rpm < 1100) {
            transition(e, LT_SIM_IDLE);
        }
        break;
    case LT_SIM_WOT:
        if (in_state > 3000UL || e->rpm > 6800) {
            transition(e, LT_SIM_HIGH_RPM);
        }
        break;
    default:
        break;
    }
}

static void sim_rpm(lt_simengine *e)
{
    int step = iabs(e->rpm_accel * 50 / 1000);

    if (e->rpm < e->target_rpm) {
        e->rpm = LT_MIN(e->rpm + step, e->target_rpm);
    } else if (e->rpm > e->target_rpm) {
        e->rpm = LT_MAX(e->rpm - step, e->target_rpm);
    }
    e->rpm = iclamp(e->rpm, 0, 7000);
    if (e->mode == LT_SIM_IDLE || e->mode == LT_SIM_WARMUP_IDLE) {
        e->rpm = iclamp(e->rpm + rng_range(e, -10, 10), 0, 7000);
    }
}

static void sim_thermal(lt_simengine *e)
{
    int target, itarget;

    if (e->mode == LT_SIM_WOT || e->mode == LT_SIM_HIGH_RPM) {
        target = 950;
    } else if (e->mode == LT_SIM_IDLE || e->mode == LT_SIM_WARMUP_IDLE) {
        target = 750;
    } else {
        target = 800;
    }
    e->coolant_dc = slew(e->coolant_dc, target, 5);
    itarget = 200 + (e->coolant_dc - 200) / 4;
    if (e->rpm > 2500) {
        itarget -= (e->rpm - 2500) / 50;
    }
    e->intake_dc = slew(e->intake_dc, itarget, 10);
}

static void sim_throttle(lt_simengine *e)
{
    e->tps = slew(e->tps, e->target_tps, 20);
    e->tps_reading = iclamp(e->tps + rng_range(e, -1, 2), 0, 100);
}

static void sim_map(lt_simengine *e)
{
    int base;

    if (e->tps < 10) {
        base = 35 + (e->rpm - 700) / 20;
    } else if (e->tps > 80) {
        base = 95 - (7000 - e->rpm) / 100;
    } else {
        base = map_value(e->tps, 10, 80, 45, 90);
    }
    if (e->rpm > 5000) {
        base += (e->rpm - 5000) / 100;
    }
    e->map_kpa = iclamp(base + rng_range(e, -2, 3), 0, 100);
}

static void sim_ignition(lt_simengine *e)
{
    int load = e->map_kpa * 100 / 100;
    int adv = 15;

    if (e->rpm > 1000) {
        adv += (e->rpm - 1000) / 200;
    }
    if (load > 80) {
        adv -= (load - 80) / 4;
    } else if (load < 40) {
        adv += (40 - load) / 8;
    }
    e->advance_deg = iclamp(adv, 5, 35);
}

static void sim_voltage(lt_simengine *e)
{
    int base = e->mode == LT_SIM_STARTUP ? 100 : 140;

    e->battery_dv = base + rng_range(e, -3, 4);
}

static void step(lt_simengine *e)
{
    e->time_ms += SIM_STEP_MS;
    e->steps++;
    if (e->steps % 20UL == 0) {
        e->secl = (e->secl + 1) & 0xFF;
    }
    state_machine(e);
    sim_rpm(e);
    sim_thermal(e);
    sim_throttle(e);
    sim_map(e);
    sim_ignition(e);
    sim_voltage(e);
}

/* ------------------------------------------------------------ public */
lt_simengine *lt_simengine_new(void)
{
    lt_simengine *e = (lt_simengine *)lt_calloc(1, sizeof(*e));

    e->rng = SIM_SEED;
    e->mode = LT_SIM_STARTUP;
    e->target_tps = e->tps = e->tps_reading = 2;
    e->coolant_dc = e->intake_dc = 200;
    e->map_kpa = 100;
    e->battery_dv = 140;
    transition(e, LT_SIM_STARTUP);
    return e;
}

static void ve_clear(lt_simengine *e)
{
    lt_free(e->ve.rpm);
    lt_free(e->ve.load);
    lt_free(e->ve.ve);
    memset(&e->ve, 0, sizeof(e->ve));
}

void lt_simengine_free(lt_simengine *e)
{
    if (e) {
        ve_clear(e);
        lt_free(e);
    }
}

void lt_simengine_tick(lt_simengine *e, unsigned dt_ms)
{
    e->acc_ms += dt_ms;
    while (e->acc_ms >= SIM_STEP_MS) {
        e->acc_ms -= SIM_STEP_MS;
        step(e);
    }
}

void lt_simengine_set_mode(lt_simengine *e, lt_sim_mode m)
{
    transition(e, m);
}

lt_sim_mode lt_simengine_mode(const lt_simengine *e)
{
    return (lt_sim_mode)e->mode;
}

void lt_simengine_reset_secl(lt_simengine *e)
{
    e->secl = 0;
}

void lt_simengine_advance_secl(lt_simengine *e, unsigned d)
{
    e->secl = (int)(((unsigned)e->secl + d) & 0xFFU);
}

/* -------------------------------------------------------------- VE */
double lt_sim_true_ve(double rpm, double load)
{
    double v = 40.0 + 25.0 * (load / 100.0) + 15.0 * (rpm / 6000.0);

    return v < 20.0 ? 20.0 : (v > 110.0 ? 110.0 : v);
}

static lt_bool ascending(const double *b, size_t n)
{
    size_t i;

    for (i = 1; i < n; i++) {
        if (!(b[i] > b[i - 1])) {
            return LT_FALSE;
        }
    }
    return LT_TRUE;
}

void lt_simengine_set_ve(lt_simengine *e, const double *rpm_bins, size_t nx,
                         const double *load_bins, size_t ny, const double *ve)
{
    size_t i;
    lt_bool any = LT_FALSE;

    ve_clear(e);
    if (nx < 2 || ny < 2 || !rpm_bins || !load_bins || !ve) {
        return;
    }
    if (!ascending(rpm_bins, nx) || !ascending(load_bins, ny)) {
        return;
    }
    for (i = 0; i < nx * ny; i++) {
        if (ve[i] > 0.0) {
            any = LT_TRUE;
            break;
        }
    }
    if (!any) {
        return;
    }
    e->ve.nx = nx;
    e->ve.ny = ny;
    e->ve.rpm = (double *)lt_malloc(nx * sizeof(double));
    e->ve.load = (double *)lt_malloc(ny * sizeof(double));
    e->ve.ve = (double *)lt_malloc(nx * ny * sizeof(double));
    memcpy(e->ve.rpm, rpm_bins, nx * sizeof(double));
    memcpy(e->ve.load, load_bins, ny * sizeof(double));
    memcpy(e->ve.ve, ve, nx * ny * sizeof(double));
}

static void segment(const double *bins, size_t n, double v, size_t *idx, double *frac)
{
    size_t last = n - 1, i;

    if (last == 0 || v <= bins[0]) {
        *idx = 0; *frac = 0.0;
        return;
    }
    if (v >= bins[last]) {
        *idx = last - 1; *frac = 1.0;
        return;
    }
    for (i = 0; i < last; i++) {
        if (bins[i] <= v && v <= bins[i + 1]) {
            double span = bins[i + 1] - bins[i];

            *idx = i;
            *frac = span > 0.0 ? (v - bins[i]) / span : 0.0;
            return;
        }
    }
    *idx = last - 1; *frac = 1.0;
}

static lt_bool current_ve(const lt_simengine *e, double rpm, double load, double *out)
{
    const sim_vectx *c = &e->ve;
    size_t x0, y0, x1, y1;
    double fx, fy, top, bot;

    if (c->nx == 0 || c->ny == 0) {
        return LT_FALSE;
    }
    segment(c->rpm, c->nx, rpm, &x0, &fx);
    segment(c->load, c->ny, load, &y0, &fy);
    x1 = LT_MIN(x0 + 1, c->nx - 1);
    y1 = LT_MIN(y0 + 1, c->ny - 1);
    top = c->ve[y0 * c->nx + x0] * (1.0 - fx) + c->ve[y0 * c->nx + x1] * fx;
    bot = c->ve[y1 * c->nx + x0] * (1.0 - fx) + c->ve[y1 * c->nx + x1] * fx;
    *out = top * (1.0 - fy) + bot * fy;
    return LT_TRUE;
}

void lt_simengine_snapshot(const lt_simengine *e, lt_sim_snapshot *out)
{
    double cur;

    memset(out, 0, sizeof(*out));
    out->secl = e->secl;
    out->rpm = e->rpm;
    out->map_kpa = e->map_kpa;
    out->baro_kpa = 100;
    out->coolant_c = e->coolant_dc / 10;
    out->iat_c = e->intake_dc / 10;
    out->tps_percent = e->tps_reading;
    out->battery_dv = e->battery_dv;
    out->advance_deg = e->advance_deg;
    out->afr_target = 14.7;
    out->ego_correction = 100.0;
    out->cranking = e->mode == LT_SIM_STARTUP;
    out->running = (e->rpm > 0 && e->mode != LT_SIM_STARTUP) ? LT_TRUE : LT_FALSE;
    if (current_ve(e, (double)e->rpm, (double)e->map_kpa, &cur)) {
        double wanted = lt_sim_true_ve((double)e->rpm, (double)e->map_kpa);

        if (cur < 1.0) {
            cur = 1.0;
        }
        out->afr = 14.7 * wanted / cur;
    } else {
        out->afr = 14.7;
    }
}

/* ------------------------------------------------------ VE seeding */
static void spread(size_t n, double lo, double hi, double *out)
{
    size_t i;

    for (i = 0; i < n; i++) {
        out[i] = lo + (hi - lo) / (double)(n - 1) * (double)i;
    }
}

/* lower-case prefix test */
static lt_bool starts_ci(const char *s, const char *prefix)
{
    size_t i;
    for (i = 0; prefix[i] != '\0'; i++) {
        int a = (unsigned char)s[i];
        if (a >= 'A' && a <= 'Z') {
            a += 'a' - 'A';
        }
        if (a != (unsigned char)prefix[i]) {
            return LT_FALSE;
        }
    }
    return LT_TRUE;
}

/* the first 3-D table whose cell constant is named like one of the prefixes */
static const lt_ini_table *table_by_cells(const lt_ini *ini, const char *const *prefixes, size_t np)
{
    size_t i, k;

    for (i = 0; i < ini->n_tables; i++) {
        const lt_ini_table *t = ini->tables[i];
        if (!t->x_const || !t->y_const || !t->z_const) {
            continue;
        }
        for (k = 0; k < np; k++) {
            if (starts_ci(t->z_const, prefixes[k])) {
                return t;
            }
        }
    }
    return NULL;
}

/* The VE table: the one [VeAnalyze] names, else (MegaSquirt INIs have no [VeAnalyze]) the
 * first table of VE cells by name (veTable1, veBins). */
const lt_ini_table *lt_sim_ve_table(const lt_ini *ini)
{
    static const char *const names[] = { "vetable", "vebins" };
    const lt_ini_table *t = NULL;

    if (!ini) {
        return NULL;
    }
    if (ini->ve_analyze.present && ini->ve_analyze.ve_table) {
        t = lt_ini_find_table(ini, ini->ve_analyze.ve_table);
        if (t && t->x_const && t->y_const && t->z_const) {
            return t;
        }
        return NULL;
    }
    if (ini->ve_analyze.present) {
        return NULL;
    }
    t = table_by_cells(ini, names, LT_COUNTOF(names));
    return t;
}

/* The AFR / lambda target table, the same way (afrTable1, lambdaTable1). */
const lt_ini_table *lt_sim_afr_table(const lt_ini *ini)
{
    static const char *const names[] = { "afrtable", "afrbins", "lambdatable" };
    const lt_ini_table *t = NULL;

    if (!ini) {
        return NULL;
    }
    if (ini->ve_analyze.present && ini->ve_analyze.target_table) {
        t = lt_ini_find_table(ini, ini->ve_analyze.target_table);
        if (t && t->x_const && t->y_const && t->z_const) {
            return t;
        }
        return NULL;
    }
    if (ini->ve_analyze.present) {
        return NULL;
    }
    t = table_by_cells(ini, names, LT_COUNTOF(names));
    return t;
}

lt_bool lt_sim_seed_ve_table(const lt_ini *ini, lt_pagesrc *dst)
{
    const lt_ini_table *t;
    const lt_ini_const *cx, *cy, *cz;
    size_t nx, ny, nz, i, j;
    double *rpm, *load, *z;
    lt_bool ok;

    if (!ini || !dst) {
        return LT_FALSE;
    }
    t = lt_sim_ve_table(ini);
    if (!t) {
        return LT_FALSE;
    }
    cx = lt_ini_find_const(ini, t->x_const);
    cy = lt_ini_find_const(ini, t->y_const);
    cz = lt_ini_find_const(ini, t->z_const);
    if (!cx || !cy || !cz) {
        return LT_FALSE;
    }
    nx = lt_shape_count(&cx->shape);
    ny = lt_shape_count(&cy->shape);
    nz = lt_shape_count(&cz->shape);
    if (nx < 2 || ny < 2 || nz != nx * ny) {
        return LT_FALSE;
    }
    rpm = (double *)lt_malloc(nx * sizeof(double));
    load = (double *)lt_malloc(ny * sizeof(double));
    z = (double *)lt_malloc(nz * sizeof(double));
    spread(nx, 500.0, 7000.0, rpm);
    spread(ny, 20.0, 100.0, load);
    for (i = 0; i < ny; i++) {
        for (j = 0; j < nx; j++) {
            z[i * nx + j] = lt_sim_true_ve(rpm[j], load[i]) * 0.85;
        }
    }
    ok = lt_ini_const_set_array(ini, cx, dst, rpm, nx) >= 0 &&
         lt_ini_const_set_array(ini, cy, dst, load, ny) >= 0 &&
         lt_ini_const_set_array(ini, cz, dst, z, nz) >= 0;
    lt_free(rpm);
    lt_free(load);
    lt_free(z);
    return ok;
}

/* lower-case substring test for axis naming heuristics */
static lt_bool has_ci(const char *s, const char *needle)
{
    size_t n, i, k;
    if (s == NULL) {
        return LT_FALSE;
    }
    n = strlen(needle);
    for (i = 0; s[i] != '\0'; i++) {
        for (k = 0; k < n; k++) {
            int a = (unsigned char)s[i + k], b = (unsigned char)needle[k];
            if (a >= 'A' && a <= 'Z') {
                a += 'a' - 'A';
            }
            if (a != b) {
                break;
            }
        }
        if (k == n) {
            return LT_TRUE;
        }
    }
    return LT_FALSE;
}

/* A sensible demo range for an axis, from its units, constant name and live channel. */
static void axis_range(const lt_ini_const *c, const char *channel, double *lo, double *hi)
{
    const char *u = c->units != NULL ? c->units : "";
    if (has_ci(u, "rpm") || has_ci(c->name, "rpm") || has_ci(channel, "rpm")) {
        *lo = 500.0; *hi = 7000.0;
    } else if (has_ci(u, "f") && (has_ci(u, "deg") || strcmp(u, "F") == 0)) {
        *lo = -40.0; *hi = 250.0;
    } else if (strcmp(u, "C") == 0 || has_ci(u, "deg c") || has_ci(u, "degc") ||
               has_ci(c->name, "clt") || has_ci(c->name, "iat") || has_ci(c->name, "temp") ||
               has_ci(channel, "coolant") || has_ci(channel, "intake")) {
        *lo = -40.0; *hi = 110.0;
    } else if (has_ci(u, "kpa") || has_ci(c->name, "map") || has_ci(c->name, "load")) {
        *lo = 20.0; *hi = 200.0;
    } else if (has_ci(u, "%") || has_ci(c->name, "tps") || has_ci(c->name, "pedal") ||
               has_ci(channel, "tps") || has_ci(channel, "throttle")) {
        *lo = 0.0; *hi = 100.0;
    } else if (strcmp(u, "V") == 0 || has_ci(u, "volt") || has_ci(c->name, "batt") ||
               has_ci(c->name, "vbat")) {
        *lo = 8.0; *hi = 16.0;
    } else if (has_ci(u, "lambda") || has_ci(c->name, "lambda")) {
        *lo = 0.7; *hi = 1.3;
    } else if (has_ci(u, "afr") || has_ci(c->name, "afr")) {
        *lo = 10.0; *hi = 18.0;
    } else {
        *lo = 0.0; *hi = 100.0;
    }
    if (c->range_resolved && c->max > c->min) {
        if (*lo < c->min) { *lo = c->min; }
        if (*hi > c->max) { *hi = c->max; }
        if (*hi <= *lo) { *lo = c->min; *hi = c->max; }
    }
}

/* Fill one axis constant with an ascending spread if it is degenerate (all values equal,
 * as on a blank demo page image), so curves and tables are usable in demo mode. */
static lt_bool seed_axis(const lt_ini *ini, lt_pagesrc *dst, const char *name, const char *channel)
{
    const lt_ini_const *c;
    size_t n, i;
    double *v, lo = 0.0, hi = 0.0;
    lt_bool flat = LT_TRUE, ok = LT_FALSE;

    if (name == NULL || (c = lt_ini_find_const(ini, name)) == NULL || c->type == LT_DT_BITS) {
        return LT_FALSE;
    }
    n = lt_shape_count(&c->shape);
    if (n < 2 || n > 4096) {
        return LT_FALSE;
    }
    v = (double *)lt_malloc(n * sizeof(double));
    if (lt_ini_const_get_array(ini, c, dst, v, n) < 0) {
        lt_free(v);
        return LT_FALSE;
    }
    for (i = 1; i < n; i++) {
        if (v[i] != v[0]) {
            flat = LT_FALSE;
            break;
        }
    }
    if (flat) {
        axis_range(c, channel, &lo, &hi);
        spread(n, lo, hi, v);
        ok = lt_ini_const_set_array(ini, c, dst, v, n) >= 0;
    }
    lt_free(v);
    return ok;
}

lt_bool lt_sim_seed_axes(const lt_ini *ini, lt_pagesrc *dst)
{
    size_t i;
    lt_bool any = LT_FALSE;

    if (!ini || !dst) {
        return LT_FALSE;
    }
    for (i = 0; i < ini->n_curves; i++) {
        const lt_ini_curve *cv = ini->curves[i];
        if (seed_axis(ini, dst, cv->x_const, cv->x_channel)) {
            any = LT_TRUE;
        }
    }
    for (i = 0; i < ini->n_tables; i++) {
        const lt_ini_table *t = ini->tables[i];
        if (seed_axis(ini, dst, t->x_const, t->x_channel)) {
            any = LT_TRUE;
        }
        if (seed_axis(ini, dst, t->y_const, t->y_channel)) {
            any = LT_TRUE;
        }
    }
    return any;
}

lt_bool lt_sim_seed_afr_target(const lt_ini *ini, lt_pagesrc *dst)
{
    const lt_ini_table *t;
    const lt_ini_const *cx, *cy, *cz, *cw;
    size_t nx, ny, nz, i, n;
    double *rpm, *load, *z, tgt = 14.7;
    lt_bool ok;

    if (!ini || !dst) {
        return LT_FALSE;
    }
    t = lt_sim_afr_table(ini);
    if (!t) {
        return LT_FALSE;
    }
    cx = lt_ini_find_const(ini, t->x_const);
    cy = lt_ini_find_const(ini, t->y_const);
    cz = lt_ini_find_const(ini, t->z_const);
    if (!cx || !cy || !cz) {
        return LT_FALSE;
    }
    nx = lt_shape_count(&cx->shape);
    ny = lt_shape_count(&cy->shape);
    nz = lt_shape_count(&cz->shape);
    if (nx < 2 || ny < 2 || nz != nx * ny) {
        return LT_FALSE;
    }
    if (cz->max < 5.0) {
        tgt = 1.0;      /* a lambda target table */
    }
    rpm = (double *)lt_malloc(nx * sizeof(double));
    load = (double *)lt_malloc(ny * sizeof(double));
    z = (double *)lt_malloc(nz * sizeof(double));
    spread(nx, 500.0, 7000.0, rpm);
    spread(ny, 20.0, 100.0, load);
    for (i = 0; i < nz; i++) {
        z[i] = tgt;
    }
    ok = lt_ini_const_set_array(ini, cx, dst, rpm, nx) >= 0 &&
         lt_ini_const_set_array(ini, cy, dst, load, ny) >= 0 &&
         lt_ini_const_set_array(ini, cz, dst, z, nz) >= 0;
    lt_free(rpm);
    lt_free(load);
    lt_free(z);
    /* warmup enrichment: decays to neutral by the warm slots (wueRates in percent, else ratio) */
    cw = lt_ini_find_const(ini, "wueRates");
    if (!cw) {
        cw = lt_ini_find_const(ini, "cltFuelCorr");
    }
    if (cw) {
        double one = cw->max > 20.0 ? 100.0 : 1.0;

        n = lt_shape_count(&cw->shape);
        if (n >= 2 && n <= 64) {
            double w[64];

            for (i = 0; i < n; i++) {
                double f = (double)i / (double)(n - 1);

                w[i] = f >= 0.6 ? one : one * (1.5 - 0.5 * f / 0.6);
            }
            (void)lt_ini_const_set_array(ini, cw, dst, w, n);
        }
    }
    return ok;
}

/* ------------------------------------------------------- och codec */
static size_t dtype_width(lt_dtype t)
{
    switch (t) {
    case LT_DT_U08: case LT_DT_S08: case LT_DT_BITS: return 1;
    case LT_DT_U16: case LT_DT_S16: return 2;
    case LT_DT_U32: case LT_DT_S32: case LT_DT_F32: return 4;
    case LT_DT_F64: return 8;
    default: return 0;
    }
}

size_t lt_sim_block_size(const lt_ini *ini)
{
    size_t i, best = 0;

    if (!ini) {
        return 0;
    }
    if (ini->proto.och_block_size > 0) {
        return (size_t)ini->proto.och_block_size;
    }
    for (i = 0; i < ini->n_channels; i++) {
        const lt_ini_channel *c = ini->channels[i];
        lt_dtype k = c->type == LT_DT_BITS ? c->bit_storage : c->type;
        size_t end;

        if (c->expression) {
            continue;
        }
        end = (size_t)c->offset + dtype_width(k);
        if (end > best) {
            best = end;
        }
    }
    return best;
}

static void write_scalar(lt_dtype t, lt_endian en, lt_u8 *block, size_t len, size_t off,
                         double raw)
{
    double lo, hi;

    if (!lt_isfinite(raw)) {
        raw = 0.0;
    }
    if (t == LT_DT_F32) {
        raw = raw > (double)FLT_MAX ? (double)FLT_MAX : (raw < -(double)FLT_MAX ?
              -(double)FLT_MAX : raw);
    } else if (t != LT_DT_F64) {
        raw = lt_round(raw);
        if (t == LT_DT_BITS) {
            t = LT_DT_U08;
        }
        if (lt_dtype_range(t, &lo, &hi)) {
            raw = raw < lo ? lo : (raw > hi ? hi : raw);
        }
    }
    if (t == LT_DT_STRING || off + dtype_width(t) > len) {
        return;
    }
    (void)lt_dtype_write(t, block, len, off, en, raw);
}

static void write_bits(lt_u8 *block, size_t len, size_t off, lt_dtype storage, lt_endian en,
                       int lo, int count, lt_u32 value)
{
    size_t w = dtype_width(storage);
    lt_u32 cur, mask;

    if (w == 0) {
        w = 1;
    }
    if (w > 4) {
        return;
    }
    if (count <= 0 || lo < 0 || (size_t)(lo + count) > w * 8 || off + w > len) {
        return;
    }
    cur = en == LT_ENDIAN_LITTLE ? lt_rd_le(block + off, (int)w) : lt_rd_be(block + off, (int)w);
    mask = count >= 32 ? LT_U32_MASK : (((lt_u32)1 << count) - 1) & LT_U32_MASK;
    cur = (cur & ~((mask << lo) & LT_U32_MASK)) | (((value & mask) << lo) & LT_U32_MASK);
    cur &= LT_U32_MASK;
    if (en == LT_ENDIAN_LITTLE) {
        lt_wr_le(block + off, (int)w, cur);
    } else {
        lt_wr_be(block + off, (int)w, cur);
    }
}

/* units naming degrees Fahrenheit: "F", "degF", "deg F", a degree sign (any encoding) + F */
static lt_bool fahrenheit(const char *u)
{
    size_t n;

    if (u == NULL) {
        return LT_FALSE;
    }
    n = strlen(u);
    while (n > 0 && u[n - 1] == ' ') {
        n--;
    }
    if (n == 0 || u[n - 1] != 'F') {
        return LT_FALSE;
    }
    if (n == 1 || (unsigned char)u[n - 2] >= 0x80 || u[n - 2] == ' ') {
        return n <= 4 || has_ci(u, "deg");
    }
    return has_ci(u, "deg") && n <= 5;
}

static lt_bool scalar_by_name(const char *n, const lt_sim_snapshot *s, double *out)
{
    if (!strcmp(n, "secl") || !strcmp(n, "seconds")) {
        *out = s->secl;
    } else if (!strcmp(n, "rpm") || !strcmp(n, "RPMValue")) {
        *out = s->rpm;
    } else if (!strcmp(n, "map") || !strcmp(n, "MAPValue")) {
        *out = s->map_kpa;
    } else if (!strcmp(n, "baro") || !strcmp(n, "baroPressure") || !strcmp(n, "barometer")) {
        *out = 100.0;
    } else if (!strcmp(n, "coolantRaw")) {
        *out = s->coolant_c + 40;
    } else if (!strcmp(n, "iatRaw")) {
        *out = s->iat_c + 40;
    } else if (!strcmp(n, "coolant")) {
        *out = s->coolant_c;
    } else if (!strcmp(n, "iat") || !strcmp(n, "intake") || !strcmp(n, "mat")) {
        *out = s->iat_c;
    } else if (!strcmp(n, "tps") || !strcmp(n, "throttle") || !strcmp(n, "TPSValue") ||
               !strcmp(n, "throttlePedalPosition")) {
        *out = s->tps_percent;
    } else if (!strcmp(n, "batteryVoltage") || !strcmp(n, "VBatt")) {
        *out = s->battery_dv / 10.0;
    } else if (!strcmp(n, "advance") || !strcmp(n, "ignitionAdvance")) {
        *out = s->advance_deg;
    } else if (!strcmp(n, "afrTarget") || !strcmp(n, "afrtgt1") || !strcmp(n, "afrtgt2")) {
        *out = s->afr_target;
    } else if (!strcmp(n, "afr") || !strcmp(n, "afrGasolineScale") || !strcmp(n, "AFRValue") ||
               !strcmp(n, "AFRValue2") || !strcmp(n, "afr1") || !strcmp(n, "afr2") ||
               !strcmp(n, "AFR")) {
        /* rusEFI names its wideband AFR AFRValue (gauges) and afrGasolineScale */
        *out = s->afr;
    } else if (!strcmp(n, "lambdaValue") || !strcmp(n, "lambdaValue2") || !strcmp(n, "lambda")) {
        *out = s->afr / 14.7;
    } else if (!strcmp(n, "targetLambda") || !strcmp(n, "lambdaTarget")) {
        *out = s->afr_target / 14.7;
    } else if (!strcmp(n, "egoCorrection") || !strcmp(n, "egoCorrection1") ||
               !strcmp(n, "egoCorrection2")) {
        *out = 100.0;
    } else if (!strcmp(n, "engine")) {
        *out = (s->running ? 1 : 0) | (s->cranking ? 2 : 0);
    } else {
        return LT_FALSE;
    }
    return LT_TRUE;
}

void lt_sim_encode_och(const lt_ini *ini, const lt_sim_snapshot *s, lt_u8 *block, size_t len)
{
    size_t i;

    if (!ini || !s || !block) {
        return;
    }
    for (i = 0; i < ini->n_channels; i++) {
        const lt_ini_channel *c = ini->channels[i];
        double phys, raw;

        if (c->expression) {
            continue;
        }
        if (c->type == LT_DT_BITS) {
            lt_u32 v;

            if (c->bit_pos < 0 || c->bit_count <= 0) {
                continue;
            }
            if (!strcmp(c->name, "running") || !strcmp(c->name, "ready")) {
                v = s->running ? 1 : 0;
            } else if (!strcmp(c->name, "crank") || !strcmp(c->name, "cranking")) {
                v = s->cranking ? 1 : 0;
            } else {
                continue;
            }
            write_bits(block, len, c->offset, c->bit_storage, ini->endian, c->bit_pos,
                       c->bit_count, v);
            continue;
        }
        if (!scalar_by_name(c->name, s, &phys)) {
            continue;
        }
        if (fahrenheit(c->units) && (!strcmp(c->name, "coolant") || !strcmp(c->name, "iat") ||
                                     !strcmp(c->name, "intake") || !strcmp(c->name, "mat"))) {
            phys = phys * 9.0 / 5.0 + 32.0;     /* MegaSquirt reports degF unless CELSIUS */
        }
        raw = c->scale != 0.0 ? (phys - c->translate) / c->scale : 0.0;
        write_scalar(c->type, ini->endian, block, len, c->offset, raw);
    }
}
