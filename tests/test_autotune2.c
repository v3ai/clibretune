/* test_autotune2.c - AutoTune preflight, replay, delay measurement, accel enrichment,
 * anomaly / health / prediction tests (spec 07 sections 7-11). */
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "lt_test.h"
#include "autotune/autotune.h"

/* ================================================================ preflight */

static const double T_OK[4] = {14.7, 14.7, 12.8, 12.8};
static const double V_OK[4] = {45, 52, 94, 98};

typedef struct pf {
    lt_preflight_input in;
    lt_at_settings s;
    lt_at_filters f;
    lt_at_authority a;
} pf;

static void pf_init(pf *p)
{
    memset(&p->in, 0, sizeof p->in);
    p->in.target_table = "afrTable1Tbl";
    p->in.target_values = T_OK;
    p->in.t_rows = p->in.t_cols = 2;
    p->in.ve_rows = p->in.ve_cols = 2;
    p->in.ve_values = V_OK;
    p->in.celsius = LT_TRUE;
    lt_at_settings_default(&p->s);
    p->s.lambda_delay_ms = 550;
    lt_at_filters_default(&p->f);
    p->f.min_clt = 60;
    lt_at_authority_default(&p->a);
}

static size_t pf_run(pf *p, lt_finding *out)
{
    return lt_preflight_check(&p->in, &p->s, &p->f, &p->a, out, 16);
}

static const lt_finding *find(const lt_finding *f, size_t n, const char *code)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (strcmp(f[i].code, code) == 0) {
            return &f[i];
        }
    }
    return NULL;
}

LT_TEST(preflight_clean)
{
    pf p;
    lt_finding f[16];
    size_t n;
    pf_init(&p);
    n = pf_run(&p, f);
    LT_CHECK_INT(n, 1);   /* only the write-mode info */
    LT_CHECK(!lt_preflight_has_blocker(f, n));
    LT_CHECK_STR(f[0].code, "collect_only");
    LT_CHECK_INT(f[0].sev, LT_SEV_INFO);
    p.in.will_write_to_ecu = LT_TRUE;
    n = pf_run(&p, f);
    LT_CHECK_STR(f[n - 1].code, "writes_live");
}

LT_TEST(preflight_target_table)
{
    pf p;
    lt_finding f[16];
    size_t n;
    const lt_finding *x;
    static const double tbad[4] = {50, 60, 70, 80};
    static const double tlam[4] = {0.8, 0.8, 1.0, 1.0};
    static const double tzero[4] = {0, 0, 0, 0};
    pf_init(&p);
    p.in.target_table = NULL;
    n = pf_run(&p, f);
    LT_CHECK(lt_preflight_has_blocker(f, n));
    x = find(f, n, "target_table_missing");
    LT_CHECK(x != NULL);
    LT_CHECK(x != NULL && strstr(x->detail, "14.7") != NULL);
    LT_CHECK(x != NULL && strcmp(x->title, "No AFR target table") == 0);
    LT_CHECK(x != NULL && strcmp(x->current, "flat 14.7") == 0);
    LT_CHECK(x != NULL && strcmp(x->suggested, "pick a target table") == 0);
    pf_init(&p);
    p.in.t_rows = 3;
    p.in.t_cols = 2;
    n = pf_run(&p, f);
    x = find(f, n, "target_table_shape");
    LT_CHECK(x != NULL && x->sev == LT_SEV_BLOCKER);
    LT_CHECK(x != NULL && strstr(x->detail, "'afrTable1Tbl' is 3x2 but the VE table is 2x2") != NULL);
    LT_CHECK(x != NULL && strcmp(x->current, "3x2") == 0 && strcmp(x->suggested, "2x2") == 0);
    pf_init(&p);
    p.in.target_values = tbad;
    n = pf_run(&p, f);
    x = find(f, n, "target_table_range");
    LT_CHECK(x != NULL && x->sev == LT_SEV_WARNING);
    LT_CHECK(x != NULL && strcmp(x->current, "50.00..80.00") == 0);
    LT_CHECK(!lt_preflight_has_blocker(f, n));
    pf_init(&p);
    p.in.target_values = tlam;
    n = pf_run(&p, f);
    LT_CHECK(find(f, n, "target_table_range") == NULL);
    pf_init(&p);
    p.in.target_values = tzero;
    n = pf_run(&p, f);
    x = find(f, n, "target_table_empty");
    LT_CHECK(x != NULL && x->sev == LT_SEV_BLOCKER);
}

LT_TEST(preflight_ve_table)
{
    pf p;
    lt_finding f[16];
    size_t n;
    static const double vz[4] = {0, 0, 0, 0};
    static const double vbig[4] = {45, 52, 94, 300};
    const lt_finding *x;
    pf_init(&p);
    p.in.ve_values = vz;
    n = pf_run(&p, f);
    LT_CHECK(find(f, n, "ve_table_zero") != NULL);
    LT_CHECK(lt_preflight_has_blocker(f, n));
    pf_init(&p);
    p.in.ve_values = vbig;
    n = pf_run(&p, f);
    x = find(f, n, "ve_table_scale");
    LT_CHECK(x != NULL && x->sev == LT_SEV_WARNING);
    LT_CHECK(x != NULL && strcmp(x->current, "max 300") == 0);
    pf_init(&p);
    p.in.ve_values = NULL;
    n = pf_run(&p, f);
    LT_CHECK(find(f, n, "ve_table_unreadable") != NULL);
}

LT_TEST(preflight_filters)
{
    pf p;
    lt_finding f[16];
    size_t n;
    const lt_finding *x;
    pf_init(&p);
    p.f.min_clt = 160;   /* untouched default on a Celsius project */
    n = pf_run(&p, f);
    x = find(f, n, "min_clt_units");
    LT_CHECK(x != NULL && x->sev == LT_SEV_BLOCKER);
    LT_CHECK(x != NULL && strcmp(x->current, "160 C") == 0 && strcmp(x->suggested, "60 C") == 0);
    pf_init(&p);
    p.in.celsius = LT_FALSE;
    p.f.min_clt = 71;   /* Celsius value on a Fahrenheit project */
    n = pf_run(&p, f);
    x = find(f, n, "min_clt_units");
    LT_CHECK(x != NULL && x->sev == LT_SEV_WARNING);
    LT_CHECK(x != NULL && strcmp(x->suggested, "140 F") == 0);
    LT_CHECK(!lt_preflight_has_blocker(f, n));
    pf_init(&p);
    p.f.min_clt = 71;
    n = pf_run(&p, f);
    LT_CHECK(find(f, n, "min_clt_units") == NULL);
    pf_init(&p);
    p.f.min_clt = 30;   /* too low, not boiling: warning */
    n = pf_run(&p, f);
    x = find(f, n, "min_clt_units");
    LT_CHECK(x != NULL && x->sev == LT_SEV_WARNING);
    pf_init(&p);
    p.f.min_rpm = 5000;
    p.f.max_rpm = 4000;
    n = pf_run(&p, f);
    x = find(f, n, "rpm_window_empty");
    LT_CHECK(x != NULL && x->sev == LT_SEV_BLOCKER);
    LT_CHECK(x != NULL && strcmp(x->current, "5000..4000") == 0);
    pf_init(&p);
    p.f.max_tps_rate = 0;
    n = pf_run(&p, f);
    x = find(f, n, "tps_rate_inert");
    LT_CHECK(x != NULL && x->sev == LT_SEV_WARNING);
    LT_CHECK(x != NULL && strcmp(x->current, "0.0") == 0 && strcmp(x->suggested, "10 %/s") == 0);
}

LT_TEST(preflight_authority_delay)
{
    pf p;
    lt_finding f[16];
    size_t n;
    const lt_finding *x;
    pf_init(&p);
    p.a.max_cell_value_change = 0;
    n = pf_run(&p, f);
    x = find(f, n, "authority_zero");
    LT_CHECK(x != NULL && x->sev == LT_SEV_BLOCKER);
    LT_CHECK(x != NULL && strcmp(x->current, "0.0 / 20.0%") == 0);
    pf_init(&p);
    p.a.min_cell_value = 200;
    p.a.max_cell_value = 100;
    n = pf_run(&p, f);
    x = find(f, n, "rails_reversed");
    LT_CHECK(x != NULL && x->sev == LT_SEV_WARNING);
    LT_CHECK(x != NULL && strcmp(x->current, "200..100") == 0);
    pf_init(&p);
    p.s.lambda_delay_ms = 0;
    n = pf_run(&p, f);
    x = find(f, n, "delay_default_curve");
    LT_CHECK(x != NULL && x->sev == LT_SEV_WARNING);
    LT_CHECK(x != NULL && strcmp(x->current, "200-50 ms (assumed)") == 0);
    p.s.lambda_delay_flow_scaled = LT_TRUE;
    n = pf_run(&p, f);
    LT_CHECK(find(f, n, "delay_default_curve") == NULL);
    p.s.lambda_delay_flow_scaled = LT_FALSE;
    p.s.lambda_delay_ms = 550;
    n = pf_run(&p, f);
    LT_CHECK(find(f, n, "delay_default_curve") == NULL);
}

LT_TEST(preflight_sorted)
{
    pf p;
    lt_finding f[16];
    size_t n, i;
    pf_init(&p);
    p.s.lambda_delay_ms = 0;              /* warning */
    p.in.target_table = NULL;             /* blocker */
    p.f.max_tps_rate = 0;                 /* warning */
    p.a.max_cell_percentage_change = 0;   /* blocker */
    n = pf_run(&p, f);
    LT_CHECK(n >= 5);
    for (i = 1; i < n; i++) {
        LT_CHECK((int)f[i - 1].sev <= (int)f[i].sev);
    }
    LT_CHECK_INT(f[0].sev, LT_SEV_BLOCKER);
    LT_CHECK_INT(f[n - 1].sev, LT_SEV_INFO);
    /* stable within a severity: target (checked first) before authority */
    LT_CHECK_STR(f[0].code, "target_table_missing");
    LT_CHECK_STR(f[1].code, "authority_zero");
    LT_CHECK_INT(lt_preflight_check(&p.in, &p.s, &p.f, &p.a, NULL, 0), n);
    LT_CHECK_INT(lt_preflight_check(&p.in, &p.s, &p.f, &p.a, f, 2), 2);
}

/* ================================================================ delay measurement */

static void mk(lt_afr_sample *a, size_t i, unsigned long t, double afr)
{
    a[i].t_ms = t;
    a[i].afr = afr;
}

LT_TEST(delay_texts)
{
    LT_CHECK_STR(lt_delay_rejection_text(LT_DR_INSUFFICIENT_BASELINE), "too few baseline samples");
    LT_CHECK_STR(lt_delay_rejection_text(LT_DR_UNSTABLE_BASELINE), "baseline unstable - hold steadier");
    LT_CHECK_STR(lt_delay_rejection_text(LT_DR_NO_RESPONSE), "no AFR response detected");
    LT_CHECK_STR(lt_delay_rejection_text(LT_DR_RESPONSE_PRECEDES_STEP), "AFR already moving at the step");
    LT_CHECK_STR(lt_delay_rejection_text(LT_DR_RESPONSE_NOT_SETTLED),
                 "still moving at end of hold - use a longer hold");
}

LT_TEST(delay_clean_step)
{
    lt_afr_sample pre[5], post[16];
    lt_delay_measurement m;
    lt_delay_rejection why;
    static const double pa[5] = {14.7, 14.68, 14.72, 14.7, 14.69};
    static const double qa[15] = {14.70, 14.70, 14.70, 14.60, 14.40, 14.15, 13.95, 13.80, 13.72,
                                  13.70, 13.70, 13.70, 13.70};
    unsigned long i;
    for (i = 0; i < 5; i++) {
        mk(pre, i, i * 40, pa[i]);
    }
    for (i = 0; i < 9; i++) {
        mk(post, i, 200 + i * 40, qa[i]);
    }
    for (i = 9; i < 13; i++) {
        mk(post, i, 200 + i * 40, 13.70);
    }
    LT_CHECK_INT(lt_detect_delay(200, pre, 5, post, 13, &m, &why), 0);
    LT_CHECK_NEAR(m.delay_ms, 192, 1.0);
    LT_CHECK_NEAR(m.excursion, 1.0, 0.05);
    LT_CHECK_NEAR(m.baseline_afr, 14.7, 0.01);
    LT_CHECK(m.leading_edge_ms < m.delay_ms);
}

LT_TEST(delay_interpolation_subsample)
{
    lt_afr_sample pre[4], post[16];
    lt_delay_measurement m;
    unsigned long i, n = 0;
    for (i = 0; i < 4; i++) {
        mk(pre, i, i * 60, 14.70);
    }
    mk(post, n++, 240, 14.70);
    mk(post, n++, 300, 14.10);
    mk(post, n++, 360, 13.85);
    mk(post, n++, 420, 13.72);
    for (i = 0; i < 8; i++) {
        mk(post, n++, 480 + i * 60, 13.70);
    }
    LT_CHECK_INT(lt_detect_delay(240, pre, 4, post, n, &m, NULL), 0);
    LT_CHECK(m.delay_ms > 0 && m.delay_ms < 59);
}

LT_TEST(delay_rejections)
{
    lt_afr_sample pre[8], post[24];
    lt_delay_measurement m;
    lt_delay_rejection why = LT_DR_NO_RESPONSE;
    unsigned long i, n;
    static const double wander[5] = {13.2, 15.4, 12.9, 15.8, 13.5};
    for (i = 0; i < 4; i++) {
        mk(pre, i, i * 40, 14.7);
    }
    /* crossing at / before the anchor */
    mk(post, 0, 240, 14.20);
    mk(post, 1, 300, 14.10);
    mk(post, 2, 360, 14.05);
    LT_CHECK(lt_detect_delay(240, pre, 4, post, 3, &m, &why) != 0);
    LT_CHECK_INT(why, LT_DR_RESPONSE_PRECEDES_STEP);
    /* single spike */
    mk(post, 0, 200, 14.7);
    mk(post, 1, 240, 13.9);
    mk(post, 2, 280, 14.7);
    mk(post, 3, 320, 14.71);
    mk(post, 4, 360, 14.69);
    LT_CHECK(lt_detect_delay(200, pre, 4, post, 5, &m, &why) != 0);
    LT_CHECK_INT(why, LT_DR_NO_RESPONSE);
    /* wandering baseline */
    for (i = 0; i < 5; i++) {
        mk(pre, i, i * 40, wander[i]);
    }
    LT_CHECK(lt_detect_delay(200, pre, 5, post, 5, &m, &why) != 0);
    LT_CHECK_INT(why, LT_DR_UNSTABLE_BASELINE);
    /* too few baseline samples */
    LT_CHECK(lt_detect_delay(200, pre, 2, post, 5, &m, &why) != 0);
    LT_CHECK_INT(why, LT_DR_INSUFFICIENT_BASELINE);
    LT_CHECK(lt_err_get() != NULL);
    /* flat trace */
    for (i = 0; i < 4; i++) {
        mk(pre, i, i * 40, 14.7);
    }
    for (i = 0; i < 12; i++) {
        mk(post, i, 200 + i * 40, 14.7);
    }
    LT_CHECK(lt_detect_delay(200, pre, 4, post, 12, &m, &why) != 0);
    LT_CHECK_INT(why, LT_DR_NO_RESPONSE);
    /* still moving at the end of the hold */
    n = 0;
    mk(post, n++, 200, 14.70);
    mk(post, n++, 240, 14.60);
    mk(post, n++, 280, 14.35);
    mk(post, n++, 320, 14.05);
    mk(post, n++, 360, 13.75);
    mk(post, n++, 400, 13.45);
    mk(post, n++, 440, 13.15);
    LT_CHECK(lt_detect_delay(200, pre, 4, post, n, &m, &why) != 0);
    LT_CHECK_INT(why, LT_DR_RESPONSE_NOT_SETTLED);
    /* NULL out/why allowed */
    LT_CHECK(lt_detect_delay(200, pre, 4, post, n, NULL, NULL) != 0);
}

LT_TEST(delay_pre_step_drift)
{
    lt_afr_sample pre[4], post[16];
    lt_delay_measurement m;
    unsigned long i, n = 0;
    for (i = 0; i < 4; i++) {
        mk(pre, i, i * 60, 14.70);
    }
    mk(post, n++, 240, 14.70);
    mk(post, n++, 300, 14.58);
    mk(post, n++, 360, 14.58);
    mk(post, n++, 420, 14.70);
    mk(post, n++, 480, 14.70);
    mk(post, n++, 540, 14.50);
    for (i = 0; i < 6; i++) {
        mk(post, n++, 600 + i * 60, 14.475);
    }
    LT_CHECK_INT(lt_detect_delay(240, pre, 4, post, n, &m, NULL), 0);
    LT_CHECK_NEAR(m.delay_ms, 274, 1.0);
}

LT_TEST(delay_sustained_after_spike)
{
    lt_afr_sample pre[4], post[16];
    lt_delay_measurement m;
    unsigned long i, n = 0;
    for (i = 0; i < 4; i++) {
        mk(pre, i, i * 40, 14.7);
    }
    mk(post, n++, 200, 14.7);
    mk(post, n++, 240, 13.9);
    mk(post, n++, 280, 14.7);
    mk(post, n++, 320, 14.3);
    mk(post, n++, 360, 14.0);
    mk(post, n++, 400, 13.85);
    mk(post, n++, 440, 13.75);
    for (i = 0; i < 4; i++) {
        mk(post, n++, 480 + i * 40, 13.70);
    }
    LT_CHECK_INT(lt_detect_delay(200, pre, 4, post, n, &m, NULL), 0);
    /* the leading edge is anchored on the real run (280..320), not the spike at 240 */
    LT_CHECK(m.leading_edge_ms + 200 >= 280 && m.leading_edge_ms + 200 <= 320);
}

LT_TEST(delay_half_after_leading)
{
    lt_afr_sample pre[4], post[16];
    lt_delay_measurement m;
    static const double a[13] = {14.70, 14.60, 14.45, 14.25, 14.05, 13.90, 13.78, 13.72, 13.70,
                                 13.70, 13.70, 13.70, 13.70};
    unsigned long i;
    for (i = 0; i < 4; i++) {
        mk(pre, i, i * 40, 14.70);
    }
    for (i = 0; i < 13; i++) {
        mk(post, i, 200 + i * 40, a[i]);
    }
    LT_CHECK_INT(lt_detect_delay(200, pre, 4, post, 13, &m, NULL), 0);
    LT_CHECK(m.delay_ms > m.leading_edge_ms);
}

LT_TEST(delay_table)
{
    lt_delay_table t;
    lt_delay_table_init(&t);
    lt_delay_table_add(&t, 900, 35, 400);
    lt_delay_table_add(&t, 950, 38, 360);
    LT_CHECK_INT(t.cells[0][0].n, 2);
    LT_CHECK_NEAR(t.cells[0][0].mean_ms, 380, 1e-9);
    LT_CHECK_NEAR(t.cells[0][0].range_ms, 40, 1e-9);
    LT_CHECK_NEAR(t.cells[0][0].min_ms, 360, 1e-9);
    LT_CHECK_NEAR(t.cells[0][0].max_ms, 400, 1e-9);
    lt_delay_table_add(&t, 3200, 85, 140);   /* rpm bin 3 (3000..4500), load bin 3 (80..100) */
    LT_CHECK_INT(t.cells[3][3].n, 1);
    LT_CHECK_NEAR(t.cells[3][3].mean_ms, 140, 1e-9);
    lt_delay_table_add(&t, 7000, 105, 90);
    LT_CHECK_INT(t.cells[4][5].n, 1);
    LT_CHECK_NEAR(t.cells[4][5].mean_ms, 90, 1e-9);
    lt_delay_table_add(&t, 1200, 40, 100);   /* edges belong to the higher bin */
    LT_CHECK_INT(t.cells[1][1].n, 1);
}

LT_TEST(flow_fit)
{
    static const double rpm[6] = {900, 1500, 2500, 3500, 5000, 6000};
    static const double load[6] = {35, 40, 50, 70, 90, 96};
    double ms[6];
    lt_flow_fit fit;
    size_t i;
    double r2[4] = {900, 1500, 2500, 5000}, l2[4] = {35, 40, 50, 90};
    double m2[4] = {1100, 700, 450, 260};
    for (i = 0; i < 6; i++) {
        ms[i] = 150.0 + 3e7 / (rpm[i] * load[i]) * 1.0;
    }
    /* generated with k = 3e7 would be huge at idle; scale it so values stay plausible */
    for (i = 0; i < 6; i++) {
        ms[i] = 150.0 + 3e7 / (rpm[i] * load[i]);
    }
    LT_CHECK(lt_fit_flow_delay(rpm, load, ms, 6, &fit));
    LT_CHECK(fabs(fit.floor_ms - 150) <= 5);
    LT_CHECK(fit.rms_ms < 5);
    LT_CHECK_INT(fit.samples, 6);
    LT_CHECK_NEAR(fit.anchor_ms, fit.floor_ms + fit.k / (800.0 * 40.0), 1e-9);
    LT_CHECK(fit.anchor_ms > fit.floor_ms);
    LT_CHECK(lt_fit_flow_delay(r2, l2, m2, 4, &fit));
    LT_CHECK(fit.k > 0);
    LT_CHECK(fit.floor_ms + fit.k / (r2[0] * l2[0]) > fit.floor_ms + fit.k / (r2[3] * l2[3]));
    LT_CHECK(!lt_fit_flow_delay(r2, l2, m2, 3, &fit));
    {
        double rr[6] = {900, 0, 2500, -5, 5000, 3000}, ll[6] = {35, 40, 0, 70, 90, 50};
        double mm[6] = {1100, 700, 450, 300, 260, 0};
        LT_CHECK(!lt_fit_flow_delay(rr, ll, mm, 6, &fit));   /* only 3 usable */
        rr[1] = 1500; ll[2] = 50; mm[5] = 200;
        LT_CHECK(lt_fit_flow_delay(rr, ll, mm, 6, &fit));
        LT_CHECK_INT(fit.samples, 5);   /* index 3 (rpm<0) is dropped */
    }
}

/* ================================================================ accel enrichment */

static size_t ae_log(lt_ae_sample *s, double peak, double resp_afr, double clt)
{
    size_t n = 0;
    unsigned long t;
    for (t = 0; t <= 8000; t += 100) {
        s[n].timestamp_ms = t;
        s[n].tps_dot = (t == 3000) ? peak : 0.0;
        s[n].afr = 14.7 + ((t >= 3800 && t < 4460) ? resp_afr : 0.0);
        s[n].afr_target = 14.7;
        s[n].clt = clt;
        n++;
    }
    return n;
}

LT_TEST(ae_config_defaults)
{
    lt_ae_config c;
    lt_ae_config_default(&c);
    LT_CHECK_INT(c.nbins, 4);
    LT_CHECK_NEAR(c.tae_bins[3], 600, 0);
    LT_CHECK_NEAR(c.tae_rates[0], 10, 0);
    LT_CHECK_NEAR(c.ae_time_ms, 160, 0);
    LT_CHECK_NEAR(c.delay_ms, 800, 0);
    LT_CHECK_NEAR(c.event_floor, 30, 0);
    LT_CHECK_NEAR(c.min_clt, 70, 0);
    LT_CHECK_INT(c.min_events, 8);
    LT_CHECK_NEAR(c.authority_pct, 5, 0);
    LT_CHECK_NEAR(c.rate_gain_pct_per_afr, 100.0 / 14.7, 1e-12);
}

LT_TEST(ae_lean_and_rich)
{
    lt_ae_sample s[100];
    lt_ae_config c;
    lt_ae_event ev[4];
    lt_ae_bin_rec rec[8];
    size_t n, nr;
    lt_ae_config_default(&c);
    c.min_events = 1;
    n = ae_log(s, 100, 0.6, 85);
    LT_CHECK_INT(lt_ae_analyze(s, n, &c, ev, 4), 1);
    LT_CHECK(ev[0].residual_afr > 0.4);
    LT_CHECK_NEAR(ev[0].peak_tps_dot, 100, 0);
    nr = lt_ae_recommend(ev, 1, &c, rec, 8);
    LT_CHECK_INT(nr, 4);
    LT_CHECK(rec[0].sufficient_data);
    LT_CHECK(rec[0].recommended_rate > rec[0].current_rate);
    LT_CHECK_NEAR(rec[0].recommended_rate, 10 + 0.6 * 100.0 / 14.7, 1e-6);
    LT_CHECK_NEAR(rec[0].lo, 60, 0);
    LT_CHECK(rec[0].has_hi);
    LT_CHECK_NEAR(rec[0].hi, 160, 0);
    LT_CHECK(!rec[3].has_hi);
    LT_CHECK(strstr(rec[0].note, "richen (+4.1)") != NULL);
    LT_CHECK(strstr(rec[0].note, "+0.60 AFR lean") != NULL);
    /* other bins held */
    LT_CHECK(!rec[1].sufficient_data);
    LT_CHECK_NEAR(rec[1].recommended_rate, rec[1].current_rate, 0);
    LT_CHECK_STR(rec[1].note, "insufficient data (need 1) - held");
    LT_CHECK(!rec[1].has_median);

    n = ae_log(s, 100, -0.6, 85);
    LT_CHECK_INT(lt_ae_analyze(s, n, &c, ev, 4), 1);
    LT_CHECK(ev[0].residual_afr < -0.4);
    lt_ae_recommend(ev, 1, &c, rec, 8);
    LT_CHECK(rec[0].recommended_rate < rec[0].current_rate);
    LT_CHECK(strstr(rec[0].note, "lean (-4.1)") != NULL);
    LT_CHECK(strstr(rec[0].note, "-0.60 AFR rich") != NULL);
}

LT_TEST(ae_gating)
{
    lt_ae_sample s[100];
    lt_ae_config c;
    lt_ae_event ev[4];
    size_t n;
    lt_ae_config_default(&c);
    c.min_events = 1;
    n = ae_log(s, 100, 0.6, 40);   /* cold engine: event ignored */
    LT_CHECK_INT(lt_ae_analyze(s, n, &c, ev, 4), 0);
    n = ae_log(s, 40, 0.6, 85);    /* below the first bin */
    LT_CHECK_INT(lt_ae_analyze(s, n, &c, ev, 4), 0);
    n = ae_log(s, 20, 0.6, 85);    /* below the event floor */
    LT_CHECK_INT(lt_ae_analyze(s, n, &c, ev, 4), 0);
    c.delay_ms = 0;                /* wrong delay: no excursion seen */
    n = ae_log(s, 100, 0.6, 85);
    {
        size_t k = lt_ae_analyze(s, n, &c, ev, 4);
        LT_CHECK(k == 0 || fabs(ev[0].residual_afr) < 0.3);
    }
    LT_CHECK_INT(lt_ae_analyze(s, 1, &c, ev, 4), 0);
}

LT_TEST(ae_insufficient_and_sufficient)
{
    lt_ae_config c;
    lt_ae_event ev[12];
    lt_ae_bin_rec rec[8];
    size_t i;
    lt_ae_config_default(&c);
    for (i = 0; i < 12; i++) {
        ev[i].peak_tps_dot = 100 + (double)i;   /* bin 0 */
        ev[i].residual_afr = 0.03;
    }
    lt_ae_recommend(ev, 12, &c, rec, 8);
    LT_CHECK(rec[0].sufficient_data);
    LT_CHECK_INT(rec[0].events, 12);
    LT_CHECK(rec[0].has_median);
    LT_CHECK_NEAR(rec[0].median_residual_afr, 0.03, 1e-12);
    LT_CHECK(strstr(rec[0].note, "on target") != NULL);
    LT_CHECK_NEAR(rec[0].recommended_rate, 10.0 + 0.03 * 100.0 / 14.7, 1e-9);
    lt_ae_recommend(ev, 7, &c, rec, 8);
    LT_CHECK(!rec[0].sufficient_data);
    LT_CHECK_NEAR(rec[0].recommended_rate, rec[0].current_rate, 0);
    LT_CHECK_STR(rec[0].note, "insufficient data (need 8) - held");
    LT_CHECK(rec[0].has_median);
    /* authority clamp */
    for (i = 0; i < 12; i++) {
        ev[i].residual_afr = 3.0;
    }
    lt_ae_recommend(ev, 12, &c, rec, 8);
    LT_CHECK_NEAR(rec[0].recommended_rate, 15.0, 1e-9);
    for (i = 0; i < 12; i++) {
        ev[i].residual_afr = -30.0;
        ev[i].peak_tps_dot = 700;   /* bin 3 */
    }
    lt_ae_recommend(ev, 12, &c, rec, 8);
    LT_CHECK_NEAR(rec[3].recommended_rate, 15.0, 1e-9);
    LT_CHECK_INT(rec[3].events, 12);
    LT_CHECK_INT(rec[0].events, 0);
}

/* ================================================================ replay */

#define NCH 9
static const char *CH_NAMES[NCH] = {"RPM", "MAP", "AFR", "VE", "CLT", "TPS", "TPSdot", "fuelcut", "accel"};

static void log_build(lt_log *l, long n, double afr_override_clt)
{
    long i;
    int c;
    memset(l, 0, sizeof *l);
    l->n_ch = NCH;
    l->names = (char **)lt_calloc(NCH, sizeof(char *));
    l->units = (char **)lt_calloc(NCH, sizeof(char *));
    for (c = 0; c < NCH; c++) {
        l->names[c] = lt_strdup(CH_NAMES[c]);
        l->units[c] = lt_strdup("");
    }
    l->n = n;
    l->t = (double *)lt_calloc((size_t)n + 1, sizeof(double));
    l->v = (double *)lt_calloc((size_t)n * NCH + 1, sizeof(double));
    for (i = 0; i < n; i++) {
        l->t[i] = (double)i * 0.1;
        LT_LOG_AT(l, i, 0) = 3000;
        LT_LOG_AT(l, i, 1) = 50;
        LT_LOG_AT(l, i, 2) = 14.7;
        LT_LOG_AT(l, i, 3) = 45;
        LT_LOG_AT(l, i, 4) = (i < 50 && afr_override_clt >= 0) ? afr_override_clt : 90;
        LT_LOG_AT(l, i, 5) = 20;
        LT_LOG_AT(l, i, 6) = 0;
        LT_LOG_AT(l, i, 7) = 0;
        LT_LOG_AT(l, i, 8) = 0;
    }
}

static void log_free(lt_log *l)
{
    int c;
    for (c = 0; c < l->n_ch; c++) {
        lt_free(l->names[c]);
        lt_free(l->units[c]);
    }
    lt_free(l->names);
    lt_free(l->units);
    lt_free(l->t);
    lt_free(l->v);
}

static void chans(lt_at_channels *c)
{
    memset(c, 0, sizeof *c);
    lt_strlcpy(c->rpm, "RPM", sizeof c->rpm);
    lt_strlcpy(c->load, "MAP", sizeof c->load);
    lt_strlcpy(c->afr, "AFR", sizeof c->afr);
    lt_strlcpy(c->ve, "VE", sizeof c->ve);
    lt_strlcpy(c->clt, "CLT", sizeof c->clt);
    lt_strlcpy(c->tps, "TPS", sizeof c->tps);
    lt_strlcpy(c->tps_rate, "TPSdot", sizeof c->tps_rate);
    lt_strlcpy(c->fuel_cut, "fuelcut", sizeof c->fuel_cut);
    lt_strlcpy(c->accel_enrich, "accel", sizeof c->accel_enrich);
}

static const double RX[3] = {2000, 3000, 4000};
static const double RY[3] = {30, 50, 70};

static void rcfg(lt_replay_config *c)
{
    lt_replay_config_default(c);
    c->filters.min_clt = 71;
    c->lambda_match_strict = LT_FALSE;
}

static int do_replay(const lt_log *l, const lt_replay_config *c, lt_replay_report *r)
{
    double ve[9], tg[9];
    lt_at_reftables ref;
    lt_at_channels ch;
    int i;
    for (i = 0; i < 9; i++) {
        ve[i] = 50;
        tg[i] = 14.0;
    }
    memset(&ref, 0, sizeof ref);
    ref.nx = 3; ref.ny = 3; ref.ve = ve; ref.target_afr = tg;
    chans(&ch);
    return lt_replay(l, &ch, RX, 3, RY, 3, &ref, c, r);
}

LT_TEST(replay_basic)
{
    lt_log l;
    lt_replay_config c;
    lt_replay_report r;
    log_build(&l, 200, -1);
    rcfg(&c);
    LT_CHECK_INT(do_replay(&l, &c, &r), 0);
    LT_CHECK_INT(r.ncells, 1);
    LT_CHECK_INT(r.cells[0].x, 1);
    LT_CHECK_INT(r.cells[0].y, 1);
    LT_CHECK_NEAR(r.cells[0].current_ve, 50, 1e-12);
    LT_CHECK(r.cells[0].delta > 0);
    LT_CHECK_NEAR(r.cells[0].proposed_ve, 52.5, 0.1);
    LT_CHECK(r.cells[0].hits > 0);
    LT_CHECK_NEAR(r.cells[0].rpm, 3000, 0);
    LT_CHECK_NEAR(r.cells[0].load, 50, 0);
    LT_CHECK_NEAR(r.cells[0].confidence, 1.0, 1e-12);
    LT_CHECK_NEAR(r.cells[0].target_afr, 14.0, 1e-12);
    LT_CHECK_NEAR(r.cells[0].mean_afr, 14.7, 1e-9);
    LT_CHECK_INT(r.n_verdicts, 200);
    LT_CHECK(r.samples_all > 100);
    LT_CHECK(r.has_validation);
    LT_CHECK_NEAR(r.validation.gain_pct, 100.0, 1e-6);
    LT_CHECK_NEAR(r.validation.worsened_pct, 0.0, 1e-9);
    LT_CHECK(r.validation.scored > 0);
    LT_CHECK_INT(r.validation.folds, 5);
    lt_replay_report_free(&r);
    log_free(&l);
}

LT_TEST(replay_cold_rejections)
{
    lt_log l;
    lt_replay_config c;
    lt_replay_report r;
    size_t i;
    int found = 0;
    log_build(&l, 50, 20);
    rcfg(&c);
    c.validate = LT_FALSE;
    LT_CHECK_INT(do_replay(&l, &c, &r), 0);
    LT_CHECK_INT(r.n_verdicts, 50);
    LT_CHECK_INT(r.samples_all, 0);
    for (i = 0; i < r.n_rejections; i++) {
        if (strcmp(r.rejections[i].reason, "clt below min_clt") == 0 && r.rejections[i].count > 0) {
            found = 1;
        }
    }
    LT_CHECK(found);
    LT_CHECK_STR(r.verdicts[0], "clt below min_clt");
    LT_CHECK(!r.has_validation);
    lt_replay_report_free(&r);
    log_free(&l);
}

LT_TEST(replay_coverage_and_validation_gating)
{
    lt_log l;
    lt_replay_config c;
    lt_replay_report r;
    log_build(&l, 120, -1);
    rcfg(&c);
    LT_CHECK_INT(do_replay(&l, &c, &r), 0);
    LT_CHECK(r.coverage[1 * 3 + 1] > 0);
    LT_CHECK_INT(r.coverage[0], 0);
    LT_CHECK_INT(r.nx, 3);
    LT_CHECK_INT(r.ny, 3);
    LT_CHECK(r.verdicts[0] == NULL);
    lt_replay_report_free(&r);
    log_free(&l);
    log_build(&l, 3, -1);
    LT_CHECK_INT(do_replay(&l, &c, &r), 0);
    LT_CHECK(!r.has_validation);
    lt_replay_report_free(&r);
    log_free(&l);
}

LT_TEST(replay_all_zero_times_is_fast_and_unsteady)
{
    lt_log l;
    lt_replay_config c;
    lt_replay_report r;
    long i;
    log_build(&l, 20000, -1);
    for (i = 0; i < l.n; i++) {
        l.t[i] = 0.0;
    }
    rcfg(&c);
    LT_CHECK_INT(do_replay(&l, &c, &r), 0);
    LT_CHECK(!r.has_validation);   /* steady mask all false -> nothing scored */
    LT_CHECK_INT(r.n_verdicts, 20000);
    lt_replay_report_free(&r);
    log_free(&l);
}

LT_TEST(replay_errors_and_lambda)
{
    lt_log l;
    lt_replay_config c;
    lt_replay_report r;
    lt_at_channels ch;
    long i;
    log_build(&l, 100, -1);
    rcfg(&c);
    c.validate = LT_FALSE;
    chans(&ch);
    lt_strlcpy(ch.rpm, "NOPE", sizeof ch.rpm);
    LT_CHECK_INT(lt_replay(&l, &ch, RX, 3, RY, 3, NULL, &c, &r), LT_ENOENT);
    LT_CHECK(strstr(lt_err_get(), "rpm") != NULL);
    chans(&ch);
    ch.afr_is_lambda = LT_TRUE;
    for (i = 0; i < l.n; i++) {
        LT_LOG_AT(&l, i, 2) = 1.0;   /* lambda 1.0 == 14.7 */
    }
    LT_CHECK_INT(lt_replay(&l, &ch, RX, 3, RY, 3, NULL, &c, &r), 0);
    LT_CHECK(r.ncells >= 1);
    LT_CHECK_NEAR(r.cells[0].mean_afr, 14.7, 1e-9);
    lt_replay_report_free(&r);
    log_free(&l);
    /* empty log */
    memset(&l, 0, sizeof l);
    l.n_ch = 0;
    chans(&ch);
    LT_CHECK_INT(lt_replay(&l, &ch, RX, 3, RY, 3, NULL, &c, &r), LT_ENOENT);
}

/* ================================================================ anomaly */

LT_TEST(anomaly_smooth_and_outlier)
{
    static const double x[4] = {1000, 2000, 3000, 4000}, y[4] = {20, 40, 60, 80};
    double z[16] = {48, 50, 52, 54, 50, 52, 54, 56, 52, 54, 56, 58, 54, 56, 58, 60};
    lt_anomaly_config c;
    lt_anomaly out[32];
    size_t n, i;
    int found = 0;
    lt_anomaly_config_default(&c);
    c.outlier_sigma = 3.0;
    n = lt_anomaly_detect(z, 4, 4, x, y, &c, out, 32);
    for (i = 0; i < n; i++) {
        LT_CHECK(out[i].type == LT_AN_FLAT);   /* only flat-ish findings (none expected) */
    }
    z[1 * 4 + 1] = 120;
    n = lt_anomaly_detect(z, 4, 4, x, y, &c, out, 32);
    LT_CHECK(n >= 1);
    for (i = 0; i < n; i++) {
        if (out[i].row == 1 && out[i].col == 1) {
            found = 1;
        }
        if (i > 0) {
            LT_CHECK(out[i - 1].severity >= out[i].severity);
        }
    }
    LT_CHECK(found);
    /* one per cell */
    for (i = 0; i < n; i++) {
        size_t j;
        for (j = i + 1; j < n; j++) {
            LT_CHECK(!(out[i].row == out[j].row && out[i].col == out[j].col));
        }
    }
    LT_CHECK_INT(lt_anomaly_detect(z, 0, 0, x, y, &c, out, 32), 0);
}

LT_TEST(anomaly_ramp_no_outliers)
{
    static const double x[6] = {500, 1500, 2500, 3500, 4500, 5500};
    static const double y[6] = {20, 35, 50, 65, 80, 95};
    double z[36];
    lt_anomaly_config c;
    lt_anomaly out[64];
    size_t r, cc, n, i;
    for (r = 0; r < 6; r++) {
        for (cc = 0; cc < 6; cc++) {
            z[r * 6 + cc] = 30 + (double)r * 4 + (double)cc * 3;
        }
    }
    lt_anomaly_config_default(&c);
    n = lt_anomaly_detect(z, 6, 6, x, y, &c, out, 64);
    for (i = 0; i < n; i++) {
        LT_CHECK(out[i].type != LT_AN_OUTLIER);
        LT_CHECK(out[i].type != LT_AN_GRADIENT);
    }
}

LT_TEST(anomaly_monotonicity_unreasonable_flat)
{
    static const double x[4] = {1000, 2000, 3000, 4000}, y[4] = {20, 40, 60, 80};
    double z[16] = {40, 45, 50, 55, 50, 55, 60, 65, 60, 65, 70, 75, 70, 75, 80, 85};
    lt_anomaly_config c;
    lt_anomaly out[32];
    size_t n, i;
    int mono = 0, low = 0, high = 0;
    lt_anomaly_config_default(&c);
    z[3 * 4 + 2] = 40;   /* VE collapses at higher load (prev 70 -> 40; row 3) */
    n = lt_anomaly_detect(z, 4, 4, x, y, &c, out, 32);
    for (i = 0; i < n; i++) {
        if (out[i].type == LT_AN_MONOTONICITY && out[i].row == 3 && out[i].col == 2) {
            mono = 1;
            LT_CHECK_NEAR(out[i].expected, 70, 1e-12);
            LT_CHECK(strstr(out[i].description, "VE dropped 43% (70.0") != NULL);
        }
    }
    LT_CHECK(mono);
    /* load floor ignores low-load transitions */
    {
        double z2[16];
        memcpy(z2, z, sizeof z2);
        z2[3 * 4 + 2] = 80;
        z2[1 * 4 + 0] = 20;   /* prev (row 0, load 20) = 40 -> fine; row1 itself = 20 (drop from 40) */
        n = lt_anomaly_detect(z2, 4, 4, x, y, &c, out, 32);
        for (i = 0; i < n; i++) {
            LT_CHECK(!(out[i].type == LT_AN_MONOTONICITY && out[i].col == 0 && out[i].row == 1));
        }
    }
    z[0] = 3;
    z[15] = 190;
    n = lt_anomaly_detect(z, 4, 4, x, y, &c, out, 32);
    for (i = 0; i < n; i++) {
        if (out[i].row == 0 && out[i].col == 0 && out[i].type == LT_AN_UNREASONABLE) {
            low = 1;
            LT_CHECK_NEAR(out[i].severity, 0.8, 1e-12);
        }
        if (out[i].row == 3 && out[i].col == 3 && out[i].type == LT_AN_UNREASONABLE) {
            high = 1;
            LT_CHECK_NEAR(out[i].severity, 0.9, 1e-12);
        }
    }
    LT_CHECK(low && high);
    {
        double zf[16];
        int k, flat = 0;
        for (k = 0; k < 16; k++) {
            zf[k] = 50 + (k % 4) * 7 + (k / 4) * 9;
        }
        zf[0] = zf[1] = zf[4] = zf[5] = 60;
        n = lt_anomaly_detect(zf, 4, 4, x, y, &c, out, 32);
        for (i = 0; i < n; i++) {
            if (out[i].type == LT_AN_FLAT && out[i].row <= 1 && out[i].col <= 1) {
                flat++;
                LT_CHECK_NEAR(out[i].severity, 0.25, 1e-12);
            }
        }
        LT_CHECK(flat >= 1);   /* cells may be claimed by a stronger anomaly */
        c.min_flat_region_size = 5;
        n = lt_anomaly_detect(zf, 4, 4, x, y, &c, out, 32);
        for (i = 0; i < n; i++) {
            LT_CHECK(out[i].type != LT_AN_FLAT);
        }
    }
}

/* ================================================================ health */

LT_TEST(health_empty_and_grades)
{
    lt_health_report h;
    double xb[1] = {1000}, yb[1] = {50};
    lt_health_score(NULL, NULL, 0, 0, xb, yb, &h);
    LT_CHECK_INT(h.overall_score, 0);
    LT_CHECK(h.grade == 'F');
    LT_CHECK_INT(h.n_recommendations, 1);
    LT_CHECK_STR(h.recommendations[0], "No table data available");
}

static void ramp_table(double *z, unsigned *hits, double *xb, double *yb, size_t n, unsigned hv)
{
    size_t r, c;
    for (c = 0; c < n; c++) {
        xb[c] = 500.0 + (double)c * 500.0;
    }
    for (r = 0; r < n; r++) {
        yb[r] = 20.0 + (double)r * 10.0;
    }
    for (r = 0; r < n; r++) {
        for (c = 0; c < n; c++) {
            z[r * n + c] = 30.0 + (double)r * 1.0 + (double)c * 1.0;
            hits[r * n + c] = hv;
        }
    }
}

LT_TEST(health_perfect_and_nocoverage)
{
    double z[100], xb[10], yb[10];
    unsigned hits[100];
    lt_health_report h;
    size_t i;
    ramp_table(z, hits, xb, yb, 10, 10);
    lt_health_score(z, hits, 10, 10, xb, yb, &h);
    LT_CHECK(h.overall_score >= 90);
    LT_CHECK(h.grade == 'A');
    LT_CHECK_INT(h.cells_all, 100);
    LT_CHECK_INT(h.coverage_cells, 100);
    LT_CHECK_NEAR(h.coverage_percent, 100, 1e-12);
    LT_CHECK(h.n_regions >= 3);
    LT_CHECK_STR(h.regions[0].name, "Idle");
    LT_CHECK_INT(h.regions[0].row0, 0);
    LT_CHECK_INT(h.regions[0].col0, 0);
    LT_CHECK_INT(h.n_recommendations, 0);
    for (i = 0; i < h.n_regions; i++) {
        LT_CHECK_INT(h.regions[i].smoothness_score, 100);
        LT_CHECK_INT(h.regions[i].monotonicity_score, 100);
    }
    for (i = 0; i < 100; i++) {
        hits[i] = 0;
    }
    {
        lt_health_report h2;
        lt_health_score(z, hits, 10, 10, xb, yb, &h2);
        LT_CHECK(h2.overall_score < h.overall_score);
        LT_CHECK_INT(h2.coverage_cells, 0);
        LT_CHECK(strstr(h2.recommendations[0], "Low data coverage (0%)") != NULL);
        LT_CHECK(h2.n_recommendations >= 2);
        LT_CHECK(strstr(h2.recommendations[1], "very low data coverage (0)") != NULL);
    }
}

LT_TEST(health_wavy_and_wot)
{
    double z[100], xb[10], yb[10];
    unsigned hits[100];
    lt_health_report h;
    size_t r, c, i;
    ramp_table(z, hits, xb, yb, 10, 10);
    for (r = 0; r < 10; r++) {
        for (c = 0; c < 10; c++) {
            z[r * 10 + c] = ((r + c) % 2) ? 20.0 : 90.0;
        }
    }
    lt_health_score(z, hits, 10, 10, xb, yb, &h);
    LT_CHECK(h.overall_score < 70);
    LT_CHECK(h.grade != 'A');
    {
        int rough = 0;
        for (i = 0; i < h.n_recommendations; i++) {
            if (strstr(h.recommendations[i], "rough VE transitions") != NULL) {
                rough = 1;
            }
        }
        LT_CHECK(rough);
    }
    /* boosted: load axis 20..250 kPa; WOT begins at the first row >= 90 kPa absolute */
    for (r = 0; r < 10; r++) {
        yb[r] = 20.0 + (double)r * 25.5;
    }
    ramp_table(z, hits, xb, yb, 10, 5);
    for (r = 0; r < 10; r++) {
        yb[r] = 20.0 + (double)r * 25.5;
    }
    lt_health_score(z, hits, 10, 10, xb, yb, &h);
    for (i = 0; i < h.n_regions; i++) {
        if (strcmp(h.regions[i].name, "WOT") == 0) {
            LT_CHECK_INT(h.regions[i].row0, 3);   /* 96.5 kPa is the first >= 90 */
            LT_CHECK_INT(h.regions[i].row1, 9);
            LT_CHECK_INT(h.regions[i].col0, 0);
            LT_CHECK_INT(h.regions[i].col1, 9);
        }
    }
}

LT_TEST(health_region_boundaries)
{
    double z[100], xb[10], yb[10];
    unsigned hits[100];
    lt_health_report h;
    size_t i;
    unsigned char seen[100];
    ramp_table(z, hits, xb, yb, 10, 10);
    /* x = 500..5000: first >= 1500 is index 2 -> idle cols 0..1; first >= 4500 is index 8 */
    lt_health_score(z, hits, 10, 10, xb, yb, &h);
    LT_CHECK_INT(h.regions[0].col1, 1);
    memset(seen, 0, sizeof seen);
    for (i = 0; i < h.n_regions; i++) {
        size_t r, c;
        for (r = h.regions[i].row0; r <= h.regions[i].row1; r++) {
            for (c = h.regions[i].col0; c <= h.regions[i].col1; c++) {
                seen[r * 10 + c]++;
            }
        }
    }
    for (i = 0; i < 100; i++) {
        LT_CHECK(seen[i] >= 1);   /* every cell is covered */
    }
    {   /* the Health panel lists them by name: no two regions share one */
        size_t k;
        for (i = 0; i < h.n_regions; i++) {
            for (k = i + 1; k < h.n_regions; k++) { LT_CHECK(strcmp(h.regions[i].name, h.regions[k].name) != 0); }
        }
    }
    /* tiny table falls back / stays valid */
    {
        double z2[4] = {40, 41, 42, 43}, xb2[2] = {1000, 2000}, yb2[2] = {30, 60};
        unsigned h2[4] = {1, 1, 1, 1};
        lt_health_report r2;
        lt_health_score(z2, h2, 2, 2, xb2, yb2, &r2);
        LT_CHECK(r2.n_regions >= 1);
        LT_CHECK(r2.overall_score <= 100);
    }
}

/* ================================================================ predictor */

LT_TEST(predict_basic)
{
    double z[25] = { 0 }, xb[5] = {1000, 2000, 3000, 4000, 5000}, yb[5] = {20, 40, 60, 80, 100};
    unsigned hits[25] = { 0 };
    lt_predict_config c;
    lt_predicted_cell out[25];
    size_t r, cc, n, i;
    int center = 0;
    lt_predict_config_default(&c);
    LT_CHECK_INT(lt_predict_cells(z, hits, 0, 0, xb, yb, &c, out, 25), 0);
    for (r = 0; r < 5; r++) {
        for (cc = 0; cc < 5; cc++) {
            z[r * 5 + cc] = 40 + (double)r * 5 + (double)cc * 5;
            hits[r * 5 + cc] = 10;
        }
    }
    LT_CHECK_INT(lt_predict_cells(z, hits, 5, 5, xb, yb, &c, out, 25), 0);   /* all known */
    hits[2 * 5 + 2] = 0;
    n = lt_predict_cells(z, hits, 5, 5, xb, yb, &c, out, 25);
    LT_CHECK_INT(n, 1);
    LT_CHECK_INT(out[0].row, 2);
    LT_CHECK_INT(out[0].col, 2);
    LT_CHECK_INT(out[0].method, LT_PM_BILINEAR);
    LT_CHECK_NEAR(out[0].predicted, 60, 1.0);
    LT_CHECK_NEAR(out[0].current, 60, 1e-12);
    LT_CHECK(out[0].confidence >= 0.7);
    LT_CHECK_INT(out[0].neighbors, 4);
    /* nothing known */
    for (i = 0; i < 25; i++) {
        hits[i] = 0;
    }
    LT_CHECK_INT(lt_predict_cells(z, hits, 5, 5, xb, yb, &c, out, 25), 0);
    LT_CHECK_INT(lt_predict_cells(z, NULL, 5, 5, xb, yb, &c, out, 25), 0);
    /* a patch of known cells, rest predicted, sorted by confidence */
    for (r = 0; r < 3; r++) {
        for (cc = 0; cc < 3; cc++) {
            hits[r * 5 + cc] = 10;
        }
    }
    n = lt_predict_cells(z, hits, 5, 5, xb, yb, &c, out, 25);
    LT_CHECK(n > 0);
    for (i = 1; i < n; i++) {
        LT_CHECK(out[i - 1].confidence >= out[i].confidence);
    }
    for (i = 0; i < n; i++) {
        LT_CHECK(out[i].predicted >= 1.0 && out[i].predicted <= 200.0);
        LT_CHECK(out[i].confidence >= c.min_confidence);
        if (out[i].row == 2 && out[i].col == 3) {
            center = 1;
        }
    }
    LT_CHECK(center);
}

LT_TEST(predict_1d_and_extrapolation)
{
    /* one known row; cells along it and beyond are predicted by 1-D rules */
    double z[15], xb[5] = {1000, 2000, 3000, 4000, 5000}, yb[3] = {20, 40, 60};
    unsigned hits[15];
    lt_predict_config c;
    lt_predicted_cell out[15];
    size_t i, n;
    int tagged = 0;
    lt_predict_config_default(&c);
    memset(hits, 0, sizeof hits);
    for (i = 0; i < 15; i++) {
        z[i] = 50;
    }
    z[5 + 0] = 50; z[5 + 1] = 55; z[5 + 3] = 65; z[5 + 4] = 70;
    hits[5 + 0] = hits[5 + 1] = hits[5 + 3] = hits[5 + 4] = 10;
    n = lt_predict_cells(z, hits, 3, 5, xb, yb, &c, out, 15);
    for (i = 0; i < n; i++) {
        if (out[i].row == 1 && out[i].col == 2) {
            tagged = 1;
            LT_CHECK_NEAR(out[i].predicted, 60, 6.0);
            LT_CHECK(out[i].confidence >= 0.3);
        }
    }
    LT_CHECK(tagged);
    /* extrapolation never produces near-zero fuel */
    memset(hits, 0, sizeof hits);
    for (i = 0; i < 15; i++) {
        z[i] = 10 - (double)(i % 5) * 4;   /* falling steeply */
    }
    hits[0] = hits[1] = hits[5] = hits[6] = 10;
    hits[10] = hits[11] = 10;
    z[0] = 20; z[1] = 10; z[5] = 20; z[6] = 10; z[10] = 20; z[11] = 10;
    n = lt_predict_cells(z, hits, 3, 5, xb, yb, &c, out, 15);
    for (i = 0; i < n; i++) {
        LT_CHECK(out[i].predicted >= 1.0);
    }
}

LT_TEST(predict_physics_disabled_by_default)
{
    double z[9] = {50, 50, 50, 50, 50, 50, 50, 50, 50}, xb[3] = {1000, 3000, 6000}, yb[3] = {30, 60, 100};
    unsigned hits[9] = {10, 0, 0, 0, 0, 0, 0, 0, 0};
    lt_predict_config c;
    lt_predicted_cell out[9];
    size_t n, i;
    lt_predict_config_default(&c);
    c.max_search_radius = 1;
    n = lt_predict_cells(z, hits, 3, 3, xb, yb, &c, out, 9);
    for (i = 0; i < n; i++) {
        LT_CHECK(out[i].method != LT_PM_PHYSICS);
    }
    c.min_confidence = 0.1;
    c.max_search_radius = 1;
    n = lt_predict_cells(z, hits, 3, 3, xb, yb, &c, out, 9);
    LT_CHECK(n == 8);
    {
        int phys = 0;
        for (i = 0; i < n; i++) {
            if (out[i].method == LT_PM_PHYSICS) {
                phys++;
                LT_CHECK_NEAR(out[i].confidence, 0.15, 1e-12);
            }
        }
        LT_CHECK(phys > 0);
    }
}

/* ================================================================ fuzz */

LT_TEST(custom_filter_fuzz)
{
    static const char *seeds[] = {
        "rpm > 2000 && tps < 50 && clt > 70", "(rpm + 500) / 1000 == 3",
        "!(tps > 30) || load % 7 == 0", "afr < 14.0 || accel_enrich",
        "((((rpm))))", "1/0", "load % 0", "-- - rpm", "tps_rate >= 1.5e3"
    };
    static const char alpha[] = "()+-*/%<>=!&| .0123456789abcxyzrpm_";
    lt_at_state *s = lt_at_new();
    lt_at_filters f;
    lt_ve_point p;
    char buf[160];
    unsigned long seed = 12345UL;
    size_t i, k, n;
    int j;
    lt_at_filters_default(&f);
    lt_ve_point_default(&p);
    p.rpm = 2500; p.tps = 25; p.clt = 85; p.load = 40; p.afr = 14.7;
    for (i = 0; i < 3000; i++) {
        const char *sd = seeds[i % (sizeof(seeds) / sizeof(seeds[0]))];
        const char *r = NULL;
        n = strlen(sd);
        memcpy(buf, sd, n + 1);
        if (i % 3 == 0) {
            n = (size_t)((seed = seed * 1103515245UL + 12345UL) >> 8) % (n + 1);
            buf[n] = '\0';
        }
        for (j = 0; j < 4 && n > 0; j++) {
            seed = seed * 1103515245UL + 12345UL;
            k = (size_t)(seed >> 8) % n;
            seed = seed * 1103515245UL + 12345UL;
            buf[k] = alpha[(seed >> 8) % (sizeof(alpha) - 1)];
        }
        lt_at_filters_set_custom(&f, buf);
        (void)lt_at_classify(s, &p, &f, &r);
    }
    /* very deep nesting and a very long expression must not overflow the stack or heap */
    {
        char *big = (char *)malloc(200001);
        size_t m;
        for (m = 0; m < 100000; m++) { big[m] = '('; }
        big[100000] = '1';
        for (m = 0; m < 100000; m++) { big[100001 + m] = ')'; }
        big[200001 - 1 + 0] = '\0';
        lt_at_filters_set_custom(&f, big);
        { const char *r = NULL; (void)lt_at_classify(s, &p, &f, &r); }
        free(big);
    }
    lt_at_filters_clear(&f);
    lt_at_free(s);
    LT_CHECK(1);
}

int main(void)
{
    LT_RUN(preflight_clean);
    LT_RUN(preflight_target_table);
    LT_RUN(preflight_ve_table);
    LT_RUN(preflight_filters);
    LT_RUN(preflight_authority_delay);
    LT_RUN(preflight_sorted);
    LT_RUN(delay_texts);
    LT_RUN(delay_clean_step);
    LT_RUN(delay_interpolation_subsample);
    LT_RUN(delay_rejections);
    LT_RUN(delay_pre_step_drift);
    LT_RUN(delay_sustained_after_spike);
    LT_RUN(delay_half_after_leading);
    LT_RUN(delay_table);
    LT_RUN(flow_fit);
    LT_RUN(ae_config_defaults);
    LT_RUN(ae_lean_and_rich);
    LT_RUN(ae_gating);
    LT_RUN(ae_insufficient_and_sufficient);
    LT_RUN(replay_basic);
    LT_RUN(replay_cold_rejections);
    LT_RUN(replay_coverage_and_validation_gating);
    LT_RUN(replay_all_zero_times_is_fast_and_unsteady);
    LT_RUN(replay_errors_and_lambda);
    LT_RUN(anomaly_smooth_and_outlier);
    LT_RUN(anomaly_ramp_no_outliers);
    LT_RUN(anomaly_monotonicity_unreasonable_flat);
    LT_RUN(health_empty_and_grades);
    LT_RUN(health_perfect_and_nocoverage);
    LT_RUN(health_wavy_and_wot);
    LT_RUN(health_region_boundaries);
    LT_RUN(predict_basic);
    LT_RUN(predict_1d_and_extrapolation);
    LT_RUN(predict_physics_disabled_by_default);
    LT_RUN(custom_filter_fuzz);
    LT_TEST_MAIN_END
}
