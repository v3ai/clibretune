/* app.c - application state: settings, project/tune/ECU lifecycle, registries.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "app_ext.h"
#include "feat.h"

#define LT_APP_VERSION "0.1.0"

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
        v = lt_props_get(&p, "scale");
        if (v != NULL) {
            int sv = lt_stricmp(v, "auto") == 0 ? 0 : atoi(v);
            /* old files stored an integer factor: 1 was the default (not a choice),
             * so it now means auto; 2..3 were explicit and become 200..300 */
            s->scale_pct = sv == 1 ? 0 : (sv >= 2 && sv <= 3) ? sv * 100 : sv;
        }
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
        if (v != NULL) { lt_strlcpy(s->last_project, v, sizeof(s->last_project)); }
        v = lt_props_get(&p, "port");
        if (v != NULL) { lt_strlcpy(s->port, v, sizeof(s->port)); }
        v = lt_props_get(&p, "heatmap");
        if (v != NULL) { lt_strlcpy(s->heatmap, v, sizeof(s->heatmap)); }
        n = 0;
        for (i = 0; i < 8; i++) {
            lt_snprintf(key, sizeof(key), "recent%d", i);
            v = lt_props_get(&p, key);
            if (v != NULL && v[0] != '\0') {
                lt_strlcpy(s->recent[n++], v, sizeof(s->recent[0]));
            }
        }
        s->n_recent = n;
    }
    lt_props_free(&p);
    lt_free(path);
    s->sidebar_w = LT_CLAMP(s->sidebar_w, 160, 600);
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
    if (s->scale_pct == 0) { lt_props_set(&p, "scale", "auto"); }
    else { lt_snprintf(num, sizeof(num), "%d", s->scale_pct); lt_props_set(&p, "scale", num); }
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

void lt_app_recent_add(lt_app *app, const char *dir)
{
    lt_app_settings *s = &app->settings;
    int i, j;
    for (i = 0; i < s->n_recent; i++) {
        if (strcmp(s->recent[i], dir) == 0) { break; }
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
        if (n == 0) { lt_app_toast(app, 2, "No channels to log"); return -1; }
        app->rec_n = n;
        app->rec = lt_rec_new((const char *const *)app->rec_names, (const char *const *)units, (int)n);
        app->rec_vals = (double *)lt_calloc(n, sizeof(double));
        lt_strv_free(units, n);
    }
    if (lt_rec_is_recording(app->rec)) {
        lt_rec_stop(app->rec);
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
    path = lt_rec_default_path(dir);
    i = (size_t)lt_rec_start(app->rec, path);
    if ((int)i < 0) { lt_app_toast(app, 3, "Cannot start log: %s", lt_err_get()); }
    else { lt_app_toast(app, 1, "Logging to %s", lt_path_base(path)); }
    lt_free(path);
    lt_free(dir);
    return 0;
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
    if (app->sim != NULL) { lt_ecusim_free(app->sim); app->sim = NULL; }
    app->link_stream = NULL;
    if (app->mem != NULL) { lt_ecu_mem_free(app->mem); app->mem = NULL; }
}

void lt_app_disconnect(lt_app *app)
{
    if (app->conn == LT_APP_OFFLINE && app->proto == NULL) { return; }
    if (app->rec != NULL && lt_rec_is_recording(app->rec)) { lt_rec_stop(app->rec); }
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

int lt_app_connect_demo(lt_app *app)
{
    lt_proto_io io;
    int rc;
    if (app->ini == NULL) { lt_app_toast(app, 3, "No ECU definition loaded"); return -1; }
    lt_app_disconnect(app);
    app->sim = lt_ecusim_new(app->ini);
    io = lt_ecusim_io(app->sim);
    rc = connect_io(app, &io, "demo", 115200L);
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
    return (lt_ecu_mem_has_pending_burn(app->mem) || lt_ecu_mem_has_dirty(app->mem)) ? LT_TRUE
                                                                                         : LT_FALSE;
}

void lt_app_edited(lt_app *app)
{
    app->modified = 1;
    if (app->mem != NULL && app->proto != NULL &&
        (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) && lt_ecu_mem_has_dirty(app->mem)) {
        if (lt_proto_write_dirty(app->proto, app->mem) < 0) {
            lt_app_toast(app, 3, "ECU write failed: %s", lt_err_get());
        }
    }
}

int lt_app_burn(lt_app *app)
{
    int rc;
    char *rp = NULL;
    if (!lt_app_can_burn(app)) { return -1; }
    if (lt_ecu_mem_has_dirty(app->mem) && lt_proto_write_dirty(app->proto, app->mem) < 0) {
        lt_app_toast(app, 3, "ECU write failed: %s", lt_err_get());
        return -1;
    }
    if (app->has_project && app->project.tune != NULL) {
        lt_tune_from_mem(app->tune, app->mem);
        if (lt_restore_create(&app->project, &rp) == 0) { lt_free(rp); }
    }
    rc = lt_proto_burn(app->proto, -1, app->mem);
    if (rc < 0) {
        lt_app_toast(app, 3, "Burn failed: %s", lt_err_get());
        return rc;
    }
    lt_app_toast(app, 1, "Burned to ECU flash");
    if (app->has_project) { lt_app_save_tune(app); }
    return 0;
}

int lt_app_service(lt_app *app)
{
    double now;
    int rc, got = 0;
    lt_ext_tick(app);
    if ((app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) || app->proto == NULL) { return 0; }
    now = lt_plat_now_ms();
    if (now - app->last_poll_ms < 1000.0 / (double)LT_CLAMP(app->settings.poll_hz, 5, 100)) { return 0; }
    app->last_poll_ms = now;
    lt_buf_clear(&app->rtbuf);
    rc = lt_proto_get_realtime(app->proto, &app->rtbuf);
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
    got = 1;
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
        lt_rec_record(app->rec, app->rec_vals, (int)app->rec_n);
    }
    return got;
}

/* ------------------------------------------------------------------ */
/* project / ini / tune                                                */
void lt_app_close_project(lt_app *app)
{
    int i;
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
    if (app->rt != NULL) { lt_ini_rt_free(app->rt); app->rt = NULL; }
    if (app->has_project) {
        lt_project_close(&app->project);
        app->has_project = 0;
        app->tune = NULL;
    } else if (app->tune != NULL) {
        lt_tune_free(app->tune);
        app->tune = NULL;
    }
    if (app->ini != NULL) { lt_ini_free(app->ini); app->ini = NULL; }
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

int lt_app_open_project(lt_app *app, const char *dir)
{
    lt_project p;
    lt_ini *ini;
    char *ip;
    if (lt_project_open(dir, &p) < 0) {
        lt_app_toast(app, 3, "Cannot open project: %s", lt_err_get());
        return -1;
    }
    ini = lt_project_load_ini(&p, NULL);
    if (ini == NULL) {
        lt_app_toast(app, 3, "Project INI failed: %s", lt_err_get());
        lt_project_close(&p);
        return -1;
    }
    lt_app_close_project(app);
    app->project = p;
    app->has_project = 1;
    ip = lt_project_ini_path(&app->project);
    lt_strlcpy(app->ini_path, ip, sizeof(app->ini_path));
    lt_free(ip);
    if (app->project.tune == NULL) { app->project.tune = lt_tune_from_defaults(ini); }
    session_begin(app, ini, app->project.tune);
    lt_app_recent_add(app, app->project.path);
    lt_feat_connset_apply(app);
    lt_app_set_status(app, "Opened project %s", app->project.cfg.name);
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
    app->modified = 0;
    lt_app_toast(app, 1, "Saved %s", lt_path_base(path));
    return 0;
}

int lt_app_load_tune(lt_app *app, const char *path)
{
    lt_tune *t;
    int pg;
    if (app->ini == NULL) { return -1; }
    t = lt_tune_load(path);
    if (t == NULL) { lt_app_toast(app, 3, "Cannot load tune: %s", lt_err_get()); return -1; }
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
    lt_app_toast(app, 1, "Loaded %s", lt_path_base(path));
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

int lt_app_start_demo(lt_app *app, const char *ini_path)
{
    char buf[512];
    if (ini_path == NULL) {
        ini_path = lt_app_find_demo_ini(buf, sizeof(buf));
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
/* target routing                                                      */
int lt_app_open_target(lt_app *app, const char *target, const char *label)
{
    lt_ini *ini = app->ini;
    int rc;
    LT_UNUSED(label);
    if (ini == NULL || target == NULL) { return -1; }
    if (lt_ini_find_table(ini, target) != NULL) { return lt_app_table_editor_open(app, target); }
    if (lt_ini_find_curve(ini, target) != NULL) { return lt_app_curve_editor_open(app, target); }
    if (lt_ini_find_dialog(ini, target) != NULL) { return lt_app_dialog_open(app, target); }
    rc = lt_ext_dispatch_target(app, target);      /* feature-registered targets */
    if (rc != LT_EXT_NO_TARGET) { return rc; }
    if (lt_str_starts(target, "std_")) {
        if (strcmp(target, "std_separator") == 0) { return 0; }
        if (lt_ini_std_panel(ini, target) != NULL) { return lt_app_dialog_open(app, target); }
        if (strcmp(target, "std_port_edit") == 0 || strcmp(target, "std_ms3SdConsole") == 0) {
            return lt_app_special_open(app, target);
        }
    }
    lt_app_toast(app, 2, "Unsupported menu target: %s", target);
    return -1;
}
