/* feat_calc.c - Tools > Calculators... (TunerStudio "Calculators"): one dialog with four tabs
 *   Expression   the INI expression language (src/ini/expr.h) evaluated live; names resolve to
 *                realtime channels, then constants of the open tune (app->varenv)
 *   Linear       two-point calibration: slope / offset of the line through two (input, output)
 *                points, input -> value and value -> input (volts <-> pressure, ...)
 *   Units        temperature, pressure, speed, fuel flow, AFR <-> lambda per fuel, injector
 *                flow at another fuel pressure
 *   Injectors    required injector size from power, BSFC, injector count and duty cycle; duty
 *                and supported power of a given injector; duty from pulse width and rpm
 * Dialog kind "calc". The maths is calc_logic.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "calc_logic.h"

#define CALC_HIST 8

typedef struct calc_state {
    int inited, tab;
    /* expression */
    char expr[256], compiled_src[256], err[160];
    lt_expr *compiled;
    char hist[CALC_HIST][200];
    int nhist;
    /* linear */
    double x1, y1, x2, y2, lin_x, lin_y;
    /* units */
    double temp_v, press_v, speed_v, flow_v, afr_v, lambda_v, inj_flow, inj_rated, inj_new;
    int temp_u, press_u, speed_u, flow_u, fuel;
    /* injectors */
    double hp, bsfc, n_inj, duty_pct, size_v, pw_ms, rpm;
    int size_u, two_stroke;
} calc_state;

static void calc_free(lt_app *app, void *p)
{
    calc_state *s = (calc_state *)p;
    LT_UNUSED(app);
    if (s->compiled != NULL) { lt_expr_free(s->compiled); s->compiled = NULL; }
}

static calc_state *calc_get(lt_app *app)
{
    calc_state *s = (calc_state *)lt_ext_state(app, "calc", sizeof(calc_state), calc_free);
    if (!s->inited) {
        s->inited = 1;
        lt_strlcpy(s->expr, "14.7 * 0.85 + pow(2, 3)", sizeof(s->expr));
        s->x1 = 0.5; s->y1 = 0.0; s->x2 = 4.5; s->y2 = 100.0; s->lin_x = 2.5; s->lin_y = 50.0;
        s->temp_v = 90.0; s->press_v = 100.0; s->speed_v = 100.0; s->flow_v = 440.0;
        s->afr_v = 14.7; s->lambda_v = 1.0; s->inj_flow = 440.0; s->inj_rated = 300.0; s->inj_new = 400.0;
        s->hp = 300.0; s->bsfc = 0.5; s->n_inj = 4.0; s->duty_pct = 80.0; s->size_v = 440.0;
        s->pw_ms = 8.0; s->rpm = 6000.0;
        {   /* scripted use (screenshots): LT_CALC_TAB=0..3, LT_CALC_EXPR=<expression> */
            char *e = lt_plat_getenv("LT_CALC_TAB");
            if (e != NULL && e[0] >= '0' && e[0] <= '3') { s->tab = e[0] - '0'; }
            lt_free(e);
            e = lt_plat_getenv("LT_CALC_EXPR");
            if (e != NULL && e[0] != '\0') { lt_strlcpy(s->expr, e, sizeof(s->expr)); }
            lt_free(e);
        }
    }
    return s;
}

static void fmtv(char *b, size_t cap, double v, int dec)
{
    lt_fmt_double(b, cap, v, dec);
}

/* ---- expression ---- */
static const lt_expr_env *calc_env(lt_app *app, lt_expr_env *blank)
{
    if (app->ini != NULL) { return &app->varenv.env; }
    lt_expr_env_init(blank);
    return blank;
}

static void expr_compile(calc_state *s)
{
    if (s->compiled != NULL && strcmp(s->compiled_src, s->expr) == 0) { return; }
    if (s->compiled != NULL) { lt_expr_free(s->compiled); s->compiled = NULL; }
    lt_strlcpy(s->compiled_src, s->expr, sizeof(s->compiled_src));
    s->err[0] = '\0';
    if (s->expr[0] == '\0') { return; }
    s->compiled = lt_expr_compile(s->expr, s->err, sizeof(s->err));
    if (s->compiled == NULL && s->err[0] == '\0') { lt_strlcpy(s->err, "Syntax error", sizeof(s->err)); }
}

/* result text of the current expression ("" when empty); 0 ok, -1 error (message in out) */
static int expr_result(lt_app *app, calc_state *s, char *out, size_t cap)
{
    lt_expr_env blank;
    lt_value v;
    int rc;
    out[0] = '\0';
    expr_compile(s);
    if (s->compiled == NULL) {
        if (s->err[0] != '\0') { lt_strlcpy(out, s->err, cap); return -1; }
        return 0;
    }
    lt_value_init(&v);
    rc = lt_expr_eval(s->compiled, calc_env(app, &blank), &v);
    if (rc != 0) {
        lt_strlcpy(out, lt_err_get(), cap);
        lt_value_clear(&v);
        return -1;
    }
    if (v.t == LT_V_NUM) { fmtv(out, cap, v.n, 6); }
    else if (v.t == LT_V_BOOL) { lt_strlcpy(out, v.b ? "true" : "false", cap); }
    else {
        char *t = lt_value_to_string(&v);
        lt_snprintf(out, cap, "\"%s\"", t != NULL ? t : "");
        lt_free(t);
    }
    lt_value_clear(&v);
    return 0;
}

static void tab_expr(lt_app *app, calc_state *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, w[3], rc, fl, i;
    char res[200], b[260];
    char **names = NULL;
    size_t nn, k;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "Expression (INI syntax: + - * / %, comparisons, && ||, ?:, bit operators; functions such as "
                   "sqrt(), pow(), abs(), min(), max(), round(), if(), table())", t->text2, LT_UI_ALIGN_LEFT);
    w[0] = -1; w[1] = 140;
    lt_ui_layout_row(ui, 2, w, 0);
    fl = lt_ui_text(ui, "cexpr", s->expr, sizeof(s->expr), 0);
    rc = expr_result(app, s, res, sizeof(res));
    if ((lt_ui_button_ex(ui, "Add to history", s->expr[0] != '\0' && rc == 0 ? 0 : LT_UI_BTN_DISABLED) ||
         (fl & LT_UI_SUBMIT)) && s->expr[0] != '\0' && rc == 0) {
        for (i = CALC_HIST - 1; i > 0; i--) { lt_strlcpy(s->hist[i], s->hist[i - 1], sizeof(s->hist[i])); }
        lt_snprintf(s->hist[0], sizeof(s->hist[0]), "%s = %s", s->expr, res);
        if (s->nhist < CALC_HIST) { s->nhist++; }
    }
    w[0] = 110; w[1] = -1;
    lt_ui_layout_row(ui, 2, w, 0);
    lt_ui_label(ui, "Result:");
    lt_ui_label_ex(ui, res[0] != '\0' ? res : "-", rc == 0 ? t->success : t->error, LT_UI_ALIGN_LEFT);
    /* the names the expression uses and what they resolve to right now */
    nn = lt_expr_identifiers(s->expr, &names);
    if (nn > 0) {
        lt_expr_env blank;
        const lt_expr_env *env = calc_env(app, &blank);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Variables", t->text, LT_UI_ALIGN_LEFT);
        for (k = 0; k < nn && k < 8; k++) {
            double v = 0.0;
            lt_bool known = env->get_var != NULL && env->get_var(env->ud, names[k], &v);
            w[0] = 220; w[1] = -1;
            lt_ui_layout_row(ui, 2, w, 0);
            lt_ui_label(ui, names[k]);
            if (known) { fmtv(b, sizeof(b), v, 6); }
            else { lt_strlcpy(b, "unknown (reads as 0)", sizeof(b)); }
            lt_ui_label_ex(ui, b, known ? t->text2 : t->warning, LT_UI_ALIGN_LEFT);
        }
        lt_expr_names_free(names, nn);
    }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, app->ini != NULL ? "Names resolve to live channels first, then to constants of the open tune."
                                        : "Open a project or the demo to use channel and constant names.",
                   t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_separator(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "History", t->text, LT_UI_ALIGN_LEFT);
    if (s->nhist == 0) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Press Enter or Add to history to keep a result.", t->muted, LT_UI_ALIGN_LEFT);
    }
    for (i = 0; i < s->nhist; i++) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, s->hist[i], t->text2, LT_UI_ALIGN_LEFT);
    }
}

/* ---- linear calibration ---- */
static void num_pair(lt_ui *ui, const char *l1, const char *id1, double *v1, const char *l2, const char *id2,
                     double *v2, int dec)
{
    int w[4];
    w[0] = 200; w[1] = 140; w[2] = 200; w[3] = 140;
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label(ui, l1);
    (void)lt_ui_number(ui, id1, v1, -1e9, 1e9, 0.1, dec);
    lt_ui_label(ui, l2);
    (void)lt_ui_number(ui, id2, v2, -1e9, 1e9, 0.1, dec);
}

static void tab_linear(lt_app *app, calc_state *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, w[4];
    double m = 0.0, c = 0.0, x = 0.0;
    char b[200], b1[48], b2[48];
    lt_bool ok;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "Two known points of a linear sensor (input = volts, ADC counts...; output = the value)",
                   t->text2, LT_UI_ALIGN_LEFT);
    num_pair(ui, "Point 1 input", "lx1", &s->x1, "Point 1 output", "ly1", &s->y1, 4);
    num_pair(ui, "Point 2 input", "lx2", &s->x2, "Point 2 output", "ly2", &s->y2, 4);
    ok = lt_calc_linear(s->x1, s->y1, s->x2, s->y2, &m, &c);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_separator(ui);
    if (!ok) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "The two inputs must differ.", t->error, LT_UI_ALIGN_LEFT);
        return;
    }
    fmtv(b1, sizeof(b1), m, 6);
    fmtv(b2, sizeof(b2), c, 6);
    w[0] = 200; w[1] = -1;
    lt_ui_layout_row(ui, 2, w, 0);
    lt_ui_label(ui, "Slope (scale):");
    lt_ui_label_ex(ui, b1, t->success, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 2, w, 0);
    lt_ui_label(ui, "Offset (translate):");
    lt_ui_label_ex(ui, b2, t->success, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 2, w, 0);
    lt_ui_label(ui, "Formula:");
    lt_snprintf(b, sizeof(b), "output = %s * input %s %s", b1, c < 0.0 ? "-" : "+", c < 0.0 ? b2 + 1 : b2);
    lt_ui_label_ex(ui, b, t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_separator(ui);
    w[0] = 200; w[1] = 140; w[2] = 60; w[3] = -1;
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label(ui, "Input -> output:");
    (void)lt_ui_number(ui, "linx", &s->lin_x, -1e9, 1e9, 0.1, 4);
    lt_ui_label_ex(ui, "=", t->muted, LT_UI_ALIGN_CENTER);
    fmtv(b, sizeof(b), lt_calc_linear_y(m, c, s->lin_x), 4);
    lt_ui_label_ex(ui, b, t->success, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label(ui, "Output -> input:");
    (void)lt_ui_number(ui, "liny", &s->lin_y, -1e9, 1e9, 0.1, 4);
    lt_ui_label_ex(ui, "=", t->muted, LT_UI_ALIGN_CENTER);
    if (lt_calc_linear_x(m, c, s->lin_y, &x)) { fmtv(b, sizeof(b), x, 4); }
    else { lt_strlcpy(b, "(flat line)", sizeof(b)); }
    lt_ui_label_ex(ui, b, t->success, LT_UI_ALIGN_LEFT);
}

/* ---- units ---- */
static void unit_row(lt_ui *ui, const char *label, const char *id, double *v, int *unit, const char *const *units,
                     int nu, const char *result)
{
    const lt_theme *t = lt_ui_theme(ui);
    int w[4];
    char cid[24];
    w[0] = 150; w[1] = 120; w[2] = 90; w[3] = -1;
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label(ui, label);
    (void)lt_ui_number(ui, id, v, -1e9, 1e9, 1.0, 3);
    lt_snprintf(cid, sizeof(cid), "%su", id);
    (void)lt_ui_combo(ui, cid, unit, units, nu);
    lt_ui_label_ex(ui, result, t->success, LT_UI_ALIGN_LEFT);
}

static void conv_text(char *out, size_t cap, double v, int from, int n, double (*conv)(double, int, int),
                      const char *(*unit)(int))
{
    int u;
    size_t len = 0;
    out[0] = '\0';
    for (u = 0; u < n; u++) {
        char num[48];
        if (u == from) { continue; }
        fmtv(num, sizeof(num), conv(v, from, u), 3);
        lt_snprintf(out + len, cap - len, "%s%s %s", len > 0 ? "   " : "= ", num, unit(u));
        len = strlen(out);
    }
}

static double g_density = 0.74;
static double flow_conv(double v, int from, int to) { return lt_calc_flow(v, from, to, g_density); }

static void tab_units(lt_app *app, calc_state *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    static const char *const tu[LT_CALC_TEMP_N] = { "\302\260C", "\302\260F", "K" };
    static const char *const pu[LT_CALC_P_N] = { "kPa", "psi", "bar", "inHg", "atm" };
    static const char *const vu[LT_CALC_V_N] = { "km/h", "mph", "m/s" };
    static const char *const fu[LT_CALC_F_N] = { "cc/min", "lb/hr", "g/s" };
    const char *fuels[16];
    const lt_calc_fuel *fuel;
    int w1 = -1, w[4], i, nf = lt_calc_fuel_count();
    char b[240], n1[48];
    for (i = 0; i < nf && i < 16; i++) { fuels[i] = lt_calc_fuel_get(i)->name; }
    if (s->fuel < 0 || s->fuel >= nf) { s->fuel = 0; }
    fuel = lt_calc_fuel_get(s->fuel);
    g_density = fuel->density;
    conv_text(b, sizeof(b), s->temp_v, s->temp_u, LT_CALC_TEMP_N, lt_calc_temp, lt_calc_temp_unit);
    unit_row(ui, "Temperature", "utemp", &s->temp_v, &s->temp_u, tu, LT_CALC_TEMP_N, b);
    conv_text(b, sizeof(b), s->press_v, s->press_u, LT_CALC_P_N, lt_calc_pressure, lt_calc_pressure_unit);
    unit_row(ui, "Pressure", "upress", &s->press_v, &s->press_u, pu, LT_CALC_P_N, b);
    conv_text(b, sizeof(b), s->speed_v, s->speed_u, LT_CALC_V_N, lt_calc_speed, lt_calc_speed_unit);
    unit_row(ui, "Speed", "uspeed", &s->speed_v, &s->speed_u, vu, LT_CALC_V_N, b);
    conv_text(b, sizeof(b), s->flow_v, s->flow_u, LT_CALC_F_N, flow_conv, lt_calc_flow_unit);
    unit_row(ui, "Fuel flow", "uflow", &s->flow_v, &s->flow_u, fu, LT_CALC_F_N, b);

    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_separator(ui);
    w[0] = 150; w[1] = 210; w[2] = -1;
    lt_ui_layout_row(ui, 3, w, 0);
    lt_ui_label(ui, "Fuel");
    (void)lt_ui_combo(ui, "ufuel", &s->fuel, fuels, nf);
    fmtv(n1, sizeof(n1), fuel->stoich, 3);
    lt_snprintf(b, sizeof(b), "stoichiometric AFR %s, density %.3f g/cc (used for fuel flow)", n1, fuel->density);
    lt_ui_label_ex(ui, b, t->text2, LT_UI_ALIGN_LEFT);
    w[0] = 150; w[1] = 120; w[2] = -1;
    lt_ui_layout_row(ui, 3, w, 0);
    lt_ui_label(ui, "AFR");
    (void)lt_ui_number(ui, "uafr", &s->afr_v, 0.0, 100.0, 0.1, 2);
    fmtv(n1, sizeof(n1), lt_calc_afr_to_lambda(s->afr_v, fuel->stoich), 3);
    lt_snprintf(b, sizeof(b), "= lambda %s", n1);
    lt_ui_label_ex(ui, b, t->success, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 3, w, 0);
    lt_ui_label(ui, "Lambda");
    (void)lt_ui_number(ui, "ulam", &s->lambda_v, 0.0, 10.0, 0.01, 3);
    fmtv(n1, sizeof(n1), lt_calc_lambda_to_afr(s->lambda_v, fuel->stoich), 2);
    lt_snprintf(b, sizeof(b), "= AFR %s (%s)", n1, fuel->name);
    lt_ui_label_ex(ui, b, t->success, LT_UI_ALIGN_LEFT);

    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_separator(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "Injector flow at another fuel pressure (any flow unit, any pressure unit)", t->text, LT_UI_ALIGN_LEFT);
    w[0] = 150; w[1] = 120; w[2] = 150; w[3] = 120;
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label(ui, "Rated flow");
    (void)lt_ui_number(ui, "uiflow", &s->inj_flow, 0.0, 1e6, 10.0, 1);
    lt_ui_label(ui, "at pressure");
    (void)lt_ui_number(ui, "uirated", &s->inj_rated, 0.0, 1e6, 10.0, 1);
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label(ui, "New pressure");
    (void)lt_ui_number(ui, "uinew", &s->inj_new, 0.0, 1e6, 10.0, 1);
    lt_ui_label(ui, "New flow");
    fmtv(n1, sizeof(n1), lt_calc_injector_at_pressure(s->inj_flow, s->inj_rated, s->inj_new), 1);
    lt_ui_label_ex(ui, n1, t->success, LT_UI_ALIGN_LEFT);
}

/* ---- injectors ---- */
static void tab_injectors(lt_app *app, calc_state *s)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    static const char *const su[2] = { "cc/min", "lb/hr" };
    int w1 = -1, w[4], ninj = (int)(s->n_inj + 0.5);
    double duty = s->duty_pct / 100.0, req, size_lb, d, live_rpm = 0.0, live_pw = 0.0;
    double dens = 0.72;
    char b[200], n1[48], n2[48];
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "Required injector size", t->text, LT_UI_ALIGN_LEFT);
    w[0] = 200; w[1] = 130; w[2] = 200; w[3] = 130;
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label(ui, "Target power (hp, crank)");
    (void)lt_ui_number(ui, "ihp", &s->hp, 0.0, 10000.0, 10.0, 0);
    lt_ui_label(ui, "BSFC (lb/hp/hr)");
    (void)lt_ui_number(ui, "ibsfc", &s->bsfc, 0.2, 1.5, 0.01, 2);
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label(ui, "Injectors (cylinders)");
    (void)lt_ui_number(ui, "ininj", &s->n_inj, 1.0, 24.0, 1.0, 0);
    lt_ui_label(ui, "Maximum duty cycle (%)");
    (void)lt_ui_number(ui, "iduty", &s->duty_pct, 1.0, 100.0, 1.0, 0);
    req = lt_calc_injector_required(s->hp, s->bsfc, ninj, duty);
    fmtv(n1, sizeof(n1), req, 1);
    fmtv(n2, sizeof(n2), lt_calc_flow(req, LT_CALC_F_LBHR, LT_CALC_F_CCMIN, dens), 0);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_snprintf(b, sizeof(b), "Required: %s lb/hr  =  %s cc/min per injector (gasoline, 0.72 g/cc)", n1, n2);
    lt_ui_label_ex(ui, b, t->success, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "Typical BSFC: 0.45-0.50 naturally aspirated, 0.55-0.65 boosted, 0.7-0.8 on E85.",
                   t->muted, LT_UI_ALIGN_LEFT);

    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_separator(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "Duty cycle of a given injector", t->text, LT_UI_ALIGN_LEFT);
    w[0] = 200; w[1] = 130; w[2] = 100; w[3] = -1;
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label(ui, "Injector size");
    (void)lt_ui_number(ui, "isize", &s->size_v, 1.0, 1e5, 10.0, 1);
    (void)lt_ui_combo(ui, "isizeu", &s->size_u, su, 2);
    lt_ui_label(ui, "");
    size_lb = s->size_u == 1 ? s->size_v : lt_calc_flow(s->size_v, LT_CALC_F_CCMIN, LT_CALC_F_LBHR, dens);
    d = lt_calc_injector_duty(s->hp, s->bsfc, ninj, size_lb);
    lt_ui_layout_row(ui, 1, &w1, 0);
    fmtv(n1, sizeof(n1), lt_calc_injector_max_hp(size_lb, s->bsfc, ninj, duty), 0);
    lt_snprintf(b, sizeof(b), "Duty cycle at %.0f hp: %.1f%%    Power supported at %.0f%% duty: %s hp", s->hp, d * 100.0,
                s->duty_pct, n1);
    lt_ui_label_ex(ui, b, d > 0.9 ? t->error : (d > duty ? t->warning : t->success), LT_UI_ALIGN_LEFT);

    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_separator(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "Duty cycle from pulse width", t->text, LT_UI_ALIGN_LEFT);
    w[0] = 200; w[1] = 130; w[2] = 200; w[3] = 130;
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label(ui, "Pulse width (ms)");
    (void)lt_ui_number(ui, "ipw", &s->pw_ms, 0.0, 100.0, 0.1, 2);
    lt_ui_label(ui, "Engine speed (rpm)");
    (void)lt_ui_number(ui, "irpm", &s->rpm, 0.0, 30000.0, 100.0, 0);
    lt_ui_layout_row(ui, 1, &w1, 0);
    (void)lt_ui_checkbox(ui, "Two-stroke (one injection per revolution)", &s->two_stroke);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_snprintf(b, sizeof(b), "Duty cycle: %.1f%%", lt_calc_duty_from_pw(s->pw_ms, s->rpm, s->two_stroke) * 100.0);
    lt_ui_label_ex(ui, b, t->success, LT_UI_ALIGN_LEFT);
    if (lt_app_channel(app, "rpm", &live_rpm) || lt_app_channel(app, "RPMValue", &live_rpm)) {
        if (lt_app_channel(app, "pulseWidth", &live_pw) || lt_app_channel(app, "pulseWidth1", &live_pw) ||
            lt_app_channel(app, "actualLastInjection", &live_pw)) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_snprintf(b, sizeof(b), "Live: %.2f ms at %.0f rpm = %.1f%% duty", live_pw, live_rpm,
                        lt_calc_duty_from_pw(live_pw, live_rpm, s->two_stroke) * 100.0);
            lt_ui_label_ex(ui, b, t->text2, LT_UI_ALIGN_LEFT);
        }
    }
}

static void calc_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    calc_state *s = calc_get(app);
    static const char *const tabs[4] = { "Expression", "Linear calibration", "Unit conversion", "Injectors" };
    int open = d->open, w1 = -1, w[2], cell = lt_ui_cell_mode(ui);
    if (lt_ui_modal_begin(ui, "Calculators", 900, 620, &open)) {
        if (cell) {
            /* terminal: a row of toggle buttons keeps the active tab's name readable */
            int wt[4], i;
            for (i = 0; i < 4; i++) { wt[i] = (int)strlen(tabs[i]) * 8 + 24; }
            lt_ui_layout_row(ui, 4, wt, 0);
            for (i = 0; i < 4; i++) {
                if (lt_ui_button_ex(ui, tabs[i], s->tab == i ? LT_UI_BTN_SELECTED : LT_UI_BTN_FLAT)) { s->tab = i; }
            }
        } else {
            lt_ui_layout_row(ui, 1, &w1, 0);
            (void)lt_ui_tabs_flat(ui, "calctabs", tabs, 4, &s->tab);
        }
        lt_ui_layout_row(ui, 1, &w1, -(cell ? 20 : 40));
        if (lt_ui_scroll_begin(ui, "calcbody")) {
            switch (s->tab) {
            case 1: tab_linear(app, s); break;
            case 2: tab_units(app, s); break;
            case 3: tab_injectors(app, s); break;
            default: tab_expr(app, s); break;
            }
            lt_ui_scroll_end(ui);
        }
        w[0] = -1; w[1] = 100;
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label(ui, "");
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

/* ---- scripted use (tests): evaluate an expression the way the Expression tab does ---- */
int lt_feat_calc_eval(lt_app *app, const char *expr, char *out, size_t cap)
{
    calc_state *s = calc_get(app);
    lt_strlcpy(s->expr, expr != NULL ? expr : "", sizeof(s->expr));
    return expr_result(app, s, out, cap);
}

static void act_calc(lt_app *app)
{
    (void)lt_app_dialog_push(app, "calc", "", "Calculators");
}

void lt_feat_calc_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "calc"; dk.draw = calc_draw; dk.close = NULL;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "Calc&ulators...", NULL, act_calc, NULL);
}
