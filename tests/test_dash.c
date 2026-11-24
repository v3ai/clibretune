/* test_dash.c - dash module: colours, painters, .dash/.gauge IO, validation, templates,
 * INI conversion, runtime state helpers. Ports the spec 05 section 8 test list.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lt.h"
#include "dash/dash.h"
#include "lt_test.h"
#include "dash/dash_priv.h"

#define FIXTURE "tests/fixtures/dash/basic.dash"
#define DEMO "tests/fixtures/ini/demo.ini"

static lt_dash_file *parse_str(const char *s)
{
    char err[160];
    return lt_dash_parse(s, strlen(s), err, sizeof err);
}

static lt_dash_file *roundtrip(const lt_dash_file *d)
{
    lt_buf b;
    char err[160];
    lt_dash_file *r;
    lt_buf_init(&b);
    lt_dash_write(d, &b);
    r = lt_dash_parse(b.data, b.len, err, sizeof err);
    lt_buf_free(&b);
    return r;
}

static lt_bool dbl_eq(double a, double b) { return a == b || (lt_isnan(a) && lt_isnan(b)); }
static lt_bool str_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return a == b;
    return strcmp(a, b) == 0;
}
static lt_bool extras_eq(const lt_dash_extras *a, const lt_dash_extras *b)
{
    size_t i;
    if (a->n != b->n) return LT_FALSE;
    for (i = 0; i < a->n; i++)
        if (strcmp(a->a[i].k, b->a[i].k) != 0 || strcmp(a->a[i].v, b->a[i].v) != 0) return LT_FALSE;
    return LT_TRUE;
}

static lt_bool gauge_eq(const lt_dash_gauge *a, const lt_dash_gauge *b)
{
    return str_eq(a->id, b->id) && a->painter == b->painter && str_eq(a->gauge_style, b->gauge_style) &&
        str_eq(a->output_channel, b->output_channel) && str_eq(a->title, b->title) &&
        str_eq(a->units, b->units) && dbl_eq(a->value, b->value) && dbl_eq(a->min, b->min) &&
        dbl_eq(a->max, b->max) && str_eq(a->min_vp, b->min_vp) && str_eq(a->max_vp, b->max_vp) &&
        a->has_default_min == b->has_default_min && a->has_default_max == b->has_default_max &&
        a->peg_limits == b->peg_limits &&
        a->has_low_warning == b->has_low_warning && a->has_high_warning == b->has_high_warning &&
        a->has_low_critical == b->has_low_critical && a->has_high_critical == b->has_high_critical &&
        (!a->has_low_warning || a->low_warning == b->low_warning) &&
        (!a->has_high_warning || a->high_warning == b->high_warning) &&
        (!a->has_low_critical || a->low_critical == b->low_critical) &&
        (!a->has_high_critical || a->high_critical == b->high_critical) &&
        a->back_color == b->back_color && a->font_color == b->font_color &&
        a->trim_color == b->trim_color && a->warn_color == b->warn_color &&
        a->critical_color == b->critical_color && a->needle_color == b->needle_color &&
        a->value_digits == b->value_digits && a->label_digits == b->label_digits &&
        str_eq(a->font_family, b->font_family) && a->font_size_adjustment == b->font_size_adjustment &&
        a->italic_font == b->italic_font && a->sweep_angle == b->sweep_angle &&
        a->start_angle == b->start_angle && a->face_angle == b->face_angle &&
        a->sweep_begin_degree == b->sweep_begin_degree && a->counter_clockwise == b->counter_clockwise &&
        dbl_eq(a->major_ticks, b->major_ticks) && dbl_eq(a->minor_ticks, b->minor_ticks) &&
        dbl_eq(a->relative_x, b->relative_x) && dbl_eq(a->relative_y, b->relative_y) &&
        dbl_eq(a->relative_width, b->relative_width) && dbl_eq(a->relative_height, b->relative_height) &&
        a->border_width == b->border_width && a->shortest_size == b->shortest_size &&
        a->shape_locked_to_aspect == b->shape_locked_to_aspect &&
        a->antialiasing_on == b->antialiasing_on &&
        str_eq(a->background_image_file_name, b->background_image_file_name) &&
        str_eq(a->needle_image_file_name, b->needle_image_file_name) &&
        a->history_delay == b->history_delay && a->needle_smoothing == b->needle_smoothing &&
        str_eq(a->short_click_action, b->short_click_action) &&
        str_eq(a->long_click_action, b->long_click_action) &&
        a->display_value_at_180 == b->display_value_at_180 &&
        str_eq(a->enabled_condition, b->enabled_condition) && a->peak_hold == b->peak_hold &&
        a->has_hysteresis == b->has_hysteresis && a->run_demo == b->run_demo &&
        extras_eq(&a->extras, &b->extras);
}

static lt_bool ind_eq(const lt_dash_indicator *a, const lt_dash_indicator *b)
{
    return str_eq(a->id, b->id) && a->painter == b->painter &&
        str_eq(a->output_channel, b->output_channel) && str_eq(a->on_text, b->on_text) &&
        str_eq(a->off_text, b->off_text) && a->on_text_color == b->on_text_color &&
        a->off_text_color == b->off_text_color &&
        a->on_background_color == b->on_background_color &&
        a->off_background_color == b->off_background_color &&
        dbl_eq(a->relative_x, b->relative_x) && dbl_eq(a->relative_width, b->relative_width) &&
        str_eq(a->enabled_condition, b->enabled_condition) &&
        str_eq(a->ecu_configuration_name, b->ecu_configuration_name) && extras_eq(&a->extras, &b->extras);
}

static lt_bool dash_eq(const lt_dash_file *a, const lt_dash_file *b)
{
    size_t i;
    const lt_dash_cluster *ca = &a->cluster, *cb = &b->cluster;
    if (!str_eq(a->author, b->author) || !str_eq(a->company, b->company) ||
        !str_eq(a->write_date, b->write_date) || !str_eq(a->file_format, b->file_format) ||
        !str_eq(a->firmware_signature, b->firmware_signature)) return LT_FALSE;
    if (ca->anti_aliasing != cb->anti_aliasing || ca->force_aspect != cb->force_aspect ||
        !dbl_eq(ca->force_aspect_width, cb->force_aspect_width) ||
        !dbl_eq(ca->force_aspect_height, cb->force_aspect_height) ||
        ca->has_dither != cb->has_dither || ca->background_color != cb->background_color ||
        !str_eq(ca->background_image_file_name, cb->background_image_file_name) ||
        ca->background_image_style != cb->background_image_style ||
        !str_eq(ca->layout, cb->layout) || !str_eq(ca->enabled_condition, cb->enabled_condition) ||
        !extras_eq(&ca->extras, &cb->extras) || ca->n_images != cb->n_images ||
        ca->n_comps != cb->n_comps) return LT_FALSE;
    for (i = 0; i < ca->n_images; i++)
        if (!str_eq(ca->images[i].file_name, cb->images[i].file_name) ||
            !str_eq(ca->images[i].data_base64, cb->images[i].data_base64) ||
            ca->images[i].type != cb->images[i].type) return LT_FALSE;
    for (i = 0; i < ca->n_comps; i++) {
        if ((ca->comps[i].gauge != NULL) != (cb->comps[i].gauge != NULL)) return LT_FALSE;
        if (ca->comps[i].gauge) {
            if (!gauge_eq(ca->comps[i].gauge, cb->comps[i].gauge)) return LT_FALSE;
        } else if (!ind_eq(ca->comps[i].indicator, cb->comps[i].indicator)) {
            return LT_FALSE;
        }
    }
    return LT_TRUE;
}

/* ---------------------------------------------------------------- colours */

LT_TEST(colors_argb)
{
    lt_color c = lt_color_from_argb(-16777216);
    LT_CHECK(LT_COLOR_A(c) == 255 && LT_COLOR_R(c) == 0 && LT_COLOR_G(c) == 0 && LT_COLOR_B(c) == 0);
    c = lt_color_from_argb(-1);
    LT_CHECK(c == LT_RGB(255, 255, 255));
    c = lt_color_from_argb(-65536);
    LT_CHECK(c == LT_RGB(255, 0, 0));
    c = lt_color_from_argb(-6710887);
    LT_CHECK(c == LT_RGB(153, 153, 153));
    LT_CHECK(lt_color_to_argb(LT_RGB(0, 0, 0)) == -16777216);
    LT_CHECK(lt_color_to_argb(LT_RGB(255, 255, 255)) == -1);
    LT_CHECK(lt_color_to_argb(LT_RGB(255, 0, 0)) == -65536);
    LT_CHECK(lt_color_to_argb(LT_RGB(153, 153, 153)) == -6710887);
    LT_CHECK(lt_color_to_argb(LT_RGBA(1, 2, 3, 0)) == 0x010203);
    LT_CHECK(lt_color_from_argb(lt_color_to_argb(LT_RGBA(9, 8, 7, 6))) == LT_RGBA(9, 8, 7, 6));
}

LT_TEST(colors_css)
{
    lt_color c = 0;
    char out[10];
    LT_CHECK(lt_color_from_css("#ff8000", &c));
    LT_CHECK(c == LT_RGB(255, 128, 0));
    LT_CHECK(lt_color_from_css("##11223344", &c));
    LT_CHECK(c == LT_RGBA(0x11, 0x22, 0x33, 0x44));
    LT_CHECK(lt_color_from_css("ABCDEF", &c));
    LT_CHECK(c == LT_RGB(0xAB, 0xCD, 0xEF));
    LT_CHECK(!lt_color_from_css("#fff", &c));
    LT_CHECK(!lt_color_from_css("#12345", &c));
    LT_CHECK(!lt_color_from_css("#gg0000", &c));
    LT_CHECK(!lt_color_from_css("", &c));
    lt_color_to_css(LT_RGB(255, 128, 0), out);
    LT_CHECK_STR(out, "#ff8000");
    lt_color_to_css(LT_RGBA(1, 2, 3, 4), out);
    LT_CHECK_STR(out, "#01020304");
}

LT_TEST(colors_lighten_darken)
{
    lt_color c = LT_RGB(100, 200, 10);
    lt_color l = lt_color_lighten(c, 60);   /* amt = round(153) = 153 */
    LT_CHECK(l == LT_RGB(253, 255, 163));
    LT_CHECK(lt_color_darken(c, 60) == LT_RGB(0, 47, 0));
    LT_CHECK(lt_color_lighten(c, -60) == lt_color_darken(c, 60));
    LT_CHECK(lt_color_lighten(LT_RGBA(1, 1, 1, 3), 10) == LT_RGB(27, 27, 27));  /* alpha dropped */
}

/* ---------------------------------------------------------------- painters */

LT_TEST(painters)
{
    LT_CHECK(lt_painter_from_ts("Analog Gauge") == LT_GP_ANALOG);
    LT_CHECK(lt_painter_from_ts("  Tachometer ") == LT_GP_TACHOMETER);
    LT_CHECK(lt_painter_from_ts("Round Gauge") == LT_GP_ROUND);
    LT_CHECK(lt_painter_from_ts("Round Analog Gauge") == LT_GP_ROUND);
    LT_CHECK(lt_painter_from_ts("com.efiAnalytics.apps.ts.dashboard.renderers.RoundAnalogGaugePainter") == LT_GP_ROUND);
    LT_CHECK(lt_painter_from_ts("Round Dashed Gauge") == LT_GP_ROUND_DASHED);
    LT_CHECK(lt_painter_from_ts("Fuel Meter") == LT_GP_FUEL_METER);
    LT_CHECK(lt_painter_from_ts("Horizontal Dashed Bar Gauge") == LT_GP_HDASHED);
    LT_CHECK(lt_painter_from_ts("Vertical Dashed Bar Gauge") == LT_GP_VDASHED);
    LT_CHECK(lt_painter_from_ts("Asymetric Sweep Gauge") == LT_GP_ASYM_SWEEP);
    LT_CHECK(lt_painter_from_ts("AsymetricSweepGaugePainter") == LT_GP_ASYM_SWEEP);
    LT_CHECK(lt_painter_from_ts("Circle Analog Gauge") == LT_GP_CIRCLE_ANALOG);
    LT_CHECK(lt_painter_from_ts("CircleAnalogGaugePainter") == LT_GP_CIRCLE_ANALOG);
    LT_CHECK(lt_painter_from_ts("Basic Analog Gauge") == LT_GP_BASIC_ANALOG);
    LT_CHECK(lt_painter_from_ts("com.x.AnalogGaugePainter") == LT_GP_ANALOG);
    LT_CHECK(lt_painter_from_ts("com.x.HorizontalBarPainter") == LT_GP_HBAR);
    LT_CHECK(lt_painter_from_ts("Vertical Bar Gauge") == LT_GP_VBAR);
    LT_CHECK(lt_painter_from_ts("Horizontal Line Gauge") == LT_GP_HLINE);
    LT_CHECK(lt_painter_from_ts("Analog Bar Gauge") == LT_GP_ANALOG_BAR);
    LT_CHECK(lt_painter_from_ts("Analog Moving Bar Gauge") == LT_GP_ANALOG_MOVING_BAR);
    LT_CHECK(lt_painter_from_ts("Histogram") == LT_GP_HISTOGRAM);
    LT_CHECK(lt_painter_from_ts("Line Graph") == LT_GP_LINE_GRAPH);
    LT_CHECK(lt_painter_from_ts("Telemetry Stat") == LT_GP_TELEMETRY_STAT);
    LT_CHECK(lt_painter_from_ts("Multi Channel Trend") == LT_GP_MULTI_TREND);
    LT_CHECK(lt_painter_from_ts("nonsense") == LT_GP_BASIC_READOUT);
    LT_CHECK(lt_painter_from_ts("") == LT_GP_BASIC_READOUT);
    LT_CHECK(lt_painter_from_ts(NULL) == LT_GP_BASIC_READOUT);
    /* every painter's TS string parses back to itself */
    {
        int i;
        for (i = 0; i <= (int)LT_GP_MULTI_TREND; i++)
            LT_CHECK(lt_painter_from_ts(lt_painter_to_ts((lt_gauge_painter)i)) == (lt_gauge_painter)i);
    }
    LT_CHECK_STR(lt_painter_to_ts(LT_GP_ASYM_SWEEP), "Asymetric Sweep Gauge");
    LT_CHECK_STR(lt_painter_to_ts(LT_GP_ROUND), "Round Analog Gauge");
    LT_CHECK(lt_indpainter_from_ts("LED") == LT_IP_LED);
    LT_CHECK(lt_indpainter_from_ts("led") == LT_IP_LED);
    LT_CHECK(lt_indpainter_from_ts("x.LedIndicatorPainter") == LT_IP_LED);
    LT_CHECK(lt_indpainter_from_ts("Bulb Indicator") == LT_IP_BULB);
    LT_CHECK(lt_indpainter_from_ts("x.BulbIndicatorPainter") == LT_IP_BULB);
    LT_CHECK(lt_indpainter_from_ts("Basic Rectangle Indicator") == LT_IP_RECTANGLE);
    LT_CHECK(lt_indpainter_from_ts("zzz") == LT_IP_RECTANGLE);
    LT_CHECK_STR(lt_indpainter_to_ts(LT_IP_LED), "LED");
    LT_CHECK_STR(lt_indpainter_to_ts(LT_IP_BULB), "Bulb Indicator");
    LT_CHECK_STR(lt_indpainter_to_ts(LT_IP_RECTANGLE), "Basic Rectangle Indicator");
}

LT_TEST(defaults)
{
    lt_dash_gauge g;
    lt_dash_indicator in;
    lt_dash_file *d;
    lt_dash_gauge_init(&g);
    LT_CHECK(g.painter == LT_GP_BASIC_READOUT);
    LT_CHECK_STR(g.gauge_style, "Basic Readout");
    LT_CHECK_NEAR(g.max, 100, 0);
    LT_CHECK(g.peg_limits && g.antialiasing_on);
    LT_CHECK(g.sweep_angle == 270 && g.start_angle == 135 && g.face_angle == 360 &&
             g.sweep_begin_degree == 135);
    LT_CHECK(g.trim_color == LT_RGB(102, 102, 153) && g.needle_color == LT_RGB(255, 102, 0));
    LT_CHECK(g.history_delay == 15000 && g.border_width == 3 && g.shortest_size == 50);
    LT_CHECK_NEAR(g.relative_width, 0.25, 0);
    LT_CHECK_NEAR(g.major_ticks, -1, 0);
    lt_dash_gauge_free(&g);
    lt_dash_indicator_init(&in);
    LT_CHECK_STR(in.on_text, "ON");
    LT_CHECK_STR(in.off_text, "OFF");
    LT_CHECK(in.off_text_color == LT_RGB(51, 51, 51) && in.on_background_color == LT_RGB(0, 255, 0));
    LT_CHECK(LT_COLOR_A(in.off_background_color) == 0);
    LT_CHECK_NEAR(in.relative_height, 0.05, 0);
    lt_dash_indicator_free(&in);
    d = lt_dash_new();
    LT_CHECK_STR(d->author, "clibretune");
    LT_CHECK_STR(d->company, "clibretune Project");
    LT_CHECK_STR(d->file_format, "3.0");
    LT_CHECK(d->cluster.anti_aliasing && !d->cluster.force_aspect);
    LT_CHECK(d->cluster.background_image_style == LT_BG_TILE);
    LT_CHECK(strlen(d->write_date) == 19);
    lt_dash_add_gauge(d);
    lt_dash_add_indicator(d);
    lt_dash_add_gauge(d);
    LT_CHECK_INT(d->cluster.n_comps, 3);
    lt_dash_remove(d, 1);
    LT_CHECK_INT(d->cluster.n_comps, 2);
    LT_CHECK(d->cluster.comps[1].gauge != NULL);
    lt_dash_remove(d, 9);
    LT_CHECK_INT(d->cluster.n_comps, 2);
    lt_dash_free(d);
}

LT_TEST(extras_sorted)
{
    lt_dash_extras e;
    memset(&e, 0, sizeof e);
    lt_dash_extras_set(&e, "b", "2");
    lt_dash_extras_set(&e, "a", "1");
    lt_dash_extras_set(&e, "c", "3");
    lt_dash_extras_set(&e, "b", "22");
    LT_CHECK_INT(e.n, 3);
    LT_CHECK_STR(e.a[0].k, "a");
    LT_CHECK_STR(e.a[1].k, "b");
    LT_CHECK_STR(e.a[2].k, "c");
    LT_CHECK_STR(lt_dash_extras_get(&e, "b"), "22");
    LT_CHECK(lt_dash_extras_get(&e, "zz") == NULL);
    while (e.n) {
        lt_free(e.a[--e.n].k);
        lt_free(e.a[e.n].v);
    }
    lt_free(e.a);
}

/* ---------------------------------------------------------------- parsing */

LT_TEST(parse_simple_dash)
{
    const char *x =
        "<?xml version=\"1.0\"?>\n<dsh xmlns=\"http://www.EFIAnalytics.com/:dsh\">\n"
        "<bibliography author=\"Test\" company=\"Co\" writeDate=\"2025-01-01\"/>\n"
        "<versionInfo fileFormat=\"3.0\" firmwareSignature=\"speeduino\"/>\n"
        "<gaugeCluster antiAliasing=\"true\"><dashComp type=\"Gauge\">"
        "<Title type=\"String\">RPM</Title><Min type=\"double\">0.0</Min>"
        "<Max type=\"double\">8000.0</Max>"
        "<GaugePainter type=\"String\">Analog Gauge</GaugePainter></dashComp></gaugeCluster></dsh>";
    lt_dash_file *d = parse_str(x);
    LT_CHECK(d != NULL);
    if (!d) return;
    LT_CHECK_STR(d->author, "Test");
    LT_CHECK_STR(d->company, "Co");
    LT_CHECK_STR(d->write_date, "2025-01-01");
    LT_CHECK_STR(d->file_format, "3.0");
    LT_CHECK_STR(d->firmware_signature, "speeduino");
    LT_CHECK_INT(d->cluster.n_comps, 1);
    LT_CHECK(d->cluster.comps[0].gauge != NULL);
    LT_CHECK(d->cluster.comps[0].gauge->painter == LT_GP_ANALOG);
    LT_CHECK_STR(d->cluster.comps[0].gauge->title, "RPM");
    LT_CHECK_NEAR(d->cluster.comps[0].gauge->max, 8000, 0);
    lt_dash_free(d);
}

LT_TEST(parse_fixture)
{
    lt_dash_file *d = lt_dash_load(FIXTURE);
    size_t i, ng = 0, ni = 0;
    lt_bool has_tach = LT_FALSE, has_vbar = LT_FALSE, has_read = LT_FALSE, has_asym = LT_FALSE;
    LT_CHECK(d != NULL);
    if (!d) return;
    for (i = 0; i < d->cluster.n_comps; i++) {
        if (d->cluster.comps[i].gauge) {
            lt_gauge_painter p = d->cluster.comps[i].gauge->painter;
            ng++;
            if (p == LT_GP_TACHOMETER) has_tach = LT_TRUE;
            if (p == LT_GP_VBAR) has_vbar = LT_TRUE;
            if (p == LT_GP_BASIC_READOUT) has_read = LT_TRUE;
            if (p == LT_GP_ASYM_SWEEP) has_asym = LT_TRUE;
        } else {
            ni++;
        }
    }
    LT_CHECK_INT(ng, 4);
    LT_CHECK_INT(ni, 2);
    LT_CHECK(has_tach && has_vbar && has_read && has_asym);
    {
        const lt_dash_gauge *g = d->cluster.comps[0].gauge;
        LT_CHECK_STR(g->title, "Engine Speed");
        LT_CHECK_STR(g->units, "rpm");
        LT_CHECK_NEAR(g->min, 0, 0);
        LT_CHECK_NEAR(g->max, 8000, 0);
        LT_CHECK(g->painter == LT_GP_TACHOMETER);
        LT_CHECK(g->peg_limits);
        LT_CHECK(LT_COLOR_R(g->needle_color) != 0);   /* -65536 */
        LT_CHECK(g->needle_color == LT_RGB(255, 0, 0));
        LT_CHECK(g->font_color == LT_RGB(255, 255, 255));
        LT_CHECK(g->trim_color == LT_RGB(153, 153, 153));
        LT_CHECK_STR(lt_dash_extras_get(&g->extras, "MinorTick"), "250");
        LT_CHECK_STR(lt_dash_extras_get(&g->extras, "MajorTick"), "1000");
        LT_CHECK_STR(lt_dash_extras_get(&g->extras, "WarnThreshold"), "6500");
        LT_CHECK_STR(lt_dash_extras_get(&g->extras, "CritThreshold"), "7200");
        LT_CHECK_NEAR(g->relative_x, 0.0125, 1e-12);
    }
    {
        const lt_dash_gauge *g = d->cluster.comps[1].gauge;
        LT_CHECK_STR(g->title, "Coolant Temp");
        LT_CHECK(g->has_high_warning && g->has_high_critical);
        LT_CHECK_NEAR(g->high_warning, 100, 0);
        LT_CHECK_NEAR(g->high_critical, 110, 0);
        LT_CHECK(g->painter == LT_GP_VBAR);
        LT_CHECK(!g->peg_limits);
        LT_CHECK_STR(g->units, "\302\260" "C");
    }
    LT_CHECK_STR(d->cluster.comps[2].gauge->title, "Air/Fuel Ratio");
    LT_CHECK_INT(d->cluster.comps[2].gauge->value_digits, 2);
    LT_CHECK(d->cluster.comps[2].gauge->has_low_warning);
    LT_CHECK_STR(d->cluster.comps[3].gauge->title, "Manifold Pressure");
    {
        const lt_dash_indicator *a = d->cluster.comps[4].indicator;
        const lt_dash_indicator *b = d->cluster.comps[5].indicator;
        LT_CHECK_STR(a->on_text, "Battery OK");
        LT_CHECK_STR(b->on_text, "Check Engine");
        LT_CHECK(a->painter == LT_IP_BULB && b->painter == LT_IP_RECTANGLE);
        LT_CHECK(a->on_background_color == LT_RGB(0, 255, 0));
        LT_CHECK_STR(lt_dash_extras_get(&a->extras, "Condition"), "battery > 12.0");
    }
    lt_dash_free(d);
}

LT_TEST(parse_edge_cases)
{
    lt_dash_file *d;
    char err[160];
    LT_CHECK(lt_dash_parse("<dsh><gaugeCluster></dsh>", 25, err, sizeof err) == NULL);
    LT_CHECK(err[0] != '\0');
    LT_CHECK(lt_dash_parse("", 0, err, sizeof err) == NULL);
    LT_CHECK(lt_dash_parse("hello", 5, err, sizeof err) == NULL);
    d = parse_str("<dashboard><gaugeCluster/></dashboard>");
    LT_CHECK(d != NULL && d->cluster.n_comps == 0);
    lt_dash_free(d);
    d = parse_str("<dashboard/>");
    LT_CHECK(d != NULL && d->cluster.n_comps == 0);
    LT_CHECK_STR(d->file_format, "3.0");
    lt_dash_free(d);
    /* minimal gauge: no painter -> BasicReadout; unknown type -> Gauge */
    d = parse_str("<d><gaugeCluster><dashComp><Title>T</Title><OutputChannel>rpm</OutputChannel>"
                  "</dashComp></gaugeCluster></d>");
    LT_CHECK(d != NULL && d->cluster.n_comps == 1);
    LT_CHECK(d->cluster.comps[0].gauge && d->cluster.comps[0].gauge->painter == LT_GP_BASIC_READOUT);
    LT_CHECK_STR(d->cluster.comps[0].gauge->output_channel, "rpm");
    lt_dash_free(d);
    /* type containing "indicator" in any case is an indicator */
    d = parse_str("<d><dashComp type=\"MyIndicator\"><OnText>X</OnText></dashComp></d>");
    LT_CHECK(d && d->cluster.comps[0].indicator && d->cluster.comps[0].indicator->on_text[0] == 'X');
    lt_dash_free(d);
}

LT_TEST(parse_property_rules)
{
    lt_dash_file *d;
    char xml[1400];
    static const char part1[] =
        "<d><dashComp type=\"Gauge\">"
        "<Max>abc</Max><Min> 5 </Min><RelativeWidth>zz</RelativeWidth>"
        "<ValueDigits>2.5</ValueDigits><PegLimits>yes</PegLimits><ItalicFont>true</ItalicFont>"
        "<LowWarning>n/a</LowWarning><HighWarning>9.5</HighWarning>"
        "<MinVP></MinVP><MaxVP>maxv</MaxVP>"
        "<BackgroundImageFileName>null</BackgroundImageFileName>"
        "<NeedleImageFileName>n.png</NeedleImageFileName>";
    static const char part1b[] =
        "<ShortClickAction></ShortClickAction><EnabledCondition></EnabledCondition>"
        "<Hysteresis>0.5</Hysteresis><Frobnicate type=\"String\">  hi </Frobnicate>"
        "<NeedleLength>0.9</NeedleLength><NeedlePivotOffsetX>1</NeedlePivotOffsetX>"
        "<NeedlePivotOffsetY>2</NeedlePivotOffsetY>"
        "<Title>a &amp; b &lt; c</Title><Units><![CDATA[x<y]]></Units>"
        "</dashComp>";
    static const char part2[] =
        "<dashComp type=\"Indicator\"><RelativeWidth>q</RelativeWidth><EcuConfigurationName>cfg"
        "</EcuConfigurationName><Title>ignored</Title><Painter>LED</Painter></dashComp></d>";

    const lt_dash_gauge *g;
    const lt_dash_indicator *in;
    lt_strlcpy(xml, part1, sizeof xml);
    lt_strlcat(xml, part1b, sizeof xml);
    lt_strlcat(xml, part2, sizeof xml);
    d = parse_str(xml);
    LT_CHECK(d != NULL);
    if (!d) return;
    g = d->cluster.comps[0].gauge;
    LT_CHECK_NEAR(g->max, 100, 0);              /* garbage -> default */
    LT_CHECK_NEAR(g->min, 5, 0);
    LT_CHECK_NEAR(g->relative_width, 0.25, 0);
    LT_CHECK_INT(g->value_digits, 0);
    LT_CHECK(g->peg_limits);                    /* "yes" is not a bool */
    LT_CHECK(g->italic_font);
    LT_CHECK(!g->has_low_warning);
    LT_CHECK(g->has_high_warning && g->high_warning == 9.5);
    LT_CHECK(g->min_vp == NULL);
    LT_CHECK_STR(g->max_vp, "maxv");
    LT_CHECK(g->background_image_file_name == NULL);
    LT_CHECK_STR(g->needle_image_file_name, "n.png");
    LT_CHECK(g->short_click_action == NULL && g->enabled_condition == NULL);
    LT_CHECK(g->has_hysteresis && g->hysteresis == 0.5);
    LT_CHECK_STR(lt_dash_extras_get(&g->extras, "Frobnicate"), "hi");
    LT_CHECK(g->has_needle_length && g->needle_length == 0.9);
    LT_CHECK(g->has_pivot && g->needle_pivot_offset_x == 1 && g->needle_pivot_offset_y == 2);
    LT_CHECK_STR(g->title, "a & b < c");
    LT_CHECK_STR(g->units, "x<y");
    in = d->cluster.comps[1].indicator;
    LT_CHECK_NEAR(in->relative_width, 0.1, 0);  /* indicator default */
    LT_CHECK_STR(in->ecu_configuration_name, "cfg");
    LT_CHECK_STR(lt_dash_extras_get(&in->extras, "Title"), "ignored");
    LT_CHECK(in->painter == LT_IP_LED);
    lt_dash_free(d);
}

LT_TEST(show_history_and_face_angle)
{
    lt_dash_file *d = parse_str("<d><dashComp><ShowHistory>true</ShowHistory></dashComp>"
                                "<dashComp><FaceAngle>180</FaceAngle></dashComp>"
                                "<dashComp><FaceAngle>junk</FaceAngle></dashComp>"
                                "<dashComp></dashComp></d>");
    LT_CHECK(d != NULL);
    LT_CHECK(d->cluster.comps[0].gauge->peak_hold);
    LT_CHECK_INT(d->cluster.comps[0].gauge->face_angle, 360);
    LT_CHECK(!d->cluster.comps[1].gauge->peak_hold);
    LT_CHECK_INT(d->cluster.comps[1].gauge->face_angle, 180);
    LT_CHECK_INT(d->cluster.comps[2].gauge->face_angle, 360);
    LT_CHECK_INT(d->cluster.comps[3].gauge->face_angle, 360);
    {
        lt_dash_file *r = roundtrip(d);
        LT_CHECK(r != NULL && r->cluster.comps[0].gauge->peak_hold);
        LT_CHECK_INT(r->cluster.comps[1].gauge->face_angle, 180);
        lt_dash_free(r);
    }
    lt_dash_free(d);
}

LT_TEST(parse_color_rules)
{
    lt_dash_file *d = parse_str(
        "<d><dashComp>"
        "<BackColor alpha=\"10\" red=\"20\" green=\"30\" blue=\"40\"/>"
        "<FontColor alpha=\"10\" red=\"20\" green=\"30\" blue=\"40\">-1</FontColor>"
        "<TrimColor red=\"1\" green=\"2\" blue=\"3\"/>"
        "<WarnColor alpha=\"x\" red=\"300\" green=\"2\" blue=\"3\"/>"
        "<CriticalColor>-16777216</CriticalColor>"
        "<NeedleColor red=\"5\">text</NeedleColor>"
        "</dashComp></d>");
    const lt_dash_gauge *g = d->cluster.comps[0].gauge;
    LT_CHECK(g->back_color == LT_RGBA(20, 30, 40, 10));
    LT_CHECK(g->font_color == LT_RGB(255, 255, 255));                /* text overrides attrs */
    LT_CHECK(g->trim_color == LT_RGBA(1, 2, 3, 0));                  /* no alpha attr -> 0 */
    LT_CHECK(g->warn_color == LT_RGBA(0, 2, 3, 255));                /* alpha fail 255, red fail 0 */
    LT_CHECK(g->critical_color == LT_RGB(0, 0, 0));
    LT_CHECK(g->needle_color == LT_RGBA(5, 0, 0, 0));
    lt_dash_free(d);
}

LT_TEST(second_cluster_replaces_attrs)
{
    lt_dash_file *d = parse_str(
        "<d><gaugeCluster forceAspect=\"true\" forceAspectWidth=\"4\" forceAspectHeight=\"3\" Foo=\"x\">"
        "<dashComp><Id>a</Id></dashComp></gaugeCluster>"
        "<gaugeCluster antiAliasing=\"false\"><dashComp><Id>b</Id></dashComp></gaugeCluster></d>");
    LT_CHECK(d != NULL);
    LT_CHECK(!d->cluster.force_aspect);
    LT_CHECK(!d->cluster.anti_aliasing);
    LT_CHECK_INT(d->cluster.extras.n, 0);
    LT_CHECK_INT(d->cluster.n_comps, 2);
    lt_dash_free(d);
}

LT_TEST(gauge_file)
{
    const char *x =
        "<gauge xmlns=\"http://www.EFIAnalytics.com/:gauge\">"
        "<bibliography author=\"A\" company=\"B\" writeDate=\"C\"/>"
        "<versionInfo fileFormat=\"1.5\"/>"
        "<imageFile fileName=\"f.png\" imageId=\"i\" type=\"GIF\"> QUJD </imageFile>"
        "<gauge><dashComp type=\"Gauge\"><Title>RPM</Title><OutputChannel>rpm</OutputChannel>"
        "</dashComp></gauge></gauge>";
    lt_gauge_file gf;
    lt_buf b;
    lt_gauge_file g2;
    char err[160];
    LT_CHECK_INT(lt_gaugefile_parse(x, strlen(x), &gf, err, sizeof err), 0);
    LT_CHECK_STR(gf.gauge.title, "RPM");
    LT_CHECK_STR(gf.gauge.output_channel, "rpm");
    LT_CHECK_STR(gf.author, "A");
    LT_CHECK_STR(gf.file_format, "1.5");
    LT_CHECK_INT(gf.n_images, 1);
    LT_CHECK(gf.images[0].type == LT_RES_GIF);
    LT_CHECK_STR(gf.images[0].data_base64, "QUJD");
    lt_buf_init(&b);
    lt_gaugefile_write(&gf, &b);
    LT_CHECK(strstr(b.data, "<gauge xmlns=\"http://www.EFIAnalytics.com/:gauge\">") != NULL);
    LT_CHECK_INT(lt_gaugefile_parse(b.data, b.len, &g2, err, sizeof err), 0);
    LT_CHECK(gauge_eq(&gf.gauge, &g2.gauge));
    LT_CHECK_STR(g2.file_format, "1.5");
    LT_CHECK_INT(g2.n_images, 1);
    lt_gaugefile_free(&g2);
    lt_buf_free(&b);
    lt_gaugefile_free(&gf);
    /* no dashComp -> default gauge; bad xml -> error */
    x = "<gauge><bibliography author=\"Z\"/></gauge>";
    LT_CHECK_INT(lt_gaugefile_parse(x, strlen(x), &gf, err, sizeof err), 0);
    LT_CHECK(gf.gauge.painter == LT_GP_BASIC_READOUT);
    LT_CHECK_STR(gf.file_format, "1.0");
    lt_gaugefile_free(&gf);
    LT_CHECK(lt_gaugefile_parse("<gauge>", 7, &gf, err, sizeof err) == LT_EDATA);
    /* first dashComp only, indicator type still read as a gauge */
    x = "<gauge><gauge><dashComp type=\"Indicator\"><Id>one</Id></dashComp>"
        "<dashComp><Id>two</Id></dashComp></gauge></gauge>";
    LT_CHECK_INT(lt_gaugefile_parse(x, strlen(x), &gf, err, sizeof err), 0);
    LT_CHECK_STR(gf.gauge.id, "one");
    lt_gaugefile_free(&gf);
}

/* ---------------------------------------------------------------- writing */

LT_TEST(write_format)
{
    lt_dash_file *d = lt_dash_new();
    lt_dash_gauge *g = lt_dash_add_gauge(d);
    lt_buf b;
    lt_dash_file *r;
    lt_free(g->title);
    g->title = lt_strdup("RPM");
    g->max = 8000.0;
    g->min = 0.0;
    g->relative_width = 0.45;
    g->enabled_condition = lt_strdup("rpm > 3000");
    g->peak_hold = LT_TRUE;
    lt_free(d->write_date);
    d->write_date = lt_strdup("2025-01-02 03:04:05");
    d->cluster.force_aspect = LT_TRUE;
    d->cluster.force_aspect_width = 16.0;
    d->cluster.force_aspect_height = 16.5;
    lt_buf_init(&b);
    lt_dash_write(d, &b);
    LT_CHECK(strncmp(b.data, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n", 54) == 0);
    LT_CHECK(strstr(b.data, "<dsh xmlns=\"http://www.EFIAnalytics.com/:dsh\">") != NULL);
    LT_CHECK(strstr(b.data, "    <bibliography author=\"clibretune\" company=\"clibretune Project\" "
                            "writeDate=\"2025-01-02 03:04:05\"/>") != NULL);
    LT_CHECK(strstr(b.data, "<versionInfo fileFormat=\"3.0\"/>") != NULL);
    LT_CHECK(strstr(b.data, "forceAspect=\"true\" forceAspectWidth=\"16\" forceAspectHeight=\"16.5\"") != NULL);
    LT_CHECK(strstr(b.data, "clusterBackgroundColor=\"-16777216\" clusterBackgroundImageStyle=\"Tile\"") != NULL);
    LT_CHECK(strstr(b.data, "<Title type=\"String\">RPM</Title>") != NULL);
    LT_CHECK(strstr(b.data, "<Max type=\"double\">8000</Max>") != NULL);
    LT_CHECK(strstr(b.data, "<Min type=\"double\">0</Min>") != NULL);
    LT_CHECK(strstr(b.data, "<RelativeWidth type=\"double\">0.45</RelativeWidth>") != NULL);
    LT_CHECK(strstr(b.data, "<PegLimits type=\"boolean\">true</PegLimits>") != NULL);
    LT_CHECK(strstr(b.data, "<ValueDigits type=\"int\">0</ValueDigits>") != NULL);
    LT_CHECK(strstr(b.data, "<BackColor type=\"java.awt.Color\" alpha=\"255\" red=\"0\" green=\"0\" "
                            "blue=\"0\">-16777216</BackColor>") != NULL);
    LT_CHECK(strstr(b.data, "<BackgroundImageFileName type=\"String\">null</BackgroundImageFileName>") != NULL);
    LT_CHECK(strstr(b.data, "<ShowHistory type=\"boolean\">true</ShowHistory>") != NULL);
    LT_CHECK(strstr(b.data, "<EnabledCondition type=\"String\"><![CDATA[rpm > 3000]]></EnabledCondition>") != NULL);
    LT_CHECK(strstr(b.data, "<GaugePainter type=\"String\">Basic Readout</GaugePainter>") != NULL);
    LT_CHECK(strstr(b.data, "<MinVP") == NULL && strstr(b.data, "<Hysteresis") == NULL);
    /* element order: Id < Title < Units < Value < OutputChannel < Min */
    LT_CHECK(strstr(b.data, "<Id ") < strstr(b.data, "<Title "));
    LT_CHECK(strstr(b.data, "<OutputChannel ") < strstr(b.data, "<Min "));
    LT_CHECK(strstr(b.data, "<NeedleColor ") < strstr(b.data, "<RelativeX "));
    LT_CHECK(strstr(b.data, "<RunDemo ") < strstr(b.data, "<EnabledCondition "));
    r = lt_dash_parse(b.data, b.len, NULL, 0);
    LT_CHECK(r != NULL);
    LT_CHECK(dash_eq(d, r));
    LT_CHECK_STR(r->cluster.comps[0].gauge->enabled_condition, "rpm > 3000");
    lt_dash_free(r);
    lt_buf_free(&b);
    lt_dash_free(d);
}

LT_TEST(fmt_double)
{
    {
        lt_dash_file *d = lt_dash_new();
        lt_dash_gauge *g = lt_dash_add_gauge(d);
        lt_buf buf;
        g->value = 0.1;
        g->min = -2.5;
        g->max = 1e20;
        g->major_ticks = 1.0 / 3.0;
        g->minor_ticks = 1e-7;
        g->relative_x = 123456.789;
        lt_buf_init(&buf);
        lt_dash_write(d, &buf);
        LT_CHECK(strstr(buf.data, ">0.1</Value>") != NULL);
        LT_CHECK(strstr(buf.data, ">-2.5</Min>") != NULL);
        LT_CHECK(strstr(buf.data, ">100000000000000000000</Max>") != NULL);
        LT_CHECK(strstr(buf.data, ">0.3333333333333333</MajorTicks>") != NULL);
        LT_CHECK(strstr(buf.data, ">0.0000001</MinorTicks>") != NULL);
        LT_CHECK(strstr(buf.data, ">123456.789</RelativeX>") != NULL);
        {
            lt_dash_file *r = lt_dash_parse(buf.data, buf.len, NULL, 0);
            LT_CHECK(r && dash_eq(d, r));
            lt_dash_free(r);
        }
        lt_buf_free(&buf);
        lt_dash_free(d);
    }
}

LT_TEST(fmt_f64_exact)
{
    char b[400];
    double v, x;
    int i;
    static const struct { double v; const char *s; } k[] = {
        { 0.0, "0" }, { 8000.0, "8000" }, { 0.45, "0.45" }, { 16.5, "16.5" }, { 16.0, "16" },
        { -2.5, "-2.5" }, { 0.1, "0.1" }, { 0.1 + 0.2, "0.30000000000000004" },
        { 1.0 / 3.0, "0.3333333333333333" }, { 1e-7, "0.0000001" }, { 1e21, "1000000000000000000000" },
        { 123456.789, "123456.789" }, { 9.5, "9.5" }, { 99.99, "99.99" }, { 0.999, "0.999" },
        { 1e15, "1000000000000000" }, { 2.0 / 3.0, "0.6666666666666666" }
    };
    for (i = 0; i < (int)(sizeof k / sizeof k[0]); i++) {
        dash_fmt_f64(b, sizeof b, k[i].v);
        LT_CHECK_STR(b, k[i].s);
    }
    dash_fmt_f64(b, sizeof b, 5e-324);
    LT_CHECK(strtod(b, NULL) == 5e-324 && strlen(b) > 300);
    dash_fmt_f64(b, sizeof b, 1.7976931348623157e308);
    LT_CHECK(strtod(b, NULL) == 1.7976931348623157e308);
    /* pseudo random doubles round-trip bit exactly */
    v = 0.7071067811865476;
    for (i = 0; i < 3000; i++) {
        v = v * 3.9999 * (1.0 - v) + 0.000001 * (double)i;
        x = v * (i % 7 == 0 ? 1e-9 : (i % 5 == 0 ? 1e9 : 1.0));
        if (i % 2) x = -x;
        dash_fmt_f64(b, sizeof b, x);
        if (strtod(b, NULL) != x) {
            LT_CHECK(0);
            break;
        }
        LT_CHECK(strchr(b, 'e') == NULL);
    }
}

LT_TEST(roundtrip_fixture_and_extras)
{
    lt_dash_file *d = lt_dash_load(FIXTURE), *r, *r2;
    lt_buf b1, b2;
    LT_CHECK(d != NULL);
    if (!d) return;
    d->cluster.background_image_file_name = lt_strdup("bg.png");
    d->cluster.background_image_style = LT_BG_FIT;
    d->cluster.layout = lt_strdup("grid");
    d->cluster.enabled_condition = lt_strdup("a && b < 3");
    lt_dash_extras_set(&d->cluster.extras, "zzCustom", "v&\"1\"");
    lt_dash_extras_set(&d->cluster.extras, "aaCustom", "<tag>");
    d->cluster.has_dither = LT_TRUE;
    d->cluster.dither_color = LT_RGB(1, 2, 3);
    r = roundtrip(d);
    LT_CHECK(r != NULL);
    LT_CHECK(dash_eq(d, r));
    LT_CHECK_STR(r->cluster.background_image_file_name, "bg.png");
    LT_CHECK(r->cluster.background_image_style == LT_BG_FIT);
    LT_CHECK_STR(r->cluster.layout, "grid");
    LT_CHECK_STR(r->cluster.enabled_condition, "a && b < 3");
    LT_CHECK_STR(lt_dash_extras_get(&r->cluster.extras, "zzCustom"), "v&\"1\"");
    LT_CHECK_STR(lt_dash_extras_get(&r->cluster.extras, "aaCustom"), "<tag>");
    LT_CHECK(r->cluster.has_dither && r->cluster.dither_color == LT_RGB(1, 2, 3));
    /* write -> parse -> write is stable text */
    lt_buf_init(&b1);
    lt_buf_init(&b2);
    lt_dash_write(r, &b1);
    r2 = lt_dash_parse(b1.data, b1.len, NULL, 0);
    lt_dash_write(r2, &b2);
    LT_CHECK_INT(b1.len, b2.len);
    LT_CHECK(b1.len == b2.len && memcmp(b1.data, b2.data, b1.len) == 0);
    /* extras of gauges survive and are written sorted */
    LT_CHECK(strstr(b1.data, "<CritThreshold type=\"String\">7200</CritThreshold>") != NULL);
    LT_CHECK(strstr(b1.data, "<CritThreshold ") < strstr(b1.data, "<MajorTick "));
    LT_CHECK(strstr(b1.data, "<MajorTick ") < strstr(b1.data, "<MinorTick "));
    LT_CHECK(strstr(b1.data, "aaCustom=\"&lt;tag&gt;\"") != NULL);
    lt_buf_free(&b1);
    lt_buf_free(&b2);
    lt_dash_free(r2);
    lt_dash_free(r);
    lt_dash_free(d);
}

LT_TEST(images_roundtrip)
{
    lt_dash_file *d = lt_dash_new(), *r;
    lt_dash_gauge *g = lt_dash_add_gauge(d);
    lt_dash_cluster *c = &d->cluster;
    lt_dash_indicator *in = lt_dash_add_indicator(d);
    c->images = (lt_dash_image *)lt_calloc(2, sizeof(lt_dash_image));
    c->n_images = 2;
    c->images[0].file_name = lt_strdup("a.png");
    c->images[0].image_id = lt_strdup("id1");
    c->images[0].type = LT_RES_PNG;
    c->images[0].data_base64 = lt_strdup("AAAA");
    c->images[1].file_name = lt_strdup("f.ttf");
    c->images[1].image_id = lt_strdup("id2");
    c->images[1].type = LT_RES_TTF;
    c->images[1].data_base64 = lt_strdup("BBBB");
    g->background_image_file_name = lt_strdup("a.png");
    in->on_image_file_name = lt_strdup("a.png");
    in->enabled_condition = lt_strdup("x > 1");
    in->ecu_configuration_name = lt_strdup("cfgname");
    in->painter = LT_IP_BULB;
    in->on_text_color = LT_RGBA(1, 2, 3, 4);
    r = roundtrip(d);
    LT_CHECK(r != NULL && dash_eq(d, r));
    LT_CHECK_INT(r->cluster.n_images, 2);
    LT_CHECK(r->cluster.images[1].type == LT_RES_TTF);
    LT_CHECK_STR(r->cluster.images[0].data_base64, "AAAA");
    lt_dash_free(r);
    lt_dash_free(d);
}

LT_TEST(save_load_file)
{
    lt_dash_file *d = lt_dash_template_basic(), *r;
    const char *path = "test_dash_tmp.dash";
    LT_CHECK_INT(lt_dash_save(d, path), 0);
    r = lt_dash_load(path);
    LT_CHECK(r != NULL && dash_eq(d, r));
    lt_dash_free(r);
    lt_dash_free(d);
    remove(path);
    LT_CHECK(lt_dash_load("build/definitely/not/here.dash") == NULL);
}

/* ---------------------------------------------------------------- validation */

LT_TEST(validate)
{
    lt_dash_file *d = lt_dash_new();
    lt_dash_report r;
    lt_dash_gauge *g1, *g2, *g3;
    lt_dash_indicator *in;
    const char *chans[] = { "rpm", "map" };
    size_t i, n_overlap = 0;

    lt_dash_validate(d, NULL, 0, &r);
    LT_CHECK(!r.is_valid);
    LT_CHECK_INT(r.n, 1);
    LT_CHECK(r.problems[0].kind == LT_DI_EMPTY && r.problems[0].is_error);
    lt_dash_report_free(&r);

    g1 = lt_dash_add_gauge(d);
    g2 = lt_dash_add_gauge(d);
    g3 = lt_dash_add_gauge(d);
    in = lt_dash_add_indicator(d);
    lt_free(g1->id); g1->id = lt_strdup("g1");
    lt_free(g2->id); g2->id = lt_strdup("g2");
    lt_free(g3->id); g3->id = lt_strdup("g3");
    lt_free(g1->output_channel); g1->output_channel = lt_strdup("rpm");
    lt_free(g2->output_channel); g2->output_channel = lt_strdup("bogus");
    lt_free(g3->output_channel); g3->output_channel = lt_strdup("rpm");
    lt_free(in->output_channel); in->output_channel = lt_strdup("nochan");
    g1->relative_x = 0; g1->relative_y = 0; g1->relative_width = .3; g1->relative_height = .3;
    g2->relative_x = .7; g2->relative_y = 0; g2->relative_width = .2; g2->relative_height = .2;
    g3->relative_x = .9; g3->relative_y = .9; g3->relative_width = .04; g3->relative_height = .2;
    g2->min = 10; g2->max = 10;                    /* invalid range */
    g1->background_image_file_name = lt_strdup("missing.png");
    lt_dash_validate(d, chans, 2, &r);
    LT_CHECK(!r.is_valid);
    LT_CHECK_INT(r.gauge_count, 3);
    LT_CHECK_INT(r.indicator_count, 1);
    LT_CHECK_INT(r.unique_channels, 2);          /* rpm, bogus */
    {
        int unk = 0, rng = 0, img = 0, tiny = 0, oob = 0;
        for (i = 0; i < r.n; i++) {
            switch (r.problems[i].kind) {
            case LT_DI_UNKNOWN_CHANNEL: unk++; LT_CHECK(r.problems[i].is_error); break;
            case LT_DI_INVALID_RANGE: rng++; LT_CHECK_STR(r.problems[i].gauge_id, "g2"); break;
            case LT_DI_MISSING_IMAGE: img++; break;
            case LT_DI_TINY_GAUGE: tiny++; LT_CHECK(!r.problems[i].is_error); break;
            case LT_DI_OUT_OF_BOUNDS: oob++; break;
            case LT_DI_OVERLAP: n_overlap++; break;
            default: break;
            }
        }
        LT_CHECK_INT(unk, 2);   /* gauge bogus + indicator nochan */
        LT_CHECK_INT(rng, 1);
        LT_CHECK_INT(img, 1);
        LT_CHECK_INT(tiny, 1);  /* g3 width .04 */
        LT_CHECK_INT(oob, 1);   /* g3 y+h = 1.1 and x+w = .94 */
        LT_CHECK_INT(n_overlap, 0);
    }
    lt_dash_report_free(&r);
    /* skipping the channel check */
    g2->min = 0;
    lt_free(g1->background_image_file_name);
    g1->background_image_file_name = NULL;
    lt_dash_validate(d, NULL, 0, &r);
    LT_CHECK(r.is_valid);
    lt_dash_report_free(&r);
    /* overlap: touching edges count */
    g2->relative_x = .3; g2->relative_y = 0; g2->relative_width = .2; g2->relative_height = .2;
    lt_dash_validate(d, NULL, 0, &r);
    n_overlap = 0;
    for (i = 0; i < r.n; i++)
        if (r.problems[i].kind == LT_DI_OVERLAP) n_overlap++;
    LT_CHECK_INT(n_overlap, 1);
    LT_CHECK(r.is_valid);
    lt_dash_report_free(&r);
    /* embedded images + fonts */
    d->cluster.images = (lt_dash_image *)lt_calloc(1, sizeof(lt_dash_image));
    d->cluster.n_images = 1;
    d->cluster.images[0].file_name = lt_strdup("bg.png");
    d->cluster.images[0].type = LT_RES_TTF;
    g1->background_image_file_name = lt_strdup("bg.png");
    lt_dash_validate(d, NULL, 0, &r);
    LT_CHECK_INT(r.embedded_image_count, 1);
    LT_CHECK(r.has_embedded_fonts);
    {
        int img = 0;
        for (i = 0; i < r.n; i++) if (r.problems[i].kind == LT_DI_MISSING_IMAGE) img++;
        LT_CHECK_INT(img, 0);
    }
    lt_dash_report_free(&r);
    /* performance warning above 50 components */
    for (i = 0; i < 48; i++) lt_dash_add_indicator(d);
    lt_dash_validate(d, NULL, 0, &r);
    {
        int perf = 0;
        for (i = 0; i < r.n; i++) if (r.problems[i].kind == LT_DI_PERFORMANCE) perf++;
        LT_CHECK_INT(perf, 1);
    }
    lt_dash_report_free(&r);
    lt_dash_free(d);
}

/* ---------------------------------------------------------------- templates */

static void check_template(lt_dash_file *d, size_t n)
{
    lt_dash_report r;
    lt_dash_file *rt;
    size_t i;
    LT_CHECK(d != NULL);
    if (!d) return;
    LT_CHECK_INT(d->cluster.n_comps, n);
    lt_dash_validate(d, NULL, 0, &r);
    LT_CHECK(r.is_valid);
    for (i = 0; i < r.n; i++)
        LT_CHECK(r.problems[i].kind != LT_DI_INVALID_RANGE && r.problems[i].kind != LT_DI_EMPTY);
    lt_dash_report_free(&r);
    rt = roundtrip(d);
    LT_CHECK(rt != NULL && dash_eq(d, rt));
    lt_dash_free(rt);
    LT_CHECK_STR(d->author, "clibretune");
    LT_CHECK_STR(d->company, "clibretune Project");
    LT_CHECK_STR(d->file_format, "3.0");
    LT_CHECK(strlen(d->write_date) == 10);
    LT_CHECK(d->cluster.background_image_style == LT_BG_STRETCH);
}

LT_TEST(template_basic)
{
    lt_dash_file *d = lt_dash_template_basic();
    const lt_dash_gauge *g;
    check_template(d, 10);
    g = d->cluster.comps[0].gauge;
    LT_CHECK_STR(g->id, "rpm");
    LT_CHECK_STR(g->title, "ENGINE RPM");
    LT_CHECK(g->painter == LT_GP_TACHOMETER);
    LT_CHECK_NEAR(g->max, 8000, 0);
    LT_CHECK(g->has_high_warning && g->high_warning == 6500 && g->high_critical == 7200);
    LT_CHECK(!g->has_low_warning);
    LT_CHECK_INT(g->font_size_adjustment, 2);
    LT_CHECK(g->font_color == LT_RGB(255, 255, 255) && g->needle_color == LT_RGB(251, 191, 36));
    LT_CHECK(g->back_color == LT_RGB(28, 32, 40) && g->trim_color == LT_RGB(148, 163, 184));
    LT_CHECK(g->warn_color == LT_RGB(234, 179, 8) && g->critical_color == LT_RGB(239, 68, 68));
    LT_CHECK_INT(g->start_angle, 135);
    LT_CHECK_INT(g->sweep_angle, 270);
    LT_CHECK_NEAR(g->major_ticks, 8, 0);
    LT_CHECK_NEAR(g->relative_width, .45, 0);
    g = d->cluster.comps[1].gauge;
    LT_CHECK_STR(g->output_channel, "afr");
    LT_CHECK_STR(g->units, ":1");
    LT_CHECK(g->painter == LT_GP_ANALOG);
    LT_CHECK(g->has_low_warning && g->low_warning == 11.5 && g->low_critical == 10.5);
    LT_CHECK_INT(g->start_angle, 225);
    LT_CHECK_INT(g->value_digits, 1);
    g = d->cluster.comps[2].gauge;
    LT_CHECK_STR(g->units, "\302\260" "C");
    LT_CHECK_NEAR(g->min, -40, 0);
    LT_CHECK(g->painter == LT_GP_HBAR);
    g = d->cluster.comps[9].gauge;
    LT_CHECK_STR(g->output_channel, "pulseWidth");
    LT_CHECK_INT(g->value_digits, 2);
    LT_CHECK_NEAR(g->relative_width, .21, 0);
    LT_CHECK(d->cluster.background_color == LT_RGB(12, 14, 20));
    LT_CHECK(!d->cluster.force_aspect);
    lt_dash_free(d);
}

LT_TEST(template_tuning)
{
    lt_dash_file *d = lt_dash_template_tuning();
    const lt_dash_gauge *g;
    check_template(d, 15);
    g = d->cluster.comps[0].gauge;
    LT_CHECK(g->painter == LT_GP_ASYM_SWEEP);
    LT_CHECK_INT(g->start_angle, 180);
    LT_CHECK_INT(g->sweep_angle, 180);
    LT_CHECK(g->needle_color == LT_RGB(56, 189, 248));
    g = d->cluster.comps[1].gauge;
    LT_CHECK(g->painter == LT_GP_ROUND);
    LT_CHECK_INT(g->value_digits, 2);
    g = d->cluster.comps[6].gauge;
    LT_CHECK_STR(g->id, "lambda_hist");
    LT_CHECK_STR(g->output_channel, "lambda");
    LT_CHECK(g->painter == LT_GP_LINE_GRAPH);
    LT_CHECK_NEAR(g->min, 0.7, 0);
    LT_CHECK_STR(g->units, "\316\273");
    LT_CHECK(g->has_low_warning && g->low_warning == .75 && g->high_warning == 1.1);
    LT_CHECK_STR(d->cluster.comps[9].gauge->output_channel, "dutyCycle");
    LT_CHECK_STR(d->cluster.comps[13].gauge->output_channel, "afrTarget");
    LT_CHECK(d->cluster.comps[13].gauge->font_color == LT_RGB(148, 163, 184));
    LT_CHECK(d->cluster.comps[14].gauge->needle_color == LT_RGB(148, 163, 184));
    lt_dash_free(d);
}

LT_TEST(template_telemetry)
{
    lt_dash_file *d = lt_dash_template_telemetry();
    size_t i, stats = 0, trends = 0, lines = 0;
    const lt_dash_gauge *g;
    LT_CHECK(d != NULL);
    if (!d) return;
    LT_CHECK(d->cluster.force_aspect);
    LT_CHECK_NEAR(d->cluster.force_aspect_width, 16, 0);
    LT_CHECK_NEAR(d->cluster.force_aspect_height, 10, 0);
    LT_CHECK(d->cluster.background_color == LT_RGB(15, 17, 20));
    for (i = 0; i < d->cluster.n_comps; i++) {
        g = d->cluster.comps[i].gauge;
        LT_CHECK(g != NULL);
        LT_CHECK_INT(g->shortest_size, 0);
        LT_CHECK_INT(g->border_width, 1);
        LT_CHECK(g->back_color == LT_RGB(26, 28, 33));
        if (g->painter == LT_GP_TELEMETRY_STAT) {
            stats++;
            LT_CHECK_INT(g->font_size_adjustment, -1);
        }
        if (g->painter == LT_GP_MULTI_TREND) trends++;
        if (g->painter == LT_GP_LINE_GRAPH) {
            lines++;
            LT_CHECK_INT(g->font_size_adjustment, 0);
        }
    }
    LT_CHECK_INT(stats, 22);
    LT_CHECK_INT(trends, 4);
    LT_CHECK_INT(lines, 16);
    LT_CHECK_INT(d->cluster.n_comps, 42);
    g = d->cluster.comps[0].gauge;
    LT_CHECK_STR(g->id, "tl_rpm");
    LT_CHECK_NEAR(g->relative_x, .005, 1e-12);
    LT_CHECK_NEAR(g->relative_width, .095, 1e-12);
    LT_CHECK(g->font_color == LT_RGB(87, 148, 242) && g->needle_color == g->font_color);
    g = d->cluster.comps[9].gauge;
    LT_CHECK_STR(g->id, "tl_duty");
    LT_CHECK_NEAR(g->relative_width, .090, 1e-12);
    g = d->cluster.comps[10 + 3].gauge;
    LT_CHECK_STR(g->id, "tl_bst");
    LT_CHECK_NEAR(g->relative_y, .060 + .037 * 3, 1e-12);
    g = d->cluster.comps[22].gauge;
    LT_CHECK_STR(g->id, "tl_trend_engine");
    LT_CHECK_STR(lt_dash_extras_get(&g->extras, "lt_series1_label"), "RPM");
    LT_CHECK_STR(lt_dash_extras_get(&g->extras, "lt_series2_channel"), "map");
    LT_CHECK_STR(lt_dash_extras_get(&g->extras, "lt_series2_color"), "#33b5ff");
    LT_CHECK_STR(lt_dash_extras_get(&g->extras, "lt_series2_min"), "0");
    LT_CHECK_STR(lt_dash_extras_get(&g->extras, "lt_series2_max"), "250");
    LT_CHECK(lt_dash_extras_get(&g->extras, "lt_series9_channel") == NULL);
    LT_CHECK_STR(lt_dash_extras_get(&g->extras, "lt_series8_channel"), "ve");
    g = d->cluster.comps[26].gauge;           /* first sparkline */
    LT_CHECK_STR(g->id, "tl_sp_rpm");
    LT_CHECK_NEAR(g->relative_y, .510, 1e-12);
    g = d->cluster.comps[41].gauge;
    LT_CHECK_NEAR(g->relative_x, .005 + (.242 + .006) * 3, 1e-12);
    LT_CHECK_NEAR(g->relative_y, .510 + (.115 + .008) * 3, 1e-12);
    lt_dash_free(d);
    d = lt_dash_template_telemetry();
    check_template(d, 42);
    lt_dash_free(d);
}

/* ---------------------------------------------------------------- INI */

LT_TEST(gauge_from_ini)
{
    const char *txt =
        "[GaugeConfigurations]\n"
        "gaugeCategory = Main\n"
        "tachometer = rpm, \"Engine Speed\", \"RPM\", 0, {rpmhigh}, 300, 600, {rpmwarn}, 7000, 0\n"
        "tpsGauge = tps, \"TPS\", \"%\", 0, 100, 0, 0, 100, 100, 1\n"
        "[OutputChannels]\nrpm = scalar, U16, 0, \"RPM\", 1, 0\n";
    lt_ini *ini = lt_ini_load_string(txt, strlen(txt), NULL, NULL);
    const lt_ini_gauge *src;
    lt_dash_gauge g;
    lt_map vars;
    lt_expr_env env;
    double hi = 8000, warn = 6500;
    LT_CHECK(ini != NULL);
    if (!ini) return;
    src = lt_ini_find_gauge(ini, "tachometer");
    LT_CHECK(src != NULL);
    lt_dash_gauge_init(&g);
    lt_dash_gauge_from_ini(src, &g, NULL);
    LT_CHECK_STR(g.id, "tachometer");
    LT_CHECK_STR(g.output_channel, "rpm");
    LT_CHECK_STR(g.title, "Engine Speed");
    LT_CHECK(g.painter == LT_GP_TACHOMETER);
    LT_CHECK_NEAR(g.max, 100, 0);               /* expr unresolved: fallback */
    lt_map_init(&vars, LT_FALSE);
    lt_map_put(&vars, "rpmhigh", &hi);
    lt_map_put(&vars, "rpmwarn", &warn);
    lt_expr_env_from_map(&env, &vars);
    lt_dash_gauge_from_ini(src, &g, &env);
    LT_CHECK_NEAR(g.max, 8000, 0);
    LT_CHECK(g.has_high_warning && g.high_warning == 6500);
    LT_CHECK(g.has_high_critical && g.high_critical == 7000);
    LT_CHECK(g.has_low_critical && g.low_critical == 300);
    LT_CHECK(g.has_low_warning && g.low_warning == 600);
    lt_dash_gauge_from_ini(lt_ini_find_gauge(ini, "tpsGauge"), &g, &env);
    LT_CHECK(g.painter == LT_GP_HBAR);
    LT_CHECK(!g.has_high_warning && !g.has_high_critical && !g.has_low_warning);
    LT_CHECK_INT(g.value_digits, 1);
    lt_dash_gauge_free(&g);
    lt_map_free(&vars);
    lt_ini_free(ini);
}

LT_TEST(from_frontpage)
{
    lt_ini *ini = lt_ini_load_file(DEMO, NULL);
    lt_dash_file *d;
    lt_dash_report r;
    size_t i, ng = 0, ni = 0;
    LT_CHECK(ini != NULL);
    if (!ini) return;
    d = lt_dash_from_frontpage(ini, NULL);
    LT_CHECK(d != NULL);
    for (i = 0; i < d->cluster.n_comps; i++) {
        if (d->cluster.comps[i].gauge) ng++;
        else ni++;
    }
    LT_CHECK(ng >= 6 && ng <= 8);
    LT_CHECK_INT(ni, ini->frontpage->n_inds);
    LT_CHECK(ni > 0);
    lt_dash_validate(d, NULL, 0, &r);
    LT_CHECK(r.is_valid);
    {
        size_t ov = 0;
        for (i = 0; i < r.n; i++)
            if (r.problems[i].kind == LT_DI_OVERLAP || r.problems[i].kind == LT_DI_OUT_OF_BOUNDS) ov++;
        LT_CHECK_INT(ov, 0);
    }
    lt_dash_report_free(&r);
    for (i = 0; i < d->cluster.n_comps; i++)
        if (d->cluster.comps[i].indicator) {
            const lt_dash_indicator *in = d->cluster.comps[i].indicator;
            LT_CHECK(lt_dash_extras_get(&in->extras, "lt_expression") != NULL);
            LT_CHECK(LT_COLOR_A(in->on_background_color) == 255);
        }
    {
        lt_dash_file *rt = roundtrip(d);
        LT_CHECK(rt && dash_eq(d, rt));
        lt_dash_free(rt);
    }
    lt_dash_free(d);
    lt_ini_free(ini);
    d = lt_dash_from_frontpage(NULL, NULL);
    LT_CHECK(d != NULL && d->cluster.n_comps == 0);
    lt_dash_free(d);
}

/* ---------------------------------------------------------------- runtime */

LT_TEST(ema)
{
    LT_CHECK_NEAR(lt_gauge_ema_step(5, 10, 0), 5, 0);
    LT_CHECK_NEAR(lt_gauge_ema_step(5, 10, -3), 5, 0);
    LT_CHECK_NEAR(lt_gauge_ema_step(0, 100, 100), 100 * (1 - exp(-100.0 / 120.0)), 1e-9);
    /* dt clamped to 100 ms */
    LT_CHECK_NEAR(lt_gauge_ema_step(0, 100, 5000), 100 * (1 - exp(-100.0 / 120.0)), 1e-9);
    LT_CHECK_NEAR(lt_gauge_ema_step(0, 100, 100), lt_gauge_ema_step(0, 100, 1000), 1e-12);
    LT_CHECK_NEAR(lt_gauge_ema_step(7, 7, 50), 7, 1e-12);
}

LT_TEST(spike)
{
    LT_CHECK(!lt_gauge_is_spike(100, 0, 5, 5));
    LT_CHECK(!lt_gauge_is_spike(100, 0, 10, 0));
    LT_CHECK(lt_gauge_is_spike(16, 0, 0, 100));
    LT_CHECK(!lt_gauge_is_spike(15, 0, 0, 100));
    LT_CHECK(lt_gauge_is_spike(0, 16, 0, 100));
}

LT_TEST(peak_hold)
{
    lt_dash_gauge g;
    lt_peak_state s;
    lt_dash_gauge_init(&g);
    g.history_value = 500;
    LT_CHECK_NEAR(lt_gauge_seed_peak(&g, 42, 0, 100), 42, 0);     /* not peak_hold */
    g.peak_hold = LT_TRUE;
    LT_CHECK_NEAR(lt_gauge_seed_peak(&g, 42, 0, 100), 100, 0);    /* clamped high */
    g.history_value = -5;
    LT_CHECK_NEAR(lt_gauge_seed_peak(&g, 42, 0, 100), 0, 0);
    g.history_value = 30;
    LT_CHECK_NEAR(lt_gauge_seed_peak(&g, 42, 0, 100), 30, 0);
    lt_dash_gauge_free(&g);

    memset(&s, 0, sizeof s);
    s.peak = 30;
    LT_CHECK_NEAR(lt_gauge_peak_step(&s, 10, 1000, 15000), 30, 0);    /* first frame starts clock */
    LT_CHECK(s.started && s.clock_ms == 1000);
    LT_CHECK_NEAR(lt_gauge_peak_step(&s, 50, 2000, 15000), 50, 0);    /* rising ratchets */
    LT_CHECK_NEAR(lt_gauge_peak_step(&s, 20, 10000, 15000), 50, 0);   /* held */
    LT_CHECK_NEAR(lt_gauge_peak_step(&s, 20, 16999, 15000), 50, 0);
    LT_CHECK_NEAR(lt_gauge_peak_step(&s, 20, 17000, 15000), 20, 0);   /* decays after delay */
    LT_CHECK(s.clock_ms == 17000);
    memset(&s, 0, sizeof s);
    s.peak = 80;
    LT_CHECK_NEAR(lt_gauge_peak_step(&s, 1, 0, 0), 80, 0);
    LT_CHECK_NEAR(lt_gauge_peak_step(&s, 1, 1e9, 0), 80, 0);          /* delay <= 0 holds forever */
    LT_CHECK_NEAR(lt_gauge_peak_step(&s, 1, 1e9, -5), 80, 0);
}

LT_TEST(stagger)
{
    int counter = 0, i, a, b;
    a = lt_gauge_stagger_slot("rpm", NULL);
    b = lt_gauge_stagger_slot("rpm", &counter);
    LT_CHECK_INT(a, b);
    LT_CHECK(a >= 0 && a < 16);
    LT_CHECK_INT(counter, 0);
    /* djb2("a") = 5381*33+97 = 177670 -> 177670 % 16 = 6 */
    LT_CHECK_INT(lt_gauge_stagger_slot("a", NULL), 6);
    for (i = 0; i < 20; i++) LT_CHECK_INT(lt_gauge_stagger_slot("", &counter), i % 16);
    LT_CHECK_INT(counter, 20);
    LT_CHECK(lt_gauge_stagger_slot("a_long_channel_name_that_wraps_int32_xxxxxxxx", NULL) < 16);
}

LT_TEST(arc_geometry)
{
    lt_dash_gauge g;
    lt_gauge_arc a;
    lt_dash_gauge_init(&g);
    lt_gauge_resolve_arc(&g, &a);
    LT_CHECK_NEAR(a.start_deg, 135, 0);
    LT_CHECK_NEAR(a.sweep_deg, 270, 0);
    LT_CHECK_NEAR(a.face_deg, 360, 0);
    LT_CHECK(a.full_circle && !a.ccw);
    LT_CHECK_NEAR(a.face_start_deg, 135 + 135 - 180, 0);
    g.face_angle = 180;
    lt_gauge_resolve_arc(&g, &a);
    LT_CHECK_NEAR(a.sweep_deg, 180, 0);                 /* clamped to face */
    LT_CHECK(!a.full_circle);
    LT_CHECK_NEAR(a.face_start_deg, 135 + 90 - 90, 0);
    g.face_angle = 182;
    g.sweep_angle = 180;
    lt_gauge_resolve_arc(&g, &a);
    LT_CHECK_NEAR(a.sweep_deg, 180, 0);
    LT_CHECK_NEAR(a.face_start_deg, (135 + 90) - 91, 0);   /* 1 deg margin each side */
    g.face_angle = 30;
    lt_gauge_resolve_arc(&g, &a);
    LT_CHECK_NEAR(a.face_deg, 360, 0);                  /* < 45 -> full */
    LT_CHECK(a.full_circle);
    g.face_angle = 999;
    lt_gauge_resolve_arc(&g, &a);
    LT_CHECK_NEAR(a.face_deg, 360, 0);
    g.face_angle = 360;
    g.sweep_angle = 0;
    lt_gauge_resolve_arc(&g, &a);
    LT_CHECK_NEAR(a.sweep_deg, 1, 0);
    g.sweep_angle = 270;
    g.counter_clockwise = LT_TRUE;
    lt_gauge_resolve_arc(&g, &a);
    LT_CHECK(a.ccw);
    LT_CHECK_NEAR(a.face_start_deg, (135 - 135) - 180, 0);
    lt_dash_gauge_free(&g);
}

LT_TEST(zones_hysteresis)
{
    lt_dash_gauge g;
    int prev = 0;
    lt_dash_gauge_init(&g);
    g.has_high_warning = g.has_high_critical = g.has_low_warning = g.has_low_critical = LT_TRUE;
    g.high_warning = 80; g.high_critical = 90; g.low_warning = 20; g.low_critical = 10;
    LT_CHECK(lt_dash_value_color(&g, 50, &prev) == g.font_color && prev == 0);
    LT_CHECK(lt_dash_value_color(&g, 80, &prev) == g.warn_color && prev == 1);
    LT_CHECK(lt_dash_value_color(&g, 90, &prev) == g.critical_color && prev == 2);
    LT_CHECK(lt_dash_value_color(&g, 15, &prev) == g.warn_color && prev == 1);
    LT_CHECK(lt_dash_value_color(&g, 10, &prev) == g.critical_color && prev == 2);
    LT_CHECK(lt_dash_value_color(&g, 21, &prev) == g.font_color && prev == 0);
    LT_CHECK(lt_dash_value_color(&g, 50, NULL) == g.font_color);
    /* hysteresis: stay critical until 5 below, stay warning until 5 below */
    g.has_hysteresis = LT_TRUE;
    g.hysteresis = 5;
    prev = 0;
    LT_CHECK(lt_dash_value_color(&g, 90, &prev) == g.critical_color);
    LT_CHECK(lt_dash_value_color(&g, 86, &prev) == g.critical_color && prev == 2);
    LT_CHECK(lt_dash_value_color(&g, 84, &prev) == g.warn_color && prev == 1);
    LT_CHECK(lt_dash_value_color(&g, 76, &prev) == g.warn_color && prev == 1);
    LT_CHECK(lt_dash_value_color(&g, 74, &prev) == g.font_color && prev == 0);
    /* low side */
    prev = 0;
    LT_CHECK(lt_dash_value_color(&g, 10, &prev) == g.critical_color);
    LT_CHECK(lt_dash_value_color(&g, 14, &prev) == g.critical_color);
    LT_CHECK(lt_dash_value_color(&g, 16, &prev) == g.warn_color);
    LT_CHECK(lt_dash_value_color(&g, 24, &prev) == g.warn_color);
    LT_CHECK(lt_dash_value_color(&g, 26, &prev) == g.font_color);
    /* unset thresholds never trigger */
    g.has_high_warning = g.has_high_critical = g.has_low_warning = g.has_low_critical = LT_FALSE;
    prev = 0;
    LT_CHECK(lt_dash_value_color(&g, 1e9, &prev) == g.font_color);
    lt_dash_gauge_free(&g);
}

LT_TEST(ring)
{
    lt_dash_ring r;
    int i;
    lt_dash_ring_init(&r, 4);
    LT_CHECK_INT(r.n, 0);
    for (i = 1; i <= 3; i++) lt_dash_ring_push(&r, i);
    LT_CHECK_INT(r.n, 3);
    LT_CHECK_NEAR(lt_dash_ring_get(&r, 0), 1, 0);
    LT_CHECK_NEAR(lt_dash_ring_get(&r, 2), 3, 0);
    for (i = 4; i <= 9; i++) lt_dash_ring_push(&r, i);
    LT_CHECK_INT(r.n, 4);
    LT_CHECK_NEAR(lt_dash_ring_get(&r, 0), 6, 0);
    LT_CHECK_NEAR(lt_dash_ring_get(&r, 3), 9, 0);
    LT_CHECK_NEAR(lt_dash_ring_get(&r, 4), 0, 0);
    lt_dash_ring_free(&r);
}

typedef struct vars { double rpm, clt, lim_hi; lt_bool has_lim; } vars;
static lt_bool vget(void *ud, const char *name, double *out)
{
    vars *v = (vars *)ud;
    if (strcmp(name, "rpm") == 0) { *out = v->rpm; return LT_TRUE; }
    if (strcmp(name, "clt") == 0) { *out = v->clt; return LT_TRUE; }
    if (strcmp(name, "limhi") == 0 && v->has_lim) { *out = v->lim_hi; return LT_TRUE; }
    if (strcmp(name, "flag") == 0) { *out = 1.0; return LT_TRUE; }
    return LT_FALSE;
}

LT_TEST(view_update)
{
    lt_dash_file *d = lt_dash_new();
    lt_dash_gauge *g = lt_dash_add_gauge(d), *g2, *g3;
    lt_dash_indicator *in = lt_dash_add_indicator(d);
    lt_dash_view *v;
    lt_expr_env env;
    vars vs;
    const lt_dash_gstate *s;
    double t;

    lt_free(g->output_channel); g->output_channel = lt_strdup("rpm");
    g->max = 8000;
    g->has_high_warning = LT_TRUE; g->high_warning = 6000;
    g->has_high_critical = LT_TRUE; g->high_critical = 7000;
    g->peak_hold = LT_TRUE;
    g->history_delay = 1000;
    g->has_hysteresis = LT_TRUE; g->hysteresis = 200;
    g->max_vp = lt_strdup("limhi");
    lt_free(in->output_channel); in->output_channel = lt_strdup("flag");
    g2 = lt_dash_add_gauge(d);
    lt_free(g2->output_channel); g2->output_channel = lt_strdup("clt");
    g2->enabled_condition = lt_strdup("clt > 50");
    g2->peg_limits = LT_TRUE;
    g3 = lt_dash_add_gauge(d);
    g3->painter = LT_GP_MULTI_TREND;
    lt_free(g3->output_channel); g3->output_channel = lt_strdup("rpm");
    lt_dash_extras_set(&g3->extras, "lt_series2_channel", "clt");

    lt_expr_env_init(&env);
    env.ud = &vs;
    env.get_var = vget;
    memset(&vs, 0, sizeof vs);
    vs.rpm = 1000; vs.clt = 20; vs.has_lim = LT_TRUE; vs.lim_hi = 9000;

    v = lt_dash_view_new(d);
    LT_CHECK_INT(lt_dash_view_count(v), 4);
    lt_dash_view_update(v, 0.0, &env);
    s = lt_dash_view_state(v, 0);
    LT_CHECK_NEAR(s->display, 1000, 0);                    /* first sample snaps */
    LT_CHECK_NEAR(s->lim.max, 9000, 0);                    /* MaxVP resolved */
    LT_CHECK_NEAR(s->peak.peak, 1000, 0);
    LT_CHECK_INT(s->zone, 0);
    LT_CHECK(lt_dash_view_state(v, 1)->on);
    LT_CHECK(!lt_dash_view_state(v, 2)->enabled);          /* clt 20 -> hidden */
    LT_CHECK_INT(lt_dash_view_state(v, 3)->n_series, 2);
    LT_CHECK_INT(lt_dash_view_state(v, 3)->ring[0].n, 1);

    vs.rpm = 7500; vs.clt = 80;
    lt_dash_view_update(v, 100.0, &env);
    s = lt_dash_view_state(v, 0);
    LT_CHECK_NEAR(s->display, lt_gauge_ema_step(1000, 7500, 100), 1e-9);
    LT_CHECK(lt_dash_gstate_spiking(s, 100.0));            /* |7500-display| > 15% of 9000 */
    LT_CHECK(lt_dash_gstate_spiking(s, 499.0));
    LT_CHECK(!lt_dash_gstate_spiking(s, 501.0));
    LT_CHECK(lt_dash_view_state(v, 2)->enabled);
    LT_CHECK_INT(lt_dash_view_state(v, 3)->ring[1].n, 2);
    LT_CHECK_NEAR(lt_dash_view_state(v, 3)->series_latest[1], 80, 0);
    for (t = 200.0; t < 3000.0; t += 100.0) lt_dash_view_update(v, t, &env);
    s = lt_dash_view_state(v, 0);
    LT_CHECK(s->display > 7400);
    LT_CHECK_INT(s->zone, 2);
    LT_CHECK_NEAR(s->peak.peak, s->display, 100);          /* peak follows within history_delay */
    vs.rpm = 6900;                                          /* below critical but within hysteresis */
    for (t = 3000.0; t < 6000.0; t += 100.0) lt_dash_view_update(v, t, &env);
    s = lt_dash_view_state(v, 0);
    LT_CHECK(s->display < 7000 && s->display > 6800);
    LT_CHECK_INT(s->zone, 2);
    vs.rpm = 6500;
    for (t = 6000.0; t < 9000.0; t += 100.0) lt_dash_view_update(v, t, &env);
    LT_CHECK_INT(lt_dash_view_state(v, 0)->zone, 1);
    /* missing channel: demo value is used */
    g2->value = 33.0;
    lt_free(g2->output_channel); g2->output_channel = lt_strdup("nochan");
    lt_dash_view_free(v);
    v = lt_dash_view_new(d);
    lt_dash_view_update(v, 0.0, &env);
    LT_CHECK_NEAR(lt_dash_view_state(v, 2)->display, 33, 0);
    LT_CHECK(lt_dash_view_state(v, 2)->enabled);           /* condition uses clt=80 */
    lt_dash_view_set_redraw_hz(v, 17);
    lt_dash_view_free(v);
    lt_dash_free(d);
}

LT_TEST(view_redraw_due)
{
    lt_dash_file *d = lt_dash_new();
    lt_dash_gauge *g = lt_dash_add_gauge(d);
    lt_dash_view *v;
    int slot, n = 0;
    double t;
    lt_free(g->output_channel);
    g->output_channel = lt_strdup("rpm");
    v = lt_dash_view_new(d);
    slot = lt_dash_view_state(v, 0)->slot;
    LT_CHECK_INT(slot, lt_gauge_stagger_slot("rpm", NULL));
    lt_dash_view_set_redraw_hz(v, 10);
    for (t = 0.0; t < 1000.0; t += 5.0)
        if (lt_dash_view_due(v, 0, t)) n++;
    LT_CHECK(n >= 9 && n <= 11);                            /* ~10 per second */
    LT_CHECK(!lt_dash_view_due(v, 5, 0));
    lt_dash_view_free(v);
    lt_dash_free(d);
}

LT_TEST(view_indicator_expression)
{
    const char *txt =
        "[OutputChannels]\nrpm = scalar, U16, 0, \"RPM\", 1, 0\n"
        "[FrontPage]\nindicator = { rpm > 500 }, \"Stopped\", \"Running\", white, black, green, black\n";
    lt_ini *ini = lt_ini_load_string(txt, strlen(txt), NULL, NULL);
    lt_dash_file *d;
    lt_dash_view *v;
    lt_expr_env env;
    vars vs;
    LT_CHECK(ini != NULL && ini->frontpage != NULL && ini->frontpage->n_inds == 1);
    d = lt_dash_from_frontpage(ini, NULL);
    LT_CHECK_INT(d->cluster.n_comps, 1);
    LT_CHECK_STR(d->cluster.comps[0].indicator->on_text, "Running");
    LT_CHECK_STR(d->cluster.comps[0].indicator->off_text, "Stopped");
    LT_CHECK(d->cluster.comps[0].indicator->on_background_color == LT_RGB(0, 128, 0) ||
             LT_COLOR_G(d->cluster.comps[0].indicator->on_background_color) > 0);
    v = lt_dash_view_new(d);
    lt_expr_env_init(&env);
    env.ud = &vs;
    env.get_var = vget;
    memset(&vs, 0, sizeof vs);
    vs.rpm = 100;
    lt_dash_view_update(v, 0, &env);
    LT_CHECK(!lt_dash_view_state(v, 0)->on);
    vs.rpm = 900;
    lt_dash_view_update(v, 10, &env);
    LT_CHECK(lt_dash_view_state(v, 0)->on);
    lt_dash_view_free(v);
    lt_dash_free(d);
    lt_ini_free(ini);
}

int main(void)
{
    LT_RUN(colors_argb);
    LT_RUN(colors_css);
    LT_RUN(colors_lighten_darken);
    LT_RUN(painters);
    LT_RUN(defaults);
    LT_RUN(extras_sorted);
    LT_RUN(parse_simple_dash);
    LT_RUN(parse_fixture);
    LT_RUN(parse_edge_cases);
    LT_RUN(parse_property_rules);
    LT_RUN(show_history_and_face_angle);
    LT_RUN(parse_color_rules);
    LT_RUN(second_cluster_replaces_attrs);
    LT_RUN(gauge_file);
    LT_RUN(write_format);
    LT_RUN(fmt_double);
    LT_RUN(fmt_f64_exact);
    LT_RUN(roundtrip_fixture_and_extras);
    LT_RUN(images_roundtrip);
    LT_RUN(save_load_file);
    LT_RUN(validate);
    LT_RUN(template_basic);
    LT_RUN(template_tuning);
    LT_RUN(template_telemetry);
    LT_RUN(gauge_from_ini);
    LT_RUN(from_frontpage);
    LT_RUN(ema);
    LT_RUN(spike);
    LT_RUN(peak_hold);
    LT_RUN(stagger);
    LT_RUN(arc_geometry);
    LT_RUN(zones_hysteresis);
    LT_RUN(ring);
    LT_RUN(view_update);
    LT_RUN(view_redraw_due);
    LT_RUN(view_indicator_expression);
    LT_TEST_MAIN_END
}
