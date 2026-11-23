/* feat_calib.c - sensor calibration dialogs: std_ms2gentherm (CLT/IAT thermistor, 3-point
 * Steinhart-Hart fit) and std_ms2geno2 (wideband AFR curve). Presets come from the INI's
 * [ReferenceTables]; the curve math is in project/calib_build.c, the write goes through the
 * Speeduino calibration command (lt_proto_write_calibration).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"

#define CH 8
#define PLOT_N 65

/* ------------------------------------------------------------------ */
/* plot                                                                */
typedef struct calib_plot {
    double y[PLOT_N];
    int n;
    double ylo, yhi;
    lt_color bg, grid, line, border;
} calib_plot;

static void plot_cb(lt_gfx *g, lt_rect r, void *user)
{
    const calib_plot *p = (const calib_plot *)user;
    int i, k;
    double span, x0, y0, x1, y1;

    if (lt_gfx_cell_mode(g) || p->n < 2) return;
    lt_gfx_fill_rect(g, r, p->bg);
    for (k = 1; k < 4; k++) {
        int yy = r.y + r.h * k / 4, xx = r.x + r.w * k / 4;
        lt_gfx_line(g, (double)r.x, (double)yy, (double)(r.x + r.w), (double)yy, 1.0, p->grid);
        lt_gfx_line(g, (double)xx, (double)r.y, (double)xx, (double)(r.y + r.h), 1.0, p->grid);
    }
    span = p->yhi - p->ylo;
    if (span < 1e-9) span = 1.0;
    x0 = 0; y0 = 0;
    for (i = 0; i < p->n; i++) {
        double v = (p->y[i] - p->ylo) / span;
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        x1 = (double)r.x + (double)(r.w - 1) * (double)i / (double)(p->n - 1);
        y1 = (double)(r.y + r.h - 1) - v * (double)(r.h - 1);
        if (i > 0) lt_gfx_line(g, x0, y0, x1, y1, lt_gfx_fpx(g, 2.0), p->line);
        x0 = x1; y0 = y1;
    }
    lt_gfx_rect(g, r, p->border);
}

static void plot_colors(lt_ui *ui, calib_plot *p)
{
    const lt_theme *t = lt_ui_theme(ui);
    p->bg = t->input; p->grid = t->border_subtle; p->line = t->primary; p->border = t->border;
}

/* ------------------------------------------------------------------ */
/* shared helpers                                                      */
static lt_inc_cache *make_cache(lt_app *app)
{
    lt_inc_cache *c = (lt_inc_cache *)lt_malloc(sizeof(*c));
    char buf[600];
    lt_inc_cache_init(c);
    if (app->has_project && app->project.path != NULL) {
        lt_inc_cache_add_path(c, app->project.path);
        lt_snprintf(buf, sizeof buf, "%s/projectCfg", app->project.path);
        lt_inc_cache_add_path(c, buf);
    }
    return c;
}

/* "" when a calibration write may go ahead, else why not */
static const char *write_block(lt_app *app)
{
    double rpm = 0.0;
    if (app->proto == NULL || (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO))
        return "Not connected to an ECU - preview only.";
    if (lt_app_channel(app, "rpm", &rpm) && rpm > 0.0)
        return "The engine is running. A calibration write blocks the ECU while it burns EEPROM; "
               "stop the engine (ignition on is fine).";
    return "";
}

static void result_note(char *buf, size_t cap, lt_app *app, const char *what, const char *tf)
{
    if (lt_proto_is_modern(app->proto))
        lt_snprintf(buf, cap, "%s written and verified by read-back: %s.", what, tf);
    else
        lt_snprintf(buf, cap, "%s written, but NOT verified: the legacy protocol has no "
                    "calibration read-back. Check the gauge afterwards.", what);
}

static void calib_close(lt_app *app, lt_app_dialog *d)
{
    if (d->data != NULL) { lt_ui_after_frame(app->ui, lt_free, d->data); }   /* the plot callback reads it */
    d->data = NULL;
}

/* ------------------------------------------------------------------ */
/* temperature                                                         */
typedef struct therm_state {
    const lt_ini_ref_table *rt;
    int sensor;              /* index into ids */
    lt_u8 ids[4];
    const char *names[4];
    int n_ids;
    int preset;              /* 0 = custom, i+1 = therm_opts[i] */
    int fahrenheit;
    double bias, t[3], r[3]; /* t in display units */
    double temps[32];
    int fit_ok;
    char err[200], note[260];
    calib_plot plot;
    double lo, hi, coef[3];
    int dirty;
} therm_state;

static void therm_load_preset(therm_state *s, int idx)
{
    const lt_ini_therm_opt *o = &s->rt->therm_opts[idx];
    int i;
    s->bias = o->bias_resistor;
    for (i = 0; i < 3; i++) {
        s->t[i] = s->fahrenheit ? lt_calib_c_to_f(o->t[i]) : o->t[i];
        s->r[i] = o->r[i];
    }
}

static void therm_refit(lt_app *app, therm_state *s)
{
    double tc[3];
    lt_therm_fit fit;
    lt_bool modern = app->proto != NULL ? lt_proto_is_modern(app->proto) : LT_FALSE;
    int i;

    for (i = 0; i < 3; i++) tc[i] = s->fahrenheit ? lt_calib_f_to_c(s->t[i]) : s->t[i];
    s->err[0] = '\0';
    s->fit_ok = lt_calib_therm_curve(s->bias, tc, s->r, modern, s->temps) == 0;
    if (!s->fit_ok) {
        lt_strlcpy(s->err, lt_err_get(), sizeof s->err);
        s->plot.n = 0;
        return;
    }
    (void)lt_therm_fit_points(&fit, s->bias, tc, s->r);
    s->coef[0] = fit.a; s->coef[1] = fit.b; s->coef[2] = fit.c;
    s->lo = 1e30; s->hi = -1e30;
    for (i = 0; i < 32; i++) {
        if (s->temps[i] < s->lo) s->lo = s->temps[i];
        if (s->temps[i] > s->hi) s->hi = s->temps[i];
    }
    for (i = 0; i < PLOT_N; i++) {
        double c = lt_therm_temp_at_adc(&fit, i * 1023 / (PLOT_N - 1));
        s->plot.y[i] = s->fahrenheit ? lt_calib_c_to_f(c) : c;
    }
    /* the ends of the ADC range run off to extremes; frame the three fit points instead */
    s->plot.ylo = s->t[0]; s->plot.yhi = s->t[0];
    for (i = 1; i < 3; i++) {
        if (s->t[i] < s->plot.ylo) s->plot.ylo = s->t[i];
        if (s->t[i] > s->plot.yhi) s->plot.yhi = s->t[i];
    }
    s->plot.ylo -= (s->plot.yhi - s->plot.ylo) * 0.15 + 1.0;
    s->plot.yhi += (s->plot.yhi - s->plot.ylo) * 0.1 + 1.0;
    s->plot.n = PLOT_N;
}

static void therm_write(lt_app *app, therm_state *s)
{
    lt_u8 wire[64];
    int rc;
    char tf[80];

    s->note[0] = '\0';
    lt_cal_encode_temperature(s->temps, wire);
    rc = lt_proto_write_calibration(app->proto, s->ids[s->sensor] == 1 ? LT_CAL_IAT : LT_CAL_CLT,
                                    wire, sizeof wire);
    if (rc != 0) {
        lt_snprintf(s->err, sizeof s->err, "Failed to write calibration: %s", lt_err_get());
        lt_app_toast(app, 3, "%s", s->err);
        return;
    }
    lt_snprintf(tf, sizeof tf, "%.1f to %.1f degC across 32 ADC bins", s->lo, s->hi);
    result_note(s->note, sizeof s->note, app, s->names[s->sensor], tf);
    lt_app_toast(app, lt_proto_is_modern(app->proto) ? 1 : 2, "%s calibration written",
                 s->names[s->sensor]);
}

static const char *therm_preset_get(void *ud, int i)
{
    const therm_state *s = (const therm_state *)ud;
    return i == 0 ? "Custom (enter points)" : s->rt->therm_opts[i - 1].name;
}

static const char *therm_sensor_get(void *ud, int i)
{
    return ((const therm_state *)ud)->names[i];
}

static void therm_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    therm_state *s = (therm_state *)d->data;
    int open = d->open, w2[2], w3[3], w1 = -1, i, prev_f, changed = 0, sel;
    char buf[160], id[16];
    const char *blk;
    static const char *const units[2] = { "Degrees C", "Degrees F" };

    if (s == NULL) {
        s = (therm_state *)lt_calloc(1, sizeof(*s));
        d->data = s;
        s->rt = lt_ini_find_ref_table(app->ini, d->target);
        if (s->rt != NULL) {
            for (i = 0; i < (int)s->rt->n_idents && s->n_ids < 4; i++) {
                if (s->rt->idents[i].id > 1) continue;
                s->ids[s->n_ids] = s->rt->idents[i].id;
                s->names[s->n_ids++] = s->rt->idents[i].label;
            }
        }
        if (s->n_ids == 0) {
            s->ids[0] = 0; s->names[0] = "Coolant Temperature (CLT)";
            s->ids[1] = 1; s->names[1] = "Air Temperature (IAT)";
            s->n_ids = 2;
        }
        s->fahrenheit = app->settings.temp_unit == 1;   /* start in the preferred unit (G-90) */
        s->bias = 2490; s->t[0] = 0; s->t[1] = 50; s->t[2] = 100;
        if (s->fahrenheit) { s->t[0] = 32; s->t[1] = 122; s->t[2] = 212; }
        s->r[0] = 94000; s->r[1] = 11000; s->r[2] = 2370;
        if (s->rt != NULL && s->rt->n_therm_opts > 0) { s->preset = 1; therm_load_preset(s, 0); }
        s->dirty = 1;
    }
    plot_colors(ui, &s->plot);
    if (lt_ui_modal_begin(ui, "Calibrate Temperature Sensors", CH * 84, 620, &open)) {
        w2[0] = CH * 18; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Apply to sensor");
        if (lt_ui_combo_cb(ui, "sensor", &s->sensor, s->n_ids, therm_sensor_get, s)) s->note[0] = 0;
        lt_ui_label(ui, "Thermistor preset");
        sel = s->preset;
        if (lt_ui_combo_cb(ui, "preset", &sel, s->rt != NULL ? (int)s->rt->n_therm_opts + 1 : 1,
                           therm_preset_get, s)) {
            s->preset = sel;
            if (sel > 0) therm_load_preset(s, sel - 1);
            s->dirty = 1;
        }
        lt_ui_label(ui, "Temperature units");
        prev_f = s->fahrenheit;
        lt_ui_combo(ui, "units", &s->fahrenheit, units, 2);
        if (prev_f != s->fahrenheit) {
            for (i = 0; i < 3; i++)
                s->t[i] = s->fahrenheit ? lt_calib_c_to_f(s->t[i]) : lt_calib_f_to_c(s->t[i]);
            s->dirty = 1;
        }
        lt_ui_label(ui, "Bias resistor (ohm)");
        if (lt_ui_number(ui, "bias", &s->bias, 1, 10000000, 10, 0)) changed = 1;

        w3[0] = CH * 18; w3[1] = CH * 22; w3[2] = -1;
        lt_ui_layout_row(ui, 3, w3, 0);
        lt_ui_label_ex(ui, "Point", th->text2, LT_UI_ALIGN_LEFT);
        lt_ui_label_ex(ui, s->fahrenheit ? "Temperature (F)" : "Temperature (C)", th->text2,
                       LT_UI_ALIGN_LEFT);
        lt_ui_label_ex(ui, "Resistance (ohm)", th->text2, LT_UI_ALIGN_LEFT);
        for (i = 0; i < 3; i++) {
            lt_ui_layout_row(ui, 3, w3, 0);
            lt_snprintf(buf, sizeof buf, "Point %d", i + 1);
            lt_ui_label(ui, buf);
            lt_snprintf(id, sizeof id, "t%d", i);
            if (lt_ui_number(ui, id, &s->t[i], -100, 1000, 1, 2)) changed = 1;
            lt_snprintf(id, sizeof id, "r%d", i);
            if (lt_ui_number(ui, id, &s->r[i], 1, 100000000, 10, 1)) changed = 1;
        }
        if (changed) { s->preset = 0; s->dirty = 1; }
        if (s->dirty) { therm_refit(app, s); s->dirty = 0; }

        lt_ui_layout_row(ui, 1, &w1, 150);
        lt_ui_draw_custom(ui, lt_ui_layout_next(ui), plot_cb, &s->plot);
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (s->fit_ok) {
            lt_snprintf(buf, sizeof buf, "Steinhart-Hart  A=%.6e  B=%.6e  C=%.6e",
                        s->coef[0], s->coef[1], s->coef[2]);
            lt_ui_label_ex(ui, buf, th->text2, LT_UI_ALIGN_LEFT);
        }
        lt_ui_label_wrap(ui, "Apply Calibration refits the curve on the ECU's own 32 ADC points "
                         "and writes it with the engine off. On the legacy serial protocol there "
                         "is no read-back; verify the gauge reads sensibly afterwards.", th->muted);
        blk = write_block(app);
        if (blk[0] != '\0') lt_ui_label_wrap(ui, blk, th->warning);
        if (s->err[0] != '\0') lt_ui_label_wrap(ui, s->err, th->error);
        if (s->note[0] != '\0') lt_ui_label_wrap(ui, s->note, th->success);
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button_ex(ui, "Apply Calibration",
                            (s->fit_ok && blk[0] == '\0') ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED)
            && s->fit_ok && blk[0] == '\0') {
            therm_write(app, s);
        }
        if (lt_ui_button(ui, s->note[0] != '\0' ? "Close" : "Cancel")) open = 0;
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

/* ------------------------------------------------------------------ */
/* AFR                                                                 */
typedef struct afr_state {
    const lt_ini_ref_table *rt;
    lt_inc_cache *cache;
    int sel[32];             /* combo index -> solution index (-1 = custom) */
    char usable[32];
    char label[32][64];
    int n_sel, preset;
    double lin[4];
    int corr_mode;           /* 0 off, 1 one point, 2 two point */
    double corr[4];
    double sig[11];
    double afr[1024];
    int ok;
    lt_bool clips;
    char desc[200], err[260], note[260];
    calib_plot plot;
} afr_state;

static const char *afr_get(void *ud, int i)
{
    return ((const afr_state *)ud)->label[i];
}

static void afr_rebuild(afr_state *s)
{
    const char *preset = NULL;
    int i, rc;

    if (s->sel[s->preset] >= 0) preset = s->rt->solutions[s->sel[s->preset]].label;
    rc = lt_calib_afr_build(s->rt, preset, s->cache, s->lin, s->corr_mode, s->corr, s->afr,
                            s->desc, sizeof s->desc);
    s->ok = rc == 0;
    s->err[0] = '\0';
    if (!s->ok) {
        lt_strlcpy(s->err, lt_err_get(), sizeof s->err);
        s->plot.n = 0;
        s->desc[0] = '\0';
        return;
    }
    s->clips = lt_calib_afr_clips(s->afr);
    for (i = 0; i < 33; i++) s->plot.y[i] = s->afr[i * 1023 / 32];
    s->plot.n = 33;
    s->plot.ylo = 5.0; s->plot.yhi = 26.0;
}

static void afr_write(lt_app *app, afr_state *s)
{
    lt_u8 wire[1024];
    int rc;

    s->note[0] = '\0';
    lt_cal_encode_o2(s->afr, wire);
    rc = lt_proto_write_calibration(app->proto, LT_CAL_O2, wire, sizeof wire);
    if (rc != 0) {
        lt_snprintf(s->err, sizeof s->err, "Failed to write AFR calibration: %s", lt_err_get());
        lt_app_toast(app, 3, "%s", s->err);
        return;
    }
    result_note(s->note, sizeof s->note, app, "AFR calibration", s->desc);
    lt_app_toast(app, lt_proto_is_modern(app->proto) ? 1 : 2, "AFR sensor calibration written");
}

static void afr_init(lt_app *app, lt_ui *ui, afr_state *s, const char *target)
{
    size_t i;
    int first_usable = -1;
    double *dummy = (double *)lt_malloc(1024 * sizeof(double));

    s->rt = lt_ini_find_ref_table(app->ini, target);
    s->cache = make_cache(app);
    s->lin[0] = 0; s->lin[1] = 10; s->lin[2] = 5; s->lin[3] = 20;
    s->corr[0] = 14.2; s->corr[1] = 14.7; s->corr[2] = 19.0; s->corr[3] = 18.25;
    if (s->rt != NULL) {
        for (i = 0; i < s->rt->n_generators; i++) {
            const lt_ini_generator *g = &s->rt->generators[i];
            if (g->kind != NULL && strcmp(g->kind, "linearGenerator") == 0 && g->has_bounds) {
                s->lin[0] = g->x_lo; s->lin[1] = g->y_lo; s->lin[2] = g->x_hi; s->lin[3] = g->y_hi;
            }
        }
        for (i = 0; i < s->rt->n_solutions && s->n_sel < 30; i++) {
            const lt_ini_solution *so = &s->rt->solutions[i];
            if (so->generator != NULL || so->label == NULL) continue;
            s->sel[s->n_sel] = (int)i;
            s->usable[s->n_sel] = lt_calib_solution_curve(s->rt, so->label, s->cache, dummy) == 0;
            if (s->usable[s->n_sel]) {
                lt_strlcpy(s->label[s->n_sel], so->label, sizeof s->label[0]);
                if (first_usable < 0) first_usable = s->n_sel;
            } else {
                lt_snprintf(s->label[s->n_sel], sizeof s->label[0], "%.40s (unavailable)",
                            so->label);
            }
            s->n_sel++;
        }
    }
    lt_free(dummy);
    s->sel[s->n_sel] = -1;
    s->usable[s->n_sel] = 1;
    lt_strlcpy(s->label[s->n_sel], "Custom Linear WB...", sizeof s->label[0]);
    s->preset = first_usable >= 0 ? first_usable : s->n_sel;
    s->n_sel++;
    plot_colors(ui, &s->plot);
    s->sig[0] = -1;
}

static void afr_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    afr_state *s = (afr_state *)d->data;
    int open = d->open, w2[2], w1 = -1, custom, h;
    double sig[11];
    char buf[160];
    const char *blk;
    static const char *const corr_modes[3] = {
        "Off - use the curve as-is", "Single point (offset)", "Two point (gain + offset)" };

    if (s == NULL) {
        s = (afr_state *)lt_calloc(1, sizeof(*s));
        d->data = s;
        afr_init(app, ui, s, d->target);
    }
    plot_colors(ui, &s->plot);
    custom = s->sel[s->preset] < 0;
    h = 480 + (s->corr_mode == 2 ? 60 : 0) + (s->corr_mode == 1 ? 60 : 0) + (custom ? 120 : 0);
    if (lt_ui_modal_begin(ui, "Calibrate AFR Sensor", CH * 84, h, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, "Maps the wideband controller's 0-5 V analog output to AFR for the "
                         "ECU's O2 input. Written to the calibration space (separate from the "
                         "tune pages).", th->text2);
        w2[0] = CH * 28; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Wideband controller");
        lt_ui_combo_cb(ui, "preset", &s->preset, s->n_sel, afr_get, s);
        if (!s->usable[s->preset]) s->preset = s->n_sel - 1;
        custom = s->sel[s->preset] < 0;
        if (custom) {
            lt_ui_label(ui, "Point 1 voltage (V)");
            lt_ui_number(ui, "v1", &s->lin[0], -20, 20, 0.01, 2);
            lt_ui_label(ui, "Point 1 AFR");
            lt_ui_number(ui, "a1", &s->lin[1], 0, 50, 0.01, 2);
            lt_ui_label(ui, "Point 2 voltage (V)");
            lt_ui_number(ui, "v2", &s->lin[2], -20, 20, 0.01, 2);
            lt_ui_label(ui, "Point 2 AFR");
            lt_ui_number(ui, "a2", &s->lin[3], 0, 50, 0.01, 2);
        }
        lt_ui_label(ui, "Correct against a reference");
        lt_ui_combo(ui, "corr", &s->corr_mode, corr_modes, 3);
        if (s->corr_mode > 0) {
            lt_ui_label(ui, s->corr_mode == 2 ? "Point 1 measured (ECU)" : "Measured (ECU)");
            lt_ui_number(ui, "m1", &s->corr[0], 0, 50, 0.01, 2);
            lt_ui_label(ui, s->corr_mode == 2 ? "Point 1 expected (reference)"
                                              : "Expected (reference)");
            lt_ui_number(ui, "e1", &s->corr[1], 0, 50, 0.01, 2);
        }
        if (s->corr_mode == 2) {
            lt_ui_label(ui, "Point 2 measured (ECU)");
            lt_ui_number(ui, "m2", &s->corr[2], 0, 50, 0.01, 2);
            lt_ui_label(ui, "Point 2 expected (reference)");
            lt_ui_number(ui, "e2", &s->corr[3], 0, 50, 0.01, 2);
        }
        sig[0] = (double)s->preset; sig[1] = s->lin[0]; sig[2] = s->lin[1]; sig[3] = s->lin[2];
        sig[4] = s->lin[3]; sig[5] = (double)s->corr_mode; sig[6] = s->corr[0];
        sig[7] = s->corr[1]; sig[8] = s->corr[2]; sig[9] = s->corr[3]; sig[10] = 0;
        if (memcmp(sig, s->sig, sizeof sig) != 0) {
            memcpy(s->sig, sig, sizeof sig);
            afr_rebuild(s);
            s->note[0] = '\0';
        }

        lt_ui_layout_row(ui, 1, &w1, 150);
        lt_ui_draw_custom(ui, lt_ui_layout_next(ui), plot_cb, &s->plot);
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (s->ok) {
            lt_snprintf(buf, sizeof buf, "0 V -> %.2f     2.5 V -> %.2f     5 V -> %.2f AFR",
                        s->afr[0], s->afr[512], s->afr[1023]);
            lt_ui_label_ex(ui, buf, th->text2, LT_UI_ALIGN_LEFT);
            if (s->clips)
                lt_ui_label_wrap(ui, "This curve leaves the 0.0-25.5 AFR the wire format can "
                                 "carry; the ends will be flat-topped in the ECU.", th->warning);
        }
        lt_ui_label_wrap(ui, "Write with the engine off. On the legacy serial protocol the ECU "
                         "sends no acknowledgement, so re-check a logged AFR value afterwards; on "
                         "the CRC protocol the write is verified automatically.", th->muted);
        blk = write_block(app);
        if (blk[0] != '\0') lt_ui_label_wrap(ui, blk, th->warning);
        if (s->err[0] != '\0') lt_ui_label_wrap(ui, s->err, th->error);
        if (s->note[0] != '\0') lt_ui_label_wrap(ui, s->note, th->success);
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button_ex(ui, "Write Calibration",
                            (s->ok && blk[0] == '\0') ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED)
            && s->ok && blk[0] == '\0') {
            afr_write(app, s);
        }
        if (lt_ui_button(ui, s->note[0] != '\0' ? "Close" : "Cancel")) open = 0;
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void afr_destroy(void *p)
{
    afr_state *s = (afr_state *)p;
    if (s->cache != NULL) {
        lt_inc_cache_free(s->cache);
        lt_free(s->cache);
    }
    lt_free(s);
}

static void afr_close(lt_app *app, lt_app_dialog *d)
{
    if (d->data != NULL) { lt_ui_after_frame(app->ui, afr_destroy, d->data); }   /* the plot callback reads it */
    d->data = NULL;
}

/* ------------------------------------------------------------------ */
static int open_therm(lt_app *app, const char *t)
{
    if (lt_ini_find_ref_table(app->ini, t) == NULL) {
        lt_app_toast(app, 2, "This definition has no thermistor calibration table");
        return -1;
    }
    return lt_app_dialog_push(app, "calib_therm", t, "Calibrate Temperature Sensors") != NULL
               ? 0 : -1;
}

static int open_afr(lt_app *app, const char *t)
{
    if (lt_ini_find_ref_table(app->ini, t) == NULL) {
        lt_app_toast(app, 2, "This definition has no AFR calibration table");
        return -1;
    }
    return lt_app_dialog_push(app, "calib_afr", t, "Calibrate AFR Sensor") != NULL ? 0 : -1;
}

void lt_feat_calib_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "calib_therm"; dk.draw = therm_draw; dk.close = calib_close;
    lt_ext_add_dialog_kind(reg, &dk);
    dk.kind = "calib_afr"; dk.draw = afr_draw; dk.close = afr_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_target(reg, "std_ms2gentherm", open_therm);
    lt_ext_add_target(reg, "std_ms2geno2", open_afr);
}
