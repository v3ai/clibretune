/* dash_tab.c - Dashboard tab: dash view, template/INI/.dash sources, context menu,
 * properties window and a minimal designer (drag/resize/add/remove/save .dash).
 * Painters live in gauges.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "app.h"
#include "../plat/plat.h"
#include "dash_gauges.h"
#include "../dash/dash_priv.h"

#define FD_OPEN_DASH (LT_APP_FD_USER0 + 40)
#define FD_SAVE_DASH (LT_APP_FD_USER0 + 41)
#define N_PAINTERS 20

enum { SRC_FRONT = 0, SRC_BASIC, SRC_TUNING, SRC_TELEM, SRC_FILE, N_SRC };
enum { M_NONE = 0, M_MOVE, M_SIZE };

typedef struct dash_state {
    lt_dash_file *file;
    lt_dash_view *view;
    lt_expr_cache cache;
    const lt_expr_env *env;
    const lt_ini *ini;
    int src;
    char path[512];
    int designer, sel, mode, ctx_idx, ctx_x, ctx_y;
    int prev_down, prev_rdown, drag_x, drag_y;
    double orig[4];
    lt_rect cluster;
    int dirty, rebuild;
    int props_open, props_idx;
    lt_rect props_rect;
    char filter[64];
    int chan_sel, hooked;
} dash_state;

static const char *const src_names[N_SRC] = {
    "ECU front page", "Basic", "Tuning", "Telemetry", "Custom file"
};

/* ---- component helpers ---- */
static void get_rel(const lt_dash_comp *c, double v[4])
{
    if (c->gauge != NULL) {
        v[0] = c->gauge->relative_x; v[1] = c->gauge->relative_y;
        v[2] = c->gauge->relative_width; v[3] = c->gauge->relative_height;
    } else {
        v[0] = c->indicator->relative_x; v[1] = c->indicator->relative_y;
        v[2] = c->indicator->relative_width; v[3] = c->indicator->relative_height;
    }
}
static void set_rel(lt_dash_comp *c, const double v[4])
{
    if (c->gauge != NULL) {
        c->gauge->relative_x = v[0]; c->gauge->relative_y = v[1];
        c->gauge->relative_width = v[2]; c->gauge->relative_height = v[3];
    } else {
        c->indicator->relative_x = v[0]; c->indicator->relative_y = v[1];
        c->indicator->relative_width = v[2]; c->indicator->relative_height = v[3];
    }
}

static double clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
static double snap(double x) { return (double)(int)(x * 200.0 + 0.5) / 200.0; }

/* cluster rectangle inside area (same letterbox rule as lt_dash_view_render) */
static lt_rect cluster_rect(const lt_dash_file *f, lt_rect area)
{
    const lt_dash_cluster *cl = &f->cluster;
    lt_rect cr = area;
    if (cl->force_aspect && cl->force_aspect_width > 0.0 && cl->force_aspect_height > 0.0 &&
        area.w > 0 && area.h > 0) {
        double want = cl->force_aspect_width / cl->force_aspect_height, have = (double)area.w / area.h;
        if (have > want) { cr.h = area.h; cr.w = (int)((double)area.h * want + 0.5); }
        else { cr.w = area.w; cr.h = (int)((double)area.w / want + 0.5); }
        cr.x = area.x + (area.w - cr.w) / 2; cr.y = area.y + (area.h - cr.h) / 2;
    }
    return cr;
}

/* ---- loading ---- */
static void state_set_file(dash_state *st, lt_dash_file *f)
{
    if (st->view != NULL) { lt_dash_view_free(st->view); st->view = NULL; }
    if (st->file != NULL) { lt_dash_free(st->file); }
    st->file = f;
    st->view = lt_dash_view_new(f);
    st->sel = -1; st->ctx_idx = -1; st->mode = M_NONE; st->props_open = 0; st->dirty = 0;
}

static void load_source(lt_app *app, dash_state *st, int src)
{
    lt_dash_file *f = NULL;
    if (app->ini == NULL) { return; }
    if (src == SRC_FRONT) {
        f = lt_dash_from_frontpage(app->ini, &app->varenv.env);
        if (f != NULL && f->cluster.n_comps == 0) { lt_dash_free(f); f = NULL; src = SRC_BASIC; }
    }
    if (f == NULL) {
        if (src == SRC_TUNING) { f = lt_dash_template_tuning(); }
        else if (src == SRC_TELEM) { f = lt_dash_template_telemetry(); }
        else { f = lt_dash_template_basic(); src = SRC_BASIC; }
        lt_dash_resolve_channels(f, app->ini);
    }
    st->src = src;
    st->path[0] = '\0';
    state_set_file(st, f);
}

static int load_file(lt_app *app, dash_state *st, const char *path)
{
    lt_dash_file *f = lt_dash_load(path);
    if (f == NULL) { lt_app_toast(app, 3, "Cannot load dashboard: %s", lt_err_get()); return -1; }
    lt_dash_resolve_channels(f, app->ini);
    st->src = SRC_FILE;
    lt_snprintf(st->path, sizeof st->path, "%s", path);
    state_set_file(st, f);
    return 0;
}

static char *dashboards_dir(lt_app *app)
{
    if (app->has_project && app->project.path != NULL) { return lt_path_join(app->project.path, "dashboards"); }
    return NULL;
}

static void load_default(lt_app *app, dash_state *st)
{
    char *dir = dashboards_dir(app);
    if (dir != NULL && app->project.cfg.dashboard != NULL && app->project.cfg.dashboard[0]) {
        char *p = lt_path_join(dir, app->project.cfg.dashboard);
        int ok = lt_file_exists(p) && load_file(app, st, p) == 0;
        lt_free(p);
        if (ok) { lt_free(dir); return; }
    }
    lt_free(dir);
    load_source(app, st, (app->ini != NULL && app->ini->frontpage != NULL &&
                          app->ini->frontpage->n_gauges >= 4) ? SRC_FRONT : SRC_BASIC);
}

/* ---- add / remove ---- */
static const char *first_channel(lt_app *app, int idx)
{
    if (app->ini == NULL || app->ini->n_channels == 0) { return ""; }
    return app->ini->channels[(size_t)idx % app->ini->n_channels]->name;
}

static void add_gauge(lt_app *app, dash_state *st, double x, double y, lt_gauge_painter pt)
{
    lt_dash_gauge *g = lt_dash_add_gauge(st->file);
    size_t n = st->file->cluster.n_comps;
    const char *ch = first_channel(app, 0);
    g->painter = pt;
    dash_set_str(&g->output_channel, ch);
    dash_set_str(&g->title, ch);
    g->back_color = LT_RGB(28, 32, 40); g->font_color = LT_RGB(255, 255, 255);
    g->trim_color = LT_RGB(148, 163, 184); g->needle_color = LT_RGB(74, 158, 248);
    g->warn_color = LT_RGB(234, 179, 8); g->critical_color = LT_RGB(239, 68, 68);
    g->min = 0.0; g->max = 100.0;
    g->relative_width = 0.25; g->relative_height = 0.3;
    g->relative_x = clampd(snap(x - 0.125), 0.0, 0.75);
    g->relative_y = clampd(snap(y - 0.15), 0.0, 0.7);
    st->sel = (int)n - 1; st->dirty = 1; st->rebuild = 1;
}

static void add_indicator(lt_app *app, dash_state *st, double x, double y)
{
    lt_dash_indicator *in = lt_dash_add_indicator(st->file);
    size_t n = st->file->cluster.n_comps;
    dash_set_str(&in->output_channel, first_channel(app, 0));
    in->relative_width = 0.12; in->relative_height = 0.06;
    in->relative_x = clampd(snap(x - 0.06), 0.0, 0.88);
    in->relative_y = clampd(snap(y - 0.03), 0.0, 0.94);
    in->on_background_color = LT_RGB(220, 40, 40); in->off_background_color = LT_RGB(40, 60, 40);
    in->on_text_color = LT_RGB(255, 255, 255); in->off_text_color = LT_RGB(180, 200, 180);
    st->sel = (int)n - 1; st->dirty = 1; st->rebuild = 1;
}

static void remove_comp(dash_state *st, int idx)
{
    if (idx < 0 || (size_t)idx >= st->file->cluster.n_comps) { return; }
    lt_dash_remove(st->file, (size_t)idx);
    st->sel = -1; st->ctx_idx = -1; st->dirty = 1; st->rebuild = 1; st->props_open = 0;
}

static void duplicate_comp(dash_state *st, int idx)
{
    lt_dash_comp *c;
    double v[4];
    size_t n;
    if (idx < 0 || (size_t)idx >= st->file->cluster.n_comps) { return; }
    c = &st->file->cluster.comps[idx];
    get_rel(c, v);
    if (c->gauge != NULL) {
        lt_dash_gauge *g = lt_dash_add_gauge(st->file);
        lt_dash_gauge *s = st->file->cluster.comps[idx].gauge;
        lt_dash_gauge keep = *g;
        /* shallow copy then deep-copy owned strings that we know about */
        *g = *s;
        g->id = NULL; g->gauge_style = NULL; g->output_channel = NULL; g->title = NULL; g->units = NULL;
        g->min_vp = NULL; g->max_vp = NULL; g->low_warning_vp = g->high_warning_vp = NULL;
        g->low_critical_vp = g->high_critical_vp = NULL; g->font_family = NULL;
        g->background_image_file_name = NULL; g->needle_image_file_name = NULL;
        g->short_click_action = NULL; g->long_click_action = NULL; g->enabled_condition = NULL;
        memset(&g->extras, 0, sizeof g->extras);
        dash_set_str(&g->output_channel, s->output_channel);
        dash_set_str(&g->title, s->title);
        dash_set_str(&g->units, s->units);
        dash_set_str(&g->min_vp, s->min_vp); dash_set_str(&g->max_vp, s->max_vp);
        dash_set_str(&g->enabled_condition, s->enabled_condition);
        LT_UNUSED(keep);
    } else {
        lt_dash_indicator *in = lt_dash_add_indicator(st->file);
        lt_dash_indicator *s = st->file->cluster.comps[idx].indicator;
        *in = *s;
        in->id = NULL; in->output_channel = NULL; in->on_text = NULL; in->off_text = NULL;
        in->on_image_file_name = NULL; in->off_image_file_name = NULL; in->font_family = NULL;
        in->short_click_action = NULL; in->long_click_action = NULL;
        in->ecu_configuration_name = NULL; in->enabled_condition = NULL;
        memset(&in->extras, 0, sizeof in->extras);
        dash_set_str(&in->output_channel, s->output_channel);
        dash_set_str(&in->on_text, s->on_text); dash_set_str(&in->off_text, s->off_text);
        dash_set_str(&in->enabled_condition, s->enabled_condition);
    }
    n = st->file->cluster.n_comps;
    v[0] = clampd(v[0] + 0.02, 0.0, 0.95); v[1] = clampd(v[1] + 0.02, 0.0, 0.95);
    set_rel(&st->file->cluster.comps[n - 1], v);
    st->sel = (int)n - 1; st->dirty = 1; st->rebuild = 1;
}

/* ---- drawing callback ---- */
static void render_cb(lt_gfx *g, lt_rect r, void *user)
{
    dash_state *st = (dash_state *)user;
    int i;
    lt_gfx_fill_rect(g, r, LT_RGB(0, 0, 0));
    if (st->view == NULL) { return; }
    lt_dash_view_render(st->view, g, r, st->env);
    if (!st->designer || lt_gfx_cell_mode(g)) { return; }
    for (i = 1; i < 20; i++) {
        lt_gfx_vline(g, st->cluster.x + st->cluster.w * i / 20, st->cluster.y, st->cluster.y + st->cluster.h - 1,
                     LT_RGBA(255, 255, 255, i % 5 == 0 ? 28 : 12));
        lt_gfx_hline(g, st->cluster.x, st->cluster.x + st->cluster.w - 1, st->cluster.y + st->cluster.h * i / 20,
                     LT_RGBA(255, 255, 255, i % 5 == 0 ? 28 : 12));
    }
    if (st->sel >= 0 && (size_t)st->sel < lt_dash_view_count(st->view)) {
        lt_rect c = lt_dash_view_component_rect(st->view, (size_t)st->sel);
        lt_color pc = LT_RGB(66, 153, 255);
        lt_gfx_rect(g, c, pc);
        lt_gfx_rect(g, lt_rect_inset(c, 1, 1), pc);
        lt_gfx_fill_rect(g, lt_rect_make(c.x + c.w - 8, c.y + c.h - 8, 8, 8), pc);
        lt_gfx_fill_rect(g, lt_rect_make(c.x - 1, c.y - 1, 5, 5), pc);
        lt_gfx_fill_rect(g, lt_rect_make(c.x + c.w - 4, c.y - 1, 5, 5), pc);
        lt_gfx_fill_rect(g, lt_rect_make(c.x - 1, c.y + c.h - 4, 5, 5), pc);
    }
}

/* ---- properties window ---- */
static void props_window(lt_app *app, dash_state *st)
{
    lt_ui *ui = app->ui;
    int w1[2], one = -1;
    lt_dash_comp *c;
    char title[96];
    if (!st->props_open) { return; }
    if (st->props_idx < 0 || (size_t)st->props_idx >= st->file->cluster.n_comps) { st->props_open = 0; return; }
    c = &st->file->cluster.comps[st->props_idx];
    lt_snprintf(title, sizeof title, "%s Properties", c->gauge ? "Gauge" : "Indicator");
    if (st->props_rect.w == 0) { st->props_rect = lt_rect_make(80, 80, 420, 520); }
    w1[0] = 120; w1[1] = -1;
    if (!lt_ui_window_begin(ui, title, &st->props_rect, &st->props_open, LT_UI_WIN_RESIZE)) { return; }
    if (c->gauge != NULL) {
        lt_dash_gauge *g = c->gauge;
        char buf[160];
        double d;
        int sel = (int)g->painter, ch = 0, i, nm = 0;
        const char *names[N_PAINTERS];
        const char **match = NULL;
        lt_ui_layout_row(ui, 2, w1, 0);
        lt_ui_label(ui, "Title");
        lt_snprintf(buf, sizeof buf, "%s", g->title ? g->title : "");
        if (lt_ui_text(ui, "dp_title", buf, sizeof buf, 0) & LT_UI_CHANGED) { dash_set_str(&g->title, buf); st->dirty = 1; }
        lt_ui_label(ui, "Units");
        lt_snprintf(buf, sizeof buf, "%s", g->units ? g->units : "");
        if (lt_ui_text(ui, "dp_units", buf, sizeof buf, 0) & LT_UI_CHANGED) { dash_set_str(&g->units, buf); st->dirty = 1; }
        for (i = 0; i < N_PAINTERS; i++) { names[i] = lt_painter_to_ts((lt_gauge_painter)i); }
        lt_ui_label(ui, "Type");
        if (lt_ui_combo(ui, "dp_type", &sel, names, N_PAINTERS)) { g->painter = (lt_gauge_painter)sel; st->dirty = 1; }
        lt_ui_label(ui, "Channel");
        lt_ui_label_ex(ui, g->output_channel ? g->output_channel : "(none)", lt_ui_theme(ui)->accent, LT_UI_ALIGN_LEFT);
        lt_ui_label(ui, "Filter");
        lt_ui_text(ui, "dp_filter", st->filter, sizeof st->filter, 0);
        if (app->ini != NULL && app->ini->n_channels > 0) {
            size_t k;
            match = (const char **)lt_malloc(sizeof(char *) * (app->ini->n_channels + 1));
            for (k = 0; k < app->ini->n_channels; k++) {
                const char *nme = app->ini->channels[k]->name;
                if (st->filter[0] == '\0' || strstr(nme, st->filter) != NULL) {
                    if (g->output_channel != NULL && strcmp(nme, g->output_channel) == 0) { ch = nm; }
                    match[nm++] = nme;
                }
            }
            st->chan_sel = ch;
            lt_ui_layout_row(ui, 1, &one, 130);
            if (nm > 0 && (lt_ui_listbox(ui, "dp_chans", &st->chan_sel, match, nm) & LT_UI_CHANGED) &&
                st->chan_sel >= 0 && st->chan_sel < nm) {
                dash_set_str(&g->output_channel, match[st->chan_sel]);
                st->dirty = 1; st->rebuild = 1;
            } else if (nm == 0) { lt_ui_label(ui, "No matching channels"); }
            lt_free((void *)match);
        }
        lt_ui_layout_row(ui, 2, w1, 0);
#define NUMF(label, id, field, lo, hi, step, dg) \
        lt_ui_label(ui, label); d = (field); \
        if (lt_ui_number(ui, id, &d, lo, hi, step, dg) & LT_UI_CHANGED) { (field) = d; st->dirty = 1; }
        NUMF("Min", "dp_min", g->min, -1e9, 1e9, 1.0, 2)
        NUMF("Max", "dp_max", g->max, -1e9, 1e9, 1.0, 2)
        d = (double)g->value_digits;
        lt_ui_label(ui, "Value digits");
        if (lt_ui_number(ui, "dp_dig", &d, 0, 6, 1.0, 0) & LT_UI_CHANGED) { g->value_digits = (int)d; st->dirty = 1; }
        lt_ui_label(ui, "Colors");
        lt_ui_color_edit(ui, "dp_cfont", &g->font_color);
        lt_ui_label(ui, "Needle / bar");
        lt_ui_color_edit(ui, "dp_cneedle", &g->needle_color);
        lt_ui_label(ui, "Back");
        lt_ui_color_edit(ui, "dp_cback", &g->back_color);
#undef NUMF
        {
            int v;
            lt_ui_label(ui, "Peak hold");
            v = g->peak_hold ? 1 : 0;
            if (lt_ui_checkbox(ui, "show", &v)) { g->peak_hold = v ? LT_TRUE : LT_FALSE; st->dirty = 1; }
            lt_ui_label(ui, "High warning");
            v = g->has_high_warning ? 1 : 0;
            if (lt_ui_checkbox(ui, "enabled", &v)) { g->has_high_warning = v ? LT_TRUE : LT_FALSE; st->dirty = 1; }
            if (g->has_high_warning) {
                d = g->high_warning;
                lt_ui_label(ui, "  value");
                if (lt_ui_number(ui, "dp_hw", &d, -1e9, 1e9, 1.0, 2) & LT_UI_CHANGED) { g->high_warning = d; st->dirty = 1; }
            }
            lt_ui_label(ui, "High critical");
            v = g->has_high_critical ? 1 : 0;
            if (lt_ui_checkbox(ui, "enabled", &v)) { g->has_high_critical = v ? LT_TRUE : LT_FALSE; st->dirty = 1; }
            if (g->has_high_critical) {
                d = g->high_critical;
                lt_ui_label(ui, "  value");
                if (lt_ui_number(ui, "dp_hc", &d, -1e9, 1e9, 1.0, 2) & LT_UI_CHANGED) { g->high_critical = d; st->dirty = 1; }
            }
        }
    } else {
        lt_dash_indicator *in = c->indicator;
        char buf[96];
        int sel = (int)in->painter, i;
        static const char *const kinds[3] = { "Rectangle", "Bulb", "LED" };
        lt_ui_layout_row(ui, 2, w1, 0);
        lt_ui_label(ui, "On text");
        lt_snprintf(buf, sizeof buf, "%s", in->on_text ? in->on_text : "");
        if (lt_ui_text(ui, "di_on", buf, sizeof buf, 0) & LT_UI_CHANGED) { dash_set_str(&in->on_text, buf); st->dirty = 1; }
        lt_ui_label(ui, "Off text");
        lt_snprintf(buf, sizeof buf, "%s", in->off_text ? in->off_text : "");
        if (lt_ui_text(ui, "di_off", buf, sizeof buf, 0) & LT_UI_CHANGED) { dash_set_str(&in->off_text, buf); st->dirty = 1; }
        lt_ui_label(ui, "Style");
        if (lt_ui_combo(ui, "di_style", &sel, kinds, 3)) { in->painter = (lt_ind_painter)sel; st->dirty = 1; }
        lt_ui_label(ui, "Channel");
        lt_snprintf(buf, sizeof buf, "%s", in->output_channel ? in->output_channel : "");
        if (lt_ui_text(ui, "di_ch", buf, sizeof buf, 0) & LT_UI_CHANGED) {
            dash_set_str(&in->output_channel, buf); st->dirty = 1; st->rebuild = 1;
        }
        lt_ui_label(ui, "On color");
        lt_ui_color_edit(ui, "di_con", &in->on_background_color);
        lt_ui_label(ui, "Off color");
        lt_ui_color_edit(ui, "di_coff", &in->off_background_color);
        LT_UNUSED(i);
    }
    lt_ui_window_end(ui);
}

/* ---- main draw ---- */
static int hit_handle(const lt_rect *c, int x, int y)
{
    return x >= c->x + c->w - 14 && y >= c->y + c->h - 14;
}

static void toolbar(lt_app *app, dash_state *st)
{
    lt_ui *ui = app->ui;
    int w[9], n = 0, want_as = 0;
    int src = st->src;
    int d = st->designer;
    w[n++] = 190; w[n++] = 120; w[n++] = 120;
    if (st->designer) { w[n++] = 110; w[n++] = 120; w[n++] = 80; w[n++] = 70; w[n++] = 70; w[n++] = 70; }
    lt_ui_layout_row(ui, n, w, 0);
    if (lt_ui_combo(ui, "dash_src", &src, src_names, N_SRC) && src != st->src) {
        if (src == SRC_FILE) {
            char *dir = dashboards_dir(app);
            lt_app_file_dialog(app, FD_OPEN_DASH, LT_UI_FILE_OPEN, dir, "", "Dashboards|*.dash|All files|*");
            lt_free(dir);
        } else { load_source(app, st, src); }
    }
    if (lt_ui_button(ui, "Open .dash...")) {
        char *dir = dashboards_dir(app);
        lt_app_file_dialog(app, FD_OPEN_DASH, LT_UI_FILE_OPEN, dir, "", "Dashboards|*.dash|All files|*");
        lt_free(dir);
    }
    if (lt_ui_toggle(ui, "Designer", &d)) { st->designer = d; if (!d) { st->sel = -1; st->mode = M_NONE; } }
    if (st->designer) {
        if (lt_ui_button(ui, "Add gauge")) { add_gauge(app, st, 0.5, 0.5, LT_GP_ANALOG); }
        if (lt_ui_button(ui, "Add indicator")) { add_indicator(app, st, 0.5, 0.5); }
        if (lt_ui_button(ui, "Delete")) { remove_comp(st, st->sel); }
        if (lt_ui_button(ui, "Save")) {
            if (st->path[0]) {
                if (lt_dash_save(st->file, st->path) == 0) { st->dirty = 0; lt_app_toast(app, 1, "Saved %s", lt_path_base(st->path)); }
                else { lt_app_toast(app, 3, "Save failed: %s", lt_err_get()); }
            } else { want_as = 1; }
        }
        if (lt_ui_button(ui, "Save as...") || want_as) {
            char *dir = dashboards_dir(app);
            lt_app_file_dialog(app, FD_SAVE_DASH, LT_UI_FILE_SAVE, dir, "dashboard.dash", "Dashboards|*.dash|All files|*");
            lt_free(dir);
        }
        lt_ui_label_ex(ui, st->dirty ? "modified" : "", lt_ui_theme(ui)->warning, LT_UI_ALIGN_LEFT);
    }
}

static void dash_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    lt_ui *ui = app->ui;
    dash_state *st = (dash_state *)tab->data;
    const char *fp;
    lt_rect area;
    lt_point m;
    int down, rdown, in_area, popup, block, i;

    if (st == NULL) {
        st = (dash_state *)lt_calloc(1, sizeof *st);
        lt_expr_cache_init(&st->cache);
        st->sel = -1; st->ctx_idx = -1;
        tab->data = st;
    }
    if (app->ini == NULL) {
        int one = -1;
        lt_ui_layout_row(ui, 1, &one, 0);
        lt_ui_label_ex(ui, "No definition loaded.", lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
        return;
    }
    if (st->file == NULL || st->ini != app->ini) {
        st->ini = app->ini;
        load_default(app, st);
        if (!st->hooked) {   /* screenshot harness hooks */
            char *e = lt_plat_getenv("LT_DASH_DESIGNER");
            if (e != NULL) { st->designer = 1; st->sel = 0; lt_free(e); }
            e = lt_plat_getenv("LT_DASH_SRC");
            if (e != NULL) { load_source(app, st, atoi(e)); lt_free(e); }
            e = lt_plat_getenv("LT_DASH_PROPS");
            if (e != NULL) { st->props_open = 1; st->props_idx = 1; lt_free(e); }
            st->hooked = 1;
        }
    }
    fp = lt_app_file_result(app, FD_OPEN_DASH);
    if (fp != NULL) { load_file(app, st, fp); }
    fp = lt_app_file_result(app, FD_SAVE_DASH);
    if (fp != NULL) {
        char path[512];
        size_t n;
        lt_snprintf(path, sizeof path, "%s", fp);
        n = strlen(path);
        if (n < 5 || strcmp(path + n - 5, ".dash") != 0) { lt_snprintf(path + n, sizeof path - n, ".dash"); }
        if (lt_dash_save(st->file, path) == 0) {
            lt_snprintf(st->path, sizeof st->path, "%s", path);
            st->src = SRC_FILE; st->dirty = 0; lt_app_toast(app, 1, "Saved %s", lt_path_base(path));
        } else { lt_app_toast(app, 3, "Save failed: %s", lt_err_get()); }
    }
    if (st->rebuild) {
        lt_dash_view_free(st->view);
        st->view = lt_dash_view_new(st->file);
        st->rebuild = 0;
    }

    toolbar(app, st);
    area = lt_rect_make(r.x, lt_ui_layout_body(ui).y, r.w, 0);
    {
        lt_rect lr = lt_ui_last_rect(ui);
        area.y = lr.y + lr.h + 4;
        area.h = r.y + r.h - area.y;
    }
    if (area.h < 20) { return; }
    st->env = &app->varenv.env;
    lt_gauges_install();
    lt_gauges_set_env(&st->cache, st->env);
    lt_dash_view_update(st->view, lt_ui_now(ui), st->env);
    st->cluster = cluster_rect(st->file, area);
    lt_ui_draw_custom(ui, area, render_cb, st);

    /* input */
    m = lt_ui_mouse(ui);
    down = lt_ui_mouse_down(ui, LT_MB_LEFT);
    rdown = lt_ui_mouse_down(ui, LT_MB_RIGHT);
    popup = lt_ui_popup_open(ui);
    block = app->n_dialogs > 0 || app->fd.open ||
            (st->props_open && lt_rect_contains(st->props_rect, m.x, m.y));
    in_area = lt_rect_contains(area, m.x, m.y) && !popup && !block;
    if (rdown && !st->prev_rdown && in_area) {
        st->ctx_idx = lt_dash_view_hit(st->view, m.x, m.y);
        st->ctx_x = m.x; st->ctx_y = m.y;
        if (st->designer && st->ctx_idx >= 0) { st->sel = st->ctx_idx; }
    }
    if (st->designer) {
        if (down && !st->prev_down && in_area) {
            int h = lt_dash_view_hit(st->view, m.x, m.y);
            st->sel = h; st->mode = M_NONE;
            if (h >= 0) {
                lt_rect c = lt_dash_view_component_rect(st->view, (size_t)h);
                get_rel(&st->file->cluster.comps[h], st->orig);
                st->drag_x = m.x; st->drag_y = m.y;
                st->mode = hit_handle(&c, m.x, m.y) ? M_SIZE : M_MOVE;
            }
        } else if (down && st->mode != M_NONE && st->sel >= 0 && (size_t)st->sel < st->file->cluster.n_comps) {
            double v[4], dx, dy;
            v[0] = st->orig[0]; v[1] = st->orig[1]; v[2] = st->orig[2]; v[3] = st->orig[3];
            dx = st->cluster.w > 0 ? (double)(m.x - st->drag_x) / st->cluster.w : 0.0;
            dy = st->cluster.h > 0 ? (double)(m.y - st->drag_y) / st->cluster.h : 0.0;
            if (st->mode == M_MOVE) {
                v[0] = clampd(snap(v[0] + dx), 0.0, 1.0 - v[2]); v[1] = clampd(snap(v[1] + dy), 0.0, 1.0 - v[3]);
            } else {
                v[2] = clampd(snap(v[2] + dx), 0.03, 1.0 - v[0]); v[3] = clampd(snap(v[3] + dy), 0.03, 1.0 - v[1]);
            }
            set_rel(&st->file->cluster.comps[st->sel], v);
            st->dirty = 1;
        }
        if (!down) { st->mode = M_NONE; }
        if (!lt_ui_text_focused(ui) && st->sel >= 0) {
            if (lt_ui_key_pressed(ui, LT_KEY_DELETE, 0) || lt_ui_key_pressed(ui, LT_KEY_BACKSPACE, 0)) { remove_comp(st, st->sel); }
        }
        if (!lt_ui_text_focused(ui) && lt_ui_key_pressed(ui, LT_KEY_ESCAPE, 0)) { st->designer = 0; st->sel = -1; }
    }
    st->prev_down = down; st->prev_rdown = rdown;

    /* context menu */
    if (lt_ui_context_begin(ui, "dash_ctx", area)) {
        int ci = st->ctx_idx;
        if (ci >= 0 && (size_t)ci < st->file->cluster.n_comps) {
            lt_dash_comp *c = &st->file->cluster.comps[ci];
            if (lt_ui_menu_item(ui, "Properties...", NULL, 0)) { st->props_open = 1; st->props_idx = ci; }
            if (c->gauge != NULL) {
                if (lt_ui_submenu_begin(ui, "Gauge type", 0)) {
                    for (i = 0; i < N_PAINTERS; i++) {
                        if (lt_ui_menu_item(ui, lt_painter_to_ts((lt_gauge_painter)i), NULL,
                                            (int)c->gauge->painter == i ? LT_UI_MENU_CHECKED : 0)) {
                            c->gauge->painter = (lt_gauge_painter)i; st->dirty = 1;
                        }
                    }
                    lt_ui_submenu_end(ui);
                }
                if (lt_ui_menu_item(ui, "Reset peak", NULL, 0)) {
                    lt_dash_gstate *gs = (lt_dash_gstate *)lt_dash_view_state(st->view, (size_t)ci);
                    if (gs != NULL) { gs->peak.started = LT_FALSE; gs->peak.peak = gs->lim.min; }
                }
            }
            if (st->designer) {
                if (lt_ui_menu_item(ui, "Duplicate", NULL, 0)) { duplicate_comp(st, ci); }
                if (lt_ui_menu_item(ui, "Remove", NULL, 0)) { remove_comp(st, ci); }
            }
            lt_ui_menu_separator(ui);
        }
        if (lt_ui_menu_item(ui, "Designer mode", NULL, st->designer ? LT_UI_MENU_CHECKED : 0)) {
            st->designer = !st->designer; if (!st->designer) { st->sel = -1; }
        }
        if (st->designer) {
            double fx = st->cluster.w > 0 ? (double)(st->ctx_x - st->cluster.x) / st->cluster.w : 0.5;
            double fy = st->cluster.h > 0 ? (double)(st->ctx_y - st->cluster.y) / st->cluster.h : 0.5;
            if (lt_ui_submenu_begin(ui, "Add gauge here", 0)) {
                for (i = 0; i < N_PAINTERS; i++) {
                    if (lt_ui_menu_item(ui, lt_painter_to_ts((lt_gauge_painter)i), NULL, 0)) {
                        add_gauge(app, st, fx, fy, (lt_gauge_painter)i);
                    }
                }
                lt_ui_submenu_end(ui);
            }
            if (lt_ui_menu_item(ui, "Add indicator here", NULL, 0)) { add_indicator(app, st, fx, fy); }
        }
        lt_ui_context_end(ui);
    }
    props_window(app, st);
}

static void dash_close(lt_app *app, lt_app_tab *tab)
{
    dash_state *st = (dash_state *)tab->data;
    LT_UNUSED(app);
    if (st == NULL) { return; }
    if (st->view != NULL) { lt_dash_view_free(st->view); }
    if (st->file != NULL) { lt_dash_free(st->file); }
    lt_expr_cache_free(&st->cache);
    lt_free(st);
    tab->data = NULL;
}

void lt_app_register_dash(lt_app *app)
{
    lt_app_tab_kind k;
    lt_gauges_install();
    k.kind = "dashboard"; k.draw = dash_draw; k.close = dash_close;
    lt_app_register_tab_kind(app, &k);
}

int lt_app_dashboard_tab_open(lt_app *app)
{
    return lt_app_tab_open(app, "dashboard", "dashboard", "Dashboard", 0) != NULL ? 0 : -1;
}
