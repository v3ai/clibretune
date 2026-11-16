/* dash_tmpl.c - default dashboard templates (spec 05 section 7) and INI conversion
 * ([GaugeConfigurations] / [FrontPage] -> dash model).
 * Non-ASCII glyphs are written as UTF-8 octal escapes to keep sources pure ASCII.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "dash_priv.h"

#define DEG   "\302\260"
#define DEGC  "\302\260" "C"
#define LAMB  "\316\273"
#define NONE  1e300          /* threshold not set */

#define C_DARKER_BG LT_RGB(12, 14, 20)
#define C_GAUGE_BG  LT_RGB(28, 32, 40)
#define C_BLUE      LT_RGB(74, 158, 248)
#define C_TEAL      LT_RGB(56, 189, 248)
#define C_AMBER     LT_RGB(251, 191, 36)
#define C_GREEN     LT_RGB(34, 197, 94)
#define C_RED       LT_RGB(239, 68, 68)
#define C_WHITE     LT_RGB(255, 255, 255)
#define C_TEXT2     LT_RGB(148, 163, 184)
#define C_WARN      LT_RGB(234, 179, 8)
#define C_CRIT      LT_RGB(239, 68, 68)

typedef struct tspec {
    const char *id, *ch, *title, *units;
    double mn, mx;
    lt_gauge_painter painter;
    double x, y, w, h;
    lt_color font, needle;
    double lw, lc, hw, hc;
    int digits, adj, start, sweep;     /* start < 0: leave angles default */
    double major, minor;               /* 0 = leave */
} tspec;

static lt_dash_file *tmpl_new(lt_color bg)
{
    lt_dash_file *d = lt_dash_new();
    char ts[32];
    dash_now_str(ts, sizeof ts, LT_FALSE);
    dash_set_str(&d->write_date, ts);
    d->cluster.anti_aliasing = LT_TRUE;
    d->cluster.force_aspect = LT_FALSE;
    d->cluster.background_color = bg;
    d->cluster.background_image_style = LT_BG_STRETCH;
    return d;
}

static void set_thresh(lt_dash_gauge *g, const tspec *s)
{
    if (s->lw != NONE) { g->has_low_warning = LT_TRUE; g->low_warning = s->lw; }
    if (s->lc != NONE) { g->has_low_critical = LT_TRUE; g->low_critical = s->lc; }
    if (s->hw != NONE) { g->has_high_warning = LT_TRUE; g->high_warning = s->hw; }
    if (s->hc != NONE) { g->has_high_critical = LT_TRUE; g->high_critical = s->hc; }
}

static lt_dash_gauge *add_spec(lt_dash_file *d, const tspec *s, lt_color back, lt_color trim)
{
    lt_dash_gauge *g = lt_dash_add_gauge(d);
    dash_set_str(&g->id, s->id);
    dash_set_str(&g->output_channel, s->ch);
    dash_set_str(&g->title, s->title);
    dash_set_str(&g->units, s->units);
    g->min = s->mn;
    g->max = s->mx;
    g->painter = s->painter;
    dash_set_str(&g->gauge_style, lt_painter_to_ts(s->painter));
    g->relative_x = s->x;
    g->relative_y = s->y;
    g->relative_width = s->w;
    g->relative_height = s->h;
    g->font_color = s->font;
    g->needle_color = s->needle;
    g->back_color = back;
    g->trim_color = trim;
    g->warn_color = C_WARN;
    g->critical_color = C_CRIT;
    g->value_digits = s->digits;
    g->font_size_adjustment = s->adj;
    if (s->start >= 0) {
        g->start_angle = s->start;
        g->sweep_begin_degree = s->start;
        g->sweep_angle = s->sweep;
    }
    if (s->major > 0.0) g->major_ticks = s->major;
    if (s->minor > 0.0) g->minor_ticks = s->minor;
    set_thresh(g, s);
    return g;
}

#define TP(id, ch, ti, un, mn, mx, p, x, y, w, h, fc, nc, lw, lc, hw, hc, dg, adj, st, sw, ma, mi) \
    { id, ch, ti, un, mn, mx, p, x, y, w, h, fc, nc, lw, lc, hw, hc, dg, adj, st, sw, ma, mi }

lt_dash_file *lt_dash_template_basic(void)
{
    static const tspec t[] = {
        TP("rpm", "rpm", "ENGINE RPM", "", 0, 8000, LT_GP_TACHOMETER, .02, .10, .45, .80,
           C_WHITE, C_AMBER, NONE, NONE, 6500, 7200, 0, 2, 135, 270, 8, 4),
        TP("afr", "afr", "AIR/FUEL RATIO", ":1", 10, 20, LT_GP_ANALOG, .52, .10, .46, .80,
           C_GREEN, C_GREEN, 11.5, 10.5, 16, NONE, 1, 1, 225, 270, 10, 5),
        TP("coolant", "coolant", "COOLANT", DEGC, -40, 120, LT_GP_HBAR, .02, .02, .23, .06,
           C_BLUE, C_BLUE, NONE, NONE, 100, 110, 0, 0, -1, 0, 0, 0),
        TP("map", "map", "MAP", "kPa", 0, 250, LT_GP_HBAR, .27, .02, .23, .06,
           C_TEAL, C_TEAL, NONE, NONE, 200, NONE, 0, 0, -1, 0, 0, 0),
        TP("tps", "tps", "THROTTLE", "%", 0, 100, LT_GP_HBAR, .52, .02, .23, .06,
           C_AMBER, C_AMBER, NONE, NONE, NONE, NONE, 0, 0, -1, 0, 0, 0),
        TP("battery", "battery", "BATT", "V", 10, 16, LT_GP_BASIC_READOUT, .77, .02, .21, .06,
           C_WHITE, C_GREEN, 11.5, 11, NONE, NONE, 1, -1, -1, 0, 0, 0),
        TP("iat", "iat", "INTAKE TEMP", DEGC, -40, 80, LT_GP_BASIC_READOUT, .02, .92, .23, .06,
           C_WHITE, C_AMBER, NONE, NONE, 50, NONE, 0, -1, -1, 0, 0, 0),
        TP("advance", "advance", "TIMING", DEG, -10, 50, LT_GP_BASIC_READOUT, .27, .92, .23, .06,
           C_WHITE, C_TEAL, NONE, NONE, NONE, NONE, 1, -1, -1, 0, 0, 0),
        TP("ve", "ve", "VE", "%", 0, 150, LT_GP_BASIC_READOUT, .52, .92, .23, .06,
           C_WHITE, C_GREEN, NONE, NONE, NONE, NONE, 0, -1, -1, 0, 0, 0),
        TP("pw", "pulseWidth", "PULSE", "ms", 0, 25, LT_GP_BASIC_READOUT, .77, .92, .21, .06,
           C_WHITE, C_BLUE, NONE, NONE, NONE, NONE, 2, -1, -1, 0, 0, 0)
    };
    lt_dash_file *d = tmpl_new(C_DARKER_BG);
    size_t i;
    for (i = 0; i < LT_COUNTOF(t); i++) add_spec(d, &t[i], C_GAUGE_BG, C_TEXT2);
    return d;
}

lt_dash_file *lt_dash_template_tuning(void)
{
    static const tspec t[] = {
        TP("rpm", "rpm", "RPM", "", 0, 8000, LT_GP_ASYM_SWEEP, .02, .02, .30, .30,
           C_WHITE, C_TEAL, NONE, NONE, 6500, 7200, 0, 0, 180, 180, 0, 0),
        TP("afr", "afr", "AFR", ":1", 10, 20, LT_GP_ROUND, .34, .02, .30, .30,
           C_GREEN, C_GREEN, 11.5, 10.5, 16, NONE, 2, 0, 135, 270, 10, 5),
        TP("map", "map", "MAP", "kPa", 0, 250, LT_GP_HBAR, .66, .02, .32, .12,
           C_TEAL, C_TEAL, NONE, NONE, 200, NONE, 0, 0, -1, 0, 0, 0),
        TP("tps", "tps", "TPS", "%", 0, 100, LT_GP_HBAR, .66, .16, .32, .12,
           C_AMBER, C_AMBER, NONE, NONE, NONE, NONE, 0, 0, -1, 0, 0, 0),
        TP("coolant", "coolant", "COOLANT", DEGC, -40, 120, LT_GP_VBAR, .02, .35, .14, .38,
           C_BLUE, C_BLUE, NONE, NONE, 100, 110, 0, 0, -1, 0, 0, 0),
        TP("iat", "iat", "IAT", DEGC, -40, 80, LT_GP_VBAR, .18, .35, .14, .38,
           C_AMBER, C_AMBER, NONE, NONE, 50, NONE, 0, 0, -1, 0, 0, 0),
        TP("lambda_hist", "lambda", "LAMBDA TREND", LAMB, 0.7, 1.3, LT_GP_LINE_GRAPH,
           .34, .35, .64, .38, C_GREEN, C_GREEN, .75, NONE, 1.1, NONE, 3, 0, -1, 0, 0, 0),
        TP("ve", "ve", "VE", "%", 0, 150, LT_GP_BASIC_READOUT, .02, .76, .14, .11,
           C_WHITE, C_GREEN, NONE, NONE, NONE, NONE, 0, -1, -1, 0, 0, 0),
        TP("pw", "pulseWidth", "PULSE", "ms", 0, 25, LT_GP_BASIC_READOUT, .18, .76, .14, .11,
           C_WHITE, C_BLUE, NONE, NONE, 20, NONE, 2, -1, -1, 0, 0, 0),
        TP("duty", "dutyCycle", "DUTY", "%", 0, 100, LT_GP_BASIC_READOUT, .34, .76, .14, .11,
           C_WHITE, C_AMBER, NONE, NONE, 85, 95, 0, -1, -1, 0, 0, 0),
        TP("advance", "advance", "TIMING", DEG, -10, 50, LT_GP_BASIC_READOUT, .50, .76, .14, .11,
           C_WHITE, C_TEAL, NONE, NONE, NONE, NONE, 1, -1, -1, 0, 0, 0),
        TP("battery", "battery", "BATT", "V", 10, 16, LT_GP_BASIC_READOUT, .66, .76, .14, .11,
           C_WHITE, C_GREEN, 11.5, 11, NONE, NONE, 1, -1, -1, 0, 0, 0),
        TP("egt", "egt", "EGT", DEGC, 0, 1000, LT_GP_BASIC_READOUT, .82, .76, .16, .11,
           C_WHITE, C_RED, NONE, NONE, 850, 950, 0, -1, -1, 0, 0, 0),
        TP("afrtarget", "afrTarget", "AFR TARGET", ":1", 10, 20, LT_GP_BASIC_READOUT,
           .02, .89, .14, .09, C_TEXT2, C_TEXT2, NONE, NONE, NONE, NONE, 1, -1, -1, 0, 0, 0),
        TP("corr", "correction", "CORRECTION", "%", 0, 200, LT_GP_BASIC_READOUT,
           .18, .89, .14, .09, C_TEXT2, C_TEXT2, NONE, NONE, NONE, NONE, 0, -1, -1, 0, 0, 0)
    };
    lt_dash_file *d = tmpl_new(C_DARKER_BG);
    size_t i;
    for (i = 0; i < LT_COUNTOF(t); i++) {
        lt_dash_gauge *g = add_spec(d, &t[i], C_GAUGE_BG, C_TEXT2);
        if (strcmp(t[i].id, "corr") == 0) g->trim_color = C_TEXT2;
    }
    return d;
}

/* ---------------------------------------------------------------- telemetry */

#define T_PANEL  LT_RGB(26, 28, 33)
#define T_LOGBG  LT_RGB(15, 17, 20)
#define T_WHITE  LT_RGB(230, 230, 235)
#define T_GRAY   LT_RGB(140, 145, 155)
#define T_BLUE   LT_RGB(87, 148, 242)
#define T_GREEN  LT_RGB(115, 191, 105)
#define T_ORANGE LT_RGB(255, 152, 48)
#define T_RED    LT_RGB(242, 73, 92)
#define T_PURPLE LT_RGB(184, 119, 217)
#define T_YELLOW LT_RGB(250, 222, 42)
#define T_CYAN   LT_RGB(51, 178, 255)

typedef struct tile {
    const char *id, *ch, *title, *units;
    double mn, mx;
    lt_color color;
    int digits;
} tile;

static lt_dash_gauge *tl_gauge(lt_dash_file *d, const tile *t, lt_gauge_painter p, double x,
                               double y, double w, double h)
{
    lt_dash_gauge *g = lt_dash_add_gauge(d);
    dash_set_str(&g->id, t->id);
    dash_set_str(&g->output_channel, t->ch);
    dash_set_str(&g->title, t->title);
    dash_set_str(&g->units, t->units);
    g->min = t->mn;
    g->max = t->mx;
    g->painter = p;
    dash_set_str(&g->gauge_style, lt_painter_to_ts(p));
    g->relative_x = x;
    g->relative_y = y;
    g->relative_width = w;
    g->relative_height = h;
    g->back_color = T_PANEL;
    g->font_color = t->color;
    g->needle_color = t->color;
    g->trim_color = T_GRAY;
    g->warn_color = C_WARN;
    g->critical_color = C_CRIT;
    g->border_width = 1;
    g->shortest_size = 0;
    g->value_digits = t->digits;
    return g;
}

typedef struct series { const char *ch, *label, *css; double mn, mx; } series;

static void add_trend(lt_dash_file *d, const char *id, const char *title, double x, double y,
                      double w, double h, const series *s, int n)
{
    lt_dash_gauge *g;
    tile t;
    lt_color c = 0;
    int i;
    char key[40], val[40];

    lt_color_from_css(s[0].css, &c);
    t.id = id; t.ch = s[0].ch; t.title = title; t.units = "";
    t.mn = s[0].mn; t.mx = s[0].mx; t.color = c; t.digits = 1;
    g = tl_gauge(d, &t, LT_GP_MULTI_TREND, x, y, w, h);
    lt_dash_extras_set(&g->extras, "lt_series1_label", s[0].label);
    for (i = 1; i < n; i++) {
        lt_snprintf(key, sizeof key, "lt_series%d_channel", i + 1);
        lt_dash_extras_set(&g->extras, key, s[i].ch);
        lt_snprintf(key, sizeof key, "lt_series%d_label", i + 1);
        lt_dash_extras_set(&g->extras, key, s[i].label);
        lt_snprintf(key, sizeof key, "lt_series%d_color", i + 1);
        lt_dash_extras_set(&g->extras, key, s[i].css);
        lt_snprintf(key, sizeof key, "lt_series%d_min", i + 1);
        lt_fmt_double(val, sizeof val, s[i].mn, 6);
        lt_dash_extras_set(&g->extras, key, val);
        lt_snprintf(key, sizeof key, "lt_series%d_max", i + 1);
        lt_fmt_double(val, sizeof val, s[i].mx, 6);
        lt_dash_extras_set(&g->extras, key, val);
    }
}

lt_dash_file *lt_dash_template_telemetry(void)
{
    static const tile kpi[] = {
        { "tl_rpm", "rpm", "RPM", "", 0, 9000, T_BLUE, 0 },
        { "tl_map", "map", "MAP", "kPa", 0, 250, T_CYAN, 0 },
        { "tl_tps", "tps", "TPS", "%", 0, 100, T_ORANGE, 0 },
        { "tl_afr", "afr", "AFR", ":1", 10, 20, T_GREEN, 1 },
        { "tl_lam", "lambda", "LAM", LAMB, 0.7, 1.3, T_GREEN, 3 },
        { "tl_clt", "coolant", "CLT", DEGC, -20, 120, T_BLUE, 0 },
        { "tl_iat", "iat", "IAT", DEGC, -20, 80, T_ORANGE, 0 },
        { "tl_spd", "speed", "SPD", "km/h", 0, 260, T_WHITE, 0 },
        { "tl_bat", "battery", "BATT", "V", 10, 16, T_YELLOW, 1 },
        { "tl_duty", "dutyCycle", "DUTY", "%", 0, 100, T_PURPLE, 0 }
    };
    static const tile col[] = {
        { "tl_ve", "ve", "VE", "%", 0, 150, T_GREEN, 0 },
        { "tl_pw", "pulseWidth", "PW", "ms", 0, 25, T_BLUE, 2 },
        { "tl_adv", "advance", "ADV", DEG, -10, 50, T_CYAN, 1 },
        { "tl_bst", "boost", "BOOST", "kPa", 0, 300, T_ORANGE, 0 },
        { "tl_baro", "baro", "BARO", "kPa", 70, 110, T_GRAY, 0 },
        { "tl_oilp", "oilPressure", "OIL P", "kPa", 0, 700, T_YELLOW, 0 },
        { "tl_oilt", "oilTemp", "OIL T", DEGC, 0, 150, T_ORANGE, 0 },
        { "tl_egt", "egt", "EGT", DEGC, 0, 1000, T_RED, 0 },
        { "tl_corr", "correction", "CORR", "%", 50, 150, T_GRAY, 0 },
        { "tl_sync", "sync", "SYNC", "", 0, 1, T_GREEN, 0 },
        { "tl_fuel", "fuelLevel", "FUEL", "%", 0, 100, T_YELLOW, 0 },
        { "tl_tgt", "afrTarget", "AFR TGT", ":1", 10, 20, T_GRAY, 1 }
    };
    static const double kpi_x[] = { .005, .105, .205, .305, .405, .505, .605, .705, .805, .905 };
    static const series eng[] = {
        { "rpm", "RPM", "#5794f2", 0, 9000 }, { "map", "MAP", "#33b5ff", 0, 250 },
        { "tps", "TPS", "#ff9830", 0, 100 }, { "speed", "SPD", "#e6e6eb", 0, 260 },
        { "dutyCycle", "DUTY", "#b877d9", 0, 100 }, { "advance", "ADV", "#33b2ff", -10, 50 },
        { "boost", "BOOST", "#f2495c", 0, 300 }, { "ve", "VE", "#fade2a", 0, 150 }
    };
    static const series fuel[] = {
        { "afr", "AFR", "#73bf69", 10, 20 }, { "lambda", "LAM", "#56a64b", 0.7, 1.3 },
        { "pulseWidth", "PW", "#5794f2", 0, 25 }, { "ve", "VE", "#fade2a", 0, 150 },
        { "correction", "CORR", "#8e8e93", 50, 150 }, { "afrTarget", "TGT", "#b877d9", 10, 20 }
    };
    static const series temp[] = {
        { "coolant", "CLT", "#5794f2", -20, 120 }, { "iat", "IAT", "#ff9830", -20, 80 },
        { "egt", "EGT", "#f2495c", 0, 1000 }, { "oilTemp", "OILT", "#fade2a", 0, 150 }
    };
    static const series pres[] = {
        { "map", "MAP", "#33b5ff", 0, 250 }, { "baro", "BARO", "#8e8e93", 70, 110 },
        { "oilPressure", "OILP", "#fade2a", 0, 700 }, { "boost", "BOOST", "#f2495c", 0, 300 },
        { "battery", "BATT", "#73bf69", 10, 16 }
    };
    static const tile spark[16] = {
        { "tl_sp_rpm", "rpm", "RPM", "", 0, 9000, T_BLUE, 0 },
        { "tl_sp_map", "map", "MAP", "kPa", 0, 250, T_CYAN, 0 },
        { "tl_sp_tps", "tps", "TPS", "%", 0, 100, T_ORANGE, 0 },
        { "tl_sp_speed", "speed", "SPEED", "km/h", 0, 260, T_WHITE, 0 },
        { "tl_sp_afr", "afr", "AFR", ":1", 10, 20, T_GREEN, 1 },
        { "tl_sp_lambda", "lambda", "LAMBDA", LAMB, 0.7, 1.3, T_GREEN, 3 },
        { "tl_sp_ve", "ve", "VE", "%", 0, 150, T_YELLOW, 0 },
        { "tl_sp_pw", "pulseWidth", "PULSE", "ms", 0, 25, T_BLUE, 2 },
        { "tl_sp_clt", "coolant", "COOLANT", DEGC, -20, 120, T_BLUE, 0 },
        { "tl_sp_iat", "iat", "IAT", DEGC, -20, 80, T_ORANGE, 0 },
        { "tl_sp_adv", "advance", "TIMING", DEG, -10, 50, T_CYAN, 1 },
        { "tl_sp_duty", "dutyCycle", "DUTY", "%", 0, 100, T_PURPLE, 0 },
        { "tl_sp_bst", "boost", "BOOST", "kPa", 0, 300, T_RED, 0 },
        { "tl_sp_oilp", "oilPressure", "OIL P", "kPa", 0, 700, T_YELLOW, 0 },
        { "tl_sp_egt", "egt", "EGT", DEGC, 0, 1000, T_RED, 0 },
        { "tl_sp_bat", "battery", "BATTERY", "V", 10, 16, T_GREEN, 1 }
    };
    lt_dash_file *d = tmpl_new(T_LOGBG);
    size_t i;

    d->cluster.force_aspect = LT_TRUE;
    d->cluster.force_aspect_width = 16.0;
    d->cluster.force_aspect_height = 10.0;
    for (i = 0; i < LT_COUNTOF(kpi); i++) {
        lt_dash_gauge *g = tl_gauge(d, &kpi[i], LT_GP_TELEMETRY_STAT, kpi_x[i], .005,
                                    i == 9 ? .090 : .095, .048);
        g->font_size_adjustment = -1;
    }
    for (i = 0; i < LT_COUNTOF(col); i++) {
        lt_dash_gauge *g = tl_gauge(d, &col[i], LT_GP_TELEMETRY_STAT, .005,
                                    .060 + .037 * (double)i, .118, .034);
        g->font_size_adjustment = -1;
    }
    add_trend(d, "tl_trend_engine", "ENGINE DYNAMICS", .130, .060, .430, .210, eng, 8);
    add_trend(d, "tl_trend_fuel", "FUEL & MIXTURE", .570, .060, .425, .210, fuel, 6);
    add_trend(d, "tl_trend_temp", "TEMPERATURES", .130, .278, .430, .223, temp, 4);
    add_trend(d, "tl_trend_pressure", "PRESSURES", .570, .278, .425, .223, pres, 5);
    for (i = 0; i < 16; i++) {
        double sw = .242, sh = .115;
        double x = .005 + (sw + .006) * (double)(i % 4);
        double y = .510 + (sh + .008) * (double)(i / 4);
        tl_gauge(d, &spark[i], LT_GP_LINE_GRAPH, x, y, sw, sh);
    }
    return d;
}

/* ---------------------------------------------------------------- INI -> dash */

static double resolve_expr(const char *expr, const lt_expr_env *env, double fb)
{
    lt_expr *e;
    double v;
    if (expr == NULL || env == NULL) return fb;
    e = lt_expr_compile(expr, NULL, 0);
    if (e == NULL) return fb;
    if (lt_expr_eval_f64(e, env, &v) != 0 || !lt_isfinite(v)) v = fb;
    lt_expr_free(e);
    return v;
}

static lt_bool name_has(const char *a, const char *b, const char *sub)
{
    char la[96], lb[96];
    size_t i;
    lt_strlcpy(la, a ? a : "", sizeof la);
    lt_strlcpy(lb, b ? b : "", sizeof lb);
    for (i = 0; la[i]; i++) if (la[i] >= 'A' && la[i] <= 'Z') la[i] = (char)(la[i] + 32);
    for (i = 0; lb[i]; i++) if (lb[i] >= 'A' && lb[i] <= 'Z') lb[i] = (char)(lb[i] + 32);
    return strstr(la, sub) != NULL || strstr(lb, sub) != NULL;
}

void lt_dash_gauge_from_ini(const lt_ini_gauge *src, lt_dash_gauge *dst, const lt_expr_env *env)
{
    double lo, hi, ld, lw, hw, hd;
    lt_dash_gauge_free(dst);
    lt_dash_gauge_init(dst);
    dash_set_str(&dst->id, src->name);
    dash_set_str(&dst->output_channel, src->channel);
    dash_set_str(&dst->title, src->title);
    dash_set_str(&dst->units, src->units);
    lo = resolve_expr(src->lo_expr, env, src->lo);
    hi = resolve_expr(src->hi_expr, env, src->hi);
    ld = resolve_expr(src->low_danger_expr, env, src->low_danger);
    lw = resolve_expr(src->low_warning_expr, env, src->low_warning);
    hw = resolve_expr(src->high_warning_expr, env, src->high_warning);
    hd = resolve_expr(src->high_danger_expr, env, src->high_danger);
    dst->min = lo;
    dst->max = hi;
    dst->min_vp = NULL;
    /* a threshold equal to the range end means "no zone" */
    if (lw > lo) { dst->has_low_warning = LT_TRUE; dst->low_warning = lw; }
    if (ld > lo) { dst->has_low_critical = LT_TRUE; dst->low_critical = ld; }
    if (hw < hi) { dst->has_high_warning = LT_TRUE; dst->high_warning = hw; }
    if (hd < hi) { dst->has_high_critical = LT_TRUE; dst->high_critical = hd; }
    dst->value_digits = src->digits;
    dst->label_digits = 0;
    dst->back_color = C_GAUGE_BG;
    dst->font_color = C_WHITE;
    dst->trim_color = C_TEXT2;
    dst->needle_color = C_AMBER;
    dst->warn_color = C_WARN;
    dst->critical_color = C_CRIT;
    if (name_has(src->name, src->channel, "tach") || name_has(src->name, src->channel, "rpm"))
        dst->painter = LT_GP_TACHOMETER;
    else if (name_has(src->name, src->channel, "tps") || name_has(src->name, src->channel, "duty") ||
             name_has(src->name, src->channel, "load") || name_has(src->name, src->channel, "pulse") ||
             name_has(src->name, src->channel, "advance") || name_has(src->name, src->channel, "ve"))
        dst->painter = LT_GP_HBAR;
    else
        dst->painter = LT_GP_ANALOG;
    dash_set_str(&dst->gauge_style, lt_painter_to_ts(dst->painter));
}

static void set_trimmed(char **dst, const char *s)
{
    char *t = lt_strdup(s ? s : "");
    lt_str_trim(t);
    lt_free(*dst);
    *dst = t;
}

lt_dash_file *lt_dash_from_frontpage(const lt_ini *ini, const lt_expr_env *env)
{
    lt_dash_file *d = lt_dash_new();
    const lt_ini_frontpage *fp = ini ? ini->frontpage : NULL;
    const lt_ini_gauge **gl;
    size_t i, n = 0, cols, rows;
    double gx0 = 0.01, gx1 = 0.99, gy0 = 0.02, gy1 = 0.78;
    char ts[32];

    dash_now_str(ts, sizeof ts, LT_FALSE);
    dash_set_str(&d->write_date, ts);
    d->cluster.background_color = C_DARKER_BG;
    d->cluster.background_image_style = LT_BG_STRETCH;
    if (fp == NULL) return d;

    gl = (const lt_ini_gauge **)lt_calloc(fp->n_gauges + 1, sizeof(*gl));
    for (i = 0; i < fp->n_gauges; i++) {
        const lt_ini_gauge *g = fp->gauges[i] && fp->gauges[i][0]
                                    ? lt_ini_find_gauge(ini, fp->gauges[i]) : NULL;
        if (g) gl[n++] = g;
    }
    if (n > 0) {
        double cw, ch;
        cols = (size_t)ceil(sqrt((double)n * 1.5));
        if (cols < 1) cols = 1;
        rows = (n + cols - 1) / cols;
        cw = (gx1 - gx0) / (double)cols;
        ch = (gy1 - gy0) / (double)rows;
        for (i = 0; i < n; i++) {
            lt_dash_gauge *g = lt_dash_add_gauge(d);
            lt_dash_gauge_from_ini(gl[i], g, env);
            g->relative_x = gx0 + cw * (double)(i % cols) + 0.004;
            g->relative_y = gy0 + ch * (double)(i / cols) + 0.004;
            g->relative_width = cw - 0.008;
            g->relative_height = ch - 0.008;
            g->shortest_size = 0;
        }
    }
    lt_free((void *)gl);
    if (fp->n_inds > 0) {
        size_t percol = fp->n_inds < 12 ? fp->n_inds : 12;
        size_t nrows = (fp->n_inds + percol - 1) / percol;
        double iw = (gx1 - gx0) / (double)percol;
        double ih = (0.99 - 0.82) / (double)nrows;
        for (i = 0; i < fp->n_inds; i++) {
            const lt_ini_fp_indicator *s = &fp->inds[i];
            lt_dash_indicator *in = lt_dash_add_indicator(d);
            lt_u32 c;
            char id[24];
            lt_snprintf(id, sizeof id, "fp_ind%u", (unsigned)(i + 1));
            dash_set_str(&in->id, id);
            set_trimmed(&in->on_text, s->label_on);
            set_trimmed(&in->off_text, s->label_off);
            lt_dash_extras_set(&in->extras, "lt_expression", s->expression ? s->expression : "");
            if (s->bg_on && lt_ini_parse_color(s->bg_on, &c)) in->on_background_color = c;
            if (s->fg_on && lt_ini_parse_color(s->fg_on, &c)) in->on_text_color = c;
            if (s->bg_off && lt_ini_parse_color(s->bg_off, &c)) in->off_background_color = c;
            if (s->fg_off && lt_ini_parse_color(s->fg_off, &c)) in->off_text_color = c;
            in->relative_x = gx0 + iw * (double)(i % percol) + 0.003;
            in->relative_y = 0.82 + ih * (double)(i / percol) + 0.003;
            in->relative_width = iw - 0.006;
            in->relative_height = ih - 0.006;
        }
    }
    return d;
}

/* ---- INTEGRATION ADDITION: map the generic template channel names onto an INI ---- */
typedef struct chan_alias { const char *generic; const char *cand[8]; } chan_alias;
static const chan_alias k_alias[] = {
    { "rpm", { "RPMValue", "rpm", "RPM", NULL } },
    { "map", { "MAPValue", "map", "MAP", NULL } },
    { "tps", { "TPSValue", "tps", "TPS", "throttle", NULL } },
    { "afr", { "afr", "AFR", "AFRValue", "afrGasolineScale", "afr1", NULL } },
    { "lambda", { "lambdaValue", "lambda", "Lambda", "lambda1", NULL } },
    { "coolant", { "coolant", "CLT", "clt", "coolantTemp", NULL } },
    { "iat", { "intake", "iat", "IAT", "MAT", "mat", NULL } },
    { "speed", { "vehicleSpeedKph", "vss", "speed", "VSS", "vehicleSpeed", NULL } },
    { "battery", { "VBatt", "batteryVoltage", "battery", "vbatt", "batt", NULL } },
    { "dutyCycle", { "dutyCycle", "injectorDutyCycle", "injectorDuty", "injDuty", "duty", "pulseDuty", NULL } },
    { "ve", { "veValue", "veCurr", "veCurr1", "VE", "ve", "VE1", NULL } },
    { "pulseWidth", { "pulseWidth", "pulseWidth1", "PW1", "pw", "actualLastInjection", NULL } },
    { "advance", { "advance", "ignitionAdvance", "ignitionAdvanceCyl1", "timing", "timingAdvance", "ignAdvance", NULL } },
    { "boost", { "boostStatus_output", "boostOutput", "boost", "boostPressure", "MAPValue",
                 NULL } },
    { "baro", { "baroPressure", "baro", "baroCorrection", NULL } },
    { "egt", { "egt1", "EGT1", "egt", NULL } },
    { "correction", { "egoCorrectionForVeAnalyze", "egoCorrection", "correction", "stftCorrection1",
                      NULL } },
    { "sync", { "sync", "syncStatus", "hasSync", NULL } },
    { "fuelLevel", { "fuelTankLevel", "fuelLevel", NULL } },
    { "afrTarget", { "afrTarget", "targetAFR", "lambdaTarget", "targetLambda", NULL } },
    { "oilPressure", { "oilPressure", "oilp", NULL } },
    { "oilTemp", { "oilTemp", "oilt", NULL } },
    { NULL, { NULL } }
};

static char *resolve_one(const lt_ini *ini, const char *name)
{
    const chan_alias *a;
    int k;

    if (!name || !name[0] || lt_ini_find_channel(ini, name)) {
        return NULL;
    }
    for (a = k_alias; a->generic; a++) {
        if (lt_stricmp(a->generic, name) != 0) continue;
        for (k = 0; a->cand[k]; k++) {
            if (lt_ini_find_channel(ini, a->cand[k])) return lt_strdup(a->cand[k]);
        }
    }
    return NULL;
}

size_t lt_dash_resolve_channels(lt_dash_file *d, const lt_ini *ini)
{
    size_t i, unresolved = 0;

    if (!d || !ini) return 0;
    for (i = 0; i < d->cluster.n_comps; i++) {
        lt_dash_comp *c = &d->cluster.comps[i];
        char **slot = c->gauge ? &c->gauge->output_channel
                               : (c->indicator ? &c->indicator->output_channel : NULL);
        char *repl;

        if (!slot || !*slot || !(*slot)[0]) continue;
        repl = resolve_one(ini, *slot);
        if (repl) {
            dash_set_str(slot, repl);
            lt_free(repl);
        } else if (!lt_ini_find_channel(ini, *slot)) {
            unresolved++;
        }
    }
    return unresolved;
}
