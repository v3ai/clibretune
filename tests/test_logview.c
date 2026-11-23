/* test_logview.c - MegaLogViewer text logs (.msl), graph setups, histogram / table generator,
 * scatter filters, X-Y overlay points and live math channels (src/datalog/dl_msl.c, dl_view.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lt.h"
#include "lt_codes.h"
#include "datalog/datalog.h"
#include "datalog/dl_view.h"
#include "ini/ini.h"
#include "plat/plat.h"
#include "lt_test.h"

#define TMP "build/lv_tmp_"
#define DEMO_INI "tests/fixtures/sim/demo.ini"

static double tnan(void)
{
    volatile double z = 0.0;
    return z / z;
}

static lt_log small_log(void)
{
    static const char *const nm[3] = { "RPM", "MAP", "AFR" };
    static const char *const un[3] = { "rpm", "kPa", "" };
    lt_log l;
    double row[3];
    int i;
    lt_log_init(&l);
    lt_log_set_channels(&l, nm, un, 3);
    for (i = 0; i < 6; i++) {
        row[0] = 1000.0 + 500.0 * (double)i;
        row[1] = 30.0 + 10.0 * (double)i;
        row[2] = 14.7 - 0.25 * (double)i;
        lt_log_append(&l, 0.1 * (double)i, row);
    }
    return l;
}

LT_TEST(msl_roundtrip)
{
    lt_log l = small_log(), r;
    lt_buf b;
    const char *s;
    lt_log_set_header(&l, "My car\nCapture Date: today");
    lt_log_add_marker(&l, 0.2, "Pull 1");
    lt_buf_init(&b);
    lt_msl_format(&b, &l);
    s = (const char *)b.data;
    LT_CHECK(strncmp(s, "\"My car\"\n\"Capture Date: today\"\nTime\tRPM\tMAP\tAFR\ns\trpm\tkPa\t\n", 59) == 0);
    LT_CHECK(strstr(s, "MARK 000 - Pull 1") != NULL);
    lt_log_init(&r);
    LT_CHECK_INT(lt_msl_parse(s, b.len, &r), 0);
    LT_CHECK_INT(r.n_ch, 3);
    LT_CHECK_INT(r.n, 6);
    LT_CHECK_STR(r.names[1], "MAP");
    LT_CHECK_STR(r.units[0], "rpm");
    LT_CHECK_STR(r.units[2], "");
    LT_CHECK(r.header != NULL);
    if (r.header != NULL) { LT_CHECK_STR(r.header, "My car\nCapture Date: today"); }
    LT_CHECK_INT(r.n_markers, 1);
    if (r.n_markers == 1) {
        LT_CHECK_STR(r.markers[0].text, "Pull 1");
        LT_CHECK_NEAR(r.markers[0].t, 0.2, 1e-9);
    }
    LT_CHECK_NEAR(r.t[5], 0.5, 1e-9);
    LT_CHECK_NEAR(LT_LOG_AT(&r, 5, 0), 3500.0, 1e-9);
    LT_CHECK_NEAR(LT_LOG_AT(&r, 3, 2), 13.95, 1e-9);
    lt_log_free(&r);
    /* through the file layer, routed by extension */
    LT_CHECK_INT(lt_log_write(TMP "a.msl", &l), 0);
    lt_log_init(&r);
    LT_CHECK_INT(lt_log_read(TMP "a.msl", &r), 0);
    LT_CHECK_INT(r.n, 6);
    LT_CHECK_INT(r.n_markers, 1);
    lt_log_free(&r);
    lt_buf_free(&b);
    lt_log_free(&l);
    (void)lt_plat_remove(TMP "a.msl");
}

LT_TEST(msl_default_header_and_blank_lines)
{
    lt_log l = small_log(), r;
    lt_buf b;
    const char *text = "\n\n\"Title\"\nTime\tA\tB\ns\tV\t%\n0\t1\t2\n0.5\t3\t4\nMARK 001 - hi there\n1\t5\t6\n";
    lt_buf_init(&b);
    lt_msl_format(&b, &l);
    LT_CHECK(strncmp((const char *)b.data, "\"", 1) == 0);
    LT_CHECK(strstr((const char *)b.data, "Capture Date:") != NULL);
    lt_buf_free(&b);
    lt_log_free(&l);
    lt_log_init(&r);
    LT_CHECK_INT(lt_msl_parse(text, strlen(text), &r), 0);
    LT_CHECK_INT(r.n, 3);
    LT_CHECK_STR(r.units[1], "%");
    LT_CHECK_INT(r.n_markers, 1);
    if (r.n_markers == 1) {
        LT_CHECK_STR(r.markers[0].text, "hi there");
        LT_CHECK_NEAR(r.markers[0].t, 0.5, 1e-9);
    }
    lt_log_free(&r);
}

LT_TEST(extension_routing)
{
    LT_CHECK_INT(lt_log_format_from_ext("x/run.msl"), LT_FMT_MSL);
    LT_CHECK_INT(lt_log_format_from_ext("RUN.MSL"), LT_FMT_MSL);
    LT_CHECK_INT(lt_log_format_from_ext("run.mlg"), LT_FMT_MLG);
    LT_CHECK_INT(lt_log_format_from_ext("run.csv"), LT_FMT_CSV);
    LT_CHECK_INT(lt_log_format_from_ext("run.txt"), LT_FMT_NONE);
    LT_CHECK(strstr(lt_log_filter_all(), "*.msl") != NULL);
    LT_CHECK(strstr(lt_log_filter_all(), "*.mlg") != NULL);
    LT_CHECK(strstr(lt_log_filter_all(), "*.csv") != NULL);
}

LT_TEST(recorder_msl)
{
    static const char *const nm[2] = { "RPM", "MAP" };
    static const char *const un[2] = { "rpm", "kPa" };
    lt_recorder *r = lt_rec_new(nm, un, 2);
    double v[2];
    lt_log l;
    char path[256];
    lt_rec_set_format(r, LT_FMT_MSL);
    LT_CHECK_INT(lt_rec_start(r, TMP "rec.mlg"), 0);
    lt_strlcpy(path, lt_rec_path(r), sizeof(path));
    LT_CHECK(strstr(path, ".msl") != NULL);
    v[0] = 900.0; v[1] = 35.0;
    LT_CHECK_INT(lt_rec_record_at(r, 1000.0, v, 2), 1);
    lt_rec_marker_at(r, 1100.0, "here");
    v[0] = 1500.0; v[1] = 50.0;
    LT_CHECK_INT(lt_rec_record_at(r, 1200.0, v, 2), 1);
    LT_CHECK_INT(lt_rec_stop(r), 0);
    lt_log_init(&l);
    LT_CHECK_INT(lt_log_read(path, &l), 0);
    LT_CHECK_INT(l.n, 2);
    LT_CHECK_INT(l.n_ch, 2);
    LT_CHECK_STR(l.units[1], "kPa");
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 0), 1500.0, 1e-9);
    LT_CHECK_NEAR(l.t[1] - l.t[0], 0.2, 1e-6);
    LT_CHECK_INT(l.n_markers, 1);
    lt_log_free(&l);
    lt_rec_free(r);
    (void)lt_plat_remove(path);
}

LT_TEST(graph_setup_json)
{
    static const char *const ch[7] = { "rawMap", "MAPValue", "RPMValue", "TPSValue", "AFRValue", "coolant", "intake" };
    lt_graph_setup g, h;
    int p;
    lt_graph_init(&g);
    lt_graph_defaults(&g, ch, 7);
    LT_CHECK_INT(g.n_pages, 1);
    LT_CHECK_INT(g.pages[0].n_panes, 4);
    LT_CHECK_STR(g.pages[0].pane[0].tr[0].channel, "RPMValue");
    LT_CHECK_STR(g.pages[0].pane[1].tr[0].channel, "MAPValue");    /* not rawMap */
    LT_CHECK_STR(g.pages[0].pane[2].tr[0].channel, "AFRValue");
    LT_CHECK_INT(g.pages[0].pane[1].tr[1].right, 1);
    p = lt_graph_add_page(&g, "Second");
    LT_CHECK_INT(p, 1);
    LT_CHECK_INT(g.pages[1].n_panes, 0);
    LT_CHECK_INT(lt_graph_add_pane(&g.pages[1]), 0);
    LT_CHECK_INT(lt_graph_pane_add(&g.pages[1].pane[0], "coolant"), 0);
    LT_CHECK_INT(lt_graph_pane_add(&g.pages[1].pane[0], "coolant"), 0);   /* already there */
    g.pages[1].pane[0].tr[0].autoscale = 0;
    g.pages[1].pane[0].tr[0].lo = -40.0;
    g.pages[1].pane[0].tr[0].hi = 120.0;
    g.pages[1].pane[0].tr[0].right = 1;
    LT_CHECK_INT(lt_graph_save(TMP "g.json", &g), 0);
    lt_graph_init(&h);
    LT_CHECK_INT(lt_graph_load(TMP "g.json", &h), 0);
    LT_CHECK_INT(h.n_pages, 2);
    if (h.n_pages == 2) {
        LT_CHECK_STR(h.pages[1].name, "Second");
        LT_CHECK_INT(h.pages[0].n_panes, 4);
        LT_CHECK_INT(h.pages[1].pane[0].n, 1);
        LT_CHECK_STR(h.pages[1].pane[0].tr[0].channel, "coolant");
        LT_CHECK_INT(h.pages[1].pane[0].tr[0].autoscale, 0);
        LT_CHECK_NEAR(h.pages[1].pane[0].tr[0].lo, -40.0, 1e-9);
        LT_CHECK_NEAR(h.pages[1].pane[0].tr[0].hi, 120.0, 1e-9);
        LT_CHECK_INT(h.pages[1].pane[0].tr[0].right, 1);
    }
    lt_graph_free(&h);
    lt_graph_init(&h);
    LT_CHECK_INT(lt_graph_load(TMP "missing.json", &h), LT_ENOENT);
    LT_CHECK_INT(lt_graph_from_json("{\"pages\": 3}", 12, &h), LT_EDATA);
    lt_graph_free(&h);
    lt_graph_free(&g);
    (void)lt_plat_remove(TMP "g.json");
    LT_CHECK_INT(lt_graph_name_rank("MAPValue", "map"), 0);
    LT_CHECK_INT(lt_graph_name_rank("mapFast", "map"), 1);
    LT_CHECK_INT(lt_graph_name_rank("rawMap", "map"), 3);
    LT_CHECK_INT(lt_graph_name_rank("RPM", "map"), -1);
}

LT_TEST(trace_range_and_ticks)
{
    lt_graph_trace t;
    double v[3], lo, hi;
    memset(&t, 0, sizeof t);
    t.autoscale = 1;
    v[0] = 10.0; v[1] = 20.0; v[2] = 30.0;
    lt_graph_trace_range(&t, v, 3, &lo, &hi);
    LT_CHECK_NEAR(lo, 9.0, 1e-9);
    LT_CHECK_NEAR(hi, 31.0, 1e-9);
    lt_graph_trace_range(&t, v, 0, &lo, &hi);
    LT_CHECK_NEAR(lo, 0.0, 1e-9);
    LT_CHECK_NEAR(hi, 1.0, 1e-9);
    t.autoscale = 0; t.lo = -5.0; t.hi = 5.0;
    lt_graph_trace_range(&t, v, 3, &lo, &hi);
    LT_CHECK_NEAR(lo, -5.0, 1e-9);
    /* a constant huge channel: +-0.5 is below its precision and left an empty range */
    t.autoscale = 1;
    v[0] = v[1] = v[2] = 1e17;
    lt_graph_trace_range(&t, v, 3, &lo, &hi);
    LT_CHECK(lo < 1e17 && hi > 1e17);
    v[0] = v[1] = v[2] = 850.0;
    lt_graph_trace_range(&t, v, 3, &lo, &hi);
    LT_CHECK_NEAR(lo, 849.5, 1e-9);
    LT_CHECK_NEAR(hi, 850.5, 1e-9);
    LT_CHECK_NEAR(lt_graph_nice_step(100.0, 4), 25.0, 1e-9);
    LT_CHECK_NEAR(lt_graph_nice_step(7.0, 4), 2.0, 1e-9);
}

LT_TEST(histogram_binning)
{
    double c[4], xb[2], yb[2];
    lt_hist2d h;
    lt_hist_auto_bins(0.0, 30.0, 4, c);
    LT_CHECK_NEAR(c[1], 10.0, 1e-9);
    LT_CHECK_NEAR(c[3], 30.0, 1e-9);
    LT_CHECK_INT(lt_hist_bin(c, 4, -100.0), 0);
    LT_CHECK_INT(lt_hist_bin(c, 4, 4.9), 0);
    LT_CHECK_INT(lt_hist_bin(c, 4, 5.0), 0);       /* tie: lower bin */
    LT_CHECK_INT(lt_hist_bin(c, 4, 5.1), 1);
    LT_CHECK_INT(lt_hist_bin(c, 4, 999.0), 3);
    LT_CHECK_INT(lt_hist_bin(c, 0, 1.0), -1);
    LT_CHECK_INT(lt_hist_bin(c, 4, tnan()), -1);
    xb[0] = 1000.0; xb[1] = 2000.0;
    yb[0] = 30.0; yb[1] = 60.0;
    lt_hist2d_init(&h, xb, 2, yb, 2);
    lt_hist2d_add(&h, 900.0, 31.0, 14.0);
    lt_hist2d_add(&h, 1100.0, 29.0, 15.0);
    lt_hist2d_add(&h, 2100.0, 70.0, 12.0);
    lt_hist2d_add(&h, 2100.0, 70.0, tnan());   /* NaN z ignored */
    lt_hist2d_finish(&h);
    LT_CHECK_INT(h.n_binned, 3);
    LT_CHECK_INT(h.hits[0], 2);
    LT_CHECK_NEAR(h.avg[0], 14.5, 1e-9);
    LT_CHECK_INT(h.hits[3], 1);
    LT_CHECK_NEAR(h.avg[3], 12.0, 1e-9);
    LT_CHECK(h.avg[1] != h.avg[1]);                /* no hits: NaN */
    LT_CHECK_NEAR(h.zmin, 12.0, 1e-9);
    LT_CHECK_NEAR(h.zmax, 14.5, 1e-9);
    LT_CHECK_INT(h.max_hits, 2);
    lt_hist2d_free(&h);
    /* from a log, masked, counts only (1-D) */
    {
        lt_log l = small_log();
        unsigned char mask[6];
        memset(mask, 1, sizeof mask);
        mask[5] = 0;
        lt_hist2d_init(&h, xb, 2, NULL, 1);
        lt_hist2d_from_log(&h, &l, 0, -1, -1, mask, 0, 99);
        LT_CHECK_INT(h.n_binned, 5);
        LT_CHECK_INT(h.hits[0], 2);                /* 1000, 1500 (tie -> lower) */
        LT_CHECK_INT(h.hits[1], 3);                /* 2000, 2500, 3000 */
        LT_CHECK_NEAR(h.avg[1], 3.0, 1e-9);
        lt_hist2d_free(&h);
        lt_log_free(&l);
    }
}

LT_TEST(scatter_filter)
{
    lt_math_hooks hk = LT_MATH_HOOKS_INI;
    lt_log l = small_log();
    unsigned char mask[6];
    double out[6];
    char err[96];
    lt_math_set_hooks(&hk);
    LT_CHECK_INT(lt_log_filter(&l, "RPM > 2000 && MAP <= 70", mask, err, sizeof err), 2);
    LT_CHECK_INT(mask[2], 0);
    LT_CHECK_INT(mask[3], 1);
    LT_CHECK_INT(mask[4], 1);
    LT_CHECK_INT(mask[5], 0);
    LT_CHECK_INT(lt_log_filter(&l, "  ", mask, err, sizeof err), 6);
    err[0] = '\0';
    LT_CHECK(lt_log_filter(&l, "RPM > ", mask, err, sizeof err) < 0);
    LT_CHECK(err[0] != '\0');
    LT_CHECK_INT(lt_log_eval_expr(&l, "MAP * 2", out, err, sizeof err), 0);
    LT_CHECK_NEAR(out[1], 80.0, 1e-9);
    lt_math_set_hooks(NULL);
    LT_CHECK(lt_log_filter(&l, "RPM > 1", mask, err, sizeof err) < 0);
    lt_log_free(&l);
}

LT_TEST(xy_ring_and_fade)
{
    lt_xyring r;
    double x, y, t;
    int i;
    lt_xyring_init(&r, 4);
    LT_CHECK_INT(lt_xyring_push(&r, tnan(), 1.0, 0.0), 0);
    for (i = 0; i < 6; i++) { LT_CHECK_INT(lt_xyring_push(&r, (double)i, (double)(i * 10), (double)i), 1); }
    LT_CHECK_INT(lt_xyring_count(&r), 4);
    lt_xyring_get(&r, 0, &x, &y, &t);
    LT_CHECK_NEAR(x, 2.0, 0);                       /* oldest kept */
    lt_xyring_get(&r, 3, &x, &y, &t);
    LT_CHECK_NEAR(y, 50.0, 0);
    lt_xyring_expire(&r, 6.0, 2.5);                 /* drops t = 2, 3 */
    LT_CHECK_INT(lt_xyring_count(&r), 2);
    lt_xyring_get(&r, 0, &x, &y, &t);
    LT_CHECK_NEAR(t, 4.0, 0);
    lt_xyring_clear(&r);
    LT_CHECK_INT(lt_xyring_count(&r), 0);
    lt_xyring_free(&r);
    LT_CHECK_NEAR(lt_xy_age_alpha(0.0, 30.0), 1.0, 1e-9);
    LT_CHECK_NEAR(lt_xy_age_alpha(30.0, 30.0), 0.15, 1e-9);
    LT_CHECK_NEAR(lt_xy_age_alpha(15.0, 30.0), 0.575, 1e-9);
    LT_CHECK_NEAR(lt_xy_age_alpha(31.0, 30.0), 0.0, 1e-9);
    LT_CHECK_NEAR(lt_xy_age_alpha(500.0, 0.0), 1.0, 1e-9);
}

/* live math through the realtime lookup, as feat_logging installs it */
typedef struct live_ctx { lt_math_live ml; lt_math_set *set; lt_ini_rt *rt; } live_ctx;
static lt_bool live_virt(void *ud, const char *name, double *out)
{
    live_ctx *c = (live_ctx *)ud;
    (void)lt_math_live_eval(&c->ml, c->set, lt_ini_rt_env(c->rt));
    return lt_math_live_get(&c->ml, c->set, name, out);
}

LT_TEST(live_math_realtime_lookup)
{
    lt_math_hooks hk = LT_MATH_HOOKS_INI;
    lt_ini *ini = lt_ini_load_file(DEMO_INI, NULL);
    const lt_ini_channel *rpm;
    lt_math_set s;
    live_ctx c;
    double v = 0.0;
    char **names, **units;
    size_t n, k;
    int found = 0;
    static const char *const vn[2] = { "rpm_k", "RPMValue" };
    static const char *const vu[2] = { "krpm", "rpm" };
    LT_CHECK(ini != NULL);
    if (ini == NULL) { return; }
    rpm = lt_ini_find_channel(ini, "RPMValue");
    LT_CHECK(rpm != NULL);
    lt_math_set_hooks(&hk);
    memset(&c, 0, sizeof c);
    s.n = 2;
    s.ch = (lt_math_channel *)lt_calloc(2, sizeof(lt_math_channel));
    s.ch[0].name = lt_strdup("rpm_k"); s.ch[0].units = lt_strdup("krpm");
    s.ch[0].expression = lt_strdup("RPMValue / 1000");
    s.ch[1].name = lt_strdup("rpm_k2"); s.ch[1].units = lt_strdup("");
    s.ch[1].expression = lt_strdup("rpm_k * 2");
    c.set = &s;
    c.rt = lt_ini_rt_new(ini);
    LT_CHECK(!lt_ini_rt_get(c.rt, "rpm_k", &v));        /* no hook yet */
    lt_ini_rt_set_virtual(c.rt, live_virt, &c);
    if (rpm != NULL) { lt_ini_rt_set(c.rt, rpm->index, 2500.0); }
    LT_CHECK(lt_ini_rt_get(c.rt, "rpm_k", &v));
    LT_CHECK_NEAR(v, 2.5, 1e-9);
    LT_CHECK(lt_ini_rt_get(c.rt, "rpm_k2", &v));
    LT_CHECK_NEAR(v, 5.0, 1e-9);
    if (rpm != NULL) { lt_ini_rt_set(c.rt, rpm->index, 4000.0); }
    LT_CHECK(lt_ini_rt_get(c.rt, "rpm_k", &v));
    LT_CHECK_NEAR(v, 4.0, 1e-9);
    LT_CHECK(!lt_ini_rt_get(c.rt, "nope", &v));
    LT_CHECK(lt_ini_rt_get(c.rt, "RPMValue", &v));       /* real channels unaffected */
    LT_CHECK_NEAR(v, 4000.0, 1e-9);
    /* virtual channels reach the recorder / picker channel list (INI duplicates skipped) */
    lt_datalog_set_virtual_channels(vn, vu, 2);
    LT_CHECK_INT(lt_datalog_virtual_count(), 2);
    n = lt_datalog_channels_from_ini(ini, &names, &units);
    for (k = 0; k < n; k++) {
        if (strcmp(names[k], "rpm_k") == 0) { found++; LT_CHECK_STR(units[k], "krpm"); }
        if (strcmp(names[k], "RPMValue") == 0) { found += 10; }
    }
    LT_CHECK_INT(found, 11);
    LT_CHECK_STR(names[n - 1], "rpm_k");
    lt_strv_free(names, n); lt_strv_free(units, n);
    lt_datalog_set_virtual_channels(NULL, NULL, 0);
    LT_CHECK_INT(lt_datalog_virtual_count(), 0);
    lt_ini_rt_set_virtual(c.rt, NULL, NULL);
    LT_CHECK(!lt_ini_rt_get(c.rt, "rpm_k", &v));
    lt_ini_rt_free(c.rt);
    lt_math_live_free(&c.ml);
    lt_math_set_free(&s);
    lt_math_set_hooks(NULL);
    lt_ini_free(ini);
}

int main(void)
{
    LT_RUN(msl_roundtrip);
    LT_RUN(msl_default_header_and_blank_lines);
    LT_RUN(extension_routing);
    LT_RUN(recorder_msl);
    LT_RUN(graph_setup_json);
    LT_RUN(trace_range_and_ticks);
    LT_RUN(histogram_binning);
    LT_RUN(scatter_filter);
    LT_RUN(xy_ring_and_fade);
    LT_RUN(live_math_realtime_lookup);
    LT_TEST_MAIN_END
}
