/* app.c - application state: settings, project/tune/ECU lifecycle, registries.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "app_ext.h"
#include "feat.h"
#include "iniext.h"
#include "search_index.h"
#include "../project/inidb.h"

static const char *g_build_id = "dev";

void lt_app_set_build_id(const char *id)
{
    if (id != NULL && id[0] != '\0') { g_build_id = id; }
}

const char *lt_app_build_id(void)
{
    return g_build_id;
}

/* ------------------------------------------------------------------ */
/* messages                                                            */
void lt_app_set_status(lt_app *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    lt_vsnprintf(app->status, sizeof(app->status), fmt, ap);
    va_end(ap);
}

void lt_app_toast(lt_app *app, int kind, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    lt_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lt_app_set_status(app, "%s", buf);
    if (app->ui != NULL) { lt_ui_toast(app->ui, kind, buf); }
}

/* ------------------------------------------------------------------ */
/* settings                                                            */
static void settings_defaults(lt_app_settings *s)
{
    memset(s, 0, sizeof(*s));
    lt_strlcpy(s->theme, "dark", sizeof(s->theme));
    s->metric = 1;
    s->sidebar_w = 260;
    s->sidebar_on = 1;
    s->sidebar_cols = 0;
    s->show_ecu_menus = 1;
    s->scale_pct = 0;
    lt_strlcpy(s->backend, "auto", sizeof(s->backend));
    s->baud = 115200;
    lt_strlcpy(s->heatmap, "tunerstudio", sizeof(s->heatmap));
    s->poll_hz = 20;
    s->reconnect_cmd = 1;
    s->reconnect_fw = 1;
    s->auto_commit = 2;
    lt_strlcpy(s->commit_fmt, "Tune saved on {date} at {time}", sizeof(s->commit_fmt));
}

/* the form a project folder is remembered and compared in: absolute, "." and ".." resolved */
static void abs_dir(const char *dir, char *out, size_t cap)
{
    char *a = lt_plat_abspath(dir);
    lt_strlcpy(out, a != NULL ? a : dir, cap);
    lt_free(a);
}

static char *settings_path(lt_app *app)
{
    if (app->config_dir == NULL) { return NULL; }
    return lt_path_join(app->config_dir, "settings.conf");
}

void lt_app_settings_load(lt_app *app)
{
    lt_props p;
    char *path = settings_path(app);
    lt_app_settings *s = &app->settings;
    const char *v;
    int i, n;
    char key[24];
    settings_defaults(s);
    (void)lt_prefs_load(&s->prefs, app->config_dir);
    if (path == NULL) { return; }
    lt_props_init(&p);
    if (lt_file_exists(path) && lt_props_load(&p, path) == 0) {
        v = lt_props_get(&p, "theme");
        if (v != NULL) { lt_strlcpy(s->theme, v, sizeof(s->theme)); }
        lt_props_get_i32(&p, "units_metric", &s->metric);
        lt_props_get_i32(&p, "sidebar_w", &s->sidebar_w);
        lt_props_get_i32(&p, "sidebar_on", &s->sidebar_on);
        lt_props_get_i32(&p, "sidebar_cols", &s->sidebar_cols);
        lt_props_get_i32(&p, "show_ecu_menus", &s->show_ecu_menus);
        /* "ui_scale" is auto or a percentage chosen in Settings. The older "scale" key is
         * deliberately ignored: early builds wrote their fixed default there (1, later
         * migrated to 100), which is indistinguishable from a choice and would pin a
         * HiDPI display at 100% forever. */
        v = lt_props_get(&p, "ui_scale");
        if (v != NULL) { s->scale_pct = lt_stricmp(v, "auto") == 0 ? 0 : atoi(v); }
        lt_props_get_i32(&p, "win_w", &s->win_w);
        lt_props_get_i32(&p, "win_h", &s->win_h);
        lt_props_get_i32(&p, "win_max", &s->win_max);
        if (lt_props_get(&p, "win_x") != NULL && lt_props_get(&p, "win_y") != NULL) {
            lt_props_get_i32(&p, "win_x", &s->win_x);
            lt_props_get_i32(&p, "win_y", &s->win_y);
            s->win_pos = 1;
        }
        lt_props_get_i32(&p, "baud", &s->baud);
        lt_props_get_i32(&p, "auto_connect", &s->auto_connect);
        lt_props_get_i32(&p, "temp_unit", &s->temp_unit);
        lt_props_get_i32(&p, "press_unit", &s->press_unit);
        lt_props_get_i32(&p, "afr_unit", &s->afr_unit);
        lt_props_get_i32(&p, "poll_hz", &s->poll_hz);
        lt_props_get_i32(&p, "reconnect_cmd", &s->reconnect_cmd);
        lt_props_get_i32(&p, "reconnect_fw", &s->reconnect_fw);
        lt_props_get_i32(&p, "auto_commit", &s->auto_commit);
        v = lt_props_get(&p, "commit_fmt");
        if (v != NULL && v[0] != '\0') { lt_strlcpy(s->commit_fmt, v, sizeof(s->commit_fmt)); }
        v = lt_props_get(&p, "backend");
        if (v != NULL) { lt_strlcpy(s->backend, v, sizeof(s->backend)); }
        v = lt_props_get(&p, "last_project");
        if (v != NULL && v[0] != '\0') { abs_dir(v, s->last_project, sizeof(s->last_project)); }
        v = lt_props_get(&p, "port");
        if (v != NULL) { lt_strlcpy(s->port, v, sizeof(s->port)); }
        v = lt_props_get(&p, "heatmap");
        if (v != NULL) { lt_strlcpy(s->heatmap, v, sizeof(s->heatmap)); }
        /* older builds stored the paths as given: resolve them, and one project reached by two
         * spellings ("proj", "./proj/", "../x/proj") is listed once */
        n = 0;
        for (i = 0; i < 8; i++) {
            int k;
            lt_snprintf(key, sizeof(key), "recent%d", i);
            v = lt_props_get(&p, key);
            if (v == NULL || v[0] == '\0') { continue; }
            abs_dir(v, s->recent[n], sizeof(s->recent[0]));
            for (k = 0; k < n && !lt_plat_path_same(s->recent[k], s->recent[n]); k++) {}
            if (k == n) { n++; }
        }
        s->n_recent = n;
    }
    lt_props_free(&p);
    lt_free(path);
    s->sidebar_w = LT_CLAMP(s->sidebar_w, 180, 4000);     /* and at most half the window when drawn */
    s->sidebar_on = s->sidebar_on ? 1 : 0;
    if (s->sidebar_cols != 0) { s->sidebar_cols = LT_CLAMP(s->sidebar_cols, 14, 80); }
    if (s->scale_pct != 0) { s->scale_pct = LT_CLAMP(s->scale_pct, 100, 300); }
    if (s->win_w < 320 || s->win_h < 240 || s->win_w > 16384 || s->win_h > 16384) { s->win_w = s->win_h = 0; }
    s->win_max = s->win_max ? 1 : 0;
    if (s->win_x < -32000 || s->win_x > 32000 || s->win_y < -32000 || s->win_y > 32000) { s->win_pos = 0; }
    s->poll_hz = LT_CLAMP(s->poll_hz, 5, 100);
}

void lt_app_settings_save(lt_app *app)
{
    lt_props p;
    char *path = settings_path(app);
    lt_app_settings *s = &app->settings;
    char key[24], num[24];
    int i;
    if (path == NULL) { return; }
    (void)lt_prefs_save(&s->prefs, app->config_dir);
    lt_props_init(&p);
    lt_props_set(&p, "theme", s->theme);
    lt_snprintf(num, sizeof(num), "%d", s->metric); lt_props_set(&p, "units_metric", num);
    lt_snprintf(num, sizeof(num), "%d", s->sidebar_w); lt_props_set(&p, "sidebar_w", num);
    lt_snprintf(num, sizeof(num), "%d", s->sidebar_on); lt_props_set(&p, "sidebar_on", num);
    lt_snprintf(num, sizeof(num), "%d", s->sidebar_cols); lt_props_set(&p, "sidebar_cols", num);
    lt_snprintf(num, sizeof(num), "%d", s->show_ecu_menus); lt_props_set(&p, "show_ecu_menus", num);
    if (s->scale_pct == 0) { lt_props_set(&p, "ui_scale", "auto"); }
    else { lt_snprintf(num, sizeof(num), "%d", s->scale_pct); lt_props_set(&p, "ui_scale", num); }
    if (s->win_w > 0 && s->win_h > 0) {
        lt_snprintf(num, sizeof(num), "%d", s->win_w); lt_props_set(&p, "win_w", num);
        lt_snprintf(num, sizeof(num), "%d", s->win_h); lt_props_set(&p, "win_h", num);
    }
    if (s->win_pos) {
        lt_snprintf(num, sizeof(num), "%d", s->win_x); lt_props_set(&p, "win_x", num);
        lt_snprintf(num, sizeof(num), "%d", s->win_y); lt_props_set(&p, "win_y", num);
    }
    lt_snprintf(num, sizeof(num), "%d", s->win_max); lt_props_set(&p, "win_max", num);
    lt_snprintf(num, sizeof(num), "%d", s->baud); lt_props_set(&p, "baud", num);
    lt_snprintf(num, sizeof(num), "%d", s->auto_connect); lt_props_set(&p, "auto_connect", num);
    lt_snprintf(num, sizeof(num), "%d", s->temp_unit); lt_props_set(&p, "temp_unit", num);
    lt_snprintf(num, sizeof(num), "%d", s->press_unit); lt_props_set(&p, "press_unit", num);
    lt_snprintf(num, sizeof(num), "%d", s->afr_unit); lt_props_set(&p, "afr_unit", num);
    lt_snprintf(num, sizeof(num), "%d", s->poll_hz); lt_props_set(&p, "poll_hz", num);
    lt_snprintf(num, sizeof(num), "%d", s->reconnect_cmd); lt_props_set(&p, "reconnect_cmd", num);
    lt_snprintf(num, sizeof(num), "%d", s->reconnect_fw); lt_props_set(&p, "reconnect_fw", num);
    lt_snprintf(num, sizeof(num), "%d", s->auto_commit); lt_props_set(&p, "auto_commit", num);
    lt_props_set(&p, "commit_fmt", s->commit_fmt);
    lt_props_set(&p, "backend", s->backend);
    lt_props_set(&p, "last_project", s->last_project);
    lt_props_set(&p, "port", s->port);
    lt_props_set(&p, "heatmap", s->heatmap);
    for (i = 0; i < s->n_recent; i++) {
        lt_snprintf(key, sizeof(key), "recent%d", i);
        lt_props_set(&p, key, s->recent[i]);
    }
    lt_props_save(&p, path);
    lt_props_free(&p);
    lt_free(path);
}

void lt_app_apply_theme(lt_app *app, const char *name)
{
    if (lt_theme_preset(lt_ui_theme(app->ui), name) == 0) {
        lt_strlcpy(app->settings.theme, name, sizeof(app->settings.theme));
        lt_ui_theme_changed(app->ui);
    }
}

void lt_app_recent_add(lt_app *app, const char *dir_in)
{
    lt_app_settings *s = &app->settings;
    char dir[sizeof(s->recent[0])];
    int i, j;
    abs_dir(dir_in, dir, sizeof(dir));
    for (i = 0; i < s->n_recent; i++) {
        if (lt_plat_path_same(s->recent[i], dir)) { break; }
    }
    if (i == s->n_recent) {
        if (s->n_recent < 8) { s->n_recent++; }
        i = s->n_recent - 1;
    }
    for (j = i; j > 0; j--) {
        lt_strlcpy(s->recent[j], s->recent[j - 1], sizeof(s->recent[0]));
    }
    lt_strlcpy(s->recent[0], dir, sizeof(s->recent[0]));
    lt_strlcpy(s->last_project, dir, sizeof(s->last_project));
    lt_app_settings_save(app);
}

/* ------------------------------------------------------------------ */
/* registries                                                          */
void lt_app_register_tab_kind(lt_app *app, const lt_app_tab_kind *k)
{
    int i;
    for (i = 0; i < app->n_tab_kinds; i++) {
        if (strcmp(app->tab_kinds[i].kind, k->kind) == 0) { app->tab_kinds[i] = *k; return; }
    }
    if (app->n_tab_kinds == app->cap_tab_kinds) {
        app->cap_tab_kinds = app->cap_tab_kinds ? app->cap_tab_kinds * 2 : 16;
        app->tab_kinds = (lt_app_tab_kind *)lt_realloc(app->tab_kinds,
                             sizeof(lt_app_tab_kind) * (size_t)app->cap_tab_kinds);
    }
    app->tab_kinds[app->n_tab_kinds++] = *k;
}

void lt_app_register_dialog_kind(lt_app *app, const lt_app_dialog_kind *k)
{
    int i;
    for (i = 0; i < app->n_dlg_kinds; i++) {
        if (strcmp(app->dlg_kinds[i].kind, k->kind) == 0) { app->dlg_kinds[i] = *k; return; }
    }
    if (app->n_dlg_kinds == app->cap_dlg_kinds) {
        app->cap_dlg_kinds = app->cap_dlg_kinds ? app->cap_dlg_kinds * 2 : 16;
        app->dlg_kinds = (lt_app_dialog_kind *)lt_realloc(app->dlg_kinds,
                             sizeof(lt_app_dialog_kind) * (size_t)app->cap_dlg_kinds);
    }
    app->dlg_kinds[app->n_dlg_kinds++] = *k;
}

lt_app_tab *lt_app_tab_find(lt_app *app, const char *kind, const char *target)
{
    int i;
    for (i = 0; i < app->n_tabs; i++) {
        if (strcmp(app->tabs[i]->kind, kind) == 0 && strcmp(app->tabs[i]->target, target) == 0) {
            return app->tabs[i];
        }
    }
    return NULL;
}

lt_app_tab *lt_app_tab_active(lt_app *app)
{
    if (app->active_tab < 0 || app->active_tab >= app->n_tabs) { return NULL; }
    return app->tabs[app->active_tab];
}

lt_app_tab *lt_app_tab_open(lt_app *app, const char *kind, const char *target,
                            const char *title, int closable)
{
    lt_app_tab *t;
    int i, known = 0, at;
    for (i = 0; i < app->n_tab_kinds; i++) {
        if (strcmp(app->tab_kinds[i].kind, kind) == 0) { known = 1; }
    }
    if (!known) { return NULL; }
    t = lt_app_tab_find(app, kind, target);
    if (t != NULL) {
        for (i = 0; i < app->n_tabs; i++) {
            if (app->tabs[i] == t) { app->active_tab = i; }
        }
        return t;
    }
    t = (lt_app_tab *)lt_calloc(1, sizeof(*t));
    lt_strlcpy(t->kind, kind, sizeof(t->kind));
    lt_strlcpy(t->target, target, sizeof(t->target));
    lt_strlcpy(t->title, title != NULL ? title : target, sizeof(t->title));
    t->closable = closable;
    if (app->n_tabs == app->cap_tabs) {
        app->cap_tabs = app->cap_tabs ? app->cap_tabs * 2 : 16;
        app->tabs = (lt_app_tab **)lt_realloc(app->tabs, sizeof(lt_app_tab *) * (size_t)app->cap_tabs);
    }
    at = app->n_tabs;
    if (!closable) { at = 0; }           /* pinned tabs (Dashboard) go first */
    for (i = app->n_tabs; i > at; i--) { app->tabs[i] = app->tabs[i - 1]; }
    app->tabs[at] = t;
    app->n_tabs++;
    app->active_tab = at;
    return t;
}

void lt_app_tab_close(lt_app *app, lt_app_tab *tab)
{
    int i, idx = -1, k;
    for (i = 0; i < app->n_tabs; i++) {
        if (app->tabs[i] == tab) { idx = i; }
    }
    if (idx < 0) { return; }
    /* this frame may already have recorded draw callbacks that point into the tab's state */
    if (app->ui != NULL) { lt_ui_discard_frame(app->ui); }
    for (k = 0; k < app->n_tab_kinds; k++) {
        if (strcmp(app->tab_kinds[k].kind, tab->kind) == 0 && app->tab_kinds[k].close != NULL) {
            app->tab_kinds[k].close(app, tab);
        }
    }
    for (i = idx; i < app->n_tabs - 1; i++) { app->tabs[i] = app->tabs[i + 1]; }
    app->n_tabs--;
    lt_free(tab);
    if (app->active_tab > idx || app->active_tab >= app->n_tabs) { app->active_tab--; }
    if (app->active_tab < 0) { app->active_tab = app->n_tabs > 0 ? 0 : -1; }
}

lt_app_dialog *lt_app_dialog_push(lt_app *app, const char *kind, const char *target,
                                  const char *title)
{
    lt_app_dialog *d;
    int i, known = 0;
    for (i = 0; i < app->n_dlg_kinds; i++) {
        if (strcmp(app->dlg_kinds[i].kind, kind) == 0) { known = 1; }
    }
    if (!known) { return NULL; }
    for (i = 0; i < app->n_dialogs; i++) {      /* one instance per kind+target */
        if (strcmp(app->dialogs[i]->kind, kind) == 0 &&
            strcmp(app->dialogs[i]->target, target != NULL ? target : "") == 0) {
            return app->dialogs[i];
        }
    }
    d = (lt_app_dialog *)lt_calloc(1, sizeof(*d));
    lt_strlcpy(d->kind, kind, sizeof(d->kind));
    lt_strlcpy(d->target, target != NULL ? target : "", sizeof(d->target));
    lt_strlcpy(d->title, title != NULL ? title : kind, sizeof(d->title));
    d->open = 1;
    if (app->n_dialogs == app->cap_dialogs) {
        app->cap_dialogs = app->cap_dialogs ? app->cap_dialogs * 2 : 8;
        app->dialogs = (lt_app_dialog **)lt_realloc(app->dialogs,
                           sizeof(lt_app_dialog *) * (size_t)app->cap_dialogs);
    }
    app->dialogs[app->n_dialogs++] = d;
    return d;
}

void lt_app_placeholder_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    const lt_theme *t = lt_ui_theme(app->ui);
    int w1 = -1;
    char buf[200];
    LT_UNUSED(r);
    lt_ui_layout_row(app->ui, 1, &w1, 0);
    lt_ui_heading(app->ui, tab->title);
    lt_snprintf(buf, sizeof(buf), "%s: %s", tab->kind, tab->target);
    lt_ui_label_ex(app->ui, buf, t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(app->ui, "This screen is not implemented yet.", t->muted, LT_UI_ALIGN_LEFT);
}

void lt_app_file_dialog(lt_app *app, int purpose, int mode, const char *dir,
                        const char *name, const char *filters)
{
    app->fd_purpose = purpose;
    lt_ui_filedlg_open(&app->fd, mode, dir, name != NULL ? name : "", filters);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
lt_app *lt_app_new(void)
{
    lt_app *app = (lt_app *)lt_calloc(1, sizeof(*app));
    lt_expr_cache_init(&app->expr);
    lt_buf_init(&app->rtbuf);
    lt_ui_filedlg_init(&app->fd);
    app->active_tab = -1;
    app->confirm_delete = -1;
    app->config_dir = lt_plat_config_dir();
    settings_defaults(&app->settings);
    lt_app_settings_load(app);
    lt_app_register_tables(app);
    lt_app_register_dash(app);
    lt_app_register_dialogs(app);
    lt_ext_new(app);
    return app;
}

void lt_app_free(lt_app *app)
{
    int i;
    if (app == NULL) { return; }
    lt_app_close_project(app);
    for (i = app->n_dialogs - 1; i >= 0; i--) { lt_free(app->dialogs[i]); }
    lt_free(app->dialogs);
    lt_free(app->tabs);
    lt_ext_free(app);
    lt_free(app->tab_kinds);
    lt_free(app->dlg_kinds);
    lt_ui_filedlg_free(&app->fd);
    lt_expr_cache_free(&app->expr);
    lt_buf_free(&app->rtbuf);
    if (app->ui != NULL) { lt_ui_destroy(app->ui); }
    if (app->cell_mode) { lt_cells_free(&app->cells); } else { lt_canvas_free(&app->canvas); }
    lt_diff_free(&app->diff);
    if (app->backend != NULL) { lt_backend_destroy(app->backend); }
    lt_free(app->config_dir);
    lt_free(app);
}

/* ------------------------------------------------------------------ */
/* pages, expression environment                                       */
lt_pagesrc lt_app_pages(lt_app *app)
{
    lt_pagesrc z;
    if (app->mem != NULL && (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO)) {
        return lt_ecu_mem_pagesrc(app->mem);
    }
    if (app->tune != NULL) { return lt_tune_pagesrc(app->tune); }
    memset(&z, 0, sizeof(z));
    return z;
}

static void rebind(lt_app *app)
{
    lt_pagesrc ps = lt_app_pages(app);
    if (app->ini == NULL) { return; }
    lt_varenv_init(&app->varenv, app->ini, &ps, app->rt, app->tune);
    app->varenv.online = (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO);
    if (app->rt != NULL) { lt_ini_rt_set_parent(app->rt, &app->varenv.consts); }
    lt_ini_resolve_dynamic(app->ini, &app->varenv.env);
}

lt_bool lt_app_channel(lt_app *app, const char *name, double *out)
{
    if (app->rt == NULL) { return LT_FALSE; }
    return lt_ini_rt_get(app->rt, name, out);
}

/* Unit converter for a channel (native INI unit -> preferred unit); identity when unknown. */
void lt_app_channel_conv(const lt_app *app, const char *name, lt_uconv *c)
{
    const lt_ini_channel *ch = app->ini != NULL && name != NULL ? lt_ini_find_channel(app->ini, name) : NULL;
    lt_uconv_make(c, ch != NULL ? ch->units : NULL);
}

lt_bool lt_app_channel_disp(lt_app *app, const char *name, double *out, const char **label)
{
    lt_uconv c;
    const lt_ini_channel *ch;
    double v = 0.0;
    if (!lt_app_channel(app, name, &v)) { return LT_FALSE; }
    lt_app_channel_conv(app, name, &c);
    ch = app->ini != NULL ? lt_ini_find_channel(app->ini, name) : NULL;
    if (out != NULL) { *out = lt_uconv_to_disp(&c, v); }
    if (label != NULL) { *label = lt_uconv_label(&c, ch != NULL && ch->units != NULL ? ch->units : ""); }
    return LT_TRUE;
}

/* ------------------------------------------------------------------ */
/* recorder                                                            */
static void rec_free(lt_app *app)
{
    if (app->rec != NULL) { lt_rec_free(app->rec); app->rec = NULL; }
    if (app->rec_names != NULL) { lt_strv_free(app->rec_names, app->rec_n); app->rec_names = NULL; }
    lt_free(app->rec_vals);
    app->rec_vals = NULL;
    app->rec_n = 0;
}

int lt_app_log_toggle(lt_app *app)
{
    char *dir, *path;
    char **units = NULL;
    size_t n, i;
    if (app->ini == NULL) { return -1; }
    if (app->rec == NULL) {
        n = lt_datalog_channels_from_ini(app->ini, &app->rec_names, &units);
        if (n == 0) {
            lt_strv_free(app->rec_names, 0);
            app->rec_names = NULL;
            lt_strv_free(units, 0);
            lt_app_toast(app, 2, "No channels to log");
            return -1;
        }
        app->rec_n = n;
        app->rec = lt_rec_new((const char *const *)app->rec_names, (const char *const *)units, (int)n);
        app->rec_vals = (double *)lt_calloc(n, sizeof(double));
        lt_strv_free(units, n);
    }
    if (lt_rec_is_recording(app->rec)) {
        lt_rec_stop(app->rec);
        app->log_files_gen++;
        lt_app_toast(app, 0, "Logging stopped (%ld rows)", lt_rec_count(app->rec));
        return 0;
    }
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) {
        lt_app_toast(app, 2, "Connect to an ECU to log data");
        return -1;
    }
    dir = app->has_project ? lt_project_file(&app->project, "datalogs")
                           : lt_path_join(app->config_dir != NULL ? app->config_dir : ".", "datalogs");
    lt_plat_mkdirs(dir);
    lt_feat_logging_apply(app);
    path = lt_rec_new_path(app->rec, dir);
    i = (size_t)lt_rec_start(app->rec, path);
    if ((int)i < 0) { lt_app_toast(app, 3, "Cannot start log: %s", lt_err_get()); }
    else {
        app->log_files_gen++;
        lt_app_toast(app, 1, "Logging to %s", lt_path_base(lt_rec_path(app->rec)));
    }
    lt_free(path);
    lt_free(dir);
    return (int)i < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* ECU link                                                            */
const char *lt_app_conn_label(const lt_app *app)
{
    switch (app->conn) {
    case LT_APP_ONLINE: return "Connected";
    case LT_APP_DEMO: return "Demo";
    case LT_APP_CONNECTING: return "Connecting...";
    default: return "Disconnected";
    }
}

static void link_free(lt_app *app)
{
    if (app->proto != NULL) {
        lt_proto_close(app->proto);
        lt_proto_free(app->proto);
        app->proto = NULL;
    }
    if (app->simlink != NULL) { lt_simlink_free(app->simlink); app->simlink = NULL; }
    if (app->sim != NULL) { lt_ecusim_free(app->sim); app->sim = NULL; }
    app->link_stream = NULL;
    if (app->mem != NULL) { lt_ecu_mem_free(app->mem); app->mem = NULL; }
}

void lt_app_disconnect(lt_app *app)
{
    if (app->conn == LT_APP_OFFLINE && app->proto == NULL) { return; }
    if (app->rec != NULL && lt_rec_is_recording(app->rec)) { lt_rec_stop(app->rec); app->log_files_gen++; }
    if (app->proto != NULL && lt_proto_state(app->proto) == LT_CONN_CONNECTED &&
        lt_proto_write_pending(app->proto)) {
        lt_proto_async_finish(app->proto);         /* edits made just before still reach the ECU */
    }
    if (app->mem != NULL && app->tune != NULL && !app->keep_project_tune) {
        lt_tune_from_mem(app->tune, app->mem);
    }
    app->keep_project_tune = 0;
    link_free(app);
    app->conn = LT_APP_OFFLINE;
    app->signature[0] = '\0';
    app->data_rate = 0.0;
    rebind(app);
    lt_app_set_status(app, "Disconnected");
}

/* In-process simulator: answers are immediate, so serial pacing sleeps
 * (inter-write delays, transmit-time waits) only add latency. */
static double sim_clock_now(void *ud) { LT_UNUSED(ud); return lt_plat_now_ms(); }
static void sim_clock_sleep(void *ud, int ms) { LT_UNUSED(ud); LT_UNUSED(ms); }

static int connect_io(lt_app *app, const lt_proto_io *io, const char *port, long baud)
{
    lt_proto_cfg cfg;
    int rc;
    lt_proto_cfg_default(&cfg);
    cfg.baud = baud;
    cfg.port_name = port;
    cfg.runtime_mode = (lt_runtime_mode)app->settings.prefs.packet_mode;
    cfg.adaptive_timing = app->settings.prefs.adaptive_timing ? LT_TRUE : LT_FALSE;
    cfg.instant_link = strcmp(port, "demo") == 0 ? LT_TRUE : LT_FALSE;
    app->proto = lt_proto_new(io, app->ini, &cfg);
    if (strcmp(port, "demo") == 0) {
        lt_proto_clock clk;
        clk.ud = NULL;
        clk.now_ms = sim_clock_now;
        clk.sleep_ms = sim_clock_sleep;
        lt_proto_set_clock(app->proto, &clk);
    }
    app->conn = LT_APP_CONNECTING;
    rc = lt_proto_connect(app->proto);
    if (rc < 0) {
        lt_app_toast(app, 3, "Connect failed: %s", lt_proto_errinfo(app->proto)->msg);
        lt_feat_error_proto(app, "Connection Failed");
        link_free(app);
        app->conn = LT_APP_OFFLINE;
        return rc;
    }
    app->mem = lt_ecu_mem_from_ini(app->ini);
    rc = lt_proto_read_all(app->proto, app->mem, LT_TRUE);
    if (rc < 0) {
        lt_app_toast(app, 3, "Reading ECU failed: %s", lt_err_get());
        lt_feat_error_proto(app, "Reading the ECU Failed");
        link_free(app);
        app->conn = LT_APP_OFFLINE;
        return rc;
    }
    lt_strlcpy(app->signature, lt_proto_signature(app->proto), sizeof(app->signature));
    /* with a project the on-connect hook (feat_sync.c) decides which tune wins */
    if (app->tune != NULL && !app->has_project) { lt_tune_from_mem(app->tune, app->mem); }
    app->last_poll_ms = 0.0;
    app->rate_t0_ms = lt_plat_now_ms();
    app->rate_frames = 0;
    app->frames_ok = app->frames_bad = 0;
    return 0;
}

static long env_long(const char *name, long dflt)
{
    char *e = lt_plat_getenv(name);
    long v = dflt;
    if (e != NULL && e[0] != '\0') { v = atol(e); }
    lt_free(e);
    return v;
}

int lt_app_connect_demo(lt_app *app)
{
    lt_proto_io io;
    int rc;
    long baud = env_long("LT_DEMO_BAUD", 0L);
    if (app->ini == NULL) { lt_app_toast(app, 3, "No ECU definition loaded"); return -1; }
    lt_app_disconnect(app);
    app->sim = lt_ecusim_new(app->ini);
    io = lt_ecusim_io(app->sim);
    if (baud > 0) {
        /* LT_DEMO_BAUD=115200 [LT_DEMO_LATENCY=ms] [LT_DEMO_CHUNK=bytes]: the simulator behind
         * a modelled serial link, with real protocol pacing, so the demo waits like a real ECU */
        lt_simlink_cfg lc;
        lc.baud = baud;
        lc.latency_ms = (double)env_long("LT_DEMO_LATENCY", 4L);
        lc.chunk = (int)env_long("LT_DEMO_CHUNK", 64L);
        app->simlink = lt_simlink_new(&io, &lc);
        io = lt_simlink_io(app->simlink);
        rc = connect_io(app, &io, "demo-link", baud);
    } else {
        rc = connect_io(app, &io, "demo", 115200L);
    }
    if (rc < 0) { return rc; }
    app->conn = LT_APP_DEMO;
    rebind(app);
    lt_app_toast(app, 1, "Demo mode: simulated ECU connected");
    lt_ext_on_connect(app);
    return 0;
}

int lt_app_connect_serial(lt_app *app, const char *port, long baud)
{
    lt_stream *s;
    lt_proto_io io;
    int rc;
    if (app->ini == NULL) { lt_app_toast(app, 3, "No ECU definition loaded"); return -1; }
    lt_app_disconnect(app);
    if (strncmp(port, "tcp:", 4) == 0) {
        char host[128];
        char *colon;
        lt_strlcpy(host, port + 4, sizeof(host));
        colon = strchr(host, ':');
        if (colon != NULL) { *colon = '\0'; }
        s = lt_tcp_connect(host, colon != NULL ? atoi(colon + 1) : LT_DEFAULT_TCP_PORT, 3000);
    } else {
        s = lt_serial_open(port, baud);
    }
    if (s == NULL) { lt_app_toast(app, 3, "Cannot open %s: %s", port, lt_err_get()); return -1; }
    app->link_stream = s;
    io = lt_proto_io_from_stream(s, LT_TRUE);
    rc = connect_io(app, &io, port, baud);
    if (rc < 0) { return rc; }
    app->conn = LT_APP_ONLINE;
    lt_strlcpy(app->settings.port, port, sizeof(app->settings.port));
    app->settings.baud = (int)baud;
    lt_app_settings_save(app);
    rebind(app);
    lt_app_toast(app, 1, "Connected to %s", port);
    lt_ext_on_connect(app);
    return 0;
}

lt_bool lt_app_can_burn(const lt_app *app)
{
    if (app->mem == NULL || app->proto == NULL) { return LT_FALSE; }
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) { return LT_FALSE; }
    return (lt_ecu_mem_has_pending_burn(app->mem) || lt_ecu_mem_has_dirty(app->mem) ||
            lt_proto_write_pending(app->proto)) ? LT_TRUE : LT_FALSE;
}

/* The changed ranges are queued, not written here: lt_proto sends them between realtime
 * requests (merging ranges that are edited again before they went out) and lt_app_service
 * reports a failure. Any blocking ECU call (burn, page read, controller command) completes
 * the queued writes first, so they can never overtake or interleave with it. */
void lt_app_edited(lt_app *app)
{
    app->modified = 1;
    if (app->mem != NULL && app->proto != NULL &&
        (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) && lt_ecu_mem_has_dirty(app->mem)) {
        if (lt_proto_write_dirty_begin(app->proto, app->mem) < 0) {
            lt_app_toast(app, 3, "ECU write failed: %s", lt_err_get());
        }
    }
}

/* A write or burn stopped part way (error, timeout, link drop): say exactly what the ECU may
 * now hold, with the protocol details. burned/total < 0 = the failure was in the RAM write. */
static void burn_failed(lt_app *app, const char *what, int burned, int total)
{
    char msg[512], det[1024];
    const lt_proto_err *pe = lt_proto_errinfo(app->proto);
    if (burned < 0) {
        lt_snprintf(msg, sizeof(msg), "%s: %s. The ECU RAM may hold only part of the changes; "
                    "nothing was burned to flash. Check the connection and write again before "
                    "burning. The tune file on disk was not changed.", what,
                    pe->msg[0] != '\0' ? pe->msg : lt_err_get());
    } else {
        lt_snprintf(msg, sizeof(msg), "%s: %s. The ECU may hold a PARTIAL BURN: %d of %d page(s) "
                    "were burned before the error. Reconnect and burn again before "
                    "turning the ECU off. The tune file on disk was not changed.", what,
                    pe->msg[0] != '\0' ? pe->msg : lt_err_get(), burned, total);
    }
    lt_feat_error_describe(pe, det, sizeof(det));
    lt_app_toast(app, 3, "%s", burned < 0 ? "ECU write failed: the ECU RAM may hold a partial update"
                                          : "Burn interrupted: the ECU may hold a partial burn");
    lt_feat_error_show(app, burned < 0 ? "ECU Write Failed" : "Burn Interrupted", msg, det);
}

int lt_app_burn(lt_app *app)
{
    int rc, before, after;
    char *rp = NULL;
    if (!lt_app_can_burn(app)) { return -1; }
    if (!lt_app_burn_gate(app)) { return -1; }     /* unacknowledged pin conflicts */
    lt_proto_async_finish(app->proto);             /* queued edit writes first */
    if (lt_proto_write_pending(app->proto) && lt_proto_write_pump(app->proto) < 0) {
        burn_failed(app, "ECU write failed", -1, -1);
        return -1;
    }
    if (lt_ecu_mem_has_dirty(app->mem) && lt_proto_write_dirty(app->proto, app->mem) < 0) {
        burn_failed(app, "ECU write failed", -1, -1);
        return -1;
    }
    if (app->has_project && app->project.tune != NULL) {
        lt_tune_from_mem(app->tune, app->mem);
        if (lt_restore_create(&app->project, &rp) == 0) { lt_free(rp); }
    }
    before = (int)lt_proto_dirty_pages(app->proto, NULL, 0);
    rc = lt_proto_burn(app->proto, -1, app->mem);
    if (rc < 0) {
        after = (int)lt_proto_dirty_pages(app->proto, NULL, 0);
        /* no written page recorded: lt_proto_burn burns page 0 alone */
        burn_failed(app, "Burn failed", before > after ? before - after : 0, before > 0 ? before : 1);
        return rc;
    }
    lt_app_toast(app, 1, "Burned to ECU flash");
    (void)lt_app_pc_after_burn(app);              /* "power cycle the ECU" notice */
    if (app->has_project) { lt_app_save_tune(app); }
    return 0;
}

/* LT_PERF=1: once a second on stderr, how long the ECU service kept the UI thread (avg/max per
 * call and in total) and how many realtime frames arrived. */
typedef struct link_perf {
    int init, on;
    double t0, sum, max;
    long calls, frames;
} link_perf;
static link_perf g_link_perf;

static void link_perf_add(lt_app *app, double t_in, double t_out, int got)
{
    link_perf *lp = &g_link_perf;
    double d = t_out - t_in;
    if (!lp->init) {
        char *e = lt_plat_getenv("LT_PERF");
        lp->init = 1;
        lp->on = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
        lt_free(e);
        lp->t0 = t_out;
    }
    if (!lp->on) { return; }
    lp->sum += d;
    if (d > lp->max) { lp->max = d; }
    lp->calls++;
    if (got > 0) { lp->frames++; }
    if (t_out - lp->t0 >= 1000.0) {
        if (!app->headless) {
            fprintf(stderr, "perf: ecu %ld frames/s, service %ld calls avg %.3f ms max %.2f ms, "
                    "UI thread in ECU service %.0f ms/s\n", lp->frames, lp->calls,
                    lp->calls > 0 ? lp->sum / (double)lp->calls : 0.0, lp->max,
                    lp->sum * 1000.0 / (t_out - lp->t0));
        }
        lp->t0 = t_out; lp->sum = 0.0; lp->max = 0.0; lp->calls = 0; lp->frames = 0;
    }
}

static int service_link(lt_app *app);

int lt_app_service(lt_app *app)
{
    double t0;
    int got;
    lt_ext_tick(app);
    if ((app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) || app->proto == NULL) { return 0; }
    t0 = lt_plat_now_ms();
    got = service_link(app);
    link_perf_add(app, t0, lt_plat_now_ms(), got);
    return got;
}

/* the demo simulator runs in-process (no link to saturate): poll it at display rate */
static double poll_period_ms(const lt_app *app)
{
    return 1000.0 / ((app->conn == LT_APP_DEMO && app->simlink == NULL)
                     ? 60.0 : (double)LT_CLAMP(app->settings.poll_hz, 5, 100));
}

/* A realtime request finished: rc 1 = new frame in rtbuf, < 0 = failed. May disconnect. */
static int rt_finished(lt_app *app, int rc, double now)
{
    if (rc < 0) {
        app->frames_bad++;
        if (lt_proto_state(app->proto) != LT_CONN_CONNECTED) {
            lt_ext_on_link_lost(app);
            lt_app_disconnect(app);
            lt_app_toast(app, 3, "ECU connection lost");
        }
        return 1;
    }
    if (app->rt != NULL) {
        lt_ini_rt_update(app->rt, (const lt_u8 *)app->rtbuf.data, app->rtbuf.len);
    }
    app->frames_ok++;
    app->rate_frames++;
    lt_app_tables_service(app);
    if (now - app->rate_t0_ms >= 1000.0) {
        app->data_rate = app->rate_frames * 1000.0 / (now - app->rate_t0_ms);
        app->rate_t0_ms = now;
        app->rate_frames = 0;
    }
    if (app->rec != NULL && lt_rec_is_recording(app->rec) && app->rt != NULL) {
        size_t i;
        for (i = 0; i < app->rec_n; i++) {
            double v = 0.0;
            lt_ini_rt_get(app->rt, app->rec_names[i], &v);
            app->rec_vals[i] = v;
        }
        if (lt_rec_record(app->rec, app->rec_vals, (int)app->rec_n) == LT_EIO) {
            lt_app_toast(app, 3, "Logging stopped: cannot write %s (disk full or removed?); %ld rows kept",
                         lt_path_base(lt_rec_path(app->rec)), lt_rec_count(app->rec));
        }
    }
    return 1;
}

/* Never waits for the ECU: collects what has arrived, queues the next request when it is due
 * and the wire is free, and returns. Realtime requests and the RAM writes of table edits
 * (queued by lt_app_edited) take turns on the wire inside lt_proto. */
static int service_link(lt_app *app)
{
    double now = lt_plat_now_ms();
    int rc, got = 0;
    if (lt_proto_write_pending(app->proto)) {
        rc = lt_proto_write_pump(app->proto);
        if (rc < 0) { lt_app_toast(app, 3, "ECU write failed: %s", lt_err_get()); }
    }
    if (lt_proto_rt_pending(app->proto)) {
        lt_buf_clear(&app->rtbuf);
        rc = lt_proto_rt_pump(app->proto, &app->rtbuf);
        if (rc != 0) {
            got = rt_finished(app, rc, now);
            if (app->proto == NULL) { return got; }
        }
    }
    if (!lt_proto_rt_pending(app->proto) && now - app->last_poll_ms >= poll_period_ms(app)) {
        rc = lt_proto_rt_begin(app->proto);
        if (rc == LT_OK) {
            app->last_poll_ms = now;
            lt_buf_clear(&app->rtbuf);
            rc = lt_proto_rt_pump(app->proto, &app->rtbuf);   /* in-process links answer at once */
            if (rc != 0) { got = rt_finished(app, rc, now); }
        } else if (rc != LT_EBUSY) {                            /* LT_EBUSY: the wire is busy */
            app->last_poll_ms = now;
            got = rt_finished(app, rc, now);
        }
    }
    return got;
}

int lt_app_service_wait_ms(lt_app *app)
{
    double left;
    int w;
    if ((app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) || app->proto == NULL) { return -1; }
    w = lt_proto_async_wait_ms(app->proto);       /* reply bytes expected / pacing gap */
    if (!lt_proto_rt_pending(app->proto)) {
        left = app->last_poll_ms + poll_period_ms(app) - lt_plat_now_ms();
        if (left > 0.0) {
            int d = (int)left + 1;
            if (w < 0 || d < w) { w = d; }
        } else if (w < 0) {
            w = 0;                                /* a request is due and the wire is free */
        }
    }
    /* feature ticks (lt_ext_tick) run from the service: keep them going while connected */
    return (w < 0 || w > 50) ? 50 : w;
}

/* ------------------------------------------------------------------ */
/* project / ini / tune                                                */
static void ini_free_later(void *p) { lt_ini_free((lt_ini *)p); }

void lt_app_close_project(lt_app *app)
{
    int i;
    /* called from a menu or dialog: what this frame drew of the tabs and dialogs is freed below */
    lt_ui_forget_custom(app->ui);
    lt_app_disconnect(app);
    rec_free(app);
    while (app->n_tabs > 0) { lt_app_tab_close(app, app->tabs[app->n_tabs - 1]); }
    app->active_tab = -1;
    for (i = app->n_dialogs - 1; i >= 0; i--) {
        lt_app_dialog *d = app->dialogs[i];
        int k;
        for (k = 0; k < app->n_dlg_kinds; k++) {
            if (strcmp(app->dlg_kinds[k].kind, d->kind) == 0 && app->dlg_kinds[k].close != NULL) {
                app->dlg_kinds[k].close(app, d);
            }
        }
        lt_free(d);
    }
    app->n_dialogs = 0;
    app->dlg_gen++;
    if (app->rt != NULL) { lt_ini_rt_free(app->rt); app->rt = NULL; }
    if (app->has_project) {
        lt_project_close(&app->project);
        app->has_project = 0;
        app->tune = NULL;
    } else if (app->tune != NULL) {
        lt_tune_free(app->tune);
        app->tune = NULL;
    }
    /* tabs drawn this frame may have queued draw callbacks that still read the definition */
    if (app->ini != NULL) { lt_ui_after_frame(app->ui, ini_free_later, app->ini); app->ini = NULL; }
    lt_sidx_free(app->sidx);
    app->sidx = NULL;
    app->reveal.comp = NULL;
    lt_expr_cache_free(&app->expr);
    lt_expr_cache_init(&app->expr);
    app->modified = 0;
    app->ini_path[0] = '\0';
    app->search[0] = '\0';
}

/* take ownership of ini + tune and bring the session up */
static void session_begin(lt_app *app, lt_ini *ini, lt_tune *tune)
{
    app->ini = ini;
    app->tune = tune;
    if (tune->pages[0] == NULL) { lt_tune_apply_to_pages(tune, ini); }
    app->rt = lt_ini_rt_new(ini);
    app->modified = 0;
    rebind(app);
    lt_app_dashboard_tab_open(app);
}

int lt_app_open_ini(lt_app *app, const char *ini_path)
{
    lt_ini *ini = lt_ini_load_file(ini_path, NULL);
    lt_tune *t;
    if (ini == NULL) { lt_app_toast(app, 3, "Cannot load INI: %s", lt_err_get()); return -1; }
    lt_app_close_project(app);
    lt_strlcpy(app->ini_path, ini_path, sizeof(app->ini_path));
    t = lt_tune_from_defaults(ini);
    session_begin(app, ini, t);
    return 0;
}

/* CurrentTune.msq exists but cannot be read (corrupt or cut off): the project opens with the
 * INI defaults, so move the file aside first - otherwise the next save would replace the
 * user's tune with defaults - and say so. */
static void keep_unreadable_tune(lt_app *app)
{
    char *cur = lt_path_join(app->project.path, "CurrentTune.msq");
    char name[64], why[256], msg[1024];
    char *dst = NULL;
    int k;
    if (!lt_file_exists(cur) || lt_project_load_tune(&app->project) == 0) {
        lt_free(cur);
        return;
    }
    lt_strlcpy(why, lt_err_get(), sizeof(why));
    for (k = 1; k < 1000; k++) {
        if (k == 1) { lt_strlcpy(name, "CurrentTune.unreadable.msq", sizeof(name)); }
        else { lt_snprintf(name, sizeof(name), "CurrentTune.unreadable-%d.msq", k); }
        lt_free(dst);
        dst = lt_path_join(app->project.path, name);
        if (!lt_file_exists(dst)) { break; }
    }
    if (lt_plat_rename(cur, dst) == 0) {
        lt_snprintf(msg, sizeof(msg), "The project tune could not be read (%s). It was kept as %s "
                    "and the project opened with the default values of the INI. Load a restore "
                    "point or a saved tune before burning.", why, name);
    } else {
        lt_snprintf(msg, sizeof(msg), "The project tune could not be read (%s) and could not be "
                    "moved aside. The project opened with the default values of the INI: do not "
                    "save before you have copied %s somewhere safe.", why, cur);
    }
    lt_app_toast(app, 3, "Project tune unreadable: opened with defaults");
    lt_feat_error_show(app, "Tune Not Loaded", msg, "");
    lt_free(dst);
    lt_free(cur);
}

/* project.json of dir could not be read: say why in plain words, with the way out (feat_projerr.c) */
static void project_json_problem(lt_app *app, const char *dir)
{
    char msg[700], why[400];
    char *f = lt_path_join(dir, "project.json"), *text;
    const char *file = f, *w = why;
    size_t len = 0, i;
    lt_strlcpy(why, lt_err_get(), sizeof(why));
    if (strncmp(why, f, strlen(f)) == 0 && why[strlen(f)] == ':') {   /* "<path>: detail" */
        w = why + strlen(f) + 1;
        while (*w == ' ') { w++; }
    }
    if (!lt_plat_is_dir(dir)) {
        lt_snprintf(msg, sizeof(msg), "The project folder was not found. It may have been moved, renamed "
                    "or deleted, or it is on a drive that is not connected.");
        file = dir;
    } else if (!lt_file_exists(f) || lt_plat_is_dir(f)) {
        lt_snprintf(msg, sizeof(msg), "This folder has no project settings file (project.json): it is "
                    "not a LibreTune project, or the file was deleted.");
    } else {
        text = lt_file_read(f, &len);
        for (i = 0; text != NULL && i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' ||
                                               text[i] == '\n' || text[i] == '\0'); i++) {}
        if (text != NULL && i == len) {
            lt_snprintf(msg, sizeof(msg), "The project settings file (project.json) is empty. It was "
                        "probably cut off when the disk filled up or the computer lost power.");
        } else if (text == NULL) {
            lt_snprintf(msg, sizeof(msg), "The project settings file (project.json) cannot be read: %s", w);
        } else {
            lt_snprintf(msg, sizeof(msg), "The project settings file (project.json) is damaged and "
                        "cannot be used (%s).", w);
        }
        lt_free(text);
    }
    lt_app_toast(app, 3, "Cannot open project: %s", msg);
    (void)lt_feat_projerr_show(app, LT_PROJERR_PROJECT, dir, file, msg);
    lt_free(f);
}

/* the project's definition is missing or does not parse */
static void project_ini_problem(lt_app *app, const lt_project *p)
{
    char msg[700], why[400];
    char *f = lt_project_ini_path(p);
    lt_strlcpy(why, lt_err_get(), sizeof(why));
    if (!lt_file_exists(f)) {
        lt_snprintf(msg, sizeof(msg), "The project's ECU definition (projectCfg/definition.ini) is "
                    "missing, so LibreTune does not know how the settings are laid out.");
    } else {
        lt_snprintf(msg, sizeof(msg), "The project's ECU definition (projectCfg/definition.ini) cannot "
                    "be read: %s", why[0] != '\0' ? why : "it is not a valid INI file.");
    }
    lt_app_toast(app, 3, "Project INI failed: %s", why);
    (void)lt_feat_projerr_show(app, LT_PROJERR_DEFINITION, p->path, f, msg);
    lt_free(f);
}

int lt_app_open_project(lt_app *app, const char *dir_in)
{
    lt_project p;
    lt_ini *ini;
    char *ip, dir[sizeof(app->settings.recent[0])];
    abs_dir(dir_in, dir, sizeof(dir));          /* project.path, recents and tabs see one spelling */
    if (lt_project_open(dir, &p) < 0) {
        project_json_problem(app, dir);
        return -1;
    }
    {   /* the project's [SettingGroups] choice (TunerStudio ecuSettings) seeds the preprocessor */
        lt_proj_symbols sy;
        lt_ini_options o;
        lt_proj_symbols_init(&sy);
        (void)lt_proj_symbols_load(p.path, &sy);
        lt_proj_symbols_options(&sy, &o);
        ini = lt_project_load_ini(&p, &o);
        lt_proj_symbols_free(&sy);
    }
    if (ini == NULL) {
        project_ini_problem(app, &p);
        lt_project_free(&p);            /* nothing was opened: nothing to save */
        return -1;
    }
    lt_app_close_project(app);
    app->project = p;
    app->has_project = 1;
    ip = lt_project_ini_path(&app->project);
    lt_strlcpy(app->ini_path, ip, sizeof(app->ini_path));
    lt_free(ip);
    if (app->project.tune == NULL) {
        if (app->project.cfg.settings.auto_load_tune) { keep_unreadable_tune(app); }
        if (app->project.tune == NULL) { app->project.tune = lt_tune_from_defaults(ini); }
    }
    session_begin(app, ini, app->project.tune);
    lt_app_recent_add(app, app->project.path);
    lt_feat_connset_apply(app);
    lt_app_set_status(app, "Opened project %s", app->project.cfg.name);
    {   /* it parsed, but would it work? (no menus and no dialogs, constants beyond the pages) */
        char why[400], msg[700];
        if (lt_ini_looks_incomplete(ini, why, sizeof(why))) {
            lt_snprintf(msg, sizeof(msg), "The project opened, but its ECU definition looks incomplete. %s",
                        why);
            (void)lt_feat_projerr_show(app, LT_PROJERR_INCOMPLETE, app->project.path, app->ini_path, msg);
        }
    }
    return 0;
}

int lt_app_new_project(lt_app *app, const char *name, const char *ini_path)
{
    lt_project p;
    lt_ini *probe = lt_ini_load_file(ini_path, NULL);
    char *dir;
    int rc;
    if (probe == NULL) { lt_app_toast(app, 3, "Cannot load INI: %s", lt_err_get()); return -1; }
    rc = lt_project_create(name, ini_path, probe->signature != NULL ? probe->signature : "", NULL, &p);
    lt_ini_free(probe);
    if (rc < 0) { lt_app_toast(app, 3, "Cannot create project: %s", lt_err_get()); return rc; }
    dir = lt_strdup(p.path);
    lt_project_close(&p);
    rc = lt_app_open_project(app, dir);
    lt_free(dir);
    return rc;
}

int lt_app_import_ts(lt_app *app, const char *ts_dir)
{
    lt_project p;
    char *dir;
    int rc = lt_project_import_ts(ts_dir, NULL, &p);
    if (rc < 0) { lt_app_toast(app, 3, "Import failed: %s", lt_err_get()); return rc; }
    {   /* carry the TunerStudio project settings (ecuSettings) over */
        lt_proj_symbols sy;
        lt_proj_symbols_init(&sy);
        (void)lt_proj_symbols_load(ts_dir, &sy);
        if (sy.present) {
            size_t i;
            for (i = 0; i < sy.n_on; i++) {
                sy.managed = (char **)lt_realloc(sy.managed, (sy.n_managed + 1) * sizeof(char *));
                sy.managed[sy.n_managed++] = lt_strdup(sy.on[i]);
            }
            (void)lt_proj_symbols_save(p.path, &sy);
        }
        lt_proj_symbols_free(&sy);
    }
    dir = lt_strdup(p.path);
    lt_project_close(&p);
    rc = lt_app_open_project(app, dir);
    lt_free(dir);
    return rc;
}

static void sync_tune(lt_app *app)
{
    if (app->mem != NULL) { lt_tune_from_mem(app->tune, app->mem); }
    lt_tune_capture_from_pages(app->tune, app->ini);
    lt_tune_touch(app->tune);
}

int lt_app_save_tune(lt_app *app)
{
    int rc;
    if (app->ini == NULL) { return -1; }
    if (!app->has_project) {
        lt_app_file_dialog(app, LT_APP_FD_SAVE_TUNE, LT_UI_FILE_SAVE, NULL, "tune.msq",
                           "Tune files|*.msq;*.xml|All files|*");
        return 0;
    }
    sync_tune(app);
    rc = lt_project_save_tune(&app->project);
    if (rc < 0) { lt_app_toast(app, 3, "Save failed: %s", lt_err_get()); return rc; }
    app->modified = 0;
    lt_app_toast(app, 1, "Tune saved");
    lt_ext_on_save(app);
    return 0;
}

int lt_app_save_tune_as(lt_app *app, const char *path)
{
    int rc;
    if (app->ini == NULL || app->tune == NULL) { return -1; }
    sync_tune(app);
    rc = lt_tune_save(app->tune, path);
    if (rc < 0) { lt_app_toast(app, 3, "Save failed: %s", lt_err_get()); return rc; }
    if (!app->has_project) { app->modified = 0; }   /* a copy: the project's own tune is still unsaved */
    lt_app_toast(app, 1, "Saved %s", lt_path_base(path));
    return 0;
}

int lt_app_load_tune(lt_app *app, const char *path)
{
    lt_tune *t;
    if (app->ini == NULL) { return -1; }
    t = lt_tune_load(path);
    if (t == NULL) { lt_app_toast(app, 3, "Cannot load tune: %s", lt_err_get()); return -1; }
    return lt_app_load_tune_mem(app, t, lt_path_base(path));
}

int lt_app_load_tune_mem(lt_app *app, lt_tune *t, const char *label)
{
    int pg;
    if (app->ini == NULL) { lt_tune_free(t); return -1; }
    lt_tune_apply_to_pages(t, app->ini);
    if (app->mem != NULL && app->conn != LT_APP_OFFLINE) {
        lt_pagesrc ps = lt_app_pages(app);
        for (pg = 0; pg < 256; pg++) {
            if (t->pages[pg] != NULL && ps.write != NULL) {
                ps.write(ps.ud, pg, 0, t->pages[pg], t->page_len[pg]);
            }
        }
        lt_tune_free(t);
        lt_app_edited(app);
    } else {
        if (app->has_project) { lt_tune_free(app->project.tune); app->project.tune = t; }
        else { lt_tune_free(app->tune); }
        app->tune = t;
        app->modified = 1;
        rebind(app);
    }
    lt_app_toast(app, 1, "Loaded %s", label != NULL ? label : "tune");
    return 0;
}

int lt_app_reset_defaults(lt_app *app)
{
    lt_tune *d;
    lt_pagesrc ps;
    int pg, n = 0;
    if (app->ini == NULL) { return -1; }
    d = lt_tune_from_defaults(app->ini);
    ps = lt_app_pages(app);
    for (pg = 0; pg < 256; pg++) {
        if (d->pages[pg] != NULL && ps.write != NULL &&
            ps.write(ps.ud, pg, 0, d->pages[pg], d->page_len[pg]) == 0) {
            n++;
        }
    }
    lt_tune_free(d);
    lt_app_edited(app);
    lt_app_toast(app, 1, "Reset %d pages to defaults", n);
    return 0;
}

const char *lt_app_find_demo_ini(char *buf, size_t cap)
{
    static const char *cand[] = { "tests/fixtures/ini/demo.ini", "../tests/fixtures/ini/demo.ini",
                                  "demo.ini", "../share/libretune/demo.ini" };
    char *env = lt_plat_getenv("LT_DEMO_INI");
    char *exe;
    size_t i;
    if (env != NULL && lt_file_exists(env)) {
        lt_strlcpy(buf, env, cap);
        lt_free(env);
        return buf;
    }
    lt_free(env);
    for (i = 0; i < LT_COUNTOF(cand); i++) {
        if (lt_file_exists(cand[i])) { lt_strlcpy(buf, cand[i], cap); return buf; }
    }
    exe = lt_plat_exe_path();
    if (exe != NULL) {
        char *sl = strrchr(exe, '/');
        if (sl != NULL) {
            *sl = '\0';
            for (i = 0; i < LT_COUNTOF(cand); i++) {
                char *p = lt_path_join(exe, cand[i]);
                if (lt_file_exists(p)) { lt_strlcpy(buf, p, cap); lt_free(p); lt_free(exe); return buf; }
                lt_free(p);
            }
            {
                char *p = lt_path_join(exe, "../../tests/fixtures/ini/demo.ini");
                if (lt_file_exists(p)) { lt_strlcpy(buf, p, cap); lt_free(p); lt_free(exe); return buf; }
                lt_free(p);
            }
        }
        lt_free(exe);
    }
    return NULL;
}

/* no demo.ini beside the program: the bundled rusEFI Proteus definition as <config>/demo.ini */
const char *lt_app_bundled_demo_ini(lt_app *app, char *buf, size_t cap)
{
    const lt_inidb_def *d = NULL;
    size_t i, len = 0;
    char *txt, *path;
    int rc;
    if (app->config_dir == NULL || lt_inidb_count() == 0) { return NULL; }
    for (i = 0; i < lt_inidb_count() && d == NULL; i++) {
        const lt_inidb_def *c = lt_inidb_at(i);
        if (strcmp(c->family, "rusefi") == 0 && strcmp(c->board, "proteus_f4") == 0) { d = c; }
    }
    if (d == NULL) { d = lt_inidb_at(0); }
    txt = lt_inidb_text(d, &len);
    path = lt_path_join(app->config_dir, "demo.ini");
    (void)lt_plat_mkdirs(app->config_dir);
    rc = txt != NULL && path != NULL ? lt_plat_write_atomic(path, txt, len) : -1;
    if (rc == 0) { lt_strlcpy(buf, path, cap); }
    lt_free(txt);
    lt_free(path);
    return rc == 0 ? buf : NULL;
}

int lt_app_start_demo(lt_app *app, const char *ini_path)
{
    char buf[512];
    if (ini_path == NULL) {
        ini_path = lt_app_find_demo_ini(buf, sizeof(buf));
        if (ini_path == NULL) { ini_path = lt_app_bundled_demo_ini(app, buf, sizeof(buf)); }
        if (ini_path == NULL) {
            lt_app_toast(app, 2, "Choose an INI file for demo mode");
            lt_app_file_dialog(app, LT_APP_FD_OPEN_INI_DEMO, LT_UI_FILE_OPEN, NULL, "",
                               "ECU definitions|*.ini|All files|*");
            return -1;
        }
    }
    if (lt_app_open_ini(app, ini_path) < 0) { return -1; }
    return lt_app_connect_demo(app);
}

/* ------------------------------------------------------------------ */
/* INI reload with the project's settings symbols                      */
typedef struct tab_memo { char kind[24], target[128], title[96]; int closable; } tab_memo;

/* the simulated ECU starts from the definition's defaults: write the kept tune into it (as
 * edits, so they show as unburned changes like before the reload). Returns bytes changed. */
static size_t push_kept_pages(lt_app *app, lt_tune *kept)
{
    lt_pagesrc ps = lt_app_pages(app);
    size_t total = 0, len, k, e, lim;
    int pg;
    const lt_u8 *cur;
    for (pg = 0; pg < 256; pg++) {
        if (kept->pages[pg] == NULL || ps.page == NULL) { continue; }
        cur = ps.page(ps.ud, pg, &len);
        if (cur == NULL) { continue; }
        lim = len < kept->page_len[pg] ? len : kept->page_len[pg];
        for (k = 0; k < lim; k = e) {
            if (cur[k] == kept->pages[pg][k]) { e = k + 1; continue; }
            for (e = k; e < lim && cur[e] != kept->pages[pg][e]; e++) {}
            if (ps.write(ps.ud, pg, k, kept->pages[pg] + k, e - k) == 0) { total += e - k; }
            cur = ps.page(ps.ud, pg, &len);
            if (cur == NULL) { break; }
        }
    }
    return total;
}

int lt_app_reload_ini(lt_app *app)
{
    lt_proj_symbols sy;
    lt_ini_options o;
    lt_ini *ini;
    lt_tune *d;
    tab_memo *memo;
    int pg, i, n, active, was, baud, modified;
    char port[128];
    if (app->ini == NULL || app->tune == NULL || app->ini_path[0] == '\0') { return -1; }
    lt_proj_symbols_init(&sy);
    if (app->has_project) { (void)lt_proj_symbols_load(app->project.path, &sy); }
    lt_proj_symbols_options(&sy, &o);
    ini = lt_ini_load_file(app->ini_path, &o);
    lt_proj_symbols_free(&sy);
    if (ini == NULL) { lt_app_toast(app, 3, "Cannot reload the INI: %s", lt_err_get()); return -1; }
    /* current values (ECU shadow, then the pages) into the tune, old layout */
    if (app->mem != NULL) { (void)lt_tune_from_mem(app->tune, app->mem); }
    (void)lt_tune_capture_from_pages(app->tune, app->ini);
    modified = app->modified;
    was = (int)app->conn;
    lt_strlcpy(port, app->settings.port, sizeof(port));
    baud = app->settings.baud;
    app->keep_project_tune = 1;                     /* the tune was captured above */
    lt_app_disconnect(app);
    app->keep_project_tune = 0;
    /* tabs hold pointers into the old definition: close them, reopen afterwards */
    n = app->n_tabs;
    active = app->active_tab;
    memo = (tab_memo *)lt_calloc((size_t)n + 1, sizeof(tab_memo));
    for (i = 0; i < n; i++) {
        lt_strlcpy(memo[i].kind, app->tabs[i]->kind, sizeof(memo[i].kind));
        lt_strlcpy(memo[i].target, app->tabs[i]->target, sizeof(memo[i].target));
        lt_strlcpy(memo[i].title, app->tabs[i]->title, sizeof(memo[i].title));
        memo[i].closable = app->tabs[i]->closable;
    }
    while (app->n_tabs > 0) { lt_app_tab_close(app, app->tabs[app->n_tabs - 1]); }
    rec_free(app);
    if (app->rt != NULL) { lt_ini_rt_free(app->rt); app->rt = NULL; }
    /* fresh pages of the new layout with defaults, the named values written over them */
    d = lt_tune_from_defaults(ini);
    for (pg = 0; pg < 256; pg++) {
        if (d->pages[pg] != NULL) { lt_tune_set_page(app->tune, pg, d->pages[pg], d->page_len[pg]); }
    }
    lt_tune_free(d);
    (void)lt_tune_apply_to_pages(app->tune, ini);
    (void)lt_tune_capture_from_pages(app->tune, ini);
    lt_ui_after_frame(app->ui, ini_free_later, app->ini);      /* see lt_app_close_project */
    app->ini = ini;
    lt_sidx_free(app->sidx);
    app->sidx = NULL;
    app->reveal.comp = NULL;
    lt_expr_cache_free(&app->expr);
    lt_expr_cache_init(&app->expr);
    app->rt = lt_ini_rt_new(ini);
    rebind(app);
    for (i = 0; i < n; i++) { (void)lt_app_tab_open(app, memo[i].kind, memo[i].target, memo[i].title, memo[i].closable); }
    if (active >= 0 && active < app->n_tabs) { app->active_tab = active; }
    lt_free(memo);
    app->modified = modified;
    if (was == (int)LT_APP_DEMO) {
        lt_tune *kept = lt_tune_new("");
        for (pg = 0; pg < 256; pg++) {
            if (app->tune->pages[pg] != NULL) { lt_tune_set_page(kept, pg, app->tune->pages[pg], app->tune->page_len[pg]); }
        }
        if (lt_app_connect_demo(app) == 0 && push_kept_pages(app, kept) > 0) {
            lt_app_edited(app);
        }
        lt_tune_free(kept);
    } else if (was == (int)LT_APP_ONLINE && port[0] != '\0') { (void)lt_app_connect_serial(app, port, baud); }
    return 0;
}

/* ------------------------------------------------------------------ */
/* target routing                                                      */
int lt_app_open_target(lt_app *app, const char *target, const char *label)
{
    lt_ini *ini = app->ini;
    int rc;
    if (ini == NULL || target == NULL) { return -1; }
    if (lt_ini_find_table(ini, target) != NULL) { return lt_app_table_editor_open(app, target); }
    if (lt_ini_find_curve(ini, target) != NULL) { return lt_app_curve_editor_open(app, target); }
    if (lt_ini_find_dialog(ini, target) != NULL) {
        const lt_ini_dialog *d = lt_ini_find_dialog(ini, target);
        if (label != NULL && label[0] != '\0' &&
            (d->title == NULL || d->title[0] == '\0' || strcmp(d->title, target) == 0)) {
            char t[96];
            size_t i, k = 0;
            for (i = 0; label[i] != '\0' && k + 1 < sizeof(t); i++) { if (label[i] != '&') { t[k++] = label[i]; } }
            t[k] = '\0';
            return lt_app_tab_open(app, "dialog", target, t, 1) != NULL ? 0 : -1;   /* untitled: the menu label */
        }
        return lt_app_dialog_open(app, target);
    }
    if (lt_ini_find_port_editor(ini, target) != NULL) { return lt_app_port_editor_open(app, target); }
    rc = lt_ext_dispatch_target(app, target);      /* feature-registered targets */
    if (rc != LT_EXT_NO_TARGET) { return rc; }
    if (lt_str_starts(target, "std_")) {
        if (strcmp(target, "std_separator") == 0) { return 0; }
        if (lt_ini_std_panel(ini, target) != NULL) { return lt_app_dialog_open(app, target); }
        if (strcmp(target, "std_port_edit") == 0) { return lt_app_port_editor_open(app, target); }
        if (strcmp(target, "std_ms3SdConsole") == 0) { return lt_app_special_open(app, target); }
    }
    lt_app_toast(app, 2, "Unsupported menu target: %s", target);
    return -1;
}
