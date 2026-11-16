/* test_gauges.c - renders every gauge painter into a canvas (gallery) and checks it is not
 * blank. Set LT_GAUGE_SHOT=path.ppm to keep the picture for visual inspection.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lt_test.h"
#include "../src/app/dash_gauges.h"

static double g_t = 0.0;

static lt_bool get_var(void *ud, const char *name, double *out)
{
    LT_UNUSED(ud);
    if (strcmp(name, "sine") == 0) { *out = 50.0 + 40.0 * sin(g_t * 0.9); return LT_TRUE; }
    if (strcmp(name, "sine2") == 0) { *out = 50.0 + 40.0 * sin(g_t * 0.6 + 1.0); return LT_TRUE; }
    return LT_FALSE;
}

static void ppm(const lt_canvas *c, const char *path)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (f == NULL) { return; }
    fprintf(f, "P6\n%d %d\n255\n", c->w, c->h);
    for (y = 0; y < c->h; y++) {
        for (x = 0; x < c->w; x++) {
            lt_u32 p = c->px[y * c->stride + x];
            fputc((int)((p >> 16) & 255), f); fputc((int)((p >> 8) & 255), f); fputc((int)(p & 255), f);
        }
    }
    fclose(f);
}

LT_TEST(gallery)
{
    static const char *names[] = { "Analog Gauge", "Basic Analog Gauge", "Circle Analog Gauge",
        "Asymmetric Sweep Gauge", "Basic Readout", "Horizontal Bar Gauge", "Vertical Bar Gauge",
        "Horizontal Line Gauge", "Horizontal Dashed Bar", "Vertical Dashed Bar", "Analog Bar Gauge",
        "Analog Moving Bar Gauge", "Histogram", "Line Graph", "Round Gauge", "Round Dashed Gauge",
        "Fuel Meter", "Tachometer", "Telemetry Stat", "Multi Channel Trend" };
    lt_dash_file *d = lt_dash_new();
    lt_dash_view *v;
    lt_canvas cv;
    lt_gfx g;
    lt_expr_env env;
    lt_expr_cache ec;
    const char *shot = getenv("LT_GAUGE_SHOT");
    int i, nz = 0, x, y, cols = 5, rows = 4, cw = 300, ch = 240;
    lt_dash_indicator *in;

    memset(&env, 0, sizeof env);
    env.get_var = get_var;
    lt_expr_cache_init(&ec);
    lt_gauges_install();
    lt_gauges_set_env(&ec, &env);
    for (i = 0; i < 20; i++) {
        lt_dash_gauge *gg = lt_dash_add_gauge(d);
        gg->painter = (lt_gauge_painter)i; LT_UNUSED(names);
        gg->title = lt_strdup(i == 17 ? "RPM x1000" : "Coolant Temp");
        gg->units = lt_strdup("C");
        gg->output_channel = lt_strdup(i == 13 || i == 19 ? "sine" : "none");
        gg->min = 0.0; gg->max = i == 17 ? 8000.0 : 100.0;
        gg->value = gg->max * (0.35 + 0.02 * i);
        if (i == 17) { gg->value = 5200.0; }
        gg->has_high_warning = LT_TRUE; gg->high_warning = gg->max * 0.75;
        gg->has_high_critical = LT_TRUE; gg->high_critical = gg->max * 0.9;
        gg->has_low_warning = (i == 0); gg->low_warning = gg->max * 0.1;
        gg->back_color = LT_RGB(28, 32, 40); gg->font_color = LT_RGB(255, 255, 255);
        gg->trim_color = LT_RGB(148, 163, 184); gg->needle_color = LT_RGB(74, 158, 248);
        gg->warn_color = LT_RGB(234, 179, 8); gg->critical_color = LT_RGB(239, 68, 68);
        gg->major_ticks = i < 3 ? 10.0 : -1.0; gg->minor_ticks = i < 3 ? 5.0 : -1.0;
        gg->value_digits = 0;
        if (i == 19) {
            lt_dash_extras_set(&gg->extras, "lt_series2_channel", "sine2");
            lt_dash_extras_set(&gg->extras, "lt_series2_color", "#ff9830");
            lt_dash_extras_set(&gg->extras, "lt_series2_label", "Second");
        }
        if (i == 2) { gg->face_angle = 180; gg->sweep_begin_degree = 180; gg->sweep_angle = 180; }
        gg->relative_x = (double)(i % cols) / cols + 0.004; gg->relative_y = (double)(i / cols) / (rows + 1) + 0.004;
        gg->relative_width = 1.0 / cols - 0.008; gg->relative_height = 1.0 / (rows + 1) - 0.008;
    }
    for (i = 0; i < 5; i++) {
        in = lt_dash_add_indicator(d);
        in->painter = (lt_ind_painter)(i % 3);
        in->on_text = lt_strdup("CHECK"); in->off_text = lt_strdup("OK");
        in->on_background_color = LT_RGB(220, 40, 40); in->off_background_color = LT_RGB(40, 60, 40);
        in->on_text_color = LT_RGB(255, 255, 255); in->off_text_color = LT_RGB(180, 200, 180);
        in->output_channel = lt_strdup(i % 2 ? "sine" : "none");
        in->value = (double)(i % 2);
        in->relative_x = (double)i / 5.0 + 0.01; in->relative_y = 4.0 / 5.0 + 0.04;
        in->relative_width = 0.16; in->relative_height = 0.1;
    }
    d->cluster.background_color = LT_RGB(0, 0, 0);
    v = lt_dash_view_new(d);
    lt_canvas_init(&cv, cols * cw, (rows + 1) * ch);
    lt_gfx_init_canvas(&g, &cv);
    for (i = 0; i < 80; i++) {
        g_t = i * 0.1;
        lt_dash_view_update(v, 100.0 * i, &env);
    }
    lt_gfx_clear(&g, LT_RGB(0, 0, 0));
    lt_dash_view_render(v, &g, lt_rect_make(0, 0, cv.w, cv.h), &env);
    for (y = 0; y < cv.h; y += 3) {
        for (x = 0; x < cv.w; x += 3) { if ((cv.px[y * cv.stride + x] & 0xFFFFFF) != 0) { nz++; } }
    }
    LT_CHECK(nz > 2000);
    if (shot != NULL) { ppm(&cv, shot); }
    lt_canvas_free(&cv);
    lt_dash_view_free(v);
    lt_dash_free(d);
    lt_expr_cache_free(&ec);
}

int main(void)
{
    LT_RUN(gallery);
    LT_TEST_MAIN_END
}
