/* test_logging.c - log analysis logic (statistics, key tracking, alert rules) and the data
 * logging features on a headless app (config, channel selection, math channels).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/app_ext.h"
#include "../src/app/feat_log.h"
#include "../src/plat/plat.h"

LT_TEST(stats_basic)
{
    double v[5];
    lt_stats s;
    v[0] = 1.0; v[1] = 2.0; v[2] = 3.0; v[3] = 4.0; v[4] = 100.0;
    LT_CHECK(lt_stats_compute(v, 5, &s) == 0);
    LT_CHECK_INT(s.n, 5);
    LT_CHECK_NEAR(s.mean, 22.0, 1e-9);
    LT_CHECK_NEAR(s.median, 3.0, 1e-9);
    LT_CHECK_NEAR(s.vmin, 1.0, 1e-9);
    LT_CHECK_NEAR(s.vmax, 100.0, 1e-9);
    LT_CHECK_NEAR(s.p25, 2.0, 1e-9);        /* ceil(.25*5)-1 = 1 */
    LT_CHECK_NEAR(s.p75, 4.0, 1e-9);
    LT_CHECK_NEAR(s.iqr, 2.0, 1e-9);
    LT_CHECK_NEAR(s.stddev, 39.0128, 0.001);
    v[0] = 2.0; v[1] = 4.0;
    LT_CHECK(lt_stats_compute(v, 2, &s) == 0);
    LT_CHECK_NEAR(s.median, 3.0, 1e-9);
    LT_CHECK_NEAR(s.stddev, 1.0, 1e-9);
    LT_CHECK(lt_stats_compute(v, 0, &s) < 0);
}

LT_TEST(stats_log_range)
{
    lt_log l;
    const char *nm[2];
    double row[2];
    lt_stats s;
    int i;
    nm[0] = "a"; nm[1] = "b";
    lt_log_init(&l);
    lt_log_set_channels(&l, nm, NULL, 2);
    for (i = 0; i < 10; i++) { row[0] = (double)i; row[1] = 5.0; lt_log_append(&l, (double)i, row); }
    LT_CHECK(lt_stats_log_channel(&l, 0, 2, 4, &s) == 0);
    LT_CHECK_NEAR(s.mean, 3.0, 1e-9);
    LT_CHECK(lt_stats_log_channel(&l, 0, 5, 100, &s) == 0);   /* clamped */
    LT_CHECK_INT(s.n, 5);
    LT_CHECK(lt_stats_log_channel(&l, 2, 0, 1, &s) < 0);
    lt_log_free(&l);
}

LT_TEST(keytrack_debounce)
{
    lt_keytrack k;
    lt_keytrack_init(&k);
    LT_CHECK_INT(lt_keytrack_update(&k, 0.0, 0.0, 100.0, 2.0), 0);
    LT_CHECK_INT(lt_keytrack_update(&k, 800.0, 100.0, 100.0, 2.0), 1);
    LT_CHECK_INT(lt_keytrack_update(&k, 0.0, 200.0, 100.0, 2.0), 0);     /* timing starts */
    LT_CHECK_INT(lt_keytrack_update(&k, 900.0, 1000.0, 100.0, 2.0), 0);  /* blip recovers */
    LT_CHECK(k.on);
    LT_CHECK_INT(lt_keytrack_update(&k, 0.0, 1100.0, 100.0, 2.0), 0);
    LT_CHECK_INT(lt_keytrack_update(&k, 0.0, 2900.0, 100.0, 2.0), 0);
    LT_CHECK_INT(lt_keytrack_update(&k, 0.0, 3200.0, 100.0, 2.0), -1);
    LT_CHECK(!k.on);
}

LT_TEST(alert_rules)
{
    lt_alert_rule r;
    double d;
    memset(&r, 0, sizeof(r));
    r.enabled = 1; r.above = 1; r.limit = 100.0;
    LT_CHECK_INT(lt_alert_rule_check(&r, 90.0), 0);
    LT_CHECK_INT(lt_alert_rule_check(&r, 101.0), 1);
    LT_CHECK_INT(lt_alert_rule_check(&r, 105.0), 0);    /* still active, no repeat */
    LT_CHECK_INT(lt_alert_rule_check(&r, 90.0), 0);
    LT_CHECK_INT(lt_alert_rule_check(&r, 101.0), 1);    /* re-armed */
    r.above = 0;
    r.active = 0;
    LT_CHECK_INT(lt_alert_rule_check(&r, 50.0), 1);
    r.enabled = 0;
    LT_CHECK_INT(lt_alert_rule_check(&r, 50.0), 0);
    LT_CHECK(lt_alert_large_change(50.0, 56.0, 5.0, 50.0, &d));
    LT_CHECK_NEAR(d, 6.0, 1e-9);
    LT_CHECK(!lt_alert_large_change(50.0, 52.0, 5.0, 10.0, &d));
    LT_CHECK(lt_alert_large_change(20.0, 23.0, 5.0, 10.0, &d));        /* 15 percent */
    LT_CHECK(!lt_alert_large_change(0.0, 0.0, 5.0, 10.0, &d));
}

static void use_tmp_config(lt_app *app, const char *dir)
{
    (void)lt_plat_mkdirs(dir);
    lt_free(app->config_dir);
    app->config_dir = lt_strdup(dir);
}

LT_TEST(logx_config_and_math)
{
    const char *dir = "build/logx_cfg";
    lt_app *app = lt_app_new();
    lt_logx *x;
    lt_math_set *ms;
    use_tmp_config(app, dir);
    (void)lt_plat_remove("build/logx_cfg/logging.conf");
    (void)lt_plat_remove("build/logx_cfg/math_channels.json");
    x = lt_logx_get(app);
    LT_CHECK_INT(x->key_rpm, 100);
    LT_CHECK_INT(x->rate_hz, 10);
    x->auto_rec = 1; x->key_rpm = 250; x->rate_hz = 25;
    lt_logx_set_excluded(x, "foo", 1);
    lt_logx_set_excluded(x, "bar", 1);
    lt_logx_set_excluded(x, "foo", 1);
    LT_CHECK_INT(x->n_off, 2);
    lt_logx_set_excluded(x, "foo", 0);
    LT_CHECK_INT(x->n_off, 1);
    LT_CHECK(lt_logx_excluded(x, "bar"));
    lt_logx_save(app);
    lt_app_free(app);
    app = lt_app_new();
    use_tmp_config(app, dir);
    x = lt_logx_get(app);
    LT_CHECK_INT(x->auto_rec, 1);
    LT_CHECK_INT(x->key_rpm, 250);
    LT_CHECK_INT(x->rate_hz, 25);
    LT_CHECK_INT(x->n_off, 1);
    LT_CHECK(lt_logx_excluded(x, "bar"));
    ms = lt_logx_math(app);
    LT_CHECK_INT(ms->n, 0);
    ms->ch = (lt_math_channel *)lt_calloc(1, sizeof(lt_math_channel));
    ms->n = 1;
    ms->ch[0].name = lt_strdup("boost"); ms->ch[0].units = lt_strdup("kPa");
    ms->ch[0].expression = lt_strdup("map - 100");
    LT_CHECK(lt_logx_math_save(app) == 0);
    lt_app_free(app);
    app = lt_app_new();
    use_tmp_config(app, dir);
    ms = lt_logx_math(app);
    LT_CHECK_INT(ms->n, 1);
    if (ms->n == 1) { LT_CHECK_STR(ms->ch[0].expression, "map - 100"); }
    lt_app_free(app);
}

LT_TEST(logx_registered)
{
    lt_app *app = lt_app_new();
    int n = lt_ext_n_settings_pages(app), i, found = 0;
    for (i = 0; i < n; i++) { if (strcmp(lt_ext_settings_title(app, i), "Data Logging") == 0) { found = 1; } }
    LT_CHECK(found);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(stats_basic);
    LT_RUN(stats_log_range);
    LT_RUN(keytrack_debounce);
    LT_RUN(alert_rules);
    LT_RUN(logx_config_and_math);
    LT_RUN(logx_registered);
    LT_TEST_MAIN_END
}
