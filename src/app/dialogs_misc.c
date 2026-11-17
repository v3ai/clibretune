/* dialogs_misc.c - base map wizard, restore points dialog and special tabs
 * (output channel status, console; logger tabs are placeholders).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dialogs.h"
#include "../basemap/basemap.h"

#define CH 8
#define CV 16

/* ================================================================== */
/* Base map wizard                                                     */
typedef struct bm_state {
    lt_engine_spec spec;
    lt_basemap map;
    int have_map;
    int which;           /* 0 VE, 1 ignition, 2 AFR */
    int write_axes;
    int fuel, asp, cycle, inj, ign, chamber;
    double cyl, disp, inj_cc, idle, redline;
    char log[320];
} bm_state;

static const char *const fuels[] = { "Gasoline", "E85", "E100", "Methanol", "LPG" };
static const char *const asps[] = { "Naturally aspirated", "Turbo", "Supercharged" };
static const char *const cycle_names[] = { "4-stroke", "2-stroke" };
static const char *const injs[] = { "Simultaneous", "Batch", "Sequential", "Throttle body" };
static const char *const igns[] = { "Wasted spark", "Coil on plug", "Distributor" };
static const char *const chambers[] = { "Open", "Quench / 2-valve", "Swirl / multi-valve" };

static void bm_regen(bm_state *s)
{
    lt_engine_spec *e = &s->spec;
    e->cylinder_count = (int)(s->cyl + 0.5);
    e->displacement_cc = s->disp;
    e->injector_size_cc = s->inj_cc;
    e->fuel = (lt_fuel_type)s->fuel;
    e->aspiration = (lt_aspiration)s->asp;
    e->engine_stroke = (lt_stroke)s->cycle;
    e->inj_mode = (lt_inj_mode)s->inj;
    e->ign_mode = (lt_ign_mode)s->ign;
    e->chamber = (lt_chamber)s->chamber;
    e->idle_rpm = (unsigned)s->idle;
    e->redline_rpm = (unsigned)s->redline;
    if (e->redline_rpm <= e->idle_rpm + 500) { e->redline_rpm = e->idle_rpm + 500; }
    if (s->have_map) { lt_basemap_free(&s->map); }
    lt_basemap_generate(e, &s->map);
    s->have_map = 1;
}

static void bm_preview(lt_app *app, bm_state *s, int w, int h)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, i, j, cw, chh = 16, hw = 40, hh = 16;
    lt_rect r;
    double lo = 1e300, hi = -1e300;
    double (*tab)[LT_BASEMAP_N];
    char b[24];
    tab = s->which == 0 ? s->map.ve : (s->which == 1 ? s->map.ign : s->map.afr);
    for (i = 0; i < LT_BASEMAP_N; i++) {
        for (j = 0; j < LT_BASEMAP_N; j++) {
            if (tab[i][j] < lo) { lo = tab[i][j]; }
            if (tab[i][j] > hi) { hi = tab[i][j]; }
        }
    }
    if (hi - lo < 1e-9) { hi = lo + 1.0; }
    lt_ui_layout_row(ui, 1, &w1, hh + chh * LT_BASEMAP_N + 4);
    r = lt_ui_layout_next(ui);
    cw = (w - hw - 4) / LT_BASEMAP_N;
    if (cw > 44) { cw = 44; }
    LT_UNUSED(h);
    for (j = 0; j < LT_BASEMAP_N; j++) {
        lt_rect q;
        q.x = r.x + hw + j * cw; q.y = r.y; q.w = cw; q.h = hh;
        lt_fmt_double(b, sizeof(b), s->map.rpm_bins[j], 0);
        lt_ui_draw_text_in(ui, q, b, t->muted, LT_UI_ALIGN_CENTER);
    }
    for (i = 0; i < LT_BASEMAP_N; i++) {
        lt_rect q;
        /* row 0 = lowest load; draw highest load on top like the table editor */
        int row = LT_BASEMAP_N - 1 - i;
        q.x = r.x; q.y = r.y + hh + i * chh; q.w = hw - 2; q.h = chh;
        lt_fmt_double(b, sizeof(b), s->map.load_bins[row], 0);
        lt_ui_draw_text_in(ui, q, b, t->muted, LT_UI_ALIGN_RIGHT);
        for (j = 0; j < LT_BASEMAP_N; j++) {
            lt_color bg = lt_heat_color(0, (tab[row][j] - lo) / (hi - lo));
            lt_rect c;
            c.x = r.x + hw + j * cw; c.y = r.y + hh + i * chh; c.w = cw - 1; c.h = chh - 1;
            lt_ui_draw_fill(ui, c, bg);
            lt_fmt_double(b, sizeof(b), tab[row][j], s->which == 0 ? 0 : 1);
            lt_ui_draw_text_in(ui, c, b, lt_color_contrast(bg), LT_UI_ALIGN_CENTER);
        }
    }
}

static void bm_apply(lt_app *app, bm_state *s)
{
    lt_pagesrc ps = lt_app_pages(app);
    char *log = NULL;
    int n;
    if (app->ini == NULL || ps.write == NULL) { lt_app_toast(app, 3, "No tune to write into"); return; }
    n = lt_basemap_apply(&s->map, app->ini, &ps, s->write_axes ? LT_TRUE : LT_FALSE, &log);
    if (n < 0) {
        lt_app_toast(app, 3, "Base map failed: %s", lt_err_get());
    } else {
        lt_app_edited(app);
        lt_app_toast(app, 1, "Base map applied (%d values written)", n);
    }
    if (log != NULL) { lt_strlcpy(s->log, log, sizeof(s->log)); lt_free(log); }
}

static void bm_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    bm_state *s = (bm_state *)d->data;
    int open = d->open, w1 = -1, w2[2], wc[2], w3[3], changed = 0;
    double v;
    if (s == NULL) {
        s = (bm_state *)lt_calloc(1, sizeof(*s));
        d->data = s;
        lt_engine_spec_default(&s->spec);
        s->cyl = s->spec.cylinder_count; s->disp = s->spec.displacement_cc;
        s->inj_cc = s->spec.injector_size_cc;
        s->idle = s->spec.idle_rpm; s->redline = s->spec.redline_rpm;
        s->write_axes = 1;
        bm_regen(s);
    }
    if (lt_ui_modal_begin(ui, "Base Map Wizard", CH * 128, CV * 38, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Describe the engine; a starting VE, ignition and AFR map is generated "
                       "and written into the current tune.", t->text2, LT_UI_ALIGN_LEFT);
        wc[0] = CH * 46; wc[1] = -1;
        lt_ui_layout_row(ui, 2, wc, 468);
        lt_ui_layout_column_begin(ui);
        w2[0] = CH * 18; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Cylinders");
        if (lt_ui_number(ui, "cyl", &s->cyl, 1, 12, 1, 0) & LT_UI_CHANGED) { changed = 1; }
        lt_ui_label(ui, "Displacement (cc)");
        if (lt_ui_number(ui, "disp", &s->disp, 50, 20000, 50, 0) & LT_UI_CHANGED) { changed = 1; }
        lt_ui_label(ui, "Injector (cc/min)");
        if (lt_ui_number(ui, "injcc", &s->inj_cc, 50, 3000, 10, 0) & LT_UI_CHANGED) { changed = 1; }
        lt_ui_label(ui, "Fuel");
        if (lt_ui_combo(ui, "fuel", &s->fuel, fuels, 5)) { changed = 1; }
        lt_ui_label(ui, "Aspiration");
        if (lt_ui_combo(ui, "asp", &s->asp, asps, 3)) { changed = 1; }
        lt_ui_label(ui, "Stroke");
        if (lt_ui_combo(ui, "stroke", &s->cycle, cycle_names, 2)) { changed = 1; }
        lt_ui_label(ui, "Injection");
        if (lt_ui_combo(ui, "inj", &s->inj, injs, 4)) { changed = 1; }
        lt_ui_label(ui, "Ignition");
        if (lt_ui_combo(ui, "ign", &s->ign, igns, 3)) { changed = 1; }
        lt_ui_label(ui, "Idle RPM");
        if (lt_ui_number(ui, "idle", &s->idle, 300, 3000, 50, 0) & LT_UI_CHANGED) { changed = 1; }
        lt_ui_label(ui, "Redline RPM");
        if (lt_ui_number(ui, "red", &s->redline, 2000, 12000, 100, 0) & LT_UI_CHANGED) { changed = 1; }
        lt_ui_label(ui, "Chamber");
        if (lt_ui_combo(ui, "chamber", &s->chamber, chambers, 3)) { s->spec.has_chamber = 1; changed = 1; }
        lt_ui_label(ui, "Boost target (kPa)");
        if (s->asp != 0) {
            v = s->spec.has_boost ? s->spec.boost_target_kpa : 150.0;
            if (lt_ui_number(ui, "boost", &v, 100, 400, 5, 0) & LT_UI_CHANGED) {
                s->spec.has_boost = LT_TRUE; s->spec.boost_target_kpa = v; changed = 1;
            }
        } else { lt_ui_label_ex(ui, "-", t->muted, LT_UI_ALIGN_LEFT); }
        lt_ui_label(ui, "Compression ratio");
        v = s->spec.has_cr ? s->spec.compression_ratio : 9.5;
        if (lt_ui_number(ui, "cr", &v, 6, 16, 0.1, 1) & LT_UI_CHANGED) {
            s->spec.has_cr = LT_TRUE; s->spec.compression_ratio = v; changed = 1;
        }
        lt_ui_label(ui, "Octane");
        v = s->spec.has_octane ? s->spec.octane : 91.0;
        if (lt_ui_number(ui, "oct", &v, 80, 110, 1, 0) & LT_UI_CHANGED) {
            s->spec.has_octane = LT_TRUE; s->spec.octane = v; changed = 1;
        }
        lt_ui_label(ui, "WOT AFR");
        v = s->spec.has_wot_afr ? s->spec.target_wot_afr : lt_engine_safe_wot_afr(&s->spec);
        if (lt_ui_number(ui, "wot", &v, 9, 16, 0.1, 1) & LT_UI_CHANGED) {
            s->spec.has_wot_afr = LT_TRUE; s->spec.target_wot_afr = v; changed = 1;
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        {
            char b[96];
            lt_snprintf(b, sizeof(b), "Calculated reqFuel: %.2f ms", s->map.req_fuel);
            lt_ui_label_ex(ui, b, t->accent, LT_UI_ALIGN_LEFT);
        }
        lt_ui_layout_column_end(ui);
        lt_ui_layout_column_begin(ui);
        w3[0] = -1; w3[1] = -1; w3[2] = -1;
        lt_ui_layout_row(ui, 3, w3, 0);
        lt_ui_radio(ui, "VE table", &s->which, 0);
        lt_ui_radio(ui, "Ignition", &s->which, 1);
        lt_ui_radio(ui, "AFR target", &s->which, 2);
        bm_preview(app, s, lt_ui_layout_width(ui), 0);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_checkbox(ui, "Overwrite the table axes with the generated bins", &s->write_axes);
        if (s->log[0] != '\0') { lt_ui_label_ex(ui, s->log, t->muted, LT_UI_ALIGN_LEFT); }
        lt_ui_layout_column_end(ui);
        if (changed) { bm_regen(s); }
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button_ex(ui, "Apply to tune", app->ini != NULL ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED)
            && app->ini != NULL) {
            bm_apply(app, s);
        }
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void bm_close(lt_app *app, lt_app_dialog *d)
{
    bm_state *s = (bm_state *)d->data;
    LT_UNUSED(app);
    if (s != NULL) {
        if (s->have_map) { lt_basemap_free(&s->map); }
        lt_free(s);
    }
    d->data = NULL;
}

/* ================================================================== */
/* Restore points                                                      */
typedef struct rp_state {
    lt_restore_info *v; size_t n;
    int sel, loaded, confirm;
} rp_state;

static void rp_reload(lt_app *app, rp_state *s)
{
    if (s->v != NULL) { lt_restore_info_free(s->v, s->n); }
    s->v = NULL; s->n = 0;
    if (app->has_project) { lt_restore_list(&app->project, &s->v, &s->n); }
    s->loaded = 1;
    if (s->sel >= (int)s->n) { s->sel = (int)s->n - 1; }
    if (s->sel < 0) { s->sel = 0; }
    s->confirm = 0;
}

static const char *rp_get(void *ud, int i)
{
    rp_state *s = (rp_state *)ud;
    static char buf[320];
    lt_snprintf(buf, sizeof(buf), "%-20s  %8lu bytes   %s", s->v[i].created,
                s->v[i].size_bytes, s->v[i].filename);
    return buf;
}

static void rp_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    rp_state *s = (rp_state *)d->data;
    int open = d->open, w1 = -1, w4[4], have;
    if (s == NULL) { s = (rp_state *)lt_calloc(1, sizeof(*s)); d->data = s; }
    if (!s->loaded) { rp_reload(app, s); }
    if (lt_ui_modal_begin(ui, "Restore Points", CH * 96, CV * 18, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (!app->has_project) {
            lt_ui_label_ex(ui, "Restore points belong to a project. Open or create a project first.",
                           t->warning, LT_UI_ALIGN_LEFT);
        } else {
            lt_ui_label_ex(ui, "A restore point is saved automatically before every burn.",
                           t->text2, LT_UI_ALIGN_LEFT);
            lt_ui_layout_row(ui, 1, &w1, CV * 9);
            if (s->n > 0) {
                int r = lt_ui_listbox_cb(ui, "rp", &s->sel, (int)s->n, rp_get, s);
                if (r & LT_UI_CHANGED) { s->confirm = 0; }
            } else {
                lt_ui_label_ex(ui, "No restore points yet.", t->muted, LT_UI_ALIGN_LEFT);
            }
        }
        have = app->has_project && s->n > 0 && s->sel >= 0 && s->sel < (int)s->n;
        w4[0] = -1; w4[1] = -1; w4[2] = -1; w4[3] = -1;
        lt_ui_layout_row(ui, 4, w4, 0);
        if (lt_ui_button_ex(ui, "Create now", app->has_project && app->tune != NULL ? 0 : LT_UI_BTN_DISABLED)
            && app->has_project && app->tune != NULL) {
            char *path = NULL;
            if (app->mem != NULL) { lt_tune_from_mem(app->tune, app->mem); }
            if (lt_restore_create(&app->project, &path) == 0) {
                lt_app_toast(app, 1, "Restore point created");
                lt_free(path);
            } else { lt_app_toast(app, 3, "Cannot create restore point: %s", lt_err_get()); }
            rp_reload(app, s);
        }
        if (lt_ui_button_ex(ui, "Restore", have ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED) && have) {
            char *path = NULL;
            if (lt_restore_path(&app->project, s->v[s->sel].filename, &path) == 0) {
                lt_app_load_tune(app, path);
                lt_free(path);
            } else { lt_app_toast(app, 3, "Cannot restore: %s", lt_err_get()); }
        }
        if (lt_ui_button_ex(ui, s->confirm ? "Really delete?" : "Delete",
                            have ? LT_UI_BTN_DANGER : LT_UI_BTN_DISABLED) && have) {
            if (!s->confirm) { s->confirm = 1; }
            else {
                if (lt_restore_delete(&app->project, s->v[s->sel].filename) < 0) {
                    lt_app_toast(app, 3, "Delete failed: %s", lt_err_get());
                }
                rp_reload(app, s);
            }
        }
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void rp_close(lt_app *app, lt_app_dialog *d)
{
    rp_state *s = (rp_state *)d->data;
    LT_UNUSED(app);
    if (s != NULL) {
        if (s->v != NULL) { lt_restore_info_free(s->v, s->n); }
        lt_free(s);
    }
    d->data = NULL;
}

/* ================================================================== */
/* Special tabs                                                        */
typedef struct och_state { char filter[64]; } och_state;

static void och_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    och_state *s = (och_state *)tab->data;
    int w1 = -1, w3[3], i;
    LT_UNUSED(r);
    if (s == NULL) { s = (och_state *)lt_calloc(1, sizeof(*s)); tab->data = s; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_heading(ui, "Output Channel Status");
    w3[0] = CH * 8; w3[1] = -1;
    lt_ui_layout_row(ui, 2, w3, 0);
    lt_ui_label(ui, "Filter");
    lt_ui_text(ui, "filter", s->filter, sizeof(s->filter), 0);
    if (app->ini == NULL || app->rt == NULL) { return; }
    if (app->conn == LT_APP_OFFLINE) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Not connected: values are zero.", t->warning, LT_UI_ALIGN_LEFT);
    }
    lt_ui_request_redraw(ui);
    lt_ui_layout_row(ui, 1, &w1, -1);
    if (lt_ui_scroll_begin(ui, "och")) {
        w3[0] = CH * 34; w3[1] = CH * 16; w3[2] = -1;
        for (i = 0; i < (int)app->ini->n_channels; i++) {
            const lt_ini_channel *c = app->ini->channels[i];
            char b[48];
            if (s->filter[0] != '\0' && !lt_dlg_contains_ci(c->name, s->filter)) { continue; }
            lt_ui_layout_row(ui, 3, w3, 0);
            lt_ui_label(ui, c->name);
            lt_fmt_double(b, sizeof(b), lt_ini_rt_value(app->rt, c->index), 3);
            lt_ui_label_ex(ui, b, t->accent, LT_UI_ALIGN_RIGHT);
            lt_ui_label_ex(ui, c->units != NULL ? c->units : "", t->muted, LT_UI_ALIGN_LEFT);
        }
        lt_ui_scroll_end(ui);
    }
}

#define CON_LINES 200
typedef struct con_state {
    char lines[CON_LINES][160];
    int n, last_n;
    char input[160];
} con_state;

static void con_add(con_state *s, const char *text)
{
    char tmp[160];
    const char *p = text;
    while (*p != '\0') {
        size_t len = 0;
        while (p[len] != '\0' && p[len] != '\n' && p[len] != '\r' && len < sizeof(tmp) - 1) { len++; }
        memcpy(tmp, p, len); tmp[len] = '\0';
        p += len;
        while (*p == '\n' || *p == '\r') { p++; }
        if (s->n >= CON_LINES) {
            memmove(s->lines[0], s->lines[1], (CON_LINES - 1) * sizeof(s->lines[0]));
            s->n = CON_LINES - 1;
        }
        lt_strlcpy(s->lines[s->n++], tmp, sizeof(s->lines[0]));
    }
}

static void console_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    con_state *s = (con_state *)tab->data;
    int w1 = -1, w2[2], i, fl;
    int online = (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) && app->proto != NULL;
    LT_UNUSED(r);
    if (s == NULL) {
        s = (con_state *)lt_calloc(1, sizeof(*s));
        tab->data = s;
        con_add(s, "ECU console (rusEFI / FOME / epicEFI). Type 'help' and press Enter.");
    }
    lt_ui_layout_row(ui, 1, &w1, -2);
    if (lt_ui_scroll_begin(ui, "con")) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        for (i = 0; i < s->n; i++) {
            lt_ui_label_ex(ui, s->lines[i], s->lines[i][0] == '>' ? t->accent : t->text, LT_UI_ALIGN_LEFT);
        }
        if (s->n != s->last_n) { lt_ui_scroll_set(ui, 0, 1 << 20); s->last_n = s->n; }
        lt_ui_scroll_end(ui);
    }
    w2[0] = -1; w2[1] = CH * 10;
    lt_ui_layout_row(ui, 2, w2, 0);
    fl = lt_ui_text(ui, "cmd", s->input, sizeof(s->input), 0);
    if ((lt_ui_button(ui, "Send") || (fl & LT_UI_SUBMIT)) && s->input[0] != '\0') {
        char *out = NULL;
        char echo[170];
        lt_snprintf(echo, sizeof(echo), "> %s", s->input);
        con_add(s, echo);
        if (!online) { con_add(s, "Not connected."); }
        else if (lt_proto_console(app->proto, s->input, 1500, &out) < 0) {
            lt_snprintf(echo, sizeof(echo), "error: %s", lt_err_get());
            con_add(s, echo);
        } else if (out != NULL) { con_add(s, out); }
        lt_free(out);
        s->input[0] = '\0';
        lt_ui_set_focus(ui, lt_ui_last_id(ui));
    }
}

static void tab_free_data(lt_app *app, lt_app_tab *tab)
{
    LT_UNUSED(app);
    lt_free(tab->data);
    tab->data = NULL;
}

/* "special" tab: dispatches on tab->target */
static void special_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    if (strcmp(tab->target, "och-status") == 0) { och_draw(app, tab, r); }
    else if (strcmp(tab->target, "console") == 0) { console_draw(app, tab, r); }
    else { lt_app_placeholder_draw(app, tab, r); }
}

void lt_dlg_register_misc(lt_app *app)
{
    lt_app_dialog_kind dk;
    lt_app_tab_kind tk;
    dk.kind = "basemap"; dk.draw = bm_draw; dk.close = bm_close;
    lt_app_register_dialog_kind(app, &dk);
    dk.kind = "restore"; dk.draw = rp_draw; dk.close = rp_close;
    lt_app_register_dialog_kind(app, &dk);
    tk.kind = "special"; tk.draw = special_draw; tk.close = tab_free_data;
    lt_app_register_tab_kind(app, &tk);
}
