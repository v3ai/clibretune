/* tables_gen.c - "Generate table" dialog: seed one VE / ignition / AFR table from engine specs,
 * with a heat-map preview, using the base map generator.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "tables_priv.h"
#include "app_ext.h"

typedef struct gen_state {
    int init;
    int fuel, asp, cycle, inj, ign;
    double cyl, disp, inj_cc, idle, redline;
} gen_state;

static const char *const g_fuels[] = { "Gasoline", "E85", "E100", "Methanol", "LPG" };
static const char *const g_asps[] = { "Naturally aspirated", "Turbo", "Supercharged" };
static const char *const g_cycles[] = { "4-stroke", "2-stroke" };

static void gen_spec_of(const gen_state *s, lt_engine_spec *e)
{
    lt_engine_spec_default(e);
    e->cylinder_count = (int)(s->cyl + 0.5);
    e->displacement_cc = s->disp;
    e->injector_size_cc = s->inj_cc;
    e->fuel = (lt_fuel_type)s->fuel;
    e->aspiration = (lt_aspiration)s->asp;
    e->engine_stroke = (lt_stroke)s->cycle;
    e->idle_rpm = (unsigned)s->idle;
    e->redline_rpm = (unsigned)s->redline;
    if (e->redline_rpm <= e->idle_rpm + 500) { e->redline_rpm = e->idle_rpm + 500; }
}

int lt_tabs_gen_values(const lt_engine_spec *spec, int role, int write_axes, const double *xb,
                       size_t nx, const double *yb, size_t ny, double *out_x, double *out_y,
                       double *out_z)
{
    lt_basemap m;
    double flat[LT_BASEMAP_N * LT_BASEMAP_N];
    size_t r, c;
    int rc;
    if (nx == 0 || ny == 0 || out_x == NULL || out_y == NULL || out_z == NULL) { return LT_EINVAL; }
    if (role != LT_ROLE_VE && role != LT_ROLE_IGNITION && role != LT_ROLE_AFR_TARGET) {
        lt_err_set("this table cannot be generated from engine specs");
        return LT_EINVAL;
    }
    lt_basemap_generate(spec, &m);
    for (r = 0; r < LT_BASEMAP_N; r++) {
        for (c = 0; c < LT_BASEMAP_N; c++) {
            flat[r * LT_BASEMAP_N + c] = role == LT_ROLE_VE ? m.ve[r][c]
                                         : (role == LT_ROLE_IGNITION ? m.ign[r][c] : m.afr[r][c]);
        }
    }
    if (write_axes) {
        lt_basemap_rpm_bins(spec->idle_rpm, spec->redline_rpm, nx, out_x);
        lt_basemap_load_bins(m.load_bins[LT_BASEMAP_N - 1], ny, out_y);
    } else {
        memcpy(out_x, xb, nx * sizeof(double));
        memcpy(out_y, yb, ny * sizeof(double));
    }
    rc = lt_tbl_rebin(m.rpm_bins, LT_BASEMAP_N, m.load_bins, LT_BASEMAP_N, flat, out_x, nx,
                      out_y, ny, LT_TRUE, out_z);
    lt_basemap_free(&m);
    return rc;
}

static void gen_preview(lt_app *app, const double *z, size_t nx, size_t ny, int zdig)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, cw, chh, x, y;
    double lo = 1e300, hi = -1e300;
    size_t i;
    lt_rect r;
    char b[32];
    chh = lt_ui_px(ui, ny > 16 ? 320 / (int)ny : 14);
    if (chh < lt_ui_px(ui, 6)) { chh = lt_ui_px(ui, 6); }
    for (i = 0; i < nx * ny; i++) {
        if (z[i] < lo) { lo = z[i]; }
        if (z[i] > hi) { hi = z[i]; }
    }
    lt_ui_layout_row_px(ui, 1, &w1, chh * (int)ny + lt_ui_px(ui, 4));
    r = lt_ui_layout_next(ui);
    cw = (int)nx > 0 ? (r.w - lt_ui_px(ui, 4)) / (int)nx : 0;
    if (cw > lt_ui_px(ui, 44)) { cw = lt_ui_px(ui, 44); }
    for (y = 0; y < (int)ny; y++) {
        for (x = 0; x < (int)nx; x++) {
            double v = z[(size_t)(ny - 1 - (size_t)y) * nx + (size_t)x];
            lt_rect q;
            lt_color bg = lt_tabs_heat(app, th, lo, hi, v);
            q.x = r.x + x * cw; q.y = r.y + y * chh; q.w = cw; q.h = chh;
            lt_ui_draw_fill(ui, q, bg);
            if (cw >= lt_ui_px(ui, 24)) {
                lt_tabs_fmt(b, sizeof(b), v, zdig);
                lt_ui_draw_text_in(ui, q, b, lt_tabs_heat_text(bg), LT_UI_ALIGN_CENTER);
            }
        }
    }
}

int lt_tabs_gen_draw(lt_app *app, lt_tabs_gen_dlg *d, const char *title, const double *xb,
                     size_t nx, const double *yb, size_t ny, int zdig, double *out_x,
                     double *out_y, double *out_z)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    gen_state *s;
    lt_engine_spec e;
    int w1 = -1, w4[4], wb[3], ok = 0;
    char b[96];
    if (!d->open) { return 0; }
    s = (gen_state *)lt_ext_state(app, "tables_gen", sizeof(*s), NULL);
    if (!s->init) {
        s->init = 1;
        s->cyl = 4; s->disp = 2000; s->inj_cc = 440; s->idle = 800; s->redline = 6500;
    }
    if (!lt_ui_modal_begin(ui, "Generate table from engine specs", 620, 560, &d->open)) { return 0; }
    lt_snprintf(b, sizeof(b), "Seed '%s' from basic engine parameters over its own axes.", title);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, b, th->text2, LT_UI_ALIGN_LEFT);
    w4[0] = 90; w4[1] = -1; w4[2] = 90; w4[3] = -1;
    lt_ui_layout_row(ui, 4, w4, 0);
    lt_ui_label(ui, "Fuel");
    lt_ui_combo(ui, "gen_fuel", &s->fuel, g_fuels, 5);
    lt_ui_label(ui, "Aspiration");
    lt_ui_combo(ui, "gen_asp", &s->asp, g_asps, 3);
    lt_ui_label(ui, "Cycle");
    lt_ui_combo(ui, "gen_cyc", &s->cycle, g_cycles, 2);
    lt_ui_label(ui, "Cylinders");
    lt_ui_number(ui, "gen_cyl", &s->cyl, 1.0, 16.0, 1.0, 0);
    lt_ui_label(ui, "Displacement cc");
    lt_ui_number(ui, "gen_disp", &s->disp, 50.0, 20000.0, 50.0, 0);
    lt_ui_label(ui, "Injector cc/min");
    lt_ui_number(ui, "gen_inj", &s->inj_cc, 50.0, 5000.0, 10.0, 0);
    lt_ui_label(ui, "Idle RPM");
    lt_ui_number(ui, "gen_idle", &s->idle, 300.0, 3000.0, 50.0, 0);
    lt_ui_label(ui, "Redline RPM");
    lt_ui_number(ui, "gen_red", &s->redline, 2000.0, 20000.0, 100.0, 0);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_checkbox(ui, "Also set the axes from the engine (idle..redline, load)", &d->write_axes);
    gen_spec_of(s, &e);
    if (lt_tabs_gen_values(&e, d->role, d->write_axes, xb, nx, yb, ny, out_x, out_y, out_z) == 0) {
        lt_ui_label_ex(ui, "Preview (high load at the top)", th->muted, LT_UI_ALIGN_LEFT);
        gen_preview(app, out_z, nx, ny, zdig);
    } else {
        lt_ui_label_ex(ui, lt_err_get(), th->error, LT_UI_ALIGN_LEFT);
    }
    wb[0] = -1; wb[1] = 100; wb[2] = 90;
    lt_ui_layout_row(ui, 3, wb, 0);
    lt_ui_label(ui, "");
    if (lt_ui_button_ex(ui, "Apply", LT_UI_BTN_DEFAULT)) { ok = 1; d->open = 0; }
    if (lt_ui_button(ui, "Cancel")) { d->open = 0; }
    lt_ui_modal_end(ui);
    return ok;
}
