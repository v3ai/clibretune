/* dash_tab.c - Dashboard tab: dash view, dashboard library selection, designer (drag/resize,
 * multi-select, undo/redo, copy/paste, alignment), context menu, properties window.
 * Panels (selector, validation, layers, palette, assets) live in dash_panels.c, painters in
 * gauges.c, settings / full screen in feat_dashboard.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "dash_tab.h"
#include "../plat/plat.h"
#include "view_kit.h"
#include "../dash/dash_priv.h"
#include "../datalog/dl_view.h"

#define FD_OPEN_DASH (LT_APP_FD_USER0 + 40)
#define FD_SAVE_DASH (LT_APP_FD_USER0 + 41)

static const char *const builtin_names[4] = { "ECU front page", "Basic", "Tuning", "Telemetry" };
static const char *const builtin_keys[4] = { "@front", "@basic", "@tuning", "@telemetry" };

/* ---- small helpers ---- */
static double clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

static int snap_cells(lt_app *app)
{
    dash_cfg *c = lt_dashx_cfg(app);
    if (c->free_move) { return 0; }
    return c->snap ? 40 : 200;
}

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

dash_state *lt_dtab_state(lt_app *app)
{
    lt_app_tab *t = lt_app_tab_find(app, "dashboard", "dashboard");
    return t != NULL ? (dash_state *)t->data : NULL;
}

/* ---- selection ---- */
static void msel_fit(dash_state *st)
{
    size_t n = st->file != NULL ? st->file->cluster.n_comps : 0;
    if (st->n_msel != n) {
        lt_free(st->msel);
        st->msel = (unsigned char *)lt_calloc(n + 1, 1);
        st->n_msel = n;
    }
}

void lt_dtab_clear_sel(dash_state *st)
{
    msel_fit(st);
    if (st->msel != NULL) { memset(st->msel, 0, st->n_msel); }
    st->sel = -1;
}

int lt_dtab_is_sel(const dash_state *st, int idx)
{
    if (idx < 0) { return 0; }
    return (size_t)idx < st->n_msel && st->msel[idx] ? 1 : 0;
}

void lt_dtab_select(dash_state *st, int idx, int add)
{
    msel_fit(st);
    if (idx < 0) { if (!add) { lt_dtab_clear_sel(st); } return; }
    if ((size_t)idx >= st->n_msel) { return; }
    if (!add) { memset(st->msel, 0, st->n_msel); }
    if (add && st->msel[idx]) {
        st->msel[idx] = 0;
        if (st->sel == idx) { st->sel = -1; }
        return;
    }
    st->msel[idx] = 1;
    st->sel = idx;
}

void lt_dtab_select_all(dash_state *st)
{
    size_t i;
    msel_fit(st);
    for (i = 0; i < st->n_msel; i++) { st->msel[i] = 1; }
    st->sel = st->n_msel > 0 ? (int)st->n_msel - 1 : -1;
}

size_t lt_dtab_sel_list(const dash_state *st, size_t *out, size_t cap)
{
    size_t i, n = 0;
    for (i = 0; i < st->n_msel && n < cap; i++) { if (st->msel[i]) { out[n++] = i; } }
    return n;
}

/* ---- history ---- */
void lt_dtab_touch(dash_state *st)
{
    st->dirty = 1; st->changed = 1; st->change_ms = lt_plat_now_ms(); st->rep_valid = 0;
}

void lt_dtab_flush(dash_state *st)
{
    if (st->changed && st->file != NULL) {
        (void)lt_dash_hist_push(&st->hist, st->file);
        st->changed = 0;
    }
}

static void hist_reset(dash_state *st)
{
    lt_dash_hist_free(&st->hist);
    lt_dash_hist_init(&st->hist);
    (void)lt_dash_hist_push(&st->hist, st->file);
    st->changed = 0;
}

/* ---- loading ---- */
char *lt_dtab_dir(lt_app *app)
{
    if (app->has_project && app->project.path != NULL) { return lt_path_join(app->project.path, "dashboards"); }
    return NULL;
}

static void adopt_file(dash_state *st, lt_dash_file *f, int fresh)
{
    if (st->view != NULL) { lt_dash_view_free(st->view); st->view = NULL; }
    if (st->file != NULL) { lt_dash_free(st->file); }
    st->file = f;
    st->view = lt_dash_view_new(f);
    st->cache_w = 0;
    lt_free(st->msel); st->msel = NULL; st->n_msel = 0;
    st->mode = DM_NONE; st->ctx_idx = -1; st->rep_valid = 0; st->compat_valid = 0;
    if (fresh) { st->sel = -1; st->props_open = 0; st->dirty = 0; st->compat_hidden = 0; hist_reset(st); st->hist_ready = 1; }
    else if (st->sel >= (int)f->cluster.n_comps) { st->sel = -1; }
    msel_fit(st);
    if (st->sel >= 0) { st->msel[st->sel] = 1; }
}

/* post-load processing shared by files and built-in sources */
static void prepare(lt_app *app, dash_state *st, lt_dash_file *f, int from_ini)
{
    dash_cfg *c = lt_dashx_cfg(app);
    lt_dash_resolve_channels(f, app->ini);
    if (c->auto_sync && !from_ini) { (void)lt_dash_sync_ranges(f, app->ini, &app->varenv.env); }
    if (from_ini && (c->ind_cols > 0 || c->ind_fill)) { lt_dash_layout_indicators(f, c->ind_cols, c->ind_fill ? LT_TRUE : LT_FALSE); }
    LT_UNUSED(st);
}

void lt_dtab_load_builtin(lt_app *app, dash_state *st, int src)
{
    lt_dash_file *f = NULL;
    if (app->ini == NULL) { return; }
    if (src == DS_FRONT) {
        f = lt_dash_from_frontpage(app->ini, &app->varenv.env);
        if (f != NULL && f->cluster.n_comps == 0) { lt_dash_free(f); f = NULL; src = DS_BASIC; }
    }
    if (f == NULL) {
        if (src == DS_TUNING) { f = lt_dash_template_tuning(); }
        else if (src == DS_TELEM) { f = lt_dash_template_telemetry(); }
        else { f = lt_dash_template_basic(); src = DS_BASIC; }
    }
    prepare(app, st, f, src == DS_FRONT);
    st->src = src;
    st->path[0] = '\0';
    adopt_file(st, f, 1);
}

static void remember(lt_app *app, const char *key)
{
    if (!app->has_project || app->project.path == NULL) { return; }
    if (app->project.cfg.dashboard != NULL && strcmp(app->project.cfg.dashboard, key) == 0) { return; }
    lt_free(app->project.cfg.dashboard);
    app->project.cfg.dashboard = lt_strdup(key);
    (void)lt_project_save_config(&app->project);
}

int lt_dtab_load_file(lt_app *app, dash_state *st, const char *path)
{
    lt_dash_file *f = lt_dash_load(path);
    char *dir;
    if (f == NULL) { lt_app_toast(app, 3, "Cannot load dashboard: %s", lt_err_get()); return -1; }
    prepare(app, st, f, 0);
    st->src = DS_FILE;
    lt_snprintf(st->path, sizeof st->path, "%s", path);
    adopt_file(st, f, 1);
    dir = lt_dtab_dir(app);
    if (dir != NULL) {
        char *mine = lt_path_join(dir, lt_path_base(path));
        if (strcmp(mine, path) == 0) { remember(app, lt_path_base(path)); }
        lt_free(mine);
        lt_free(dir);
    }
    return 0;
}

static void load_default(lt_app *app, dash_state *st)
{
    char *dir = lt_dtab_dir(app);
    const char *want = app->has_project ? app->project.cfg.dashboard : NULL;
    if (want != NULL && want[0] == '@') {
        int i;
        for (i = 0; i < 4; i++) {
            if (strcmp(want, builtin_keys[i]) == 0) { lt_free(dir); lt_dtab_load_builtin(app, st, i); return; }
        }
    }
    if (dir != NULL && want != NULL && want[0]) {
        char *p = lt_path_join(dir, want);
        int ok = lt_file_exists(p) && lt_dtab_load_file(app, st, p) == 0;
        lt_free(p);
        if (ok) { lt_free(dir); return; }
    }
    lt_free(dir);
    lt_dtab_load_builtin(app, st, (app->ini != NULL && app->ini->frontpage != NULL &&
                                   app->ini->frontpage->n_gauges >= 4) ? DS_FRONT : DS_BASIC);
}

void lt_dtab_reset_defaults(lt_app *app, dash_state *st)
{
    lt_dtab_load_builtin(app, st, (app->ini != NULL && app->ini->frontpage != NULL &&
                                   app->ini->frontpage->n_gauges >= 4) ? DS_FRONT : DS_BASIC);
    remember(app, builtin_keys[st->src]);
    lt_app_toast(app, 1, "Default gauges reloaded");
}

void lt_dtab_lib_refresh(lt_app *app, dash_state *st)
{
    char *dir = lt_dtab_dir(app);
    int i;
    lt_dashlib_free(&st->lib);
    lt_dashlib_init(&st->lib);
    (void)lt_dashlib_scan(&st->lib, dir, "User");
    lt_free(dir);
    for (i = 0; i < 4; i++) {          /* built-in sources are listed as "Reference" entries */
        lt_dashlib_entry *e;
        if (st->lib.n == st->lib.cap) {
            st->lib.cap = st->lib.cap ? st->lib.cap * 2 : 16;
            st->lib.e = (lt_dashlib_entry *)lt_realloc(st->lib.e, sizeof(lt_dashlib_entry) * st->lib.cap);
        }
        e = &st->lib.e[st->lib.n++];
        e->name = lt_strdup(builtin_names[i]);
        e->path = lt_strdup(builtin_keys[i]);
        e->category = lt_strdup("Reference");
        e->size = 0;
    }
    lt_dashlib_sort(&st->lib);
    st->lib_valid = 1;
}

const char *lt_dtab_current_name(const dash_state *st, char *buf, size_t cap)
{
    if (st->src != DS_FILE || st->path[0] == '\0') {
        lt_strlcpy(buf, st->src < 4 ? builtin_names[st->src] : "Dashboard", cap);
        if (st->src == DS_FILE) { lt_strlcpy(buf, "Untitled", cap); }
    } else {
        const char *b = lt_path_base(st->path);
        size_t k = strlen(b);
        if (k > 5 && lt_stricmp(b + k - 5, ".dash") == 0) { k -= 5; }
        if (k >= cap) { k = cap - 1; }
        memcpy(buf, b, k);
        buf[k] = '\0';
    }
    return buf;
}

int lt_dtab_save(lt_app *app, dash_state *st)
{
    if (st->path[0] == '\0') { return -1; }
    if (lt_dash_save(st->file, st->path) != 0) { lt_app_toast(app, 3, "Save failed: %s", lt_err_get()); return -1; }
    st->dirty = 0;
    lt_app_toast(app, 1, "Saved %s", lt_path_base(st->path));
    return 0;
}

void lt_dtab_revalidate(lt_app *app, dash_state *st)
{
    const char **names = NULL;
    size_t i, n = 0;
    if (st->file == NULL) { return; }
    lt_dash_report_free(&st->rep);
    lt_dash_compat_free(&st->compat);
    if (app->ini != NULL && app->ini->n_channels > 0) {
        names = (const char **)lt_malloc(sizeof(char *) * app->ini->n_channels);
        for (i = 0; i < app->ini->n_channels; i++) { names[n++] = app->ini->channels[i]->name; }
    }
    lt_dash_validate(st->file, names, n, &st->rep);
    lt_free((void *)names);
    lt_dash_compat_check(st->file, app->ini, &st->compat);
    st->rep_valid = 1; st->compat_valid = 1;
}

/* ---- add / remove / clipboard ---- */
static const char *first_channel(lt_app *app, int idx)
{
    if (app->ini == NULL || app->ini->n_channels == 0) { return ""; }
    return app->ini->channels[(size_t)idx % app->ini->n_channels]->name;
}

void lt_dtab_add_gauge(lt_app *app, dash_state *st, double x, double y, lt_gauge_painter pt, const char *chan)
{
    lt_dash_gauge *g = lt_dash_add_gauge(st->file);
    size_t n = st->file->cluster.n_comps;
    const char *ch = (chan != NULL && chan[0]) ? chan : first_channel(app, 0);
    int cells = snap_cells(app);
    char id[40];
    g->painter = pt;
    lt_snprintf(id, sizeof id, "gauge_%lu", (unsigned long)n);
    dash_set_str(&g->id, id);
    dash_set_str(&g->output_channel, ch);
    dash_set_str(&g->title, ch);
    g->back_color = LT_RGB(28, 32, 40); g->font_color = LT_RGB(255, 255, 255);
    g->trim_color = LT_RGB(148, 163, 184); g->needle_color = LT_RGB(74, 158, 248);
    g->warn_color = LT_RGB(234, 179, 8); g->critical_color = LT_RGB(239, 68, 68);
    g->min = 0.0; g->max = 100.0;
    g->relative_width = 0.25; g->relative_height = 0.3;
    g->relative_x = clampd(lt_dash_snap(x - 0.125, cells), 0.0, 0.75);
    g->relative_y = clampd(lt_dash_snap(y - 0.15, cells), 0.0, 0.7);
    if (lt_dashx_cfg(app)->auto_sync) { (void)lt_dash_sync_ranges(st->file, app->ini, &app->varenv.env); }
    lt_dtab_select(st, (int)n - 1, 0);
    lt_dtab_touch(st); st->rebuild = 1;
}

void lt_dtab_add_indicator(lt_app *app, dash_state *st, double x, double y, const char *chan)
{
    lt_dash_indicator *in = lt_dash_add_indicator(st->file);
    size_t n = st->file->cluster.n_comps;
    int cells = snap_cells(app);
    char id[40];
    lt_snprintf(id, sizeof id, "indicator_%lu", (unsigned long)n);
    dash_set_str(&in->id, id);
    dash_set_str(&in->output_channel, (chan != NULL && chan[0]) ? chan : first_channel(app, 0));
    in->relative_width = 0.12; in->relative_height = 0.06;
    in->relative_x = clampd(lt_dash_snap(x - 0.06, cells), 0.0, 0.88);
    in->relative_y = clampd(lt_dash_snap(y - 0.03, cells), 0.0, 0.94);
    in->on_background_color = LT_RGB(220, 40, 40); in->off_background_color = LT_RGB(40, 60, 40);
    in->on_text_color = LT_RGB(255, 255, 255); in->off_text_color = LT_RGB(180, 200, 180);
    lt_dtab_select(st, (int)n - 1, 0);
    lt_dtab_touch(st); st->rebuild = 1;
}

void lt_dtab_remove_sel(dash_state *st)
{
    size_t idx[512], n, i;
    n = lt_dtab_sel_list(st, idx, 512);
    if (n == 0 && st->sel >= 0) { idx[0] = (size_t)st->sel; n = 1; }
    if (n == 0) { return; }
    for (i = n; i > 0; i--) { lt_dash_remove(st->file, idx[i - 1]); }
    lt_dtab_clear_sel(st);
    st->ctx_idx = -1; st->props_open = 0;
    lt_dtab_touch(st); st->rebuild = 1;
}

static void set_new_selection(dash_state *st, size_t first, size_t count)
{
    size_t i;
    st->n_msel = 0; lt_free(st->msel); st->msel = NULL;
    msel_fit(st);
    for (i = first; i < first + count && i < st->n_msel; i++) { st->msel[i] = 1; st->sel = (int)i; }
}

void lt_dtab_copy(lt_app *app, dash_state *st)
{
    size_t idx[512], n = lt_dtab_sel_list(st, idx, 512);
    if (n == 0 && st->sel >= 0) { idx[0] = (size_t)st->sel; n = 1; }
    if (n == 0) { return; }
    lt_free(st->clip);
    st->clip = lt_dash_clip_copy(st->file, idx, n);
    lt_ui_clipboard_set(app->ui, st->clip);
    lt_app_set_status(app, "Copied %lu component(s)", (unsigned long)n);
}

void lt_dtab_paste(lt_app *app, dash_state *st)
{
    char *sys = lt_ui_clipboard_get(app->ui);
    const char *txt = st->clip;
    size_t first, n;
    if (sys != NULL && strstr(sys, "<dashFile") != NULL) { txt = sys; }
    if (txt == NULL) { lt_free(sys); return; }
    n = lt_dash_clip_paste(st->file, txt, 0.03, 0.03, &first);
    lt_free(sys);
    if (n == 0) { return; }
    st->rebuild = 1;
    set_new_selection(st, first, n);
    lt_dtab_touch(st);
}

static void replace_from_history(lt_app *app, dash_state *st, lt_dash_file *f)
{
    if (f == NULL) { return; }
    prepare(app, st, f, 0);          /* channels are stored resolved already; sync only when enabled */
    adopt_file(st, f, 0);
    st->dirty = 1; st->changed = 0;
}

void lt_dtab_undo(lt_app *app, dash_state *st)
{
    lt_dtab_flush(st);
    replace_from_history(app, st, lt_dash_hist_undo(&st->hist));
}

void lt_dtab_redo(lt_app *app, dash_state *st)
{
    lt_dtab_flush(st);
    replace_from_history(app, st, lt_dash_hist_redo(&st->hist));
}

/* ---- demo sweep: feeds moving values for the channels the dashboard uses ---- */
typedef struct demo_ctx {
    dash_state *st;
    lt_expr_env real;
    double t;
} demo_ctx;
static demo_ctx g_demo;

static lt_bool demo_get(void *ud, const char *name, double *out)
{
    size_t i;
    const lt_dash_cluster *c;
    if (g_demo.st == NULL || g_demo.st->file == NULL || name == NULL) { return g_demo.real.get_var(ud, name, out); }
    c = &g_demo.st->file->cluster;
    for (i = 0; i < c->n_comps; i++) {
        const lt_dash_gauge *g = c->comps[i].gauge;
        const lt_dash_indicator *in = c->comps[i].indicator;
        const char *ch = g != NULL ? g->output_channel : in->output_channel;
        double ph, f;
        if (ch == NULL || strcmp(ch, name) != 0) { continue; }
        ph = (double)(lt_hash_str(ch, LT_FALSE) % 628UL) / 100.0;
        f = 0.5 + 0.5 * sin(g_demo.t * 0.0009 + ph);
        if (g != NULL) { *out = g->min + (g->max - g->min) * f; }
        else { *out = f > 0.5 ? 1.0 : 0.0; }
        return LT_TRUE;
    }
    return g_demo.real.get_var(ud, name, out);
}

static const lt_expr_env *pick_env(lt_app *app, dash_state *st, double now)
{
    static lt_expr_env de;
    if (!lt_dashx_cfg(app)->demo) { g_demo.st = NULL; return &app->varenv.env; }
    g_demo.st = st; g_demo.real = app->varenv.env; g_demo.t = now;
    de = app->varenv.env;
    de.get_var = demo_get;
    return &de;
}

/* ---- refresh cap cache (copy of the rendered cluster, reused between updates) ---- */
static void cache_store(dash_state *st, lt_gfx *g, lt_rect r)
{
    lt_canvas *c = g->canvas;
    int x = r.x + g->ox, y = r.y + g->oy, j;
    size_t cells;
    if (c == NULL || r.w <= 0 || r.h <= 0 || x < 0 || y < 0 || x + r.w > c->w || y + r.h > c->h ||
        !lt_mul_size((size_t)r.w, (size_t)r.h, &cells)) { st->cache_w = 0; return; }
    if (st->cache_px == NULL || st->cache_w != r.w || st->cache_h != r.h) {
        st->cache_px = (lt_u32 *)lt_realloc(st->cache_px, sizeof(lt_u32) * (cells + 1));
    }
    for (j = 0; j < r.h; j++) {
        memcpy(st->cache_px + (size_t)j * (size_t)r.w, c->px + (size_t)(y + j) * (size_t)c->pitch + (size_t)x,
               sizeof(lt_u32) * (size_t)r.w);
    }
    st->cache_w = r.w; st->cache_h = r.h;
}

/* ---- drawing callback ---- */
static void render_cb(lt_gfx *g, lt_rect r, void *user)
{
    dash_state *st = (dash_state *)user;
    int i;
    size_t k;
    if (st->view == NULL) { lt_gfx_fill_rect(g, r, LT_RGB(0, 0, 0)); return; }
    if (lt_gfx_recording(g)) {
        /* retained rendering: the canvas cannot be read back, and an unchanged cluster costs
         * nothing to re-record (its tiles keep their pixels); between refreshes the view state is
         * unchanged, so this draws exactly what the cached copy would show */
        if (st->cache_px != NULL) { lt_free(st->cache_px); st->cache_px = NULL; }
        lt_gfx_fill_rect(g, r, LT_RGB(0, 0, 0));
        lt_dash_view_render(st->view, g, r, st->env);
        st->cache_w = r.w; st->cache_h = r.h;
    } else if (!st->refresh_now && st->cache_px != NULL && st->cache_w == r.w && st->cache_h == r.h && g->canvas != NULL) {
        lt_gfx_blit(g, r.x, r.y, st->cache_px, r.w, r.h, r.w);
    } else {
        lt_gfx_fill_rect(g, r, LT_RGB(0, 0, 0));
        lt_dash_view_render(st->view, g, r, st->env);
        if (!lt_gfx_cell_mode(g)) { cache_store(st, g, r); }
    }
    if (!st->designer || lt_gfx_cell_mode(g)) { return; }
    for (i = 1; i < 20; i++) {
        lt_gfx_vline(g, st->cluster.x + st->cluster.w * i / 20, st->cluster.y, st->cluster.y + st->cluster.h - 1,
                     LT_RGBA(255, 255, 255, i % 5 == 0 ? 28 : 12));
        lt_gfx_hline(g, st->cluster.x, st->cluster.x + st->cluster.w - 1, st->cluster.y + st->cluster.h * i / 20,
                     LT_RGBA(255, 255, 255, i % 5 == 0 ? 28 : 12));
    }
    for (k = 0; k < st->n_msel && k < lt_dash_view_count(st->view); k++) {
        lt_rect c;
        lt_color pc = (int)k == st->sel ? LT_RGB(66, 153, 255) : LT_RGB(120, 190, 255);
        if (!st->msel[k]) { continue; }
        c = lt_dash_view_component_rect(st->view, k);
        lt_gfx_rect(g, c, pc);
        if ((int)k == st->sel) {
            lt_gfx_rect(g, lt_rect_inset(c, 1, 1), pc);
            lt_gfx_fill_rect(g, lt_rect_make(c.x + c.w - 8, c.y + c.h - 8, 8, 8), pc);
            lt_gfx_fill_rect(g, lt_rect_make(c.x - 1, c.y - 1, 5, 5), pc);
            lt_gfx_fill_rect(g, lt_rect_make(c.x + c.w - 4, c.y - 1, 5, 5), pc);
            lt_gfx_fill_rect(g, lt_rect_make(c.x - 1, c.y + c.h - 4, 5, 5), pc);
        }
    }
    if (st->mode == DM_BAND) {
        lt_rect b = lt_rect_make(st->drag_x < st->ctx_x ? st->drag_x : st->ctx_x,
                                 st->drag_y < st->ctx_y ? st->drag_y : st->ctx_y,
                                 st->drag_x < st->ctx_x ? st->ctx_x - st->drag_x : st->drag_x - st->ctx_x,
                                 st->drag_y < st->ctx_y ? st->ctx_y - st->drag_y : st->drag_y - st->ctx_y);
        lt_gfx_fill_rect(g, b, LT_RGBA(66, 153, 255, 40));
        lt_gfx_rect(g, b, LT_RGB(66, 153, 255));
    }
}

/* ---- properties window ---- */
static void image_combo(lt_ui *ui, dash_state *st, const char *id, char **slot)
{
    const lt_dash_cluster *c = &st->file->cluster;
    const char **items = (const char **)lt_malloc(sizeof(char *) * (c->n_images + 1));
    int sel = 0, n = 1;
    size_t i;
    items[0] = "(none)";
    for (i = 0; i < c->n_images; i++) {
        items[n] = c->images[i].image_id != NULL ? c->images[i].image_id : "?";
        if (*slot != NULL && strcmp(*slot, items[n]) == 0) { sel = n; }
        n++;
    }
    if (lt_ui_combo(ui, id, &sel, items, n)) {
        dash_set_str(slot, sel == 0 ? NULL : items[sel]);
        lt_dtab_touch(st);
    }
    lt_free((void *)items);
}

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
    if (st->props_rect.w == 0) { st->props_rect = lt_rect_make(lt_ui_px(ui, 80), lt_ui_px(ui, 80), lt_ui_px(ui, 420), lt_ui_px(ui, 560)); }
    w1[0] = 120; w1[1] = -1;
    if (!lt_ui_window_begin(ui, title, &st->props_rect, &st->props_open, LT_UI_WIN_RESIZE)) { return; }
    if (c->gauge != NULL) {
        lt_dash_gauge *g = c->gauge;
        char buf[160];
        double d;
        int sel = (int)g->painter, ch = 0, i, nm = 0;
        const char *names[DT_N_PAINTERS];
        const char **match = NULL;
        lt_ui_layout_row(ui, 2, w1, 0);
        lt_ui_label(ui, "Title");
        lt_snprintf(buf, sizeof buf, "%s", g->title ? g->title : "");
        if (lt_ui_text(ui, "dp_title", buf, sizeof buf, 0) & LT_UI_CHANGED) { dash_set_str(&g->title, buf); lt_dtab_touch(st); }
        lt_ui_label(ui, "Units");
        lt_snprintf(buf, sizeof buf, "%s", g->units ? g->units : "");
        if (lt_ui_text(ui, "dp_units", buf, sizeof buf, 0) & LT_UI_CHANGED) { dash_set_str(&g->units, buf); lt_dtab_touch(st); }
        for (i = 0; i < DT_N_PAINTERS; i++) { names[i] = lt_painter_to_ts((lt_gauge_painter)i); }
        lt_ui_label(ui, "Type");
        if (lt_ui_combo(ui, "dp_type", &sel, names, DT_N_PAINTERS)) { g->painter = (lt_gauge_painter)sel; lt_dtab_touch(st); }
        lt_ui_label(ui, "Channel");
        lt_ui_label_ex(ui, g->output_channel ? g->output_channel : "(none)", lt_ui_theme(ui)->accent, LT_UI_ALIGN_LEFT);
        lt_ui_label(ui, "Filter");
        lt_ui_text(ui, "dp_filter", st->filter, sizeof st->filter, 0);
        if (app->ini != NULL && app->ini->n_channels > 0) {
            size_t k;
            match = (const char **)lt_malloc(sizeof(char *) * (app->ini->n_channels + lt_datalog_virtual_count() + 1));
            for (k = 0; k < app->ini->n_channels + lt_datalog_virtual_count(); k++) {   /* + live math channels */
                const char *nme = k < app->ini->n_channels ? app->ini->channels[k]->name
                                                           : lt_datalog_virtual_name(k - app->ini->n_channels);
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
                lt_dtab_touch(st); st->rebuild = 1;
            } else if (nm == 0) { lt_ui_label(ui, "No matching channels"); }
            lt_free((void *)match);
        }
        lt_ui_layout_row(ui, 2, w1, 0);
#define NUMF(label, id, field, lo, hi, step, dg) \
        lt_ui_label(ui, label); d = (field); \
        if (lt_ui_number(ui, id, &d, lo, hi, step, dg) & LT_UI_CHANGED) { (field) = d; lt_dtab_touch(st); }
        NUMF("Min", "dp_min", g->min, -1e9, 1e9, 1.0, 2)
        NUMF("Max", "dp_max", g->max, -1e9, 1e9, 1.0, 2)
        d = (double)g->value_digits;
        lt_ui_label(ui, "Value digits");
        if (lt_ui_number(ui, "dp_dig", &d, 0, 6, 1.0, 0) & LT_UI_CHANGED) { g->value_digits = (int)d; lt_dtab_touch(st); }
        lt_ui_label(ui, "Colors");
        if (lt_ui_color_edit(ui, "dp_cfont", &g->font_color)) { lt_dtab_touch(st); }
        lt_ui_label(ui, "Needle / bar");
        if (lt_ui_color_edit(ui, "dp_cneedle", &g->needle_color)) { lt_dtab_touch(st); }
        lt_ui_label(ui, "Back");
        if (lt_ui_color_edit(ui, "dp_cback", &g->back_color)) { lt_dtab_touch(st); }
#undef NUMF
        {
            int v;
            lt_ui_label(ui, "Peak hold");
            v = g->peak_hold ? 1 : 0;
            if (lt_ui_checkbox(ui, "show", &v)) { g->peak_hold = v ? LT_TRUE : LT_FALSE; lt_dtab_touch(st); }
            lt_ui_label(ui, "Antialiasing");
            v = g->antialiasing_on ? 1 : 0;
            if (lt_ui_checkbox(ui, "on", &v)) { g->antialiasing_on = v ? LT_TRUE : LT_FALSE; lt_dtab_touch(st); }
            lt_ui_label(ui, "High warning");
            v = g->has_high_warning ? 1 : 0;
            if (lt_ui_checkbox(ui, "enabled", &v)) { g->has_high_warning = v ? LT_TRUE : LT_FALSE; lt_dtab_touch(st); }
            if (g->has_high_warning) {
                d = g->high_warning;
                lt_ui_label(ui, "  value");
                if (lt_ui_number(ui, "dp_hw", &d, -1e9, 1e9, 1.0, 2) & LT_UI_CHANGED) { g->high_warning = d; lt_dtab_touch(st); }
            }
            lt_ui_label(ui, "High critical");
            v = g->has_high_critical ? 1 : 0;
            if (lt_ui_checkbox(ui, "enabled", &v)) { g->has_high_critical = v ? LT_TRUE : LT_FALSE; lt_dtab_touch(st); }
            if (g->has_high_critical) {
                d = g->high_critical;
                lt_ui_label(ui, "  value");
                if (lt_ui_number(ui, "dp_hc", &d, -1e9, 1e9, 1.0, 2) & LT_UI_CHANGED) { g->high_critical = d; lt_dtab_touch(st); }
            }
        }
        lt_ui_label(ui, "Background image");
        image_combo(ui, st, "dp_bgimg", &g->background_image_file_name);
        lt_ui_label(ui, "Needle image");
        image_combo(ui, st, "dp_ndimg", &g->needle_image_file_name);
        lt_ui_label(ui, "Enabled when");
        lt_snprintf(buf, sizeof buf, "%s", g->enabled_condition ? g->enabled_condition : "");
        if (lt_ui_text(ui, "dp_cond", buf, sizeof buf, 0) & LT_UI_CHANGED) { dash_set_str(&g->enabled_condition, buf[0] ? buf : NULL); lt_dtab_touch(st); }
        lt_ui_label(ui, "Click action");
        lt_snprintf(buf, sizeof buf, "%s", g->short_click_action ? g->short_click_action : "");
        if (lt_ui_text(ui, "dp_click", buf, sizeof buf, 0) & LT_UI_CHANGED) { dash_set_str(&g->short_click_action, buf[0] ? buf : NULL); lt_dtab_touch(st); }
        lt_ui_label(ui, "Long click action");
        lt_snprintf(buf, sizeof buf, "%s", g->long_click_action ? g->long_click_action : "");
        if (lt_ui_text(ui, "dp_lclick", buf, sizeof buf, 0) & LT_UI_CHANGED) { dash_set_str(&g->long_click_action, buf[0] ? buf : NULL); lt_dtab_touch(st); }
    } else {
        lt_dash_indicator *in = c->indicator;
        char buf[96];
        int sel = (int)in->painter, v;
        static const char *const kinds[3] = { "Rectangle", "Bulb", "LED" };
        lt_ui_layout_row(ui, 2, w1, 0);
        lt_ui_label(ui, "On text");
        lt_snprintf(buf, sizeof buf, "%s", in->on_text ? in->on_text : "");
        if (lt_ui_text(ui, "di_on", buf, sizeof buf, 0) & LT_UI_CHANGED) { dash_set_str(&in->on_text, buf); lt_dtab_touch(st); }
        lt_ui_label(ui, "Off text");
        lt_snprintf(buf, sizeof buf, "%s", in->off_text ? in->off_text : "");
        if (lt_ui_text(ui, "di_off", buf, sizeof buf, 0) & LT_UI_CHANGED) { dash_set_str(&in->off_text, buf); lt_dtab_touch(st); }
        lt_ui_label(ui, "Style");
        if (lt_ui_combo(ui, "di_style", &sel, kinds, 3)) { in->painter = (lt_ind_painter)sel; lt_dtab_touch(st); }
        lt_ui_label(ui, "Channel");
        lt_snprintf(buf, sizeof buf, "%s", in->output_channel ? in->output_channel : "");
        if (lt_ui_text(ui, "di_ch", buf, sizeof buf, 0) & LT_UI_CHANGED) {
            dash_set_str(&in->output_channel, buf); lt_dtab_touch(st); st->rebuild = 1;
        }
        lt_ui_label(ui, "On color");
        if (lt_ui_color_edit(ui, "di_con", &in->on_background_color)) { lt_dtab_touch(st); }
        lt_ui_label(ui, "Off color");
        if (lt_ui_color_edit(ui, "di_coff", &in->off_background_color)) { lt_dtab_touch(st); }
        lt_ui_label(ui, "Antialiasing");
        v = in->antialiasing_on ? 1 : 0;
        if (lt_ui_checkbox(ui, "on", &v)) { in->antialiasing_on = v ? LT_TRUE : LT_FALSE; lt_dtab_touch(st); }
        lt_ui_label(ui, "Enabled when");
        lt_snprintf(buf, sizeof buf, "%s", in->enabled_condition ? in->enabled_condition : "");
        if (lt_ui_text(ui, "di_cond", buf, sizeof buf, 0) & LT_UI_CHANGED) { dash_set_str(&in->enabled_condition, buf[0] ? buf : NULL); lt_dtab_touch(st); }
        lt_ui_label(ui, "Click action");
        lt_snprintf(buf, sizeof buf, "%s", in->short_click_action ? in->short_click_action : "");
        if (lt_ui_text(ui, "di_click", buf, sizeof buf, 0) & LT_UI_CHANGED) { dash_set_str(&in->short_click_action, buf[0] ? buf : NULL); lt_dtab_touch(st); }
    }
    lt_ui_window_end(ui);
}

/* ---- toolbar ---- */
enum { T_SEL, T_OPEN, T_VALID, T_FULL, T_DESIGN, T_ADDG, T_ADDI, T_DEL, T_UNDO, T_REDO, T_COPY, T_PASTE,
       T_LAYERS, T_ASSETS, T_SYNC, T_SAVE, T_SAVEAS, T_NONE };

static void dt_item(lt_vk_item *it, int kind, int icon, const char *label, const char *tip)
{
    memset(it, 0, sizeof(*it));
    it->kind = kind; it->icon = icon; it->label = label; it->tip = tip;
}

static void open_file_dialog(lt_app *app)
{
    char *dir = lt_dtab_dir(app);
    lt_app_file_dialog(app, FD_OPEN_DASH, LT_UI_FILE_OPEN, dir, "", "Dashboards|*.dash|All files|*");
    lt_free(dir);
}

static void dash_toolbar(lt_app *app, dash_state *st)
{
    lt_ui *ui = app->ui;
    int w1 = -1, want_as = 0, hit, n = 0, ids[24];
    lt_vk_item it[24];
    lt_rect rr;
    char name[96], lab[120], vlab[40];
    lt_dtab_current_name(st, name, sizeof name);
    lt_snprintf(lab, sizeof lab, "%s%s", name, st->dirty ? " *" : "");
    if (st->rep_valid && (st->rep.n > 0)) {
        size_t i, ne = 0;
        for (i = 0; i < st->rep.n; i++) { if (st->rep.problems[i].is_error) { ne++; } }
        lt_snprintf(vlab, sizeof vlab, "Validate (%lu)", (unsigned long)st->rep.n);
        (void)ne;
    } else {
        lt_snprintf(vlab, sizeof vlab, "Validate");
    }
    lt_ui_layout_row(ui, 1, &w1, lt_ui_cell_mode(ui) ? 0 : 32);
    rr = lt_ui_layout_next(ui);
    dt_item(&it[n], LT_VK_BTN, LT_ICON_LAYOUT_DASHBOARD, lab, "Choose, create, import or manage dashboards"); ids[n++] = T_SEL;
    dt_item(&it[n], LT_VK_BTN, LT_ICON_FOLDER_OPEN, "Open...", "Open a .dash file"); ids[n++] = T_OPEN;
    dt_item(&it[n], LT_VK_BTN, st->rep_valid && !st->rep.is_valid ? LT_ICON_TRIANGLE_ALERT : LT_ICON_SHIELD_CHECK, vlab,
            "Check the dashboard against the ECU definition"); it[n].on = st->valid_open; ids[n++] = T_VALID;
    dt_item(&it[n], LT_VK_BTN, LT_ICON_MAXIMIZE, NULL, "Full screen (F11)"); ids[n++] = T_FULL;
    dt_item(&it[n], LT_VK_BTN, LT_ICON_PENCIL, "Designer", "Edit the layout: drag, resize, add and remove gauges");
    it[n].on = st->designer; ids[n++] = T_DESIGN;
    if (st->designer) {
        dt_item(&it[n], LT_VK_SEP, 0, NULL, NULL); ids[n++] = -1;
        dt_item(&it[n], LT_VK_BTN, LT_ICON_GAUGE, "Add", "Gauge palette: pick a gauge type and channel"); it[n].on = st->palette_open; ids[n++] = T_ADDG;
        dt_item(&it[n], LT_VK_BTN, LT_ICON_CIRCLE_DOT, NULL, "Add an indicator"); ids[n++] = T_ADDI;
        dt_item(&it[n], LT_VK_BTN, LT_ICON_TRASH_2, NULL, "Delete the selected components (Del)"); it[n].disabled = st->sel < 0; ids[n++] = T_DEL;
        dt_item(&it[n], LT_VK_BTN, LT_ICON_UNDO_2, NULL, "Undo (Ctrl+Z)"); it[n].disabled = !(lt_dash_hist_can_undo(&st->hist) || st->changed); ids[n++] = T_UNDO;
        dt_item(&it[n], LT_VK_BTN, LT_ICON_REDO_2, NULL, "Redo (Ctrl+Shift+Z)"); it[n].disabled = !lt_dash_hist_can_redo(&st->hist); ids[n++] = T_REDO;
        dt_item(&it[n], LT_VK_BTN, LT_ICON_COPY, NULL, "Copy (Ctrl+C)"); it[n].disabled = st->sel < 0; ids[n++] = T_COPY;
        dt_item(&it[n], LT_VK_BTN, LT_ICON_CLIPBOARD_PASTE, NULL, "Paste (Ctrl+V)"); ids[n++] = T_PASTE;
        dt_item(&it[n], LT_VK_SEP, 0, NULL, NULL); ids[n++] = -1;
        dt_item(&it[n], LT_VK_BTN, LT_ICON_LAYERS, "Layers", "Layer order and visibility"); it[n].on = st->layers_open; ids[n++] = T_LAYERS;
        dt_item(&it[n], LT_VK_BTN, LT_ICON_IMAGE, "Assets", "Embedded images and fonts"); it[n].on = st->assets_open; ids[n++] = T_ASSETS;
        dt_item(&it[n], LT_VK_BTN, LT_ICON_REFRESH_CW, "Sync", "Adopt gauge ranges and limits from the ECU definition"); ids[n++] = T_SYNC;
        dt_item(&it[n], LT_VK_SEP, 0, NULL, NULL); ids[n++] = -1;
        dt_item(&it[n], LT_VK_BTN, LT_ICON_SAVE, "Save", "Save the dashboard"); ids[n++] = T_SAVE;
        dt_item(&it[n], LT_VK_BTN, LT_ICON_FILE_DOWN, NULL, "Save as..."); ids[n++] = T_SAVEAS;
    }
    dt_item(&it[n], LT_VK_END, 0, NULL, NULL);
    hit = lt_vk_toolbar_in(ui, it, rr);
    if (hit >= 0) {
        switch (ids[hit]) {
        case T_SEL: st->selector_open = 1; st->lib_valid = 0; break;
        case T_OPEN: open_file_dialog(app); break;
        case T_VALID: st->valid_open = !st->valid_open; if (st->valid_open && !st->rep_valid) { lt_dtab_revalidate(app, st); } break;
        case T_FULL: (void)lt_dashx_set_kiosk(app, 1); break;
        case T_DESIGN:
            st->designer = !st->designer;
            if (!st->designer) { lt_dtab_clear_sel(st); st->mode = DM_NONE; }
            break;
        case T_ADDG: st->palette_open = !st->palette_open; break;
        case T_ADDI: lt_dtab_add_indicator(app, st, 0.5, 0.5, NULL); break;
        case T_DEL: lt_dtab_remove_sel(st); break;
        case T_UNDO: lt_dtab_undo(app, st); break;
        case T_REDO: lt_dtab_redo(app, st); break;
        case T_COPY: lt_dtab_copy(app, st); break;
        case T_PASTE: lt_dtab_paste(app, st); break;
        case T_LAYERS: st->layers_open = !st->layers_open; break;
        case T_ASSETS: st->assets_open = !st->assets_open; break;
        case T_SYNC: {
            size_t k = lt_dash_sync_ranges(st->file, app->ini, &app->varenv.env);
            if (k > 0) { lt_dtab_touch(st); st->rebuild = 1; }
            lt_app_toast(app, 0, "%lu gauge(s) updated from the ECU definition", (unsigned long)k);
            break; }
        case T_SAVE:
            if (st->path[0]) { (void)lt_dtab_save(app, st); }
            else { want_as = 1; }
            break;
        case T_SAVEAS: want_as = 1; break;
        default: break;
        }
    }
    if (want_as) {
        char *dir = lt_dtab_dir(app);
        char fn[120];
        lt_snprintf(fn, sizeof fn, "%s.dash", name);
        lt_app_file_dialog(app, FD_SAVE_DASH, LT_UI_FILE_SAVE, dir, fn, "Dashboards|*.dash|All files|*");
        lt_free(dir);
    }
}

/* ---- main draw ---- */
static int hit_handle(const lt_rect *c, int x, int y)
{
    return x >= c->x + c->w - 14 && y >= c->y + c->h - 14;
}

static void rect_sel(dash_state *st, int x0, int y0, int x1, int y1, int add)
{
    size_t i;
    lt_rect b = lt_rect_make(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 < x1 ? x1 - x0 : x0 - x1, y0 < y1 ? y1 - y0 : y0 - y1);
    if (!add) { lt_dtab_clear_sel(st); }
    for (i = 0; i < lt_dash_view_count(st->view); i++) {
        lt_rect c = lt_dash_view_component_rect(st->view, i);
        if (c.x < b.x + b.w && c.x + c.w > b.x && c.y < b.y + b.h && c.y + c.h > b.y) {
            if (i < st->n_msel) { st->msel[i] = 1; st->sel = (int)i; }
        }
    }
}

static void nudge(lt_app *app, dash_state *st, int dx, int dy, int big)
{
    size_t idx[512], n = lt_dtab_sel_list(st, idx, 512), i;
    double step = big ? 0.05 : 1.0 / (double)(snap_cells(app) > 0 ? snap_cells(app) : 200);
    for (i = 0; i < n; i++) {
        double v[4];
        lt_dash_comp_get_rect(&st->file->cluster.comps[idx[i]], v);
        v[0] = clampd(v[0] + dx * step, 0.0, 1.0 - v[2]);
        v[1] = clampd(v[1] + dy * step, 0.0, 1.0 - v[3]);
        lt_dash_comp_set_rect(&st->file->cluster.comps[idx[i]], v);
    }
    if (n > 0) { lt_dtab_touch(st); }
}

static void run_action(lt_app *app, const char *action)
{
    if (action == NULL || action[0] == '\0') { return; }
    if (lt_ext_dispatch_target(app, action) == LT_EXT_NO_TARGET) { (void)lt_app_open_target(app, action, action); }
}

static void hover_tip(lt_app *app, dash_state *st, lt_point m)
{
    lt_ui *ui = app->ui;
    const lt_dash_comp *c;
    const lt_dash_gstate *gs;
    char l1[160], l2[160], l3[160];
    int w, tw, x, y, rh = lt_ui_row_height(ui);
    lt_rect box;
    if (st->hover_idx < 0 || (size_t)st->hover_idx >= st->file->cluster.n_comps) { return; }
    if (lt_ui_cell_mode(ui) || lt_ui_now(ui) - st->hover_ms < 500.0) { lt_ui_request_redraw(ui); return; }
    c = &st->file->cluster.comps[st->hover_idx];
    gs = lt_dash_view_state(st->view, (size_t)st->hover_idx);
    if (c->gauge != NULL) {
        const lt_dash_gauge *g = c->gauge;
        lt_dash_comp_label(c, l1, sizeof l1);
        lt_snprintf(l2, sizeof l2, "Value %.*f %s   range %g .. %g", g->value_digits > 0 ? g->value_digits + 1 : 2,
                    gs != NULL ? gs->raw : 0.0,
                    gs != NULL && gs->unit_label != NULL ? gs->unit_label : (g->units ? g->units : ""),
                    gs != NULL && gs->has_value ? gs->lim.min : g->min,
                    gs != NULL && gs->has_value ? gs->lim.max : g->max);
        lt_snprintf(l3, sizeof l3, "Channel %s", g->output_channel ? g->output_channel : "-");
    } else {
        lt_dash_comp_label(c, l1, sizeof l1);
        lt_snprintf(l2, sizeof l2, "State %s", gs != NULL && gs->on ? "on" : "off");
        lt_snprintf(l3, sizeof l3, "Channel %s", c->indicator->output_channel && c->indicator->output_channel[0]
                    ? c->indicator->output_channel : "(expression)");
    }
    tw = lt_ui_text_width(ui, l1);
    w = lt_ui_text_width(ui, l2); if (w > tw) { tw = w; }
    w = lt_ui_text_width(ui, l3); if (w > tw) { tw = w; }
    x = m.x + lt_ui_px(ui, 14); y = m.y + lt_ui_px(ui, 18);
    if (x + tw + 16 > st->area.x + st->area.w) { x = m.x - tw - 24; }
    if (y + rh * 3 + 8 > st->area.y + st->area.h) { y = m.y - rh * 3 - 12; }
    box = lt_rect_make(x, y, tw + 16, rh * 3 + 8);
    lt_ui_draw_box(ui, box, lt_ui_theme(ui)->elevated, lt_ui_theme(ui)->border, 4);
    lt_ui_draw_text_in(ui, lt_rect_make(box.x + 8, box.y + 4, tw, rh), l1, lt_ui_theme(ui)->text, LT_UI_ALIGN_LEFT);
    lt_ui_draw_text_in(ui, lt_rect_make(box.x + 8, box.y + 4 + rh, tw, rh), l2, lt_ui_theme(ui)->text2, LT_UI_ALIGN_LEFT);
    lt_ui_draw_text_in(ui, lt_rect_make(box.x + 8, box.y + 4 + 2 * rh, tw, rh), l3, lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
}

static void context_menu(lt_app *app, dash_state *st, lt_rect area)
{
    lt_ui *ui = app->ui;
    dash_cfg *cfg = lt_dashx_cfg(app);
    int i;
    if (!lt_ui_context_begin(ui, "dash_ctx", area)) { return; }
    {
        int ci = st->ctx_idx;
        if (ci >= 0 && (size_t)ci < st->file->cluster.n_comps) {
            lt_dash_comp *c = &st->file->cluster.comps[ci];
            if (lt_ui_menu_item(ui, "Properties...", NULL, 0)) { st->props_open = 1; st->props_idx = ci; }
            if (c->gauge != NULL) {
                if (lt_ui_submenu_begin(ui, "Gauge type", 0)) {
                    for (i = 0; i < DT_N_PAINTERS; i++) {
                        if (lt_ui_menu_item(ui, lt_painter_to_ts((lt_gauge_painter)i), NULL,
                                            (int)c->gauge->painter == i ? LT_UI_MENU_CHECKED : 0)) {
                            c->gauge->painter = (lt_gauge_painter)i; lt_dtab_touch(st);
                        }
                    }
                    lt_ui_submenu_end(ui);
                }
                if (lt_ui_menu_item(ui, "Reset peak", NULL, 0)) {
                    lt_dash_gstate *gs = (lt_dash_gstate *)lt_dash_view_state(st->view, (size_t)ci);
                    if (gs != NULL) { gs->peak.started = LT_FALSE; gs->peak.peak = gs->lim.min; }
                }
                if (lt_ui_menu_item(ui, "Reset value", NULL, 0)) {
                    lt_dash_gstate *gs = (lt_dash_gstate *)lt_dash_view_state(st->view, (size_t)ci);
                    if (gs != NULL) { gs->has_value = LT_FALSE; gs->peak.started = LT_FALSE; }
                }
                if (lt_ui_menu_item(ui, "Antialiasing", NULL, c->gauge->antialiasing_on ? LT_UI_MENU_CHECKED : 0)) {
                    c->gauge->antialiasing_on = !c->gauge->antialiasing_on; lt_dtab_touch(st);
                }
            }
            if (st->designer) {
                if (lt_ui_menu_item(ui, "Duplicate", NULL, 0)) {
                    int ni = lt_dash_comp_clone(st->file, (size_t)ci);
                    if (ni >= 0) { st->rebuild = 1; lt_dtab_select(st, ni, 0); lt_dtab_touch(st); }
                }
                if (lt_ui_menu_item(ui, "Copy", "Ctrl+C", 0)) { lt_dtab_copy(app, st); }
                if (lt_ui_menu_item(ui, "Bring to front", NULL, 0)) {
                    size_t ni = lt_dash_comp_move_z(st->file, (size_t)ci, st->file->cluster.n_comps - 1);
                    st->rebuild = 1; lt_dtab_select(st, (int)ni, 0); lt_dtab_touch(st);
                }
                if (lt_ui_menu_item(ui, "Send to back", NULL, 0)) {
                    (void)lt_dash_comp_move_z(st->file, (size_t)ci, 0);
                    st->rebuild = 1; lt_dtab_select(st, 0, 0); lt_dtab_touch(st);
                }
                if (lt_ui_menu_item(ui, "Remove", "Del", 0)) { lt_dtab_select(st, ci, 0); lt_dtab_remove_sel(st); }
            }
            lt_ui_menu_separator(ui);
        }
        if (st->designer && lt_ui_menu_item(ui, "Paste", "Ctrl+V", 0)) { lt_dtab_paste(app, st); }
        if (lt_ui_menu_item(ui, "Reload default gauges", NULL, 0)) { st->mgmt = MG_RESET; st->mgmt_open = 1; }
        if (lt_ui_menu_item(ui, "Gauge demo", NULL, cfg->demo ? LT_UI_MENU_CHECKED : 0)) { cfg->demo = !cfg->demo; }
        if (lt_ui_menu_item(ui, "Antialiasing (all)", NULL, st->file->cluster.anti_aliasing ? LT_UI_MENU_CHECKED : 0)) {
            st->file->cluster.anti_aliasing = !st->file->cluster.anti_aliasing; lt_dtab_touch(st);
        }
        if (lt_ui_menu_item(ui, "Designer mode", NULL, st->designer ? LT_UI_MENU_CHECKED : 0)) {
            st->designer = !st->designer; if (!st->designer) { lt_dtab_clear_sel(st); }
        }
        if (lt_ui_menu_item(ui, "Full screen", "F11", 0)) { (void)lt_dashx_set_kiosk(app, 1); }
        if (st->designer) {
            double fx = st->cluster.w > 0 ? (double)(st->ctx_x - st->cluster.x) / st->cluster.w : 0.5;
            double fy = st->cluster.h > 0 ? (double)(st->ctx_y - st->cluster.y) / st->cluster.h : 0.5;
            if (lt_ui_submenu_begin(ui, "Add gauge here", 0)) {
                for (i = 0; i < DT_N_PAINTERS; i++) {
                    if (lt_ui_menu_item(ui, lt_painter_to_ts((lt_gauge_painter)i), NULL, 0)) {
                        lt_dtab_add_gauge(app, st, fx, fy, (lt_gauge_painter)i, NULL);
                    }
                }
                lt_ui_submenu_end(ui);
            }
            if (lt_ui_menu_item(ui, "Add indicator here", NULL, 0)) { lt_dtab_add_indicator(app, st, fx, fy, NULL); }
        }
    }
    lt_ui_context_end(ui);
}

static void dash_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    lt_ui *ui = app->ui;
    dash_state *st = (dash_state *)tab->data;
    dash_cfg *cfg = lt_dashx_cfg(app);
    const char *fp;
    lt_rect area;
    lt_point m;
    int down, rdown, in_area, popup, block, kiosk = cfg->kiosk;
    double now = lt_ui_now(ui);

    if (st == NULL) {
        st = (dash_state *)lt_calloc(1, sizeof *st);
        lt_expr_cache_init(&st->cache);
        st->sel = -1; st->ctx_idx = -1; st->hover_idx = -1; st->press_idx = -1;
        lt_dash_hist_init(&st->hist);
        lt_dashlib_init(&st->lib);
        lt_dashlib_init(&st->imp_lib);
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
            if (e != NULL) { st->designer = 1; lt_dtab_select(st, 0, 0); lt_free(e); }
            e = lt_plat_getenv("LT_DASH_SRC");
            if (e != NULL) { lt_dtab_load_builtin(app, st, atoi(e)); lt_free(e); }
            e = lt_plat_getenv("LT_DASH_PROPS");
            if (e != NULL) { st->props_open = 1; st->props_idx = 1; lt_free(e); }
            e = lt_plat_getenv("LT_DASH_UI");
            if (e != NULL) {
                if (strstr(e, "selector")) { st->selector_open = 1; }
                if (strstr(e, "validate")) { st->valid_open = 1; }
                if (strstr(e, "layers")) { st->layers_open = 1; st->designer = 1; }
                if (strstr(e, "palette")) { st->palette_open = 1; st->designer = 1; }
                if (strstr(e, "assets")) { st->assets_open = 1; st->designer = 1; }
                if (strstr(e, "zoom")) { st->zoom_open = 1; st->zoom_idx = 0; }
                if (strstr(e, "multi")) { st->designer = 1; lt_dtab_select(st, 0, 0); lt_dtab_select(st, 1, 1); lt_dtab_select(st, 2, 1); }
                if (strstr(e, "demo")) { cfg->demo = 1; }
                if (strstr(e, "reset")) { st->mgmt = MG_RESET; st->mgmt_open = 1; }
                if (strstr(e, "new")) { st->mgmt = MG_NEW; st->mgmt_open = 1; lt_snprintf(st->mgmt_name, sizeof st->mgmt_name, "My Dashboard"); }
                if (strstr(e, "kiosk")) { (void)lt_dashx_set_kiosk(app, 1); kiosk = cfg->kiosk; }
                lt_free(e);
            }
            e = lt_plat_getenv("LT_DASH_IMPORT");
            if (e != NULL) { lt_dpanel_import_path(app, st, e); lt_free(e); }
            st->hooked = 1;
        }
    }
    fp = lt_app_file_result(app, FD_OPEN_DASH);
    if (fp != NULL) { (void)lt_dtab_load_file(app, st, fp); }
    fp = lt_app_file_result(app, FD_SAVE_DASH);
    if (fp != NULL) {
        char path[512];
        size_t n;
        lt_snprintf(path, sizeof path, "%s", fp);
        n = strlen(path);
        if (n < 5 || strcmp(path + n - 5, ".dash") != 0) { lt_snprintf(path + n, sizeof path - n, ".dash"); }
        if (lt_dash_save(st->file, path) == 0) {
            lt_snprintf(st->path, sizeof st->path, "%s", path);
            st->src = DS_FILE; st->dirty = 0; lt_app_toast(app, 1, "Saved %s", lt_path_base(path));
            st->lib_valid = 0;
        } else { lt_app_toast(app, 3, "Save failed: %s", lt_err_get()); }
    }
    (void)lt_dpanel_file_results(app, st);
    if (st->rebuild) {
        lt_dash_view_free(st->view);
        st->view = lt_dash_view_new(st->file);
        st->rebuild = 0;
        st->cache_w = 0;
        msel_fit(st);
    }
    if (!st->rep_valid) { lt_dtab_revalidate(app, st); }

    if (!kiosk) {
        dash_toolbar(app, st);
        lt_dpanel_compat_bar(app, st);
        if (st->designer) { lt_dpanel_align_bar(app, st); }
        area = lt_rect_make(r.x, lt_ui_layout_body(ui).y, r.w, 0);
        {
            lt_rect lr = lt_ui_last_rect(ui);
            area.y = lr.y + lr.h + (lt_ui_cell_mode(ui) ? 0 : 4);
            area.h = r.y + r.h - area.y;
        }
    } else {
        area = r;
    }
    if (area.h < 20) { return; }
    st->area = area;
    lt_dash_view_set_redraw_hz(st->view, cfg->refresh_hz);
    lt_dash_view_set_cell_auto(st->view, lt_ui_cell_mode(ui) && !st->designer);
    st->env = pick_env(app, st, now);
    lt_gauges_install();
    lt_gauges_set_env(&st->cache, st->env);
    {
        double interval = 1000.0 / (double)(cfg->refresh_hz > 0 ? cfg->refresh_hz : 30);
        st->refresh_now = lt_ui_cell_mode(ui) || st->cache_w == 0 || now - st->cache_ms >= interval ||
                          now < st->cache_ms;
        if (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) {
            if (app->anim_ms <= 0.0 || interval < app->anim_ms) { app->anim_ms = interval; }
        }
        if (st->refresh_now) {
            lt_dash_view_update(st->view, now, st->env);
            st->cache_ms = now;
        }
    }
    st->cluster = cluster_rect(st->file, area);
    lt_ui_draw_custom(ui, area, render_cb, st);

    /* input */
    m = lt_ui_mouse(ui);
    down = lt_ui_mouse_down(ui, LT_MB_LEFT);
    rdown = lt_ui_mouse_down(ui, LT_MB_RIGHT);
    popup = lt_ui_popup_open(ui);
    block = app->n_dialogs > 0 || app->fd.open || st->selector_open || st->mgmt_open || st->imp_open ||
            (st->props_open && lt_rect_contains(st->props_rect, m.x, m.y)) ||
            (st->layers_open && lt_rect_contains(st->layers_rect, m.x, m.y)) ||
            (st->palette_open && lt_rect_contains(st->palette_rect, m.x, m.y)) ||
            (st->assets_open && lt_rect_contains(st->assets_rect, m.x, m.y)) ||
            (st->valid_open && lt_rect_contains(st->valid_rect, m.x, m.y)) ||
            (st->zoom_open && lt_rect_contains(st->zoom_rect, m.x, m.y));
    in_area = lt_rect_contains(area, m.x, m.y) && !popup && !block;
    if (rdown && !st->prev_rdown && in_area) {
        st->ctx_idx = lt_dash_view_hit(st->view, m.x, m.y);
        st->ctx_x = m.x; st->ctx_y = m.y;
        if (st->designer && st->ctx_idx >= 0 && !lt_dtab_is_sel(st, st->ctx_idx)) { lt_dtab_select(st, st->ctx_idx, 0); }
    }
    /* hover tooltip */
    if (in_area && !st->designer && !down && !kiosk) {
        int h = lt_dash_view_hit(st->view, m.x, m.y);
        if (h != st->hover_idx) { st->hover_idx = h; st->hover_ms = now; }
        hover_tip(app, st, m);
    } else {
        st->hover_idx = -1;
    }
    if (st->designer) {
        int mods = lt_ui_mods(ui);
        int adding = (mods & (LT_MOD_SHIFT | LT_MOD_CTRL)) != 0;
        if (down && !st->prev_down && in_area) {
            int h = lt_dash_view_hit(st->view, m.x, m.y);
            st->mode = DM_NONE; st->moved = 0;
            if (h >= 0) {
                lt_rect c = lt_dash_view_component_rect(st->view, (size_t)h);
                size_t k;
                if (adding) { lt_dtab_select(st, h, 1); }
                else if (!lt_dtab_is_sel(st, h)) { lt_dtab_select(st, h, 0); }
                else { st->sel = h; }
                lt_free(st->orig_all);
                st->n_orig = st->file->cluster.n_comps;
                st->orig_all = (double *)lt_calloc(st->n_orig * 4 + 1, sizeof(double));
                for (k = 0; k < st->n_orig; k++) { lt_dash_comp_get_rect(&st->file->cluster.comps[k], st->orig_all + 4 * k); }
                st->drag_x = m.x; st->drag_y = m.y;
                if (!cfg->lock) { st->mode = hit_handle(&c, m.x, m.y) && !adding ? DM_SIZE : DM_MOVE; }
            } else {
                if (!adding) { lt_dtab_clear_sel(st); }
                st->mode = DM_BAND; st->drag_x = m.x; st->drag_y = m.y; st->ctx_x = m.x; st->ctx_y = m.y;
            }
        } else if (down && st->mode == DM_BAND) {
            st->ctx_x = m.x; st->ctx_y = m.y;
        } else if (down && (st->mode == DM_MOVE || st->mode == DM_SIZE) && st->sel >= 0 &&
                   st->orig_all != NULL && st->n_orig == st->file->cluster.n_comps) {
            double dx = st->cluster.w > 0 ? (double)(m.x - st->drag_x) / st->cluster.w : 0.0;
            double dy = st->cluster.h > 0 ? (double)(m.y - st->drag_y) / st->cluster.h : 0.0;
            int cells = snap_cells(app);
            size_t k;
            if (m.x != st->drag_x || m.y != st->drag_y) { st->moved = 1; }
            if (st->mode == DM_MOVE) {
                double minx = 2.0, miny = 2.0, maxx = -1.0, maxy = -1.0, sx, sy;
                for (k = 0; k < st->n_msel; k++) {
                    const double *o = st->orig_all + 4 * k;
                    if (!st->msel[k]) { continue; }
                    if (o[0] < minx) { minx = o[0]; }
                    if (o[1] < miny) { miny = o[1]; }
                    if (o[0] + o[2] > maxx) { maxx = o[0] + o[2]; }
                    if (o[1] + o[3] > maxy) { maxy = o[1] + o[3]; }
                }
                /* snap the group's top-left and keep it inside the cluster */
                sx = clampd(lt_dash_snap(minx + dx, cells), 0.0, 1.0 - (maxx - minx)) - minx;
                sy = clampd(lt_dash_snap(miny + dy, cells), 0.0, 1.0 - (maxy - miny)) - miny;
                for (k = 0; k < st->n_msel; k++) {
                    double v[4];
                    if (!st->msel[k]) { continue; }
                    v[0] = st->orig_all[4 * k] + sx; v[1] = st->orig_all[4 * k + 1] + sy;
                    v[2] = st->orig_all[4 * k + 2]; v[3] = st->orig_all[4 * k + 3];
                    lt_dash_comp_set_rect(&st->file->cluster.comps[k], v);
                }
            } else {
                double v[4];
                const double *o = st->orig_all + 4 * (size_t)st->sel;
                v[0] = o[0]; v[1] = o[1];
                v[2] = clampd(lt_dash_snap(o[2] + dx, cells), 0.03, 1.0 - v[0]);
                v[3] = clampd(lt_dash_snap(o[3] + dy, cells), 0.03, 1.0 - v[1]);
                lt_dash_comp_set_rect(&st->file->cluster.comps[st->sel], v);
            }
            lt_dtab_touch(st);
        }
        if (!down) {
            if (st->mode == DM_BAND) { rect_sel(st, st->drag_x, st->drag_y, m.x, m.y, adding); }
            st->mode = DM_NONE;
        }
        if (!lt_ui_text_focused(ui)) {
            if (lt_ui_key_pressed(ui, LT_KEY_DELETE, 0) || lt_ui_key_pressed(ui, LT_KEY_BACKSPACE, 0)) { lt_dtab_remove_sel(st); }
            if (lt_ui_key_pressed(ui, 'z', LT_MOD_CTRL)) { lt_dtab_undo(app, st); }
            if (lt_ui_key_pressed(ui, 'z', LT_MOD_CTRL | LT_MOD_SHIFT) || lt_ui_key_pressed(ui, 'y', LT_MOD_CTRL)) { lt_dtab_redo(app, st); }
            if (lt_ui_key_pressed(ui, 'c', LT_MOD_CTRL)) { lt_dtab_copy(app, st); }
            if (lt_ui_key_pressed(ui, 'x', LT_MOD_CTRL)) { lt_dtab_copy(app, st); lt_dtab_remove_sel(st); }
            if (lt_ui_key_pressed(ui, 'v', LT_MOD_CTRL)) { lt_dtab_paste(app, st); }
            if (lt_ui_key_pressed(ui, 'a', LT_MOD_CTRL)) { lt_dtab_select_all(st); }
            if (!cfg->lock) {
                if (lt_ui_key_pressed(ui, LT_KEY_LEFT, 0)) { nudge(app, st, -1, 0, 0); }
                if (lt_ui_key_pressed(ui, LT_KEY_RIGHT, 0)) { nudge(app, st, 1, 0, 0); }
                if (lt_ui_key_pressed(ui, LT_KEY_UP, 0)) { nudge(app, st, 0, -1, 0); }
                if (lt_ui_key_pressed(ui, LT_KEY_DOWN, 0)) { nudge(app, st, 0, 1, 0); }
                if (lt_ui_key_pressed(ui, LT_KEY_LEFT, LT_MOD_SHIFT)) { nudge(app, st, -1, 0, 1); }
                if (lt_ui_key_pressed(ui, LT_KEY_RIGHT, LT_MOD_SHIFT)) { nudge(app, st, 1, 0, 1); }
                if (lt_ui_key_pressed(ui, LT_KEY_UP, LT_MOD_SHIFT)) { nudge(app, st, 0, -1, 1); }
                if (lt_ui_key_pressed(ui, LT_KEY_DOWN, LT_MOD_SHIFT)) { nudge(app, st, 0, 1, 1); }
            }
            if (lt_ui_key_pressed(ui, LT_KEY_ESCAPE, 0)) { st->designer = 0; lt_dtab_clear_sel(st); }
        }
    } else if (!popup) {
        /* live view: click actions, double-click (gauge zoom / full screen) */
        if (down && !st->prev_down && in_area) {
            int h = lt_dash_view_hit(st->view, m.x, m.y);
            double dt = now - st->last_click_ms;
            st->down_ms = now; st->long_fired = 0; st->moved = 0;
            if (dt < 400.0 && abs(m.x - st->last_click_x) < 6 && abs(m.y - st->last_click_y) < 6) {
                if (h >= 0 && h == st->last_click_idx) { st->zoom_open = 1; st->zoom_idx = h; st->zoom_rect.w = 0; }
                else if (h < 0 && !kiosk) { (void)lt_dashx_set_kiosk(app, 1); }
                else if (h < 0 && kiosk) { (void)lt_dashx_set_kiosk(app, 0); }
                st->last_click_ms = -1000.0;
            } else {
                st->last_click_ms = now; st->last_click_x = m.x; st->last_click_y = m.y; st->last_click_idx = h;
            }
            st->press_idx = h;
        }
        if (down && in_area && !st->long_fired && st->press_idx >= 0 && (size_t)st->press_idx < st->file->cluster.n_comps &&
            now - st->down_ms > 700.0) {
            const lt_dash_comp *c = &st->file->cluster.comps[st->press_idx];
            const char *a = c->gauge ? c->gauge->long_click_action : c->indicator->long_click_action;
            st->long_fired = 1;
            if (a != NULL && a[0]) { run_action(app, a); }
        }
        if (!down && st->prev_down && !st->long_fired && st->press_idx >= 0 && (size_t)st->press_idx < st->file->cluster.n_comps &&
            lt_dash_view_hit(st->view, m.x, m.y) == st->press_idx && now - st->down_ms < 700.0 &&
            st->last_click_ms > 0.0) {
            const lt_dash_comp *c = &st->file->cluster.comps[st->press_idx];
            const char *a = c->gauge ? c->gauge->short_click_action : c->indicator->short_click_action;
            if (a != NULL && a[0]) { run_action(app, a); }
        }
        if (!down) { st->press_idx = -1; }
        if (lt_ui_key_pressed(ui, LT_KEY_ESCAPE, 0) && st->zoom_open) { st->zoom_open = 0; }
    }
    st->prev_down = down; st->prev_rdown = rdown;
    /* commit a pending change once the pointer is released and typing paused */
    if (st->changed && !down && lt_plat_now_ms() - st->change_ms > 350.0) {
        lt_dtab_flush(st);
    }

    if (!kiosk) { context_menu(app, st, area); }
    props_window(app, st);
    if (!kiosk) {
        lt_dpanel_validation(app, st);
        lt_dpanel_layers(app, st);
        lt_dpanel_palette(app, st);
        lt_dpanel_assets(app, st);
    }
    lt_dpanel_zoom(app, st);
    lt_dpanel_selector(app, st);
    if (st->changed) { lt_ui_request_redraw(ui); }
}

static void dash_destroy(void *p)
{
    dash_state *st = (dash_state *)p;
    if (st->view != NULL) { lt_dash_view_free(st->view); }
    if (st->file != NULL) { lt_dash_free(st->file); }
    lt_dash_hist_free(&st->hist);
    lt_dash_report_free(&st->rep);
    lt_dash_compat_free(&st->compat);
    lt_dash_compat_free(&st->imp_rep);
    lt_dashlib_free(&st->lib);
    lt_dashlib_free(&st->imp_lib);
    lt_expr_cache_free(&st->cache);
    lt_free(st->msel);
    lt_free(st->orig_all);
    lt_free(st->clip);
    lt_free(st->cache_px);
    lt_free(st);
}

static void dash_close(lt_app *app, lt_app_tab *tab)
{
    dash_state *st = (dash_state *)tab->data;
    if (st == NULL) { return; }
    if (lt_dashx_cfg(app)->kiosk) { (void)lt_dashx_set_kiosk(app, 0); }
    g_demo.st = NULL;
    tab->data = NULL;
    lt_ui_after_frame(app->ui, dash_destroy, st);   /* this frame's render callback reads it */
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

/* the kiosk frame draws the tab body itself */
void lt_dtab_draw_body(lt_app *app, lt_app_tab *tab, lt_rect r) { dash_draw(app, tab, r); }

/* settings changed: rebuild an INI-derived dashboard so indicator layout options apply */
void lt_dtab_cfg_changed(lt_app *app)
{
    dash_state *st = lt_dtab_state(app);
    if (st != NULL && st->file != NULL && st->src == DS_FRONT && st->path[0] == '\0' && !st->dirty) {
        lt_dtab_load_builtin(app, st, DS_FRONT);
    }
    if (st != NULL) { st->cache_w = 0; }
}
