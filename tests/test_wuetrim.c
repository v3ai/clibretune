/* test_wuetrim.c - WUE Analyze and Trim Table Analyze engines: convergence on synthetic
 * warm-ups, filters, warm-end pinning, per-cylinder trim recovery, balancing, log replay. */
#include <string.h>
#include <math.h>
#include "lt.h"
#include "lt_test.h"
#include "autotune/at_wue.h"
#include "autotune/at_trim.h"

#define NB 8
static const double g_clt[NB] = { -20.0, 0.0, 20.0, 40.0, 60.0, 70.0, 80.0, 90.0 };
static const double g_wue[NB] = { 1.60, 1.45, 1.30, 1.15, 1.05, 1.00, 1.00, 1.00 };

/* the synthetic engine: lean by 8% below 10 C, and 3% lean everywhere (a VE error) */
static double lean_of(double clt) { return (clt < 10.0 ? 1.08 : 1.0) * 1.03; }

static void wue_setup(lt_wue_state **s, lt_wue_config *c)
{
    *s = lt_wue_new();
    LT_CHECK(lt_wue_set_curve(*s, g_clt, g_wue, NB, 0.0, 1.0) == 0);
    lt_wue_config_default(c);
    c->min_samples = 10;
}

/* warm-up from -20 C to 92 C at 10 Hz: 2 s cranking, then idle */
static void warmup(lt_wue_state *s, const lt_wue_config *c, int with_transients)
{
    unsigned long t;
    for (t = 0; t < 2400000UL; t += 100UL) {
        lt_wue_point p;
        double sec = (double)t / 1000.0;
        lt_wue_point_default(&p);
        p.t_ms = t;
        p.rpm = sec < 2.0 ? 200.0 : 900.0;
        p.clt = -20.0 + sec * 0.05;
        if (p.clt > 92.0) { p.clt = 92.0; }
        p.tps = 2.0;
        p.target_afr = 14.7;
        p.afr = 14.7 * lean_of(p.clt);
        if (with_transients && (t / 100UL) % 50UL == 7UL) {
            p.tps_rate = 300.0;         /* a blip: rich/lean garbage that must be dropped */
            p.afr = 19.0;
        }
        lt_wue_add(s, &p, c);
    }
}

LT_TEST(wue_converges_cold_bins_only)
{
    lt_wue_state *s;
    lt_wue_config c;
    lt_wue_bin b[NB];
    size_t i;
    wue_setup(&s, &c);
    lt_wue_start(s);
    warmup(s, &c, 1);
    lt_wue_stop(s);
    LT_CHECK(lt_wue_bins(s, &c, b, NB) == NB);
    LT_CHECK(lt_wue_warm_index(s, &c) == 5);
    LT_CHECK_NEAR(lt_wue_warm_factor(s, &c, NULL), 1.03, 0.002);
    /* the cold bins get the 8% they lack, relative to the warm engine */
    LT_CHECK(b[0].status == LT_WUE_REC);
    LT_CHECK_NEAR(b[0].recommended, 1.60 * 1.08, 0.01);
    LT_CHECK_NEAR(b[0].factor, 1.08, 0.005);
    LT_CHECK(b[1].factor > 1.05 && b[1].factor < 1.08);
    LT_CHECK(b[1].recommended > 1.45 * 1.05);
    LT_CHECK(b[2].recommended > 1.30 && b[2].recommended < 1.30 * 1.08);
    /* warmer bins are left alone, the warm end is pinned to unity */
    LT_CHECK_NEAR(b[3].recommended, 1.15, 0.005);
    LT_CHECK_NEAR(b[4].recommended, 1.05, 0.005);
    for (i = 5; i < NB; i++) {
        LT_CHECK(b[i].status == LT_WUE_WARM);
        LT_CHECK_NEAR(b[i].recommended, 1.0, 1e-9);
    }
    /* monotonic: never less enrichment when colder */
    for (i = 0; i + 1 < NB; i++) { LT_CHECK(b[i].recommended >= b[i + 1].recommended - 1e-9); }
    lt_wue_free(s);
}

LT_TEST(wue_filters_reject_transients_and_cranking)
{
    lt_wue_state *s;
    lt_wue_config c;
    lt_wue_bin b[NB];
    lt_at_reject rj[16];
    size_t n, i;
    unsigned long t, crank = 0, after = 0, acc = 0, rate = 0, thr = 0, clt = 0;
    wue_setup(&s, &c);
    lt_wue_start(s);
    for (t = 0; t < 60000UL; t += 100UL) {
        lt_wue_point p;
        double sec = (double)t / 1000.0;
        lt_wue_point_default(&p);
        p.t_ms = t;
        p.clt = 0.0;
        p.tps = 2.0;
        p.afr = 18.5;                   /* very lean: would move bins if anything got through */
        p.target_afr = 14.7;
        p.rpm = sec < 3.0 ? 250.0 : 1000.0;
        if (sec >= 33.0) {
            /* engine running past the after-start window: every sample is a transient */
            int k = (int)((t / 100UL) % 4UL);
            if (k == 0) { p.tps_rate = 200.0; }
            else if (k == 1) { p.tps = 80.0; }
            else if (k == 2) { p.accel_enrich_active = 1; }
            else if ((t / 400UL) % 2UL == 0UL) { p.clt = 40.0; }   /* a coolant sensor jump */
            else { p.tps_rate = 150.0; }
        }
        lt_wue_add(s, &p, &c);
    }
    LT_CHECK(lt_wue_accepted(s) < 10);
    n = lt_wue_rejections(s, rj, 16);
    for (i = 0; i < n; i++) {
        if (strcmp(rj[i].reason, "cranking") == 0) { crank = rj[i].count; }
        if (strcmp(rj[i].reason, "accel enrichment active") == 0) { acc = rj[i].count; }
        if (strcmp(rj[i].reason, "after-start enrichment") == 0) { after = rj[i].count; }
        if (strcmp(rj[i].reason, "tps_rate above max_tps_rate") == 0) { rate = rj[i].count; }
        if (strcmp(rj[i].reason, "throttle above max") == 0) { thr = rj[i].count; }
        if (strcmp(rj[i].reason, "coolant changing too fast") == 0) { clt = rj[i].count; }
    }
    LT_CHECK(crank == 30);              /* 3 s cranking */
    LT_CHECK(acc > 50);
    LT_CHECK(after >= 290);             /* 30 s after the start */
    LT_CHECK(rate > 50 && thr > 50);
    LT_CHECK(clt > 0);
    lt_wue_bins(s, &c, b, NB);
    LT_CHECK(b[1].status != LT_WUE_REC); /* the few survivors are below min_samples */
    LT_CHECK_NEAR(b[1].recommended, 1.45, 1e-9);
    lt_wue_free(s);
}

LT_TEST(wue_warm_end_pinned_and_locks)
{
    static const double vals[NB] = { 1.60, 1.45, 1.30, 1.15, 1.05, 1.03, 1.02, 1.01 };
    lt_wue_state *s = lt_wue_new();
    lt_wue_config c;
    lt_wue_bin b[NB];
    lt_wue_set_curve(s, g_clt, vals, NB, 0.0, 1.0);
    lt_wue_config_default(&c);
    LT_CHECK(lt_wue_warm_index(s, &c) == NB - 1);       /* nothing neutral: the last bin */
    c.warm_clt = 70.0;
    LT_CHECK(lt_wue_warm_index(s, &c) == 5);
    lt_wue_lock(s, 6, LT_TRUE);
    lt_wue_bins(s, &c, b, NB);
    LT_CHECK_NEAR(b[5].recommended, 1.0, 1e-9);         /* pinned even without data */
    LT_CHECK(b[6].status == LT_WUE_LOCKED);
    LT_CHECK_NEAR(b[6].recommended, 1.02, 1e-9);
    LT_CHECK_NEAR(b[7].recommended, 1.0, 1e-9);
    LT_CHECK(b[0].status == LT_WUE_NO_DATA);
    /* percent curves: unity 100; zero-based curves: offset 100 */
    lt_wue_set_curve(s, g_clt, vals, NB, 100.0, 100.0);
    LT_CHECK_NEAR(lt_wue_unity_value(s), 0.0, 1e-9);
    lt_wue_free(s);
}

LT_TEST(wue_target_offset_and_lambda_table)
{
    static const double rx[2] = { 500.0, 6000.0 }, ry[2] = { 20.0, 100.0 };
    static const double lam[4] = { 1.0, 1.0, 1.0, 1.0 };
    static const double offc[2] = { -20.0, 60.0 }, offv[2] = { -1.47, 0.0 };
    lt_wue_state *s;
    lt_wue_config c;
    lt_wue_bin b[NB];
    unsigned long t;
    wue_setup(&s, &c);
    lt_wue_set_target_table(s, rx, 2, ry, 2, lam);
    lt_wue_set_afr_offset(s, offc, offv, 2);
    LT_CHECK_NEAR(lt_wue_target_at(s, 1000.0, 40.0, 0.0), 14.7, 1e-9);
    LT_CHECK_NEAR(lt_wue_afr_offset_at(s, -20.0), -1.47, 1e-9);
    LT_CHECK_NEAR(lt_wue_afr_offset_at(s, 20.0), -0.735, 1e-9);
    lt_wue_start(s);
    for (t = 0; t < 600000UL; t += 100UL) {
        lt_wue_point p;
        lt_wue_point_default(&p);
        p.t_ms = t;
        p.rpm = 1000.0;
        p.load = 40.0;
        p.clt = -20.0;
        p.afr = 14.7 - 1.47;            /* exactly the offset target: nothing to correct */
        lt_wue_add(s, &p, &c);
    }
    lt_wue_bins(s, &c, b, NB);
    LT_CHECK(b[0].status == LT_WUE_REC);
    LT_CHECK_NEAR(b[0].recommended, 1.60, 1e-6);
    LT_CHECK_NEAR(b[0].target_mean, 13.23, 1e-6);
    lt_wue_free(s);
}

LT_TEST(wue_replay_matches_live)
{
    static const char *const names[6] = { "RPMValue", "coolant", "lambdaValue", "TPSValue", "isCranking", "egoCorrection" };
    lt_log log;
    lt_wue_state *s, *live;
    lt_wue_config c;
    lt_wue_channels ch;
    lt_wue_bin a[NB], b[NB];
    unsigned long t;
    long acc;
    size_t i;
    lt_ini_filter fl;
    unsigned char en = 1;
    lt_log_init(&log);
    lt_log_set_channels(&log, names, NULL, 6);
    wue_setup(&s, &c);
    wue_setup(&live, &c);
    lt_wue_start(live);
    for (t = 0; t < 2400000UL; t += 100UL) {
        double v[6], sec = (double)t / 1000.0;
        lt_wue_point p;
        v[0] = sec < 2.0 ? 200.0 : 900.0;
        v[1] = -20.0 + sec * 0.05; if (v[1] > 92.0) { v[1] = 92.0; }
        v[2] = lean_of(v[1]) / 1.02;    /* lambda; the closed loop adds 2% on top */
        v[3] = 2.0;
        v[4] = sec < 2.0 ? 1.0 : 0.0;
        v[5] = 102.0;
        lt_log_append(&log, sec, v);
        lt_wue_point_default(&p);
        p.t_ms = t; p.rpm = v[0]; p.clt = v[1]; p.afr = v[2] * 14.7; p.tps = v[3];
        p.cranking = (int)v[4]; p.ego = 1.02; p.target_afr = 14.7;
        lt_wue_add(live, &p, &c);
    }
    memset(&ch, 0, sizeof(ch));
    lt_strlcpy(ch.at.rpm, "RPMValue", sizeof(ch.at.rpm));
    lt_strlcpy(ch.at.clt, "coolant", sizeof(ch.at.clt));
    lt_strlcpy(ch.at.afr, "lambdaValue", sizeof(ch.at.afr));
    lt_strlcpy(ch.at.tps, "TPSValue", sizeof(ch.at.tps));
    lt_strlcpy(ch.cranking, "isCranking", sizeof(ch.cranking));
    lt_strlcpy(ch.ego, "egoCorrection", sizeof(ch.ego));
    ch.at.afr_is_lambda = LT_TRUE;
    acc = lt_wue_replay(s, &log, &ch, &c, NULL, 0, NULL);
    LT_CHECK(acc > 1000);
    LT_CHECK(acc == (long)lt_wue_accepted(live));
    lt_wue_bins(s, &c, a, NB);
    lt_wue_bins(live, &c, b, NB);
    for (i = 0; i < NB; i++) { LT_CHECK_NEAR(a[i].recommended, b[i].recommended, 1e-6); }
    LT_CHECK_NEAR(a[0].recommended, 1.60 * 1.08, 0.01);
    /* a declared INI filter that rejects everything warm */
    memset(&fl, 0, sizeof(fl));
    fl.name = (char *)"warmFilter"; fl.display = (char *)"Too warm"; fl.channel = (char *)"coolant";
    fl.op = LT_FOP_GT; fl.dflt = 30.0;
    acc = lt_wue_replay(s, &log, &ch, &c, &fl, 1, &en);
    lt_wue_bins(s, &c, a, NB);
    LT_CHECK(a[4].hits == 0);
    LT_CHECK(a[0].hits > 0);
    {
        lt_at_reject rj[16];
        size_t n = lt_wue_rejections(s, rj, 16), k, found = 0;
        for (k = 0; k < n; k++) { if (strcmp(rj[k].reason, "Too warm") == 0) { found = rj[k].count; } }
        LT_CHECK(found > 1000);
    }
    /* no coolant channel: refused */
    lt_strlcpy(ch.at.clt, "nope", sizeof(ch.at.clt));
    LT_CHECK(lt_wue_replay(s, &log, &ch, &c, NULL, 0, NULL) < 0);
    lt_log_free(&log);
    lt_wue_free(s);
    lt_wue_free(live);
}

LT_TEST(wue_units)
{
    lt_ini_const k;
    double off = 0.0, un = 0.0;
    memset(&k, 0, sizeof(k));
    k.units = (char *)"ratio"; k.max = 5.0;
    lt_wue_units_for(&k, 0.0, &off, &un);
    LT_CHECK(off == 0.0 && un == 1.0);
    k.units = (char *)"%"; k.max = 255.0;
    lt_wue_units_for(&k, 0.0, &off, &un);
    LT_CHECK(off == 0.0 && un == 100.0);
    lt_wue_units_for(&k, 100.0, &off, &un);
    LT_CHECK(off == 100.0 && un == 100.0);
}

/* ---------------------------------------------------------------- trims */

#define TN 4
static const double g_rpm[TN] = { 1000.0, 2000.0, 3000.0, 4000.0 };
static const double g_load[TN] = { 30.0, 50.0, 70.0, 90.0 };
static const double g_cyl_err[4] = { 1.04, 0.97, 1.02, 1.00 };   /* lean > 1 */

static void trim_setup(lt_trim_state **s, lt_trim_config *c, size_t ncyl)
{
    double z[TN * TN];
    lt_trim_units u;
    size_t i;
    memset(z, 0, sizeof(z));
    u.offset = 100.0; u.scale = 100.0;   /* percent add */
    *s = lt_trim_new(ncyl);
    for (i = 0; i < ncyl; i++) { LT_CHECK(lt_trim_set_table(*s, i, g_rpm, TN, g_load, TN, z, &u) == 0); }
    lt_trim_config_default(c);
    c->f.min_clt = 60.0;
    c->set.lambda_delay_ms = 100.0;
}

/* steady blocks over every cell; measured = target * VE error * cylinder error */
static void trim_feed(lt_trim_state *s, const lt_trim_config *c, size_t cyl, double ve_err,
                      double ve_factor_known, unsigned long *t)
{
    size_t x, y, k;
    for (y = 0; y < TN; y++) {
        for (x = 0; x < TN; x++) {
            for (k = 0; k < 60; k++) {
                lt_ve_point p;
                lt_ve_point_default(&p);
                p.timestamp_ms = *t;
                p.rpm = g_rpm[x];
                p.load = g_load[y];
                p.clt = 90.0;
                p.afr = 13.5 * ve_err * g_cyl_err[cyl];
                lt_trim_add(s, cyl, &p, 13.5, ve_factor_known, 0.0, c);
                *t += 100UL;
            }
        }
    }
}

static double rec_at(const lt_at_rec *r, size_t n, unsigned x, unsigned y)
{
    size_t i;
    for (i = 0; i < n; i++) { if (r[i].cell_x == x && r[i].cell_y == y) { return r[i].recommended_value; } }
    return -999.0;
}

LT_TEST(trim_recovers_cylinder_error)
{
    lt_trim_state *s;
    lt_trim_config c;
    lt_at_rec r[TN * TN];
    size_t cyl, n;
    unsigned long t = 1000;
    trim_setup(&s, &c, 4);
    lt_trim_start(s);
    /* the wideband moves from cylinder to cylinder; the main VE is 6% lean and AutoTune
     * already knows it (ve_factor 1.06): the trims see only the per-cylinder part */
    for (cyl = 0; cyl < 4; cyl++) { trim_feed(s, &c, cyl, 1.06, 1.06, &t); }
    lt_trim_stop(s);
    for (cyl = 0; cyl < 4; cyl++) {
        n = lt_trim_recommendations(s, cyl, &c, r, TN * TN);
        LT_CHECK(lt_trim_samples(s, cyl) > 500);
        if (g_cyl_err[cyl] == 1.0) {
            LT_CHECK(n == TN * TN);
            LT_CHECK_NEAR(rec_at(r, n, 2, 1), 0.0, 1e-9);
        } else {
            LT_CHECK(n == TN * TN);
            LT_CHECK_NEAR(rec_at(r, n, 0, 0), (g_cyl_err[cyl] - 1.0) * 100.0, 0.05);
            LT_CHECK_NEAR(rec_at(r, n, 3, 3), (g_cyl_err[cyl] - 1.0) * 100.0, 0.05);
        }
    }
    /* collecting again on cylinder 2 keeps the others */
    lt_trim_reset_cyl(s, 1);
    LT_CHECK(lt_trim_samples(s, 1) == 0 && lt_trim_samples(s, 0) > 0);
    lt_trim_free(s);
}

LT_TEST(trim_reference_sensor_and_balance)
{
    lt_trim_state *s;
    lt_trim_config c;
    lt_at_rec r[TN * TN];
    size_t cyl, n, x, y, k;
    unsigned long t = 1000;
    double sum, mean;
    trim_setup(&s, &c, 4);
    lt_trim_start(s);
    /* per-cylinder sensors plus a collector sensor: the VE error cancels out */
    for (y = 0; y < TN; y++) {
        for (x = 0; x < TN; x++) {
            for (k = 0; k < 60; k++) {
                for (cyl = 0; cyl < 4; cyl++) {
                    lt_ve_point p;
                    lt_ve_point_default(&p);
                    p.timestamp_ms = t; p.rpm = g_rpm[x]; p.load = g_load[y]; p.clt = 90.0;
                    p.afr = 13.5 * 1.1 * g_cyl_err[cyl];
                    lt_trim_add(s, cyl, &p, 0.0, 1.0, 13.5 * 1.1 * 1.0075, &c);
                }
                t += 100UL;
            }
        }
    }
    n = lt_trim_recommendations(s, 0, &c, r, TN * TN);
    LT_CHECK_NEAR(rec_at(r, n, 1, 1), (1.04 / 1.0075 - 1.0) * 100.0, 0.05);
    /* balancing: the average multiplier of the four cylinders stays at 1.0 */
    c.balance = LT_TRUE;
    sum = 0.0;
    for (cyl = 0; cyl < 4; cyl++) {
        n = lt_trim_recommendations(s, cyl, &c, r, TN * TN);
        sum += rec_at(r, n, 1, 1);
    }
    mean = sum / 4.0;
    LT_CHECK_NEAR(mean, 0.0, 1e-6);
    lt_trim_free(s);
}

LT_TEST(trim_filters_and_locks)
{
    lt_trim_state *s;
    lt_trim_config c;
    lt_at_rec r[TN * TN];
    lt_at_reject rj[8];
    size_t n, k, i;
    unsigned long t = 1000, rail = 0;
    trim_setup(&s, &c, 2);
    lt_at_lock_cell(lt_trim_engine(s, 0), 0, 0);
    lt_trim_start(s);
    for (k = 0; k < 200; k++) {
        lt_ve_point p;
        lt_ve_point_default(&p);
        p.timestamp_ms = t; p.rpm = 1000.0; p.load = 30.0; p.clt = 90.0;
        p.afr = k % 2 ? 9.0 : 15.0;     /* every other sample at the sensor rail */
        if (k % 4 == 1) { p.tps_rate = 500.0; p.afr = 15.0; }
        lt_trim_add(s, 0, &p, 13.5, 1.0, 0.0, &c);
        t += 100UL;
    }
    n = lt_trim_recommendations(s, 0, &c, r, TN * TN);
    LT_CHECK(n == 0);                    /* the only visited cell is locked */
    n = lt_trim_rejections(s, 0, rj, 8);
    for (i = 0; i < n; i++) { if (strcmp(rj[i].reason, "afr at sensor rail") == 0) { rail = rj[i].count; } }
    LT_CHECK(rail == 50);
    lt_trim_free(s);
}

LT_TEST(trim_replay)
{
    static const char *const names[5] = { "RPMValue", "MAPValue", "lambdaValue", "coolant", "lambdaValue2" };
    lt_log log;
    lt_trim_state *s;
    lt_trim_config c;
    lt_at_channels ch;
    lt_at_rec r[TN * TN];
    size_t x, y, k, n;
    double sec = 0.0;
    long acc;
    static const double vef[TN * TN] = { 1.05, 1.05, 1.05, 1.05, 1.05, 1.05, 1.05, 1.05,
                                         1.05, 1.05, 1.05, 1.05, 1.05, 1.05, 1.05, 1.05 };
    lt_log_init(&log);
    lt_log_set_channels(&log, names, NULL, 5);
    for (y = 0; y < TN; y++) {
        for (x = 0; x < TN; x++) {
            for (k = 0; k < 60; k++) {
                double v[5];
                v[0] = g_rpm[x]; v[1] = g_load[y]; v[2] = 1.05 * 0.97 / 1.0; v[3] = 90.0; v[4] = 1.05;
                lt_log_append(&log, sec, v);
                sec += 0.1;
            }
        }
    }
    trim_setup(&s, &c, 3);
    memset(&ch, 0, sizeof(ch));
    lt_strlcpy(ch.rpm, "RPMValue", sizeof(ch.rpm));
    lt_strlcpy(ch.load, "MAPValue", sizeof(ch.load));
    lt_strlcpy(ch.afr, "lambdaValue", sizeof(ch.afr));
    lt_strlcpy(ch.clt, "coolant", sizeof(ch.clt));
    ch.afr_is_lambda = LT_TRUE;
    /* lambda target 1.0 everywhere, AutoTune VE correction pending: +5% */
    {
        double tz[TN * TN];
        for (k = 0; k < TN * TN; k++) { tz[k] = 1.0; }
        lt_trim_set_target_table(s, g_rpm, TN, g_load, TN, tz);
        lt_trim_set_ve_factor_table(s, g_rpm, TN, g_load, TN, vef);
    }
    acc = lt_trim_replay(s, 1, &log, &ch, NULL, &c);
    LT_CHECK(acc > 800);
    n = lt_trim_recommendations(s, 1, &c, r, TN * TN);
    LT_CHECK(n == TN * TN);
    LT_CHECK_NEAR(rec_at(r, n, 2, 2), -3.0, 0.05);
    LT_CHECK(lt_trim_samples(s, 0) == 0);
    /* the same log against the reference channel */
    acc = lt_trim_replay(s, 2, &log, &ch, "lambdaValue2", &c);
    n = lt_trim_recommendations(s, 2, &c, r, TN * TN);
    LT_CHECK_NEAR(rec_at(r, n, 0, 3), -3.0, 0.05);
    lt_log_free(&log);
    lt_trim_free(s);
}

LT_TEST(trim_units)
{
    lt_ini_const k;
    lt_trim_units u;
    memset(&k, 0, sizeof(k));
    k.min = -25.0; k.max = 25.0;
    lt_trim_units_detect(&k, &u);
    LT_CHECK_NEAR(lt_trim_to_mult(&u, 5.0), 1.05, 1e-12);
    LT_CHECK_NEAR(lt_trim_from_mult(&u, 0.97), -3.0, 1e-9);
    k.min = 50.0; k.max = 150.0;
    lt_trim_units_detect(&k, &u);
    LT_CHECK_NEAR(lt_trim_to_mult(&u, 105.0), 1.05, 1e-12);
    k.min = 0.5; k.max = 1.5;
    lt_trim_units_detect(&k, &u);
    LT_CHECK_NEAR(lt_trim_to_mult(&u, 1.05), 1.05, 1e-12);
}

LT_TEST(lookups)
{
    static const double x[3] = { 0.0, 10.0, 20.0 }, y[3] = { 1.0, 2.0, 4.0 };
    static const double tx[2] = { 0.0, 10.0 }, ty[2] = { 0.0, 10.0 }, tz[4] = { 0.0, 10.0, 20.0, 30.0 };
    LT_CHECK_NEAR(lt_at_curve_lookup(x, y, 3, -5.0), 1.0, 1e-12);
    LT_CHECK_NEAR(lt_at_curve_lookup(x, y, 3, 15.0), 3.0, 1e-12);
    LT_CHECK_NEAR(lt_at_curve_lookup(x, y, 3, 50.0), 4.0, 1e-12);
    LT_CHECK_NEAR(lt_at_table_lookup(tx, 2, ty, 2, tz, 5.0, 5.0), 15.0, 1e-12);
    LT_CHECK_NEAR(lt_at_table_lookup(tx, 2, ty, 2, tz, 10.0, 0.0), 10.0, 1e-12);
}

int main(void)
{
    LT_RUN(wue_converges_cold_bins_only);
    LT_RUN(wue_filters_reject_transients_and_cranking);
    LT_RUN(wue_warm_end_pinned_and_locks);
    LT_RUN(wue_target_offset_and_lambda_table);
    LT_RUN(wue_replay_matches_live);
    LT_RUN(wue_units);
    LT_RUN(trim_recovers_cylinder_error);
    LT_RUN(trim_reference_sensor_and_balance);
    LT_RUN(trim_filters_and_locks);
    LT_RUN(trim_replay);
    LT_RUN(trim_units);
    LT_RUN(lookups);
    LT_TEST_MAIN_END
}
