/* test_wuetrim_app.c - AutoTune heat-map contexts, WUE / Trim Table Analyze tabs in demo mode,
 * INI discovery of the WUE channels and the per-cylinder trim tables.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/at_ui.h"
#include "../src/autotune/at_wue.h"
#include "../src/autotune/at_trim.h"

static void frame(lt_app *app)
{
    lt_ui_begin(app->ui, &app->gfx, lt_plat_now_ms());
    lt_app_frame(app);
    lt_ui_end(app->ui);
}

static lt_app *demo_app(void)
{
    char buf[512];
    lt_app *app = lt_app_new();
    if (lt_app_find_demo_ini(buf, sizeof(buf)) == NULL || lt_app_start_demo(app, buf) != 0) {
        lt_app_free(app);
        return NULL;
    }
    return app;
}

static int same(lt_color a, lt_color b) { return LT_COLOR_R(a) == LT_COLOR_R(b) && LT_COLOR_G(a) == LT_COLOR_G(b) && LT_COLOR_B(a) == LT_COLOR_B(b); }

/* the change and coverage colours follow their own heat-map contexts (scheme or custom stops) */
LT_TEST(heat_contexts)
{
    lt_app *app = demo_app();
    lt_prefs *p;
    lt_color a, b, c;
    LT_CHECK(app != NULL);
    if (app == NULL) { return; }
    p = &app->settings.prefs;
    /* custom two-stop gradients per context */
    lt_strlcpy(p->heat_scheme[LT_HEAT_CHANGE], "custom", sizeof(p->heat_scheme[0]));
    p->heat_n[LT_HEAT_CHANGE] = 2;
    p->heat_stops[LT_HEAT_CHANGE][0] = 0x0000FFUL;
    p->heat_stops[LT_HEAT_CHANGE][1] = 0xFF0000UL;
    lt_strlcpy(p->heat_scheme[LT_HEAT_COVERAGE], "custom", sizeof(p->heat_scheme[0]));
    p->heat_n[LT_HEAT_COVERAGE] = 2;
    p->heat_stops[LT_HEAT_COVERAGE][0] = 0x000000UL;
    p->heat_stops[LT_HEAT_COVERAGE][1] = 0x00FF00UL;
    a = lt_at_ui_heat(app, LT_HEAT_CHANGE, 0.0);
    b = lt_at_ui_heat(app, LT_HEAT_CHANGE, 1.0);
    LT_CHECK(same(a, LT_RGB(0, 0, 255)));
    LT_CHECK(same(b, LT_RGB(255, 0, 0)));
    /* richer (positive fuel change) runs to the low end, leaner to the high end, 0 in the middle */
    LT_CHECK(same(lt_at_ui_change(app, 0.10, 0.05), a));
    LT_CHECK(same(lt_at_ui_change(app, -0.10, 0.05), b));
    c = lt_at_ui_change(app, 0.0, 0.05);
    LT_CHECK(LT_COLOR_R(c) > 100 && LT_COLOR_R(c) < 156 && LT_COLOR_B(c) > 100 && LT_COLOR_B(c) < 156);
    LT_CHECK(same(lt_at_ui_heat(app, LT_HEAT_COVERAGE, 1.0), LT_RGB(0, 255, 0)));
    LT_CHECK(same(lt_at_ui_heat(app, LT_HEAT_COVERAGE, 0.0), LT_RGB(0, 0, 0)));
    /* a named scheme in a context is used as such, independent of the value context */
    lt_strlcpy(p->heat_scheme[LT_HEAT_COVERAGE], "grayscale", sizeof(p->heat_scheme[0]));
    c = lt_at_ui_heat(app, LT_HEAT_COVERAGE, 0.3);
    LT_CHECK(same(c, lt_heat_color(lt_prefs_scheme_index("grayscale"), 0.3)));
    LT_CHECK(!same(c, lt_at_ui_heat(app, LT_HEAT_CHANGE, 0.3)));
    lt_app_free(app);
}

LT_TEST(ini_discovery)
{
    lt_app *app = demo_app();
    lt_trim_table_ref r[LT_TRIM_MAX_CYL];
    const char *lam[16];
    lt_wue_channels ch;
    size_t n, i;
    LT_CHECK(app != NULL);
    if (app == NULL) { return; }
    n = lt_trim_find_tables(app->ini, r, LT_TRIM_MAX_CYL);
    LT_CHECK(n == 12);
    for (i = 0; i < n; i++) { LT_CHECK(r[i].cylinder == (int)i + 1); }
    LT_CHECK(lt_trim_lambda_channels(app->ini, lam, 16) >= 1);
    lt_wue_channels_from_ini(app->ini, &ch);
    LT_CHECK(ch.at.clt[0] != '\0');
    LT_CHECK(ch.at.afr[0] != '\0');
    lt_app_free(app);
}

static void run_frames(lt_app *app, int n)
{
    int i;
    for (i = 0; i < n; i++) { lt_app_service(app); frame(app); }
}

/* both tabs open from their targets and analyse the simulated demo sessions */
LT_TEST(tabs_render_demo)
{
    lt_app *app = demo_app();
    LT_CHECK(app != NULL);
    if (app == NULL) { return; }
    LT_CHECK(lt_app_open_target(app, "wue-analyze-demo", NULL) == 0);
    lt_app_screenshot(app, "build/lt_test_wue.ppm", 1400, 900, 3);
    LT_CHECK(lt_app_tab_find(app, "wueanalyze", "wueanalyze") != NULL);
    LT_CHECK(lt_app_open_target(app, "trim-analyze-demo", NULL) == 0);
    lt_app_screenshot(app, "build/lt_test_trim.ppm", 1400, 900, 3);
    LT_CHECK(lt_app_tab_find(app, "trimanalyze", "trimanalyze") != NULL);
    run_frames(app, 3);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(heat_contexts);
    LT_RUN(ini_discovery);
    LT_RUN(tabs_render_demo);
    LT_TEST_MAIN_END
}
