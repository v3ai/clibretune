/* test_autotune.c - AutoTune engine tests (spec 07 sections 1-6, 12). */
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include "lt.h"
#include "lt_test.h"
#include "autotune/autotune.h"

static const double X3[3] = {1000, 2000, 3000};
static const double Y3[3] = {10, 20, 30};

typedef struct fx {
    lt_at_state *s;
    lt_at_settings set;
    lt_at_filters f;
    lt_at_authority a;
    unsigned long ts;
    const double *xb, *yb;
    size_t nx, ny;
} fx;

static void fx_init(fx *x, const double *xb, size_t nx, const double *yb, size_t ny)
{
    x->s = lt_at_new();
    lt_at_settings_default(&x->set);
    x->set.base_weight = 0.0;
    x->set.min_change = 0.0;
    lt_at_filters_default(&x->f);
    lt_at_authority_default(&x->a);
    x->ts = 0;
    x->xb = xb;
    x->yb = yb;
    x->nx = nx;
    x->ny = ny;
    lt_at_start(x->s);
}

static void fx_free(fx *x)
{
    lt_at_filters_clear(&x->f);
    lt_at_free(x->s);
}

static void feed(fx *x, double rpm, double load, double afr, double ve)
{
    lt_ve_point p;
    lt_ve_point_default(&p);
    p.rpm = rpm;
    p.load = load;
    p.map = load;
    p.afr = afr;
    p.ve = ve;
    p.clt = 170;
    p.tps = 30;
    p.timestamp_ms = x->ts;
    x->ts += 200;
    lt_at_add_data_point(x->s, &p, x->xb, x->nx, x->yb, x->ny, &x->set, &x->f, &x->a);
}

static size_t recs_of(fx *x, lt_at_rec *out, size_t cap)
{
    size_t n = lt_at_recommendations(x->s, out, cap);
    return n < cap ? n : cap;
}

static const char *classify(const lt_at_state *s, const lt_ve_point *p, const lt_at_filters *f)
{
    const char *r = NULL;
    if (lt_at_classify(s, p, f, &r) == 0) {
        return NULL;
    }
    return r;
}

/* ---- constants / bins / weights ---- */

LT_TEST(normalise_and_defaults)
{
    lt_at_settings s;
    lt_at_authority a;
    lt_at_filters f;
    lt_at_settings_default(&s);
    lt_at_authority_default(&a);
    lt_at_filters_default(&f);
    LT_CHECK_NEAR(lt_at_normalise_to_afr(1.0), 14.7, 1e-12);
    LT_CHECK_NEAR(lt_at_normalise_to_afr(0.9), 13.23, 1e-12);
    LT_CHECK_NEAR(lt_at_normalise_to_afr(12.5), 12.5, 1e-12);
    LT_CHECK_NEAR(lt_at_normalise_to_afr(2.0), 2.0, 1e-12);
    LT_CHECK_NEAR(s.base_weight, 20.0, 0);
    LT_CHECK_NEAR(s.min_change, 1.0, 0);
    LT_CHECK_NEAR(s.target_afr, 14.7, 0);
    LT_CHECK_INT(s.update_rate_ms, 100);
    LT_CHECK_NEAR(s.lambda_delay_floor_ms, 120.0, 0);
    LT_CHECK_NEAR(a.max_cell_value_change, 10.0, 0);
    LT_CHECK_NEAR(a.max_cell_percentage_change, 20.0, 0);
    LT_CHECK_NEAR(a.min_cell_value, 0.0, 0);
    LT_CHECK_NEAR(a.max_cell_value, 255.0, 0);
    LT_CHECK_NEAR(f.max_tps_rate, 50.0, 0);
    LT_CHECK_NEAR(f.min_clt, 160.0, 0);
    LT_CHECK(f.exclude_accel_enrich);
}

LT_TEST(find_bin)
{
    static const double rpm[6] = {1000, 2000, 3000, 4000, 5000, 6000};
    LT_CHECK_INT(lt_at_find_bin(6400, rpm, 6), 5);
    LT_CHECK_INT(lt_at_find_bin(600, rpm, 6), 0);
    LT_CHECK_INT(lt_at_find_bin(3400, rpm, 6), 2);
    LT_CHECK_INT(lt_at_find_bin(9000, rpm, 6), -1);
    LT_CHECK_INT(lt_at_find_bin(100, rpm, 6), -1);
    LT_CHECK_INT(lt_at_find_bin(3000.05, rpm, 6), 2);
    LT_CHECK_INT(lt_at_find_bin(5, rpm, 0), -1);
    LT_CHECK_INT(lt_at_find_bin(5, rpm, 1), 0);
    LT_CHECK_INT(lt_at_find_bin(2500, rpm, 6), 1);   /* tie: first wins */
}

LT_TEST(hit_weights)
{
    static const double rpm[5] = {800, 1200, 2000, 4000, 6500};
    static const double load[4] = {30, 50, 70, 100};
    double w, sq, lin;
    LT_CHECK_NEAR(lt_at_hit_weight(LT_HW_UNIFORM, 1234, 41, 1, 0, rpm, 5, load, 4), 1.0, 0);
    LT_CHECK_NEAR(lt_at_hit_weight(LT_HW_CELL_PROXIMITY, 1200, 50, 1, 1, rpm, 5, load, 4), 1.0, 0);
    LT_CHECK_NEAR(lt_at_hit_weight(LT_HW_CELL_PROXIMITY, 1600, 50, 1, 1, rpm, 5, load, 4), 0.5,
                  1e-12);
    /* wide bin: a 200 offset on the wide span weighs far more than on the narrow one */
    w = lt_at_hit_weight(LT_HW_CELL_PROXIMITY, 4200, 50, 3, 1, rpm, 5, load, 4);
    LT_CHECK(w > 0.9);
    LT_CHECK_NEAR(lt_at_hit_weight(LT_HW_CELL_PROXIMITY, 600, 50, 0, 1, rpm, 5, load, 4), 1.0, 0);
    LT_CHECK_NEAR(lt_at_hit_weight(LT_HW_CELL_PROXIMITY, 7000, 50, 4, 1, rpm, 5, load, 4), 1.0, 0);
    lin = lt_at_hit_weight(LT_HW_CELL_PROXIMITY, 1600, 60, 1, 1, rpm, 5, load, 4);
    LT_CHECK_NEAR(lin, 0.25, 1e-12);   /* 0.5 * 0.5 */
    sq = lt_at_hit_weight(LT_HW_CELL_PROXIMITY_SQUARED, 1600, 50, 1, 1, rpm, 5, load, 4);
    LT_CHECK_NEAR(sq, 0.25, 1e-12);
    LT_CHECK(sq <= lt_at_hit_weight(LT_HW_CELL_PROXIMITY, 1600, 50, 1, 1, rpm, 5, load, 4));
    LT_CHECK_NEAR(lt_at_hit_weight(LT_HW_CELL_CENTRE_ONLY, 1600, 50, 1, 1, rpm, 5, load, 4), 0.0, 0);
    LT_CHECK_NEAR(lt_at_hit_weight(LT_HW_CELL_CENTRE_ONLY, 3400, 50, 2, 1, rpm, 5, load, 4), 0.0, 0);
    LT_CHECK_NEAR(lt_at_hit_weight(LT_HW_CELL_CENTRE_ONLY, 2200, 50, 2, 1, rpm, 5, load, 4), 1.0, 0);
    LT_CHECK_NEAR(lt_at_hit_weight(LT_HW_CELL_CENTRE_ONLY, 2000, 50, 2, 1, rpm, 5, load, 4), 1.0, 0);
    LT_CHECK_NEAR(lt_at_hit_weight(LT_HW_CELL_PROXIMITY_SQUARED, 2000, 50, 2, 1, rpm, 5, load, 4),
                  1.0, 0);
}

LT_TEST(authority_limits)
{
    lt_at_authority a;
    double r;
    lt_at_authority_default(&a);
    a.max_cell_value_change = 15;
    a.max_cell_percentage_change = 30;
    a.min_cell_value = 40;
    a.max_cell_value = 120;
    LT_CHECK_NEAR(lt_at_apply_authority_limits(100, 150, &a), 115, 1e-9);
    LT_CHECK_NEAR(lt_at_apply_authority_limits(100, 60, &a), 85, 1e-9);
    /* ceiling / floor */
    a.max_cell_value_change = 100;
    a.max_cell_percentage_change = 100;
    a.min_cell_value = 80;
    a.max_cell_value = 130;
    LT_CHECK_NEAR(lt_at_apply_authority_limits(100, 60, &a), 80, 1e-9);
    LT_CHECK_NEAR(lt_at_apply_authority_limits(125, 200, &a), 130, 1e-9);
    LT_CHECK_NEAR(lt_at_apply_authority_limits(130, 200, &a), 130, 1e-9);
    /* reversed rails do not crash */
    a.min_cell_value = 200;
    a.max_cell_value = 50;
    r = lt_at_apply_authority_limits(100, 120, &a);
    LT_CHECK(r >= 50 && r <= 200);
    /* negative limits */
    lt_at_authority_default(&a);
    a.max_cell_value_change = -10;
    a.max_cell_percentage_change = -20;
    LT_CHECK_NEAR(lt_at_apply_authority_limits(100, 150, &a), 110, 1e-9);
    LT_CHECK_NEAR(lt_at_apply_authority_limits(100, 50, &a), 90, 1e-9);
    /* NaN begin does not misbehave */
    lt_at_authority_default(&a);
    r = lt_at_apply_authority_limits(0.0 / 0.0 + 0.0, 100, &a);
    LT_CHECK(r == r || r != r);
    /* default rails */
    LT_CHECK_NEAR(lt_at_clamp_to_rails(&a, -5), 0, 0);
    LT_CHECK_NEAR(lt_at_clamp_to_rails(&a, 300), 255, 0);
    LT_CHECK_NEAR(lt_at_apply_authority_limits(250, 300, &a), 255, 1e-9);
}

LT_TEST(delay_curve)
{
    LT_CHECK_INT(lt_at_default_delay_ms(800), 200);
    LT_CHECK_INT(lt_at_default_delay_ms(6000), 50);
    LT_CHECK_INT(lt_at_default_delay_ms(1000), 194);
    LT_CHECK_INT(lt_at_default_delay_ms(100), 200);
    LT_CHECK_INT(lt_at_default_delay_ms(9000), 50);
    LT_CHECK_INT(lt_at_default_delay_ms(3400), 125);
}

LT_TEST(flow_scaled_table)
{
    static const double xb[3] = {800, 3000, 6000};
    static const double yb[3] = {40, 70, 100};
    double ve[9], t[9];
    size_t i, j;
    for (i = 0; i < 9; i++) {
        ve[i] = 50;
    }
    lt_at_flow_scaled_delay_table(ve, xb, 3, yb, 3, 1050, 120, t);
    LT_CHECK_NEAR(t[0], 1050, 1);
    LT_CHECK(t[8] >= 120 && t[8] < 400);
    for (j = 0; j < 3; j++) {
        for (i = 0; i < 3; i++) {
            LT_CHECK(t[j * 3 + i] >= 120 && t[j * 3 + i] <= 1050);
            if (i > 0) {
                LT_CHECK(t[j * 3 + i] < t[j * 3 + i - 1]);
            }
            if (j > 0) {
                LT_CHECK(t[j * 3 + i] < t[(j - 1) * 3 + i]);
            }
        }
    }
    lt_at_flow_scaled_delay_table(NULL, xb, 3, yb, 3, 1050, 120, t);
    LT_CHECK_NEAR(t[0], 1050, 1);
}

/* ---- delay buffer ---- */

LT_TEST(find_delayed_point)
{
    lt_at_state *s = lt_at_new();
    lt_ve_point p, out;
    int i;
    lt_ve_point_default(&p);
    p.rpm = 2000;
    lt_at_set_buffer_max_age_ms(s, 5000);
    for (i = 0; i < 20; i++) {
        p.timestamp_ms = (unsigned long)i * 150;
        lt_at_push_sample(s, &p);
    }
    LT_CHECK_INT(lt_at_mean_sample_gap_ms(s), 150);
    LT_CHECK(lt_at_find_delayed(s, 2850, 975, &out));
    LT_CHECK(out.timestamp_ms == 1800 || out.timestamp_ms == 1950);
    lt_at_free(s);

    s = lt_at_new();
    lt_at_set_buffer_max_age_ms(s, 5000);
    for (i = 0; i < 6; i++) {
        p.timestamp_ms = (unsigned long)i * 150;
        lt_at_push_sample(s, &p);
    }
    for (i = 0; i < 6; i++) {
        p.timestamp_ms = 2400UL + (unsigned long)i * 150;
        lt_at_push_sample(s, &p);
    }
    LT_CHECK(!lt_at_find_delayed(s, 2850, 1200, &out));
    LT_CHECK_INT(lt_at_buffer_count(s), 12);
    lt_at_free(s);
}

LT_TEST(buffer_sizing)
{
    fx x;
    unsigned long ts;
    lt_at_filters f;
    lt_at_settings set;
    static const double xb[2] = {1000, 2000}, yb[2] = {50, 100};
    lt_at_state *e;
    fx_init(&x, xb, 2, yb, 2);
    x.set.lambda_delay_ms = 900;
    LT_CHECK_INT(lt_at_required_buffer_ms(x.s, &x.set, &x.f), 1400);
    LT_CHECK_INT(lt_at_configured_or_curve_delay_ms(&x.set, 1500), 900);
    for (ts = 0; ts <= 1000; ts += 100) {
        lt_ve_point p;
        lt_ve_point_default(&p);
        p.rpm = 1500;
        p.load = 75;
        p.afr = 14;
        p.ve = 50;
        p.clt = 80;
        p.tps = 10;
        p.timestamp_ms = ts;
        lt_at_add_data_point(x.s, &p, xb, 2, yb, 2, &x.set, &x.f, &x.a);
    }
    LT_CHECK_INT(lt_at_buffer_oldest_ms(x.s), 0);
    LT_CHECK(lt_at_buffer_max_age_ms(x.s) >= 900);
    fx_free(&x);

    e = lt_at_new();
    lt_at_settings_default(&set);
    lt_at_filters_default(&f);
    LT_CHECK_INT(lt_at_required_buffer_ms(e, &set, &f), 500);
    f.min_steady_ms = 2000;
    LT_CHECK_INT(lt_at_required_buffer_ms(e, &set, &f), 2500);
    f.min_steady_ms = 0;
    {
        double dl[4];
        lt_at_reftables r;
        dl[0] = 100; dl[1] = 300; dl[2] = 0; dl[3] = 50;
        memset(&r, 0, sizeof r);
        r.nx = 2;
        r.ny = 2;
        r.lambda_delay = dl;
        lt_at_set_reference_tables(e, &r);
        LT_CHECK_INT(lt_at_required_buffer_ms(e, &set, &f), 800);
    }
    lt_at_free(e);
}

/* ---- accumulation (12.1) ---- */

LT_TEST(accumulate)
{
    fx x;
    lt_at_rec r[9];
    size_t n;
    fx_init(&x, X3, 3, Y3, 3);
    feed(&x, 1000, 10, 14.7, 50);
    feed(&x, 1000, 10, 13.0, 50);
    feed(&x, 1000, 10, 12.0, 50);
    n = recs_of(&x, r, 9);
    LT_CHECK_INT(n, 1);
    LT_CHECK_INT(r[0].cell_x, 0);
    LT_CHECK_INT(r[0].cell_y, 0);
    LT_CHECK(r[0].hit_count >= 1);
    LT_CHECK_NEAR(r[0].target_afr, 14.7, 1e-12);
    LT_CHECK(r[0].recommended_value < r[0].beginning_value);
    LT_CHECK_NEAR(r[0].beginning_value, 50, 1e-12);
    fx_free(&x);
}

LT_TEST(target_table_used)
{
    static const double tgt[9] = {12.5, 13, 13.5, 13, 13.5, 14, 14, 14.5, 15};
    lt_at_reftables ref;
    fx x;
    lt_at_rec r[9];
    size_t n;
    fx_init(&x, X3, 3, Y3, 3);
    memset(&ref, 0, sizeof ref);
    ref.nx = 3;
    ref.ny = 3;
    ref.target_afr = tgt;
    lt_at_set_reference_tables(x.s, &ref);
    feed(&x, 1000, 10, 12.5, 60);
    feed(&x, 1000, 10, 12.5, 60);
    n = recs_of(&x, r, 9);
    LT_CHECK_INT(n, 1);
    LT_CHECK_NEAR(r[0].target_afr, 12.5, 1e-12);
    LT_CHECK_NEAR(r[0].recommended_value, 60, 1e-6);
    LT_CHECK_NEAR(r[0].beginning_value, 60, 1e-12);
    fx_free(&x);
}

LT_TEST(target_table_lambda_normalised)
{
    double tgt[9];
    lt_at_reftables ref;
    fx x;
    lt_at_rec r[9];
    int i;
    for (i = 0; i < 9; i++) {
        tgt[i] = 0.9;
    }
    fx_init(&x, X3, 3, Y3, 3);
    memset(&ref, 0, sizeof ref);
    ref.nx = 3; ref.ny = 3; ref.target_afr = tgt;
    lt_at_set_reference_tables(x.s, &ref);
    feed(&x, 1000, 10, 13.23, 60);
    feed(&x, 1000, 10, 13.23, 60);
    LT_CHECK_INT(recs_of(&x, r, 9), 1);
    LT_CHECK_NEAR(r[0].target_afr, 13.23, 1e-9);
    LT_CHECK_NEAR(r[0].recommended_value, 60, 1e-6);
    fx_free(&x);
}

LT_TEST(strict_lambda_match)
{
    fx x;
    lt_ve_point p;
    lt_at_rec r[4];
    fx_init(&x, X3, 3, Y3, 3);
    x.set.base_weight = 20;
    x.set.min_change = 1;
    lt_ve_point_default(&p);
    p.rpm = 1000; p.load = 10; p.afr = 13; p.ve = 50; p.clt = 170; p.tps = 30;
    p.timestamp_ms = 10000;
    lt_at_set_strict_lambda_match(x.s, LT_TRUE);
    lt_at_add_data_point(x.s, &p, X3, 3, Y3, 3, &x.set, &x.f, &x.a);
    LT_CHECK_INT(lt_at_recommendations(x.s, r, 4), 0);
    LT_CHECK_INT(lt_at_total_samples(x.s), 1);   /* counted although dropped */
    lt_at_set_strict_lambda_match(x.s, LT_FALSE);
    p.timestamp_ms = 10200;
    lt_at_add_data_point(x.s, &p, X3, 3, Y3, 3, &x.set, &x.f, &x.a);
    LT_CHECK(lt_at_recommendations(x.s, r, 4) > 0);
    fx_free(&x);
}

LT_TEST(cma_alternating)
{
    static const double xb[2] = {1000, 2000}, yb[2] = {10, 20};
    fx x;
    lt_at_rec r[4];
    int i;
    fx_init(&x, xb, 2, yb, 2);
    for (i = 0; i < 12; i++) {
        feed(&x, 1000, 10, (i % 2) ? 16.0 : 15.0, 50);
    }
    LT_CHECK_INT(recs_of(&x, r, 4), 1);
    LT_CHECK(r[0].hit_count >= 4);
    LT_CHECK_NEAR(r[0].recommended_value, 50 * 15.5 / 14.7, 1.0);
    LT_CHECK(r[0].recommended_value > 50 * 15.0 / 14.7 + 0.5);
    LT_CHECK(r[0].recommended_value < 50 * 16.0 / 14.7 - 0.5);
    LT_CHECK(r[0].hit_percentage > 0 && r[0].hit_percentage <= 100);
    fx_free(&x);
}

LT_TEST(y_axis_filter)
{
    lt_at_state *s = lt_at_new();
    lt_at_filters f;
    lt_ve_point p;
    lt_at_filters_default(&f);
    lt_at_filters_set_y_range(&f, "15.0", "25.0");
    lt_ve_point_default(&p);
    p.rpm = 1500; p.clt = 170; p.afr = 14.7;
    p.load = 10;
    LT_CHECK_STR(classify(s, &p, &f), "load below min_y_axis");
    p.load = 30;
    LT_CHECK_STR(classify(s, &p, &f), "load above max_y_axis");
    p.load = 20;
    LT_CHECK(classify(s, &p, &f) == NULL);
    lt_at_filters_set_y_range(&f, " 15 ", "bogus");
    p.load = 30;
    LT_CHECK(classify(s, &p, &f) == NULL);   /* unparsable bound is ignored */
    p.load = 10;
    LT_CHECK_STR(classify(s, &p, &f), "load below min_y_axis");
    lt_at_filters_clear(&f);
    lt_at_free(s);
}

static double ramp_run(double base_weight, double min_change)
{
    fx x;
    lt_at_rec r[4];
    double v;
    fx_init(&x, X3, 3, Y3, 3);
    x.set.base_weight = base_weight;
    x.set.min_change = min_change;
    feed(&x, 1000, 10, 13.0, 50);
    feed(&x, 1000, 10, 12.5, 50);
    feed(&x, 1000, 10, 12.0, 50);
    recs_of(&x, r, 4);
    v = r[0].recommended_value;
    fx_free(&x);
    return v;
}

LT_TEST(ramp_holds_sparse_cell)
{
    double raw = ramp_run(0, 0), ramped = ramp_run(20, 0);
    LT_CHECK(raw < 50);
    LT_CHECK(fabs(ramped - 50) < fabs(raw - 50));
    LT_CHECK(ramped < 50);
}

LT_TEST(ramp_arithmetic)
{
    static const double ve[1] = {100}, tg[1] = {14.0};
    static const double xb[1] = {3000}, yb[1] = {50};
    lt_at_reftables ref;
    fx x;
    lt_at_rec r[2];
    int i;
    fx_init(&x, xb, 1, yb, 1);
    memset(&ref, 0, sizeof ref);
    ref.nx = ref.ny = 1;
    ref.ve = ve;
    ref.target_afr = tg;
    lt_at_set_reference_tables(x.s, &ref);
    lt_at_set_strict_lambda_match(x.s, LT_FALSE);
    x.set.base_weight = 20;
    x.a.max_cell_value_change = 50;
    for (i = 0; i < 5; i++) {
        feed(&x, 3000, 50, 16.8, 100);
    }
    LT_CHECK_INT(recs_of(&x, r, 2), 1);
    LT_CHECK_NEAR(r[0].hit_weighting, 5, 1e-12);
    LT_CHECK_NEAR(r[0].recommended_value, 105, 1e-9);
    for (i = 0; i < 20; i++) {
        feed(&x, 3000, 50, 16.8, 100);
    }
    recs_of(&x, r, 2);
    LT_CHECK_NEAR(r[0].recommended_value, 120, 1e-9);
    fx_free(&x);
    /* base_weight 0 -> confidence 1 immediately */
    fx_init(&x, xb, 1, yb, 1);
    lt_at_set_reference_tables(x.s, &ref);
    lt_at_set_strict_lambda_match(x.s, LT_FALSE);
    x.set.base_weight = 0;
    x.a.max_cell_value_change = 50;
    feed(&x, 3000, 50, 16.8, 100);
    recs_of(&x, r, 2);
    LT_CHECK_NEAR(r[0].recommended_value, 120, 1e-9);
    fx_free(&x);
}

static double min_change_run(double afr)
{
    static const double ve[1] = {100}, tg[1] = {14.0};
    static const double xb[1] = {3000}, yb[1] = {50};
    lt_at_reftables ref;
    fx x;
    lt_at_rec r[2];
    double v;
    fx_init(&x, xb, 1, yb, 1);
    memset(&ref, 0, sizeof ref);
    ref.nx = ref.ny = 1;
    ref.ve = ve;
    ref.target_afr = tg;
    lt_at_set_reference_tables(x.s, &ref);
    lt_at_set_strict_lambda_match(x.s, LT_FALSE);
    x.set.base_weight = 0;
    x.set.min_change = 1.0;
    feed(&x, 3000, 50, afr, 100);
    recs_of(&x, r, 2);
    v = r[0].recommended_value;
    fx_free(&x);
    return v;
}

LT_TEST(min_change)
{
    LT_CHECK_NEAR(min_change_run(14.0 * 1.004), 100, 1e-12);
    LT_CHECK_NEAR(min_change_run(14.0 * 0.997), 100, 1e-12);
    LT_CHECK_NEAR(min_change_run(14.0), 100, 1e-12);
    LT_CHECK_NEAR(min_change_run(14.0 * 1.02), 102, 1e-9);
}

LT_TEST(anchored_on_table_cell)
{
    static const double xb[3] = {2000, 3000, 4000}, yb[3] = {30, 50, 70};
    double ve[9], tg[9];
    lt_at_reftables ref;
    fx x;
    lt_at_rec r[9];
    int i;
    for (i = 0; i < 9; i++) {
        ve[i] = 50;
        tg[i] = 14.0;
    }
    memset(&ref, 0, sizeof ref);
    ref.nx = 3; ref.ny = 3; ref.ve = ve; ref.target_afr = tg;
    fx_init(&x, xb, 3, yb, 3);
    x.set.base_weight = 20;
    x.set.min_change = 1.0;
    x.f.min_clt = 71;
    lt_at_set_reference_tables(x.s, &ref);
    lt_at_set_strict_lambda_match(x.s, LT_FALSE);
    for (i = 0; i < 40; i++) {
        lt_ve_point p;
        lt_ve_point_default(&p);
        p.rpm = 3000; p.load = 50; p.ve = 45; p.afr = 14.7; p.clt = 90;
        p.fuel_cut_active = 0; p.accel_enrich_active = 0;
        p.timestamp_ms = (unsigned long)i * 100;
        lt_at_add_data_point(x.s, &p, xb, 3, yb, 3, &x.set, &x.f, &x.a);
    }
    LT_CHECK_INT(recs_of(&x, r, 9), 1);
    LT_CHECK_INT(r[0].cell_x, 1);
    LT_CHECK_INT(r[0].cell_y, 1);
    LT_CHECK_NEAR(r[0].beginning_value, 50, 1e-12);
    LT_CHECK_NEAR(r[0].recommended_value, 52.5, 0.1);
    lt_at_free(x.s);
    /* without a VE reference the beginning value is the sample's */
    x.s = lt_at_new();
    lt_at_start(x.s);
    lt_at_set_strict_lambda_match(x.s, LT_FALSE);
    for (i = 0; i < 40; i++) {
        lt_ve_point p;
        lt_ve_point_default(&p);
        p.rpm = 3000; p.load = 50; p.ve = 45; p.afr = 14.7; p.clt = 90;
        p.timestamp_ms = (unsigned long)i * 100;
        lt_at_add_data_point(x.s, &p, xb, 3, yb, 3, &x.set, &x.f, &x.a);
    }
    recs_of(&x, r, 9);
    LT_CHECK_NEAR(r[0].beginning_value, 45, 1e-12);
    fx_free(&x);
}

LT_TEST(rejection_counting)
{
    static const double xb[1] = {1000}, yb[1] = {50};
    fx x;
    lt_at_reject rej[8];
    size_t i, n;
    fx_init(&x, xb, 1, yb, 1);
    for (i = 0; i < 2; i++) {
        lt_ve_point p;
        lt_ve_point_default(&p);
        p.rpm = 1000; p.load = 50; p.clt = 100; p.timestamp_ms = i * 100;
        lt_at_add_data_point(x.s, &p, xb, 1, yb, 1, &x.set, &x.f, &x.a);
    }
    {
        lt_ve_point p;
        lt_ve_point_default(&p);
        p.rpm = 1000; p.load = 50; p.clt = 180; p.tps_rate = 80; p.timestamp_ms = 300;
        lt_at_add_data_point(x.s, &p, xb, 1, yb, 1, &x.set, &x.f, &x.a);
        lt_ve_point_default(&p);
        p.rpm = 1000; p.load = 50; p.clt = 180; p.afr = 14.7; p.ve = 50; p.timestamp_ms = 400;
        lt_at_add_data_point(x.s, &p, xb, 1, yb, 1, &x.set, &x.f, &x.a);
    }
    n = lt_at_rejections(x.s, rej, 8);
    LT_CHECK_INT(n, 2);
    LT_CHECK_STR(rej[0].reason, "clt below min_clt");
    LT_CHECK_INT(rej[0].count, 2);
    LT_CHECK_STR(rej[1].reason, "tps_rate above max_tps_rate");
    LT_CHECK_INT(rej[1].count, 1);
    LT_CHECK_INT(lt_at_total_samples(x.s), 1);
    lt_at_start(x.s);
    LT_CHECK_INT(lt_at_rejections(x.s, rej, 8), 0);
    LT_CHECK_INT(lt_at_total_samples(x.s), 0);
    fx_free(&x);
}

LT_TEST(rejection_order_ties)
{
    static const double xb[1] = {1000}, yb[1] = {50};
    fx x;
    lt_at_reject rej[8];
    lt_ve_point p;
    fx_init(&x, xb, 1, yb, 1);
    lt_ve_point_default(&p);
    p.rpm = 100; p.load = 50; p.clt = 180;
    lt_at_add_data_point(x.s, &p, xb, 1, yb, 1, &x.set, &x.f, &x.a);   /* rpm out of range */
    p.rpm = 1000; p.clt = 10; p.timestamp_ms = 10;
    lt_at_add_data_point(x.s, &p, xb, 1, yb, 1, &x.set, &x.f, &x.a);   /* clt */
    LT_CHECK_INT(lt_at_rejections(x.s, rej, 8), 2);
    LT_CHECK_STR(rej[0].reason, "clt below min_clt");   /* tie -> reason ascending */
    LT_CHECK_STR(rej[1].reason, "rpm out of range");
    fx_free(&x);
}

LT_TEST(reasons)
{
    lt_at_state *s = lt_at_new();
    lt_at_filters f;
    lt_ve_point p;
    size_t i;
    static const double bad[5] = {9.9, 9.99, 19.6, 22, 0};
    static const double good[5] = {11.5, 12.7, 14.7, 16, 19.4};
    lt_at_filters_default(&f);
    lt_ve_point_default(&p);
    p.rpm = 2000; p.clt = 180; p.load = 50;
    LT_CHECK(classify(s, &p, &f) == NULL);
    p.fuel_cut_active = 1;
    LT_CHECK_STR(classify(s, &p, &f), "overrun fuel cut");
    p.fuel_cut_active = 0;
    LT_CHECK(classify(s, &p, &f) == NULL);
    p.fuel_cut_active = -1;
    LT_CHECK(classify(s, &p, &f) == NULL);
    for (i = 0; i < 5; i++) {
        p.afr = bad[i];
        LT_CHECK_STR(classify(s, &p, &f), "afr at sensor rail");
        p.afr = good[i];
        LT_CHECK(classify(s, &p, &f) == NULL);
    }
    p.afr = 14.7;
    p.accel_enrich_active = 1;
    LT_CHECK_STR(classify(s, &p, &f), "accel enrichment active");
    f.exclude_accel_enrich = LT_FALSE;
    LT_CHECK(classify(s, &p, &f) == NULL);
    f.exclude_accel_enrich = LT_TRUE;
    p.accel_enrich_active = 0;
    LT_CHECK(classify(s, &p, &f) == NULL);
    p.accel_enrich_active = -1;
    LT_CHECK(classify(s, &p, &f) == NULL);
    p.rpm = 500;
    LT_CHECK_STR(classify(s, &p, &f), "rpm out of range");
    p.rpm = 8000;
    LT_CHECK_STR(classify(s, &p, &f), "rpm out of range");
    p.rpm = 2000; p.clt = 100;
    LT_CHECK_STR(classify(s, &p, &f), "clt below min_clt");
    p.clt = 180; p.tps_rate = 80;
    LT_CHECK_STR(classify(s, &p, &f), "tps_rate above max_tps_rate");
    p.tps_rate = -80;
    LT_CHECK_STR(classify(s, &p, &f), "tps_rate above max_tps_rate");
    p.tps_rate = 50;
    LT_CHECK(classify(s, &p, &f) == NULL);
    LT_CHECK_NEAR(f.max_tps_rate, 50, 0);
    lt_at_free(s);
}

LT_TEST(filter_order)
{
    lt_at_state *s = lt_at_new();
    lt_at_filters f;
    lt_ve_point p;
    lt_at_filters_default(&f);
    lt_ve_point_default(&p);
    p.rpm = 500; p.clt = 10; p.afr = 5; p.fuel_cut_active = 1; p.tps_rate = 99;
    LT_CHECK_STR(classify(s, &p, &f), "rpm out of range");
    p.rpm = 2000;
    LT_CHECK_STR(classify(s, &p, &f), "clt below min_clt");
    p.clt = 180;
    LT_CHECK_STR(classify(s, &p, &f), "overrun fuel cut");
    p.fuel_cut_active = 0;
    LT_CHECK_STR(classify(s, &p, &f), "afr at sensor rail");
    p.afr = 14;
    LT_CHECK_STR(classify(s, &p, &f), "tps_rate above max_tps_rate");
    lt_at_filters_clear(&f);
    lt_at_free(s);
}

LT_TEST(custom_filter)
{
    lt_at_state *s = lt_at_new();
    lt_at_filters f;
    lt_ve_point p;
    lt_at_filters_default(&f);
    f.min_clt = 70;
    lt_ve_point_default(&p);
    p.rpm = 2500; p.tps = 25; p.clt = 85; p.load = 40; p.afr = 14.7;
    lt_at_filters_set_custom(&f, "rpm > 2000 && tps < 50 && clt > 70");
    LT_CHECK(classify(s, &p, &f) == NULL);
    lt_at_filters_set_custom(&f, "rpm > 3000");
    LT_CHECK_STR(classify(s, &p, &f), "custom_filter");
    lt_at_filters_set_custom(&f, "rpm >");
    LT_CHECK_STR(classify(s, &p, &f), "custom_filter");
    lt_at_filters_set_custom(&f, "bogus_var > 1");
    LT_CHECK_STR(classify(s, &p, &f), "custom_filter");
    lt_at_filters_set_custom(&f, "(rpm + 500) / 1000 == 3");
    LT_CHECK(classify(s, &p, &f) == NULL);
    lt_at_filters_set_custom(&f, "!(tps > 30) || load % 7 == 0");
    LT_CHECK(classify(s, &p, &f) == NULL);
    lt_at_filters_set_custom(&f, "afr < 14.0 || accel_enrich");
    LT_CHECK_STR(classify(s, &p, &f), "custom_filter");
    lt_at_filters_set_custom(&f, "1");
    LT_CHECK(classify(s, &p, &f) == NULL);
    lt_at_filters_set_custom(&f, "0");
    LT_CHECK_STR(classify(s, &p, &f), "custom_filter");
    lt_at_filters_set_custom(&f, "   ");
    LT_CHECK(classify(s, &p, &f) == NULL);
    lt_at_filters_set_custom(&f, "true");
    LT_CHECK(classify(s, &p, &f) == NULL);
    lt_at_filters_set_custom(&f, "1 / 0");
    LT_CHECK_STR(classify(s, &p, &f), "custom_filter");
    lt_at_filters_set_custom(&f, NULL);
    LT_CHECK(f.custom_filter == NULL);
    lt_at_filters_clear(&f);
    lt_at_free(s);
}

LT_TEST(steady_filter)
{
    lt_at_state *s = lt_at_new();
    lt_at_filters f;
    lt_ve_point p;
    unsigned long t;
    static const double xb[2] = {1000, 2000}, yb[2] = {50, 100};
    lt_at_settings set;
    lt_at_authority a;
    lt_at_filters_default(&f);
    lt_at_settings_default(&set);
    lt_at_authority_default(&a);
    f.min_clt = 70;
    f.min_steady_ms = 500;
    lt_at_start(s);
    lt_ve_point_default(&p);
    p.rpm = 1500; p.load = 75; p.clt = 90; p.afr = 14.7; p.ve = 50;
    /* the buffer does not reach back far enough -> not steady */
    p.timestamp_ms = 0;
    lt_at_add_data_point(s, &p, xb, 2, yb, 2, &set, &f, &a);
    LT_CHECK_INT(lt_at_total_samples(s), 0);
    for (t = 100; t <= 1000; t += 100) {
        p.timestamp_ms = t;
        lt_at_add_data_point(s, &p, xb, 2, yb, 2, &set, &f, &a);
    }
    LT_CHECK(lt_at_total_samples(s) > 0);
    {
        lt_at_reject rej[4];
        size_t n = lt_at_rejections(s, rej, 4);
        LT_CHECK(n >= 1);
        LT_CHECK_STR(rej[0].reason, "rpm/load not steady");
    }
    /* a sudden rpm jump inside the window is not steady */
    p.rpm = 1800;
    p.timestamp_ms = 1100;
    LT_CHECK(lt_at_classify(s, &p, &f, NULL) != 0);
    lt_at_free(s);
}

LT_TEST(tps_load_axis_attribution)
{
    static const double xb[4] = {1000, 2000, 3000, 4000};
    static const double yb[5] = {0, 25, 50, 75, 100};
    fx x;
    lt_at_rec r[8];
    fx_init(&x, xb, 4, yb, 5);
    lt_at_set_strict_lambda_match(x.s, LT_FALSE);
    feed(&x, 3000, 75, 14.0, 50);
    feed(&x, 3000, 75, 14.0, 50);
    LT_CHECK_INT(recs_of(&x, r, 8), 1);
    LT_CHECK_INT(r[0].cell_x, 2);
    LT_CHECK_INT(r[0].cell_y, 3);
    fx_free(&x);
}

LT_TEST(out_of_axis_samples_dropped)
{
    static const double xb[3] = {1000, 2000, 3000};
    fx x;
    fx_init(&x, xb, 3, Y3, 3);
    lt_at_set_strict_lambda_match(x.s, LT_FALSE);
    x.f.max_rpm = 20000;
    feed(&x, 9000, 10, 14.0, 50);
    feed(&x, 9000, 10, 14.0, 50);
    LT_CHECK_INT(lt_at_recommendations(x.s, NULL, 0), 0);
    LT_CHECK_INT(lt_at_total_samples(x.s), 2);
    fx_free(&x);
}

LT_TEST(lock_set)
{
    lt_at_state *s = lt_at_new();
    lt_at_lock_cell(s, 1, 2);
    lt_at_lock_cell(s, 1, 2);
    lt_at_lock_cell(s, 3, 4);
    LT_CHECK(lt_at_is_cell_locked(s, 1, 2));
    lt_at_unlock_cell(s, 1, 2);
    LT_CHECK(!lt_at_is_cell_locked(s, 1, 2));
    LT_CHECK(lt_at_is_cell_locked(s, 3, 4));
    lt_at_unlock_cell(s, 9, 9);
    LT_CHECK(lt_at_is_cell_locked(s, 3, 4));
    lt_at_start(s);   /* locks survive start() */
    LT_CHECK(lt_at_is_cell_locked(s, 3, 4));
    lt_at_free(s);
}

LT_TEST(locked_cell_not_tuned)
{
    fx x;
    lt_at_rec r[4];
    fx_init(&x, X3, 3, Y3, 3);
    lt_at_set_strict_lambda_match(x.s, LT_FALSE);
    lt_at_lock_cell(x.s, 0, 0);
    feed(&x, 1000, 10, 13.0, 50);
    feed(&x, 1000, 10, 13.0, 50);
    LT_CHECK_INT(lt_at_recommendations(x.s, r, 4), 0);
    lt_at_unlock_cell(x.s, 0, 0);
    feed(&x, 1000, 10, 13.0, 50);
    LT_CHECK_INT(lt_at_recommendations(x.s, r, 4), 1);
    fx_free(&x);
}

LT_TEST(not_running_ignores)
{
    fx x;
    fx_init(&x, X3, 3, Y3, 3);
    lt_at_stop(x.s);
    LT_CHECK(!lt_at_is_running(x.s));
    feed(&x, 1000, 10, 13.0, 50);
    feed(&x, 1000, 10, 13.0, 50);
    LT_CHECK_INT(lt_at_total_samples(x.s), 0);
    lt_at_set_table_name(x.s, "veTable1Tbl");
    lt_at_start(x.s);
    lt_at_stop(x.s);
    LT_CHECK_STR(lt_at_table_name(x.s), "veTable1Tbl");
    fx_free(&x);
}

LT_TEST(hit_weighting_modes_accumulate)
{
    static const double xb[3] = {1000, 2000, 3000};
    fx x;
    lt_at_rec r[4];
    fx_init(&x, xb, 3, Y3, 3);
    lt_at_set_strict_lambda_match(x.s, LT_FALSE);
    x.set.hit_weighting = LT_HW_CELL_PROXIMITY;
    feed(&x, 1500, 10, 14.0, 50);
    feed(&x, 1500, 10, 14.0, 50);
    LT_CHECK_INT(recs_of(&x, r, 4), 1);
    LT_CHECK_INT(r[0].cell_x, 0);   /* tie 1500 -> first bin */
    LT_CHECK_NEAR(r[0].hit_weighting, 1.0, 1e-9);   /* 2 x 0.5 */
    LT_CHECK_INT(r[0].hit_count, 2);
    fx_free(&x);
}

LT_TEST(adopt_ini_min_clt)
{
    lt_ini ini;
    lt_ini_filter fl[2];
    lt_at_filters f;
    memset(&ini, 0, sizeof ini);
    memset(fl, 0, sizeof fl);
    fl[0].name = (char *)"minRPMFilter"; fl[0].dflt = 500;
    fl[1].name = (char *)"minCltFilter"; fl[1].dflt = 71;
    ini.ve_analyze.filters = fl;
    ini.ve_analyze.n_filters = 2;
    lt_at_filters_default(&f);
    lt_at_filters_adopt_ini(&f, &ini);
    LT_CHECK_NEAR(f.min_clt, 71, 0);
    f.min_clt = 55;
    lt_at_filters_adopt_ini(&f, &ini);
    LT_CHECK_NEAR(f.min_clt, 55, 0);
    lt_at_filters_default(&f);
    lt_at_filters_adopt_ini(&f, NULL);
    LT_CHECK_NEAR(f.min_clt, 160, 0);
}

/* ---- proposals ---- */

LT_TEST(proposal_summary)
{
    double z[9] = {40, 41, 42, 43, 44, 45, 46, 47, 48};
    lt_at_rec r[3];
    double big = 99;
    size_t n;
    memset(r, 0, sizeof r);
    r[0].cell_x = 1; r[0].cell_y = 0; r[0].recommended_value = 43.5;   /* +2.5 */
    r[1].cell_x = 9; r[1].cell_y = 9; r[1].recommended_value = 100;    /* stale */
    r[2].cell_x = 0; r[2].cell_y = 0; r[2].recommended_value = 40;     /* no-op */
    n = lt_at_proposal_summary(r, 3, z, 3, 3, &big);
    LT_CHECK_INT(n, 1);
    LT_CHECK_NEAR(big, 2.5, 1e-12);
    r[0].recommended_value = 39;   /* -2.0 keeps its sign */
    n = lt_at_proposal_summary(r, 3, z, 3, 3, &big);
    LT_CHECK_INT(n, 1);
    LT_CHECK_NEAR(big, -2.0, 1e-12);
    n = lt_at_proposal_summary(r, 0, z, 3, 3, &big);
    LT_CHECK_INT(n, 0);
    LT_CHECK_NEAR(big, 0.0, 0);
}

/* ---- declared filters ---- */

typedef struct vals {
    const char *names[4];
    double v[4];
    size_t n;
} vals;

static lt_bool get_var(void *ud, const char *name, double *out)
{
    const vals *d = (const vals *)ud;
    size_t i;
    for (i = 0; i < d->n; i++) {
        if (strcmp(d->names[i], name) == 0) {
            *out = d->v[i];
            return LT_TRUE;
        }
    }
    return LT_FALSE;
}

static void mk_filter(lt_ini_filter *f, const char *name, const char *ch, lt_filter_op op,
                      double v)
{
    memset(f, 0, sizeof *f);
    f->name = (char *)name;
    f->channel = (char *)ch;
    f->op = op;
    f->dflt = v;
}

LT_TEST(declared_filters)
{
    lt_ini_filter clt, acc, ase, pw;
    mk_filter(&clt, "minCltFilter", "coolant", LT_FOP_LT, 71);
    mk_filter(&acc, "accelFilter", "engine", LT_FOP_BITAND, 16);
    mk_filter(&ase, "aseFilter", "engine", LT_FOP_BITAND, 4);
    mk_filter(&pw, "pw", "pulseWidth", LT_FOP_EQ, 0);
    LT_CHECK(lt_at_filter_rejects(&clt, LT_TRUE, LT_TRUE, 40));
    LT_CHECK(!lt_at_filter_rejects(&clt, LT_TRUE, LT_TRUE, 85));
    LT_CHECK(!lt_at_filter_rejects(&clt, LT_TRUE, LT_TRUE, 71));
    LT_CHECK(lt_at_filter_rejects(&acc, LT_TRUE, LT_TRUE, 16));
    LT_CHECK(lt_at_filter_rejects(&acc, LT_TRUE, LT_TRUE, 24));
    LT_CHECK(lt_at_filter_rejects(&acc, LT_TRUE, LT_TRUE, 17));
    LT_CHECK(!lt_at_filter_rejects(&acc, LT_TRUE, LT_TRUE, 9));
    LT_CHECK(!lt_at_filter_rejects(&acc, LT_TRUE, LT_TRUE, 1));
    LT_CHECK(lt_at_filter_rejects(&ase, LT_TRUE, LT_TRUE, 5));
    LT_CHECK(!lt_at_filter_rejects(&ase, LT_TRUE, LT_TRUE, 17));
    LT_CHECK(lt_at_filter_rejects(&pw, LT_TRUE, LT_TRUE, 0));
    LT_CHECK(!lt_at_filter_rejects(&pw, LT_TRUE, LT_TRUE, 3.2));
    LT_CHECK(!lt_at_filter_rejects(&clt, LT_TRUE, LT_FALSE, 0));      /* missing channel */
    LT_CHECK(!lt_at_filter_rejects(&clt, LT_FALSE, LT_TRUE, 10));     /* disabled */
    acc.dflt = 0;
    LT_CHECK(!lt_at_filter_rejects(&acc, LT_TRUE, LT_TRUE, 255));     /* mask 0 */
    pw.op = LT_FOP_NE;
    LT_CHECK(lt_at_filter_rejects(&pw, LT_TRUE, LT_TRUE, 3.2));
    LT_CHECK(!lt_at_filter_rejects(&pw, LT_TRUE, LT_TRUE, 0));
    pw.op = LT_FOP_GT;
    pw.dflt = 50;
    LT_CHECK(lt_at_filter_rejects(&pw, LT_TRUE, LT_TRUE, 60));
    LT_CHECK(!lt_at_filter_rejects(&pw, LT_TRUE, LT_TRUE, 50));
}

LT_TEST(first_rejecting_and_tally)
{
    lt_ini_filter fl[2];
    lt_expr_env env;
    vals v;
    static const double samples[4][2] = {{30, 1}, {40, 1}, {85, 16}, {85, 1}};
    unsigned counts[2] = {0, 0};
    size_t i;
    int r;
    mk_filter(&fl[0], "minCltFilter", "coolant", LT_FOP_LT, 71);
    mk_filter(&fl[1], "accelFilter", "engine", LT_FOP_BITAND, 16);
    memset(&env, 0, sizeof env);
    env.ud = &v;
    env.get_var = get_var;
    v.names[0] = "coolant";
    v.names[1] = "engine";
    v.n = 2;
    v.v[0] = 30;
    v.v[1] = 16;
    LT_CHECK_INT(lt_at_first_rejecting(fl, 2, &env), 0);
    v.v[0] = 85;
    LT_CHECK_INT(lt_at_first_rejecting(fl, 2, &env), 1);
    v.v[1] = 1;
    LT_CHECK_INT(lt_at_first_rejecting(fl, 2, &env), -1);
    v.n = 1;   /* engine channel missing */
    v.v[1] = 16;
    LT_CHECK_INT(lt_at_first_rejecting(fl, 2, &env), -1);
    v.n = 2;
    for (i = 0; i < 4; i++) {
        v.v[0] = samples[i][0];
        v.v[1] = samples[i][1];
        r = lt_at_first_rejecting(fl, 2, &env);
        if (r >= 0) {
            counts[r]++;
        }
    }
    LT_CHECK_INT(counts[0], 2);
    LT_CHECK_INT(counts[1], 1);
}

/* ---- fuel guard (fake ini built by hand) ---- */

typedef struct fakeini {
    lt_ini ini;
    lt_ini_table t[6];
    lt_ini_table *tp[6];
} fakeini;

static void fi_add(fakeini *f, const char *name, const char *title, lt_table_role role, int twod)
{
    size_t n = f->ini.n_tables;
    memset(&f->t[n], 0, sizeof f->t[n]);
    f->t[n].name = (char *)name;
    f->t[n].title = (char *)title;
    f->t[n].role = role;
    f->t[n].y_const = twod ? (char *)"y" : NULL;
    f->tp[n] = &f->t[n];
    lt_map_put(&f->ini.table_idx, name, &f->t[n]);
    f->ini.n_tables++;
    f->ini.tables = f->tp;
}

static void fi_init(fakeini *f)
{
    memset(f, 0, sizeof *f);
    lt_map_init(&f->ini.table_idx, LT_FALSE);
}

LT_TEST(fuel_guard_labelled)
{
    fakeini f;
    const char *m;
    const char *names[8];
    size_t n;
    fi_init(&f);
    fi_add(&f, "veTable1Tbl", "VE Table", LT_ROLE_VE, 1);
    fi_add(&f, "veTable2Tbl", "VE Table 2", LT_ROLE_VE, 1);
    fi_add(&f, "sparkTbl", "Spark Advance", LT_ROLE_IGNITION, 1);
    fi_add(&f, "afrTbl", "AFR Targets", LT_ROLE_AFR_TARGET, 1);
    fi_add(&f, "wueTbl", NULL, LT_ROLE_WARMUP, 1);
    fi_add(&f, "otherTbl", "Boost", LT_ROLE_OTHER, 1);
    LT_CHECK(lt_fuel_tune_refusal(&f.ini, "veTable1Tbl") == NULL);
    m = lt_fuel_tune_refusal(&f.ini, "sparkTbl");
    LT_CHECK(m != NULL && strstr(m, "ignition") != NULL);
    LT_CHECK(m != NULL && strstr(m, "Spark Advance is an ignition table.") != NULL);
    m = lt_fuel_tune_refusal(&f.ini, "afrTbl");
    LT_CHECK(m != NULL && strstr(m, "AFR target") != NULL);
    m = lt_fuel_tune_refusal(&f.ini, "wueTbl");
    LT_CHECK(m != NULL && strstr(m, "wueTbl is a warm-up enrichment table.") != NULL);
    m = lt_fuel_tune_refusal(&f.ini, "otherTbl");
    LT_CHECK(m != NULL && strstr(m, "not a fuel table") != NULL);
    m = lt_fuel_tune_refusal(&f.ini, "nope");
    LT_CHECK(m != NULL && strstr(m, "nope is not a table in this definition.") != NULL);
    n = lt_fuel_tunable_tables(&f.ini, names, 8);
    LT_CHECK_INT(n, 2);
    LT_CHECK_STR(names[0], "veTable1Tbl");
    LT_CHECK_STR(names[1], "veTable2Tbl");
    lt_map_free(&f.ini.table_idx);
}

LT_TEST(fuel_guard_unlabelled)
{
    fakeini f;
    const char *names[8];
    size_t n;
    fi_init(&f);
    fi_add(&f, "zTbl", "Z", LT_ROLE_OTHER, 1);
    fi_add(&f, "aTbl", "A", LT_ROLE_OTHER, 1);
    fi_add(&f, "lineTbl", "Line", LT_ROLE_OTHER, 0);   /* 2-D curve-like: not offered */
    fi_add(&f, "sparkTbl", "Spark", LT_ROLE_IGNITION, 1);
    LT_CHECK(lt_fuel_tune_refusal(&f.ini, "aTbl") == NULL);
    LT_CHECK(lt_fuel_tune_refusal(&f.ini, "sparkTbl") != NULL);
    n = lt_fuel_tunable_tables(&f.ini, names, 8);
    LT_CHECK_INT(n, 2);
    LT_CHECK_STR(names[0], "aTbl");
    LT_CHECK_STR(names[1], "zTbl");
    LT_CHECK_INT(lt_fuel_tunable_tables(&f.ini, NULL, 0), 2);
    lt_map_free(&f.ini.table_idx);
}

LT_TEST(channels_from_ini)
{
    lt_ini ini;
    lt_ini_channel ch[6];
    lt_at_channels c;
    size_t i;
    const char *nm[6] = {"RPMValue", "fuelload", "coolant", "TPSValue", "lambdaValue", "veCurr"};
    memset(&ini, 0, sizeof ini);
    memset(ch, 0, sizeof ch);
    lt_map_init(&ini.channel_idx, LT_FALSE);
    for (i = 0; i < 6; i++) {
        ch[i].name = (char *)nm[i];
        lt_map_put(&ini.channel_idx, nm[i], &ch[i]);
    }
    ini.ve_analyze.lambda_channel = (char *)"lambdaValue";
    lt_at_channels_from_ini(&ini, &c);
    LT_CHECK_STR(c.rpm, "RPMValue");
    LT_CHECK_STR(c.load, "fuelload");
    LT_CHECK_STR(c.clt, "coolant");
    LT_CHECK_STR(c.tps, "TPSValue");
    LT_CHECK_STR(c.afr, "lambdaValue");
    LT_CHECK_STR(c.ve, "veCurr");
    LT_CHECK(c.afr_is_lambda);
    LT_CHECK(c.maf[0] == '\0');
    ini.ve_analyze.lambda_channel = NULL;
    lt_at_channels_from_ini(&ini, &c);
    LT_CHECK(c.afr[0] == '\0');
    lt_map_free(&ini.channel_idx);
}

int main(void)
{
    LT_RUN(normalise_and_defaults);
    LT_RUN(find_bin);
    LT_RUN(hit_weights);
    LT_RUN(authority_limits);
    LT_RUN(delay_curve);
    LT_RUN(flow_scaled_table);
    LT_RUN(find_delayed_point);
    LT_RUN(buffer_sizing);
    LT_RUN(accumulate);
    LT_RUN(target_table_used);
    LT_RUN(target_table_lambda_normalised);
    LT_RUN(strict_lambda_match);
    LT_RUN(cma_alternating);
    LT_RUN(y_axis_filter);
    LT_RUN(ramp_holds_sparse_cell);
    LT_RUN(ramp_arithmetic);
    LT_RUN(min_change);
    LT_RUN(anchored_on_table_cell);
    LT_RUN(rejection_counting);
    LT_RUN(rejection_order_ties);
    LT_RUN(reasons);
    LT_RUN(filter_order);
    LT_RUN(custom_filter);
    LT_RUN(steady_filter);
    LT_RUN(tps_load_axis_attribution);
    LT_RUN(out_of_axis_samples_dropped);
    LT_RUN(lock_set);
    LT_RUN(locked_cell_not_tuned);
    LT_RUN(not_running_ignores);
    LT_RUN(hit_weighting_modes_accumulate);
    LT_RUN(adopt_ini_min_clt);
    LT_RUN(proposal_summary);
    LT_RUN(declared_filters);
    LT_RUN(first_rejecting_and_tally);
    LT_RUN(fuel_guard_labelled);
    LT_RUN(fuel_guard_unlabelled);
    LT_RUN(channels_from_ini);
    LT_TEST_MAIN_END
}
