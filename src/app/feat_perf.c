/* feat_perf.c - Performance / economy calculator dialog (G-35): vehicle specs (weight, frontal area,
 * Cd, tyres, drivetrain loss, final drive and gear ratios, injectors), wheel power and torque from
 * live rpm / speed / acceleration (or typed values), acceleration estimates, gearing table and fuel
 * economy (cc/min, mpg, l/100 km, trip totals) from the live injector pulse width.
 * Tools -> Performance Calculator...   (dialog kind "perf"). The maths is dlg_perf.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "feat.h"
#include "dlg_perf.h"

#define PERF_FILE "vehicle_specs.properties"

typedef struct perf_state {
    int loaded;
    double weight, frontal, cd, tire, loss, final_drive, inj_cc, redline;
    double n_inj;
    int weight_kg, fuel_units, realtime, two_stroke;
    char gears[96];
    double m_rpm, m_speed, m_accel, m_pw;       /* typed values (speed in the preferred unit, accel in g) */
    /* live sampling */
    double prev_v, accel_ms2, accel_f;
    unsigned long prev_ms, trip_ms;
    double trip_m, trip_cc;
    char msg[160];
} perf_state;

static void defaults(perf_state *s)
{
    lt_perf_specs d;
    lt_perf_specs_default(&d);
    s->weight = 3000.0; s->weight_kg = 0; s->frontal = 22.0; s->cd = d.cd; s->tire = d.tire_diameter_in;
    s->loss = d.drivetrain_loss_pct; s->final_drive = d.final_drive; s->inj_cc = d.injector_cc_min;
    s->n_inj = (double)d.n_injectors; s->redline = 6500.0;
    lt_strlcpy(s->gears, "2.97, 2.07, 1.43, 1.00, 0.84", sizeof(s->gears));
    s->m_rpm = 3000.0; s->m_speed = 60.0; s->m_accel = 0.3; s->m_pw = 3.0;
    s->realtime = 1; s->fuel_units = 0;
}

static char *spec_path(lt_app *app)
{
    return lt_path_join(app->config_dir, PERF_FILE);
}

static void put_f(lt_props *p, const char *k, double v)
{
    char b[48];
    lt_fmt_double(b, sizeof(b), v, 6);
    lt_props_set(p, k, b);
}

static void spec_load(lt_app *app, perf_state *s)
{
    lt_props p;
    char *path = spec_path(app);
    const char *g;
    int i = 0;
    defaults(s);
    s->loaded = 1;
    if (path == NULL) { return; }
    lt_props_init(&p);
    if (lt_file_exists(path) && lt_props_load(&p, path) == 0) {
        lt_props_get_f64(&p, "weight", &s->weight);
        lt_props_get_i32(&p, "weight_kg", &s->weight_kg);
        lt_props_get_f64(&p, "frontal_sqft", &s->frontal);
        lt_props_get_f64(&p, "cd", &s->cd);
        lt_props_get_f64(&p, "tire_in", &s->tire);
        lt_props_get_f64(&p, "loss", &s->loss);
        lt_props_get_f64(&p, "final_drive", &s->final_drive);
        lt_props_get_f64(&p, "injector_cc", &s->inj_cc);
        lt_props_get_f64(&p, "injectors", &s->n_inj);
        lt_props_get_f64(&p, "redline", &s->redline);
        lt_props_get_i32(&p, "fuel_units", &s->fuel_units);
        g = lt_props_get(&p, "gears");
        if (g != NULL) { lt_strlcpy(s->gears, g, sizeof(s->gears)); }
    }
    LT_UNUSED(i);
    lt_props_free(&p);
    lt_free(path);
}

static void spec_save(lt_app *app, perf_state *s)
{
    lt_props p;
    char *path = spec_path(app);
    if (path == NULL) { return; }
    (void)lt_plat_mkdirs(app->config_dir);
    lt_props_init(&p);
    put_f(&p, "weight", s->weight); put_f(&p, "frontal_sqft", s->frontal); put_f(&p, "cd", s->cd);
    put_f(&p, "tire_in", s->tire); put_f(&p, "loss", s->loss); put_f(&p, "final_drive", s->final_drive);
    put_f(&p, "injector_cc", s->inj_cc); put_f(&p, "injectors", s->n_inj); put_f(&p, "redline", s->redline);
    lt_props_set(&p, "gears", s->gears);
    {
        char b[16];
        lt_snprintf(b, sizeof(b), "%d", s->weight_kg); lt_props_set(&p, "weight_kg", b);
        lt_snprintf(b, sizeof(b), "%d", s->fuel_units); lt_props_set(&p, "fuel_units", b);
    }
    if (lt_props_save(&p, path) == 0) { lt_snprintf(s->msg, sizeof(s->msg), "%s", "Vehicle specs saved"); }
    else { lt_snprintf(s->msg, sizeof(s->msg), "Cannot save: %s", lt_err_get()); }
    lt_props_free(&p);
    lt_free(path);
}

static void build_specs(const perf_state *s, lt_perf_specs *out)
{
    lt_perf_specs d;
    int n;
    lt_perf_specs_default(&d);
    *out = d;
    out->mass_kg = s->weight_kg ? s->weight : s->weight * LT_PERF_LBS_TO_KG;
    out->frontal_m2 = s->frontal * LT_PERF_SQFT_TO_SQM;
    out->cd = s->cd;
    out->drivetrain_loss_pct = s->loss;
    out->tire_diameter_in = s->tire;
    out->final_drive = s->final_drive;
    out->injector_cc_min = s->inj_cc;
    out->n_injectors = (int)(s->n_inj + 0.5);
    n = lt_perf_parse_gears(s->gears, out->gears, LT_PERF_MAX_GEARS);
    out->ngears = n > 0 ? n : 0;
}

/* first channel that exists */
static lt_bool chan_any(lt_app *app, const char *const *names, int n, double *out, const char **which)
{
    int i;
    for (i = 0; i < n; i++) {
        if (lt_app_channel(app, names[i], out)) { *which = names[i]; return LT_TRUE; }
    }
    return LT_FALSE;
}

/* live sample: rpm, speed (m/s), acceleration (m/s2, smoothed), pulse width (ms); flags say which exist */
typedef struct perf_live { double rpm, v_ms, accel, pw; int have_rpm, have_v, have_pw; } perf_live;

static void live_sample(lt_app *app, perf_state *s, perf_live *lv)
{
    static const char *const rn[] = { "RPMValue", "rpm", "RPM", "rpmValue" };
    static const char *const vn[] = { "vss", "VSS", "vehicleSpeed", "VehicleSpeed", "speed", "vssSpeed", "SpeedValue" };
    static const char *const pn[] = { "actualLastInjection", "pulseWidth", "pw1", "PW1", "pulseWidth1", "pw" };
    const char *which = "";
    double v = 0.0;
    unsigned long now = (unsigned long)lt_plat_now_ms();
    memset(lv, 0, sizeof(*lv));
    lv->have_rpm = chan_any(app, rn, 4, &lv->rpm, &which) ? 1 : 0;
    lv->have_pw = chan_any(app, pn, 6, &lv->pw, &which) ? 1 : 0;
    if (chan_any(app, vn, 7, &v, &which)) {
        lt_uconv c;
        lt_app_channel_conv(app, which, &c);
        /* native speed unit: INI says (kind SPEED: 0 km/h, 1 mph); anything else is read as km/h */
        lv->v_ms = (c.kind == LT_UK_SPEED && c.src == 1) ? v * LT_PERF_MPH_TO_MS : v / 3.6;
        lv->have_v = 1;
        if (s->prev_ms != 0 && now > s->prev_ms && now - s->prev_ms < 1000UL) {
            double a = lt_perf_accel(s->prev_v, lv->v_ms, (double)(now - s->prev_ms) / 1000.0);
            s->accel_f += (a - s->accel_f) * 0.2;
        }
        s->prev_v = lv->v_ms; s->prev_ms = now;
        lv->accel = s->accel_f;
    }
}

static void perf_stat(lt_ui *ui, const lt_theme *t, lt_rect r, const char *value, const char *label)
{
    if (lt_ui_cell_mode(ui)) {
        char b[96];
        lt_snprintf(b, sizeof(b), "%s %s", label, value);
        lt_ui_draw_text_in(ui, r, b, t->text, 0);
        return;
    }
    lt_ui_draw_box(ui, r, t->bg_app, t->border_subtle, lt_ui_px(ui, 4));
    lt_ui_draw_text_in(ui, lt_rect_make(r.x, r.y + lt_ui_px(ui, 3), r.w, r.h / 2), value, t->text, 1);
    lt_ui_draw_text_in(ui, lt_rect_make(r.x, r.y + r.h / 2, r.w, r.h / 2 - lt_ui_px(ui, 2)), label, t->muted, 1);
}

static void num_row(lt_ui *ui, const char *l1, const char *id1, double *v1, double lo1, double hi1, double st1, int d1,
                    const char *l2, const char *id2, double *v2, double lo2, double hi2, double st2, int d2)
{
    int w[4];
    w[0] = 190; w[1] = 120; w[2] = 190; w[3] = 120;
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label(ui, l1);
    (void)lt_ui_number(ui, id1, v1, lo1, hi1, st1, d1);
    if (l2 != NULL) {
        lt_ui_label(ui, l2);
        (void)lt_ui_number(ui, id2, v2, lo2, hi2, st2, d2);
    }
}

static void perf_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    perf_state *s = (perf_state *)lt_ext_state(app, "perf", sizeof(perf_state), NULL);
    int open = d->open, w1 = -1, w[6], cell = lt_ui_cell_mode(ui), metric = app->settings.metric ? 1 : 0, i;
    static const char *const wunits[2] = { "lbs", "kg" };
    static const char *const funits[3] = { "US gallons", "Imperial gallons", "Liters" };
    char b[160], v1[48], v2[48], v3[48], v4[48];
    lt_perf_specs sp;
    lt_perf_result res;
    perf_live lv;
    double rpm, v_ms, accel, pw_ms, cc, gal_hr, trip_fuel;
    lt_rect r;
    if (!s->loaded) { spec_load(app, s); }
    if (lt_ui_modal_begin(ui, "Performance Calculator", 960, 720, &open)) {
        live_sample(app, s, &lv);
        build_specs(s, &sp);
        lt_ui_layout_row(ui, 1, &w1, -(cell ? 20 : 40));
        if (lt_ui_scroll_begin(ui, "perfscroll")) {
            /* ---- vehicle specifications ---- */
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "Vehicle specifications", t->text, LT_UI_ALIGN_LEFT);
            w[0] = 190; w[1] = 120; w[2] = 90; w[3] = -1;
            lt_ui_layout_row(ui, 4, w, 0);
            lt_ui_label(ui, "Vehicle weight");
            (void)lt_ui_number(ui, "pweight", &s->weight, 100.0, 50000.0, 50.0, 0);
            (void)lt_ui_combo(ui, "pwunit", &s->weight_kg, wunits, 2);
            lt_ui_label(ui, "");
            num_row(ui, "Frontal area (sq ft)", "pfront", &s->frontal, 5.0, 100.0, 0.5, 1,
                    "Drag coefficient (Cd)", "pcd", &s->cd, 0.1, 1.0, 0.01, 2);
            num_row(ui, "Tire diameter (in)", "ptire", &s->tire, 10.0, 60.0, 0.5, 1,
                    "Drivetrain loss (%)", "ploss", &s->loss, 0.0, 60.0, 1.0, 0);
            num_row(ui, "Final drive ratio", "pfinal", &s->final_drive, 1.0, 10.0, 0.01, 2,
                    "Redline (rpm)", "predline", &s->redline, 1000.0, 20000.0, 100.0, 0);
            num_row(ui, "Injector size (cc/min)", "pinj", &s->inj_cc, 10.0, 5000.0, 10.0, 0,
                    "Number of injectors", "pninj", &s->n_inj, 1.0, 16.0, 1.0, 0);
            w[0] = 190; w[1] = -1;
            lt_ui_layout_row(ui, 2, w, 0);
            lt_ui_label(ui, "Gear ratios (comma separated)");
            (void)lt_ui_text(ui, "pgears", s->gears, sizeof(s->gears), 0);
            w[0] = 120; w[1] = 120; w[2] = -1;
            lt_ui_layout_row(ui, 3, w, 0);
            if (lt_ui_button(ui, "Reset")) { double keep = 0.0; (void)keep; defaults(s); s->loaded = 1; }
            if (lt_ui_button_ex(ui, "Save specs", LT_UI_BTN_PRIMARY)) { spec_save(app, s); }
            lt_ui_label_ex(ui, s->msg, t->text2, LT_UI_ALIGN_LEFT);

            /* ---- input values ---- */
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_separator(ui);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "Input values", t->text, LT_UI_ALIGN_LEFT);
            lt_ui_layout_row(ui, 1, &w1, 0);
            (void)lt_ui_checkbox(ui, "Use realtime data (rpm, vehicle speed, acceleration, pulse width)", &s->realtime);
            rpm = s->m_rpm;
            v_ms = metric ? s->m_speed / 3.6 : s->m_speed * LT_PERF_MPH_TO_MS;
            accel = s->m_accel * LT_PERF_GRAVITY;
            pw_ms = s->m_pw;
            if (s->realtime) {
                if (lv.have_rpm) { rpm = lv.rpm; }
                if (lv.have_v) { v_ms = lv.v_ms; accel = lv.accel; }
                if (lv.have_pw) { pw_ms = lv.pw; }
                lt_snprintf(b, sizeof(b), "RPM %s   speed %s   accel %.2f g   pulse width %s", lv.have_rpm ? "live" : "(typed)",
                            lv.have_v ? "live" : "(typed)", accel / LT_PERF_GRAVITY, lv.have_pw ? "live" : "(typed)");
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_ex(ui, b, t->muted, LT_UI_ALIGN_LEFT);
            }
            if (!s->realtime || !lv.have_rpm || !lv.have_v || !lv.have_pw) {
                num_row(ui, "RPM (typed)", "pmrpm", &s->m_rpm, 0.0, 20000.0, 100.0, 0,
                        metric ? "Speed (typed, km/h)" : "Speed (typed, mph)", "pmspd", &s->m_speed, 0.0, 400.0, 5.0, 1);
                num_row(ui, "Acceleration (typed, g)", "pmacc", &s->m_accel, 0.0, 2.0, 0.05, 2,
                        "Pulse width (typed, ms)", "pmpw", &s->m_pw, 0.0, 100.0, 0.1, 2);
            }
            lt_perf_compute(&sp, v_ms, accel, rpm, &res);

            /* ---- results ---- */
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_separator(ui);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "Performance results", t->text, LT_UI_ALIGN_LEFT);
            w[0] = -1; w[1] = -1; w[2] = -1; w[3] = -1;
            lt_ui_layout_row(ui, cell ? 2 : 4, w, cell ? 0 : 46);
            lt_snprintf(v1, sizeof(v1), "%.1f HP", res.hp);
            lt_snprintf(v2, sizeof(v2), "%.1f lb-ft", res.tq_lbft);
            lt_snprintf(v3, sizeof(v3), "%.1f Nm", res.tq_nm);
            lt_snprintf(v4, sizeof(v4), "%.1f kW", res.hp * LT_PERF_HP_W / 1000.0);
            r = lt_ui_layout_next(ui); perf_stat(ui, t, r, v1, "Engine horsepower");
            r = lt_ui_layout_next(ui); perf_stat(ui, t, r, v2, "Engine torque");
            r = lt_ui_layout_next(ui); perf_stat(ui, t, r, v3, "Torque (Nm)");
            r = lt_ui_layout_next(ui); perf_stat(ui, t, r, v4, "Power (kW)");
            lt_ui_layout_row(ui, cell ? 2 : 4, w, cell ? 0 : 46);
            lt_snprintf(v1, sizeof(v1), "%.1f s", res.zero60_s);
            lt_snprintf(v2, sizeof(v2), "%.2f s", res.quarter_s);
            lt_snprintf(v3, sizeof(v3), metric ? "%.1f km/h" : "%.1f mph", metric ? res.quarter_mph * 1.609344 : res.quarter_mph);
            lt_snprintf(v4, sizeof(v4), "%.0f / %.0f N", res.drag_n, res.rolling_n);
            r = lt_ui_layout_next(ui); perf_stat(ui, t, r, v1, "0-60 mph (estimate)");
            r = lt_ui_layout_next(ui); perf_stat(ui, t, r, v2, "1/4 mile ET");
            r = lt_ui_layout_next(ui); perf_stat(ui, t, r, v3, "1/4 mile trap speed");
            r = lt_ui_layout_next(ui); perf_stat(ui, t, r, v4, "Aero drag / rolling");
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "Physics based estimate; actual results vary with conditions.", t->muted, LT_UI_ALIGN_LEFT);

            /* ---- gearing ---- */
            if (sp.ngears > 0) {
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_separator(ui);
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_ex(ui, "Gearing", t->text, LT_UI_ALIGN_LEFT);
                w[0] = 70; w[1] = 90; w[2] = 190; w[3] = 190; w[4] = -1;
                lt_ui_layout_row(ui, 5, w, 0);
                lt_ui_label_ex(ui, "Gear", t->muted, LT_UI_ALIGN_LEFT);
                lt_ui_label_ex(ui, "Ratio", t->muted, LT_UI_ALIGN_LEFT);
                lt_ui_label_ex(ui, "Speed at the current rpm", t->muted, LT_UI_ALIGN_LEFT);
                lt_snprintf(b, sizeof(b), "Speed at %.0f rpm", s->redline);
                lt_ui_label_ex(ui, b, t->muted, LT_UI_ALIGN_LEFT);
                lt_ui_label(ui, "");
                for (i = 0; i < sp.ngears; i++) {
                    double a = lt_perf_speed_at_rpm(&sp, sp.gears[i], rpm), z = lt_perf_speed_at_rpm(&sp, sp.gears[i], s->redline);
                    lt_ui_layout_row(ui, 5, w, 0);
                    lt_snprintf(b, sizeof(b), "%d", i + 1); lt_ui_label(ui, b);
                    lt_snprintf(b, sizeof(b), "%.2f", sp.gears[i]); lt_ui_label(ui, b);
                    lt_snprintf(b, sizeof(b), metric ? "%.1f km/h" : "%.1f mph", metric ? a * 3.6 : a / LT_PERF_MPH_TO_MS);
                    lt_ui_label(ui, b);
                    lt_snprintf(b, sizeof(b), metric ? "%.1f km/h" : "%.1f mph", metric ? z * 3.6 : z / LT_PERF_MPH_TO_MS);
                    lt_ui_label(ui, b);
                    lt_ui_label(ui, "");
                }
            }

            /* ---- fuel economy ---- */
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_separator(ui);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "Fuel economy", t->text, LT_UI_ALIGN_LEFT);
            cc = lt_perf_fuel_cc_min(pw_ms, rpm, sp.n_injectors, sp.injector_cc_min, s->two_stroke);
            {
                double cc_u = s->fuel_units == 2 ? cc / 1000.0 : (s->fuel_units == 1 ? cc / 4546.09 : cc / LT_PERF_CC_PER_GAL_US);
                gal_hr = cc_u * 60.0;
            }
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_snprintf(b, sizeof(b), "Fuel flow %.0f cc/min   %.2f %s per hour", cc, gal_hr,
                        s->fuel_units == 2 ? "liters" : (s->fuel_units == 1 ? "imp. gallons" : "US gallons"));
            lt_ui_label_ex(ui, b, t->text2, LT_UI_ALIGN_LEFT);
            if (cc > 0.0 && v_ms > 0.0) {
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_snprintf(b, sizeof(b), "%.1f mpg (US)   %.1f l/100 km   %.1f km/l", lt_perf_mpg(v_ms, cc), lt_perf_l100(v_ms, cc),
                            lt_perf_kpl(v_ms, cc));
                lt_ui_label_ex(ui, b, t->success, LT_UI_ALIGN_LEFT);
            } else {
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_ex(ui, "Economy needs fuel flow and a vehicle speed (live or typed).", t->muted, LT_UI_ALIGN_LEFT);
            }
            /* trip totals while the dialog is open */
            {
                unsigned long now = (unsigned long)lt_plat_now_ms();
                if (s->trip_ms != 0 && now > s->trip_ms && now - s->trip_ms < 1000UL) {
                    double dt = (double)(now - s->trip_ms) / 1000.0;
                    s->trip_m += v_ms * dt;
                    s->trip_cc += cc / 60.0 * dt;
                }
                s->trip_ms = now;
            }
            trip_fuel = s->fuel_units == 2 ? s->trip_cc / 1000.0 : (s->fuel_units == 1 ? s->trip_cc / 4546.09 : s->trip_cc / LT_PERF_CC_PER_GAL_US);
            w[0] = 190; w[1] = 190; w[2] = 120; w[3] = -1;
            lt_ui_layout_row(ui, 4, w, 0);
            lt_ui_label(ui, "Fuel units");
            (void)lt_ui_combo(ui, "pfuel", &s->fuel_units, funits, 3);
            if (lt_ui_button(ui, "Reset trip")) { s->trip_m = 0.0; s->trip_cc = 0.0; }
            lt_snprintf(b, sizeof(b), metric ? "Trip %.2f km, %.3f %s" : "Trip %.2f miles, %.3f %s",
                        metric ? s->trip_m / 1000.0 : s->trip_m / LT_PERF_MPH_TO_MS / 3600.0, trip_fuel,
                        s->fuel_units == 2 ? "liters" : (s->fuel_units == 1 ? "imp. gal" : "US gal"));
            lt_ui_label_ex(ui, b, t->text2, LT_UI_ALIGN_LEFT);
            lt_ui_layout_row(ui, 1, &w1, 0);
            (void)lt_ui_checkbox(ui, "Two-stroke (one injection per revolution)", &s->two_stroke);
            lt_ui_scroll_end(ui);
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void act_perf(lt_app *app)
{
    (void)lt_app_dialog_push(app, "perf", "", "Performance Calculator");
}

void lt_feat_perf_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "perf"; dk.draw = perf_draw; dk.close = NULL;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "&Performance Calculator...", NULL, act_perf, NULL);
}
