/* dash_panels.c - panels of the dashboard tab: dashboard selector with management (new, rename,
 * duplicate, delete, export, import, reset), import dialog with compatibility report, validation
 * panel and compatibility bar, layers, gauge palette with channel browser, asset manager, gauge
 * zoom window and the alignment bar.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "dash_tab.h"
#include "../plat/plat.h"
#include "view_kit.h"
#include "../dash/dash_priv.h"
#include "../datalog/dl_view.h"

#define FD_IMPORT (LT_APP_FD_USER0 + 42)
#define FD_EXPORT (LT_APP_FD_USER0 + 43)
#define FD_ASSET  (LT_APP_FD_USER0 + 44)
#define FD_TSDIR  (LT_APP_FD_USER0 + 45)

#define CH 8
#define CV 16

void lt_dpanel_request(lt_app *app, dash_state *st, int what)
{
    char *dir = lt_dtab_dir(app);
    switch (what) {
    case DP_IMPORT:
        lt_app_file_dialog(app, FD_IMPORT, LT_UI_FILE_OPEN, NULL, "", "Dashboards|*.dash|All files|*");
        break;
    case DP_IMPORT_TS:
        lt_app_file_dialog(app, FD_TSDIR, LT_UI_FILE_OPEN, NULL, "", "All files|*");
        break;
    case DP_EXPORT: {
        char fn[120];
        lt_snprintf(fn, sizeof fn, "%s", lt_path_base(st->mgmt_target));
        lt_app_file_dialog(app, FD_EXPORT, LT_UI_FILE_SAVE, NULL, fn, "Dashboards|*.dash|All files|*");
        break; }
    case DP_ADD_ASSET:
        lt_app_file_dialog(app, FD_ASSET, LT_UI_FILE_OPEN, NULL, "", "Images and fonts|*.png;*.gif;*.ttf|All files|*");
        break;
    default: break;
    }
    lt_free(dir);
}

/* ---- file dialog results ---- */
static void imp_preview(lt_app *app, dash_state *st, const char *path)
{
    char *dir = lt_dtab_dir(app);
    lt_dash_compat_free(&st->imp_rep);
    (void)lt_dashlib_preview(path, app->ini, dir, &st->imp_rep);
    lt_free(dir);
    st->imp_have = 1;
}

void lt_dpanel_import_path(lt_app *app, dash_state *st, const char *path)
{
    lt_snprintf(st->imp_path, sizeof st->imp_path, "%s", path);
    st->imp_ts = 0; st->imp_open = 1; st->imp_sel = 0;
    lt_dashlib_free(&st->imp_lib); lt_dashlib_init(&st->imp_lib);
    imp_preview(app, st, path);
}

int lt_dpanel_file_results(lt_app *app, dash_state *st)
{
    const char *fp;
    int did = 0;
    fp = lt_app_file_result(app, FD_IMPORT);
    if (fp != NULL) {
        lt_dpanel_import_path(app, st, fp);
        did = 1;
    }
    fp = lt_app_file_result(app, FD_TSDIR);
    if (fp != NULL) {
        char dir[512];
        const char *b = lt_path_base(fp);
        size_t n = (size_t)(b - fp);
        if (n > 0) { n--; }
        if (n >= sizeof dir) { n = sizeof dir - 1; }
        memcpy(dir, fp, n); dir[n] = '\0';
        if (n == 0) { lt_strlcpy(dir, ".", sizeof dir); }
        lt_dashlib_free(&st->imp_lib); lt_dashlib_init(&st->imp_lib);
        (void)lt_dashlib_scan_ts(&st->imp_lib, dir);
        lt_dashlib_sort(&st->imp_lib);
        st->imp_ts = 1; st->imp_open = 1; st->imp_sel = 0; st->imp_have = 0;
        lt_dash_compat_free(&st->imp_rep);
        if (st->imp_lib.n > 0) { imp_preview(app, st, st->imp_lib.e[0].path); lt_strlcpy(st->imp_path, st->imp_lib.e[0].path, sizeof st->imp_path); }
        did = 1;
    }
    fp = lt_app_file_result(app, FD_EXPORT);
    if (fp != NULL) {
        char dest[512];
        size_t n;
        lt_snprintf(dest, sizeof dest, "%s", fp);
        n = strlen(dest);
        if (n < 5 || lt_stricmp(dest + n - 5, ".dash") != 0) { lt_snprintf(dest + n, sizeof dest - n, ".dash"); }
        if (st->mgmt_target[0] != '\0' && lt_dashlib_export(st->mgmt_target, dest) == 0) {
            lt_app_toast(app, 1, "Exported to %s", lt_path_base(dest));
        } else { lt_app_toast(app, 3, "Export failed: %s", lt_err_get()); }
        did = 1;
    }
    fp = lt_app_file_result(app, FD_ASSET);
    if (fp != NULL) {
        size_t len = 0;
        char *data = lt_file_read(fp, &len);
        if (data != NULL && len > 0) {
            st->asset_sel = lt_dash_asset_add(st->file, fp, data, len);
            lt_dtab_touch(st);
        } else { lt_app_toast(app, 3, "Cannot read %s", lt_path_base(fp)); }
        lt_free(data);
        did = 1;
    }
    return did;
}

/* ---- selector ---- */
typedef struct sel_rows {
    int n;
    int ent[256];                 /* library index, or -1 for a header */
    char lab[256][96];
} sel_rows;

static const char *row_get(void *ud, int i)
{
    sel_rows *r = (sel_rows *)ud;
    return i >= 0 && i < r->n ? r->lab[i] : "";
}

static void build_rows(dash_state *st, sel_rows *r)
{
    size_t i, k;
    int cnt;
    r->n = 0;
    for (i = 0; i < st->lib.n && r->n < 250; i++) {
        const char *cat = st->lib.e[i].category;
        int seen = 0;
        for (k = 0; k < i; k++) { if (strcmp(st->lib.e[k].category, cat) == 0) { seen = 1; break; } }
        if (seen) { continue; }
        cnt = 0;
        for (k = 0; k < st->lib.n; k++) { if (strcmp(st->lib.e[k].category, cat) == 0) { cnt++; } }
        r->ent[r->n] = -1;
        lt_snprintf(r->lab[r->n], sizeof r->lab[0], "%s (%d)", cat, cnt);
        r->n++;
        for (k = 0; k < st->lib.n && r->n < 255; k++) {
            if (strcmp(st->lib.e[k].category, cat) != 0) { continue; }
            r->ent[r->n] = (int)k;
            lt_snprintf(r->lab[r->n], sizeof r->lab[0], "    %s", st->lib.e[k].name);
            r->n++;
        }
    }
}

static int is_builtin(const lt_dashlib_entry *e) { return e->path[0] == '@'; }

static void open_entry(lt_app *app, dash_state *st, const lt_dashlib_entry *e)
{
    int i;
    if (is_builtin(e)) {
        static const char *const keys[4] = { "@front", "@basic", "@tuning", "@telemetry" };
        for (i = 0; i < 4; i++) {
            if (strcmp(e->path, keys[i]) == 0) { lt_dtab_load_builtin(app, st, i); break; }
        }
        if (app->has_project && app->project.path != NULL) {
            lt_free(app->project.cfg.dashboard);
            app->project.cfg.dashboard = lt_strdup(e->path);
            (void)lt_project_save_config(&app->project);
        }
    } else {
        (void)lt_dtab_load_file(app, st, e->path);
    }
}

static void do_mgmt_name(lt_app *app, dash_state *st, int *open)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w2[2], w1 = -1, ok;
    static int tmpl = 4;
    static const char *const tmpls[5] = { "Blank", "Basic", "Tuning", "Telemetry", "Copy of the current dashboard" };
    const char *title = st->mgmt == MG_NEW ? "New Dashboard" : (st->mgmt == MG_RENAME ? "Rename Dashboard" : "Duplicate Dashboard");
    char *dir = lt_dtab_dir(app);
    if (lt_ui_modal_begin(ui, title, CH * 60, st->mgmt == MG_NEW ? 190 : 150, open)) {
        w2[0] = CH * 12; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Name");
        lt_ui_text(ui, "mg_name", st->mgmt_name, sizeof st->mgmt_name, 0);
        if (st->mgmt == MG_NEW) {
            lt_ui_label(ui, "Start from");
            (void)lt_ui_combo(ui, "mg_tmpl", &tmpl, tmpls, 5);
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        ok = lt_dashlib_name_ok(st->mgmt_name) && dir != NULL;
        if (dir == NULL) { lt_ui_label_ex(ui, "Open a project to keep dashboards in it.", t->warning, LT_UI_ALIGN_LEFT); }
        else if (!ok) { lt_ui_label_ex(ui, "Use letters, digits, spaces and - _ (no / \\ : * ? \" < > |).", t->muted, LT_UI_ALIGN_LEFT); }
        else { lt_ui_label(ui, ""); }
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button_ex(ui, st->mgmt == MG_RENAME ? "Rename" : (st->mgmt == MG_DUP ? "Duplicate" : "Create"),
                            ok ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED) && ok) {
            char *np = NULL;
            int rc = 0;
            if (st->mgmt == MG_NEW) {
                lt_dash_file *f = NULL;
                if (tmpl == 0) { f = lt_dash_new(); }
                else if (tmpl == 1) { f = lt_dash_template_basic(); }
                else if (tmpl == 2) { f = lt_dash_template_tuning(); }
                else if (tmpl == 3) { f = lt_dash_template_telemetry(); }
                if (f != NULL) { lt_dash_resolve_channels(f, app->ini); }
                rc = lt_dashlib_create(dir, st->mgmt_name, f != NULL ? f : st->file, &np);
                if (f != NULL) { lt_dash_free(f); }
            } else if (st->mgmt == MG_DUP) { rc = lt_dashlib_duplicate(st->mgmt_target, st->mgmt_name, &np); }
            else { rc = lt_dashlib_rename(st->mgmt_target, st->mgmt_name, &np); }
            if (rc == 0 && np != NULL) {
                int was_current = strcmp(st->mgmt_target, st->path) == 0;
                if (st->mgmt == MG_RENAME && was_current) {
                    lt_snprintf(st->path, sizeof st->path, "%s", np);
                    if (app->has_project) {
                        lt_free(app->project.cfg.dashboard);
                        app->project.cfg.dashboard = lt_strdup(lt_path_base(np));
                        (void)lt_project_save_config(&app->project);
                    }
                } else if (st->mgmt != MG_RENAME) { (void)lt_dtab_load_file(app, st, np); }
                st->lib_valid = 0;
                *open = 0;
            } else { lt_app_toast(app, 3, "%s", lt_err_get()); }
            lt_free(np);
        }
        if (lt_ui_button(ui, "Cancel")) { *open = 0; }
        lt_ui_modal_end(ui);
    }
    lt_free(dir);
}

static void selector_modal(lt_app *app, dash_state *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    sel_rows *rows;
    int open = st->selector_open, w1 = -1, w5[5], act = -1, cur = -1, i;
    const lt_dashlib_entry *e = NULL;
    char *dir = lt_dtab_dir(app);
    int user_sel;
    if (!st->lib_valid) { lt_dtab_lib_refresh(app, st); }
    rows = (sel_rows *)lt_calloc(1, sizeof *rows);
    build_rows(st, rows);
    if (st->sel_row < 0 || st->sel_row >= rows->n || rows->ent[st->sel_row] < 0) {
        for (i = 0; i < rows->n; i++) {
            if (rows->ent[i] >= 0 && ((st->src == DS_FILE && strcmp(st->lib.e[rows->ent[i]].path, st->path) == 0) ||
                                      (st->src != DS_FILE && is_builtin(&st->lib.e[rows->ent[i]]) &&
                                       strcmp(st->lib.e[rows->ent[i]].name, st->src == DS_FRONT ? "ECU front page" :
                                              (st->src == DS_BASIC ? "Basic" : (st->src == DS_TUNING ? "Tuning" : "Telemetry"))) == 0))) {
                cur = i; break;
            }
        }
        st->sel_row = cur >= 0 ? cur : 1;
    }
    if (lt_ui_modal_begin(ui, "Select Dashboard", CH * 76, 400, &open)) {
        int ri = st->sel_row, ch;
        lt_ui_layout_row(ui, 1, &w1, CV * 14);
        ch = lt_ui_listbox_cb(ui, "sel_list", &ri, rows->n, row_get, rows);
        if (ri >= 0 && ri < rows->n && rows->ent[ri] >= 0) { st->sel_row = ri; }
        if ((ch & LT_UI_ACTIVATED) && ri >= 0 && ri < rows->n && rows->ent[ri] >= 0) { act = 0; }
        user_sel = st->sel_row >= 0 && st->sel_row < rows->n ? rows->ent[st->sel_row] : -1;
        if (user_sel >= 0) { e = &st->lib.e[user_sel]; }
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (e != NULL) {
            char info[300];
            if (is_builtin(e)) { lt_snprintf(info, sizeof info, "Built-in (generated from the ECU definition)"); }
            else { lt_snprintf(info, sizeof info, "%s   %ld bytes", e->path, e->size); }
            lt_ui_label_ex(ui, info, t->muted, LT_UI_ALIGN_LEFT);
        } else { lt_ui_label(ui, ""); }
        w5[0] = -1; w5[1] = -1; w5[2] = -1; w5[3] = -1; w5[4] = -1;
        lt_ui_layout_row(ui, 5, w5, 0);
        if (lt_ui_button_ex(ui, "Open", e != NULL ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED)) { act = 0; }
        if (lt_ui_button_ex(ui, "New...", dir != NULL ? 0 : LT_UI_BTN_DISABLED)) { act = 1; }
        if (lt_ui_button_ex(ui, "Duplicate...", (e != NULL && !is_builtin(e)) ? 0 : LT_UI_BTN_DISABLED)) { act = 2; }
        if (lt_ui_button_ex(ui, "Rename...", (e != NULL && !is_builtin(e)) ? 0 : LT_UI_BTN_DISABLED)) { act = 3; }
        if (lt_ui_button_ex(ui, "Delete...", (e != NULL && !is_builtin(e)) ? LT_UI_BTN_DANGER : LT_UI_BTN_DISABLED)) { act = 4; }
        lt_ui_layout_row(ui, 5, w5, 0);
        if (lt_ui_button_ex(ui, "Export...", (e != NULL && !is_builtin(e)) ? 0 : LT_UI_BTN_DISABLED)) { act = 5; }
        if (lt_ui_button_ex(ui, "Import...", dir != NULL ? 0 : LT_UI_BTN_DISABLED)) { act = 6; }
        if (lt_ui_button_ex(ui, "From TunerStudio...", dir != NULL ? 0 : LT_UI_BTN_DISABLED)) { act = 7; }
        if (lt_ui_button(ui, "Reset to defaults")) { act = 8; }
        if (lt_ui_button(ui, "Close")) { open = 0; }
        if (dir == NULL) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "No project is open: dashboards cannot be created or imported.", t->warning, LT_UI_ALIGN_LEFT);
        }
        lt_ui_modal_end(ui);
    }
    st->selector_open = open;
    if (act >= 0) {
        if (act == 0 && e != NULL) { open_entry(app, st, e); st->selector_open = 0; }
        else if (act == 1) { st->mgmt = MG_NEW; st->mgmt_open = 1; lt_snprintf(st->mgmt_name, sizeof st->mgmt_name, "My Dashboard"); }
        else if (act == 2 && e != NULL) {
            char nm[96];
            lt_dashlib_unique(dir, e->name, nm, sizeof nm);
            st->mgmt = MG_DUP; st->mgmt_open = 1;
            lt_snprintf(st->mgmt_name, sizeof st->mgmt_name, "%s", nm);
            lt_snprintf(st->mgmt_target, sizeof st->mgmt_target, "%s", e->path);
        } else if (act == 3 && e != NULL) {
            st->mgmt = MG_RENAME; st->mgmt_open = 1;
            lt_snprintf(st->mgmt_name, sizeof st->mgmt_name, "%s", e->name);
            lt_snprintf(st->mgmt_target, sizeof st->mgmt_target, "%s", e->path);
        } else if (act == 4 && e != NULL) {
            st->mgmt = MG_DELETE; st->mgmt_open = 1;
            lt_snprintf(st->mgmt_name, sizeof st->mgmt_name, "%s", e->name);
            lt_snprintf(st->mgmt_target, sizeof st->mgmt_target, "%s", e->path);
        } else if (act == 5 && e != NULL) {
            lt_snprintf(st->mgmt_target, sizeof st->mgmt_target, "%s", e->path);
            lt_dpanel_request(app, st, DP_EXPORT);
        } else if (act == 6) { lt_dpanel_request(app, st, DP_IMPORT); }
        else if (act == 7) { lt_dpanel_request(app, st, DP_IMPORT_TS); }
        else if (act == 8) { st->mgmt = MG_RESET; st->mgmt_open = 1; }
    }
    lt_free(dir);
    lt_free(rows);
}

static void import_modal(lt_app *app, dash_state *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int open = st->imp_open, w1 = -1, w3[3], act = 0;
    char *dir = lt_dtab_dir(app);
    if (lt_ui_modal_begin(ui, st->imp_ts ? "Import from TunerStudio" : "Import Dashboard", CH * 78, 380, &open)) {
        if (st->imp_ts) {
            int ch;
            sel_rows *rows = (sel_rows *)lt_calloc(1, sizeof *rows);
            size_t i;
            for (i = 0; i < st->imp_lib.n && rows->n < 255; i++) {
                rows->ent[rows->n] = (int)i;
                lt_snprintf(rows->lab[rows->n], sizeof rows->lab[0], "%s", st->imp_lib.e[i].name);
                rows->n++;
            }
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (rows->n == 0) { lt_ui_label_ex(ui, "No .dash files were found next to that file.", t->warning, LT_UI_ALIGN_LEFT); }
            else {
                int prev = st->imp_sel;
                lt_ui_layout_row(ui, 1, &w1, CV * 6);
                ch = lt_ui_listbox_cb(ui, "imp_list", &st->imp_sel, rows->n, row_get, rows);
                LT_UNUSED(ch);
                if (st->imp_sel != prev && st->imp_sel >= 0 && st->imp_sel < rows->n) {
                    lt_snprintf(st->imp_path, sizeof st->imp_path, "%s", st->imp_lib.e[st->imp_sel].path);
                    imp_preview(app, st, st->imp_path);
                }
            }
            lt_free(rows);
        } else {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, st->imp_path, t->text2, LT_UI_ALIGN_LEFT);
        }
        if (st->imp_have) {
            const lt_dash_compat *c = &st->imp_rep;
            char line[200];
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (!c->parsed) {
                lt_ui_label_ex(ui, c->err[0] ? c->err : "The file cannot be read.", t->error, LT_UI_ALIGN_LEFT);
            } else {
                lt_snprintf(line, sizeof line, "%lu gauge(s), %lu indicator(s), %lu channel(s) used",
                            (unsigned long)c->gauges, (unsigned long)c->indicators, (unsigned long)c->n_channels);
                lt_ui_label(ui, line);
                lt_snprintf(line, sizeof line, "Compatible with this ECU definition: %d%%", c->percent);
                lt_ui_progress(ui, (double)c->percent / 100.0, line);
                if (c->n_unresolved > 0) {
                    char list[240];
                    size_t i;
                    list[0] = '\0';
                    for (i = 0; i < c->n_unresolved && strlen(list) < 180; i++) {
                        lt_snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ", " : "", c->unresolved[i]);
                    }
                    lt_snprintf(line, sizeof line, "Not found in the ECU definition (%lu):", (unsigned long)c->n_unresolved);
                    lt_ui_label_ex(ui, line, t->warning, LT_UI_ALIGN_LEFT);
                    lt_ui_label_wrap(ui, list, t->text2);
                }
                if (c->duplicate) {
                    lt_ui_label_ex(ui, "A dashboard with this name already exists in the project.", t->warning, LT_UI_ALIGN_LEFT);
                }
            }
        }
        w3[0] = -1; w3[1] = -1; w3[2] = -1;
        lt_ui_layout_row(ui, 3, w3, 0);
        {
            int ok = st->imp_have && st->imp_rep.parsed && dir != NULL;
            if (lt_ui_button_ex(ui, st->imp_rep.duplicate ? "Import as copy" : "Import", ok ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED)) { act = 1; }
            if (lt_ui_button_ex(ui, "Replace existing", (ok && st->imp_rep.duplicate) ? LT_UI_BTN_DANGER : LT_UI_BTN_DISABLED)) { act = 2; }
            if (lt_ui_button(ui, "Cancel")) { open = 0; }
        }
        lt_ui_modal_end(ui);
    }
    st->imp_open = open;
    if (act > 0 && dir != NULL) {
        char *dest = NULL;
        if (lt_dashlib_import(st->imp_path, dir, act == 2, &dest) == 0) {
            lt_app_toast(app, 1, "Imported %s", lt_path_base(dest));
            (void)lt_dtab_load_file(app, st, dest);
            st->lib_valid = 0;
            st->imp_open = 0;
        } else { lt_app_toast(app, 3, "Import failed: %s", lt_err_get()); }
        lt_free(dest);
    }
    lt_free(dir);
}

void lt_dpanel_selector(lt_app *app, dash_state *st)
{
    lt_ui *ui = app->ui;
    if (st->selector_open && !st->mgmt_open && !st->imp_open) { selector_modal(app, st); }
    if (st->imp_open) { import_modal(app, st); }
    if (st->mgmt_open) {
        int open = st->mgmt_open;
        if (st->mgmt == MG_NEW || st->mgmt == MG_RENAME || st->mgmt == MG_DUP) { do_mgmt_name(app, st, &open); }
        else if (st->mgmt == MG_DELETE) {
            char msg[200];
            int res;
            lt_snprintf(msg, sizeof msg, "Delete the dashboard '%s'? This cannot be undone.", st->mgmt_name);
            res = lt_ui_msgbox(ui, "mg_del", "Delete Dashboard", msg, "Delete|Cancel", &open);
            if (res == 1) {
                int was_current = strcmp(st->mgmt_target, st->path) == 0;
                if (lt_dashlib_delete(st->mgmt_target) == 0) {
                    lt_app_toast(app, 1, "Deleted %s", st->mgmt_name);
                    st->lib_valid = 0;
                    if (was_current) { lt_dtab_reset_defaults(app, st); }
                } else { lt_app_toast(app, 3, "%s", lt_err_get()); }
            }
        } else if (st->mgmt == MG_RESET) {
            int res = lt_ui_msgbox(ui, "mg_reset", "Reload Default Gauges",
                                   "Replace the current dashboard with the default gauges of this ECU definition? "
                                   "Unsaved changes are lost.", "Reload|Cancel", &open);
            if (res == 1) { lt_dtab_reset_defaults(app, st); st->lib_valid = 0; }
        }
        st->mgmt_open = open;
    }
}

/* ---- compatibility bar and validation panel ---- */
void lt_dpanel_compat_bar(lt_app *app, dash_state *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w2[2];
    char msg[300];
    lt_rect r;
    if (!st->compat_valid || st->compat_hidden || st->compat.n_unresolved == 0) { return; }
    {
        char list[200];
        size_t i;
        list[0] = '\0';
        for (i = 0; i < st->compat.n_unresolved && strlen(list) < 120; i++) {
            lt_snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ", " : "", st->compat.unresolved[i]);
        }
        lt_snprintf(msg, sizeof msg, "Compatibility %d%%: %lu of %lu channels are not in this ECU definition (%s%s)",
                    st->compat.percent, (unsigned long)st->compat.n_unresolved, (unsigned long)st->compat.n_channels, list,
                    st->compat.n_unresolved > 6 ? ", ..." : "");
    }
    w2[0] = -1; w2[1] = 84;
    lt_ui_layout_row(ui, 2, w2, 0);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_box(ui, r, t->elevated, t->warning, 4);
    lt_ui_draw_text_in(ui, lt_rect_inset(r, 6, 0), msg, t->warning, LT_UI_ALIGN_LEFT);
    if (lt_ui_button_ex(ui, "Dismiss", LT_UI_BTN_FLAT)) { st->compat_hidden = 1; }
}

void lt_dpanel_validation(lt_app *app, dash_state *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    char title[96];
    int one = -1, pass, any = 0;
    size_t i, ne = 0;
    if (!st->valid_open) { return; }
    if (!st->rep_valid) { lt_dtab_revalidate(app, st); }
    for (i = 0; i < st->rep.n; i++) { if (st->rep.problems[i].is_error) { ne++; } }
    if (st->valid_rect.w == 0) {
        st->valid_rect = lt_rect_make(st->area.x + lt_ui_px(ui, 40), st->area.y + lt_ui_px(ui, 20), lt_ui_px(ui, 520), lt_ui_px(ui, 320));
    }
    lt_snprintf(title, sizeof title, "Validation: %lu error(s), %lu warning(s)", (unsigned long)ne, (unsigned long)(st->rep.n - ne));
    if (!lt_ui_window_begin(ui, title, &st->valid_rect, &st->valid_open, LT_UI_WIN_RESIZE)) { return; }
    lt_ui_layout_row(ui, 1, &one, 0);
    if (st->rep.n == 0) { lt_ui_label_ex(ui, "No issues detected.", t->success, LT_UI_ALIGN_LEFT); }
    for (pass = 0; pass < 2; pass++) {
        any = 0;
        for (i = 0; i < st->rep.n; i++) {
            const lt_dash_issue *is = &st->rep.problems[i];
            char row[260];
            if ((pass == 0) != (is->is_error ? 1 : 0)) { continue; }
            if (!any) {
                lt_ui_layout_row(ui, 1, &one, 0);
                lt_ui_label_ex(ui, pass == 0 ? "Errors" : "Warnings", pass == 0 ? t->error : t->warning, LT_UI_ALIGN_LEFT);
                any = 1;
            }
            lt_snprintf(row, sizeof row, "%s%s%s", is->gauge_id, is->gauge_id[0] ? ": " : "", is->message);
            lt_ui_layout_row(ui, 1, &one, 0);
            lt_ui_push_id_int(ui, (int)i);
            if (lt_ui_button_ex(ui, row, LT_UI_BTN_FLAT)) {
                size_t k;
                for (k = 0; k < st->file->cluster.n_comps; k++) {
                    const char *id = st->file->cluster.comps[k].gauge ? st->file->cluster.comps[k].gauge->id
                                                                      : st->file->cluster.comps[k].indicator->id;
                    if (id != NULL && is->gauge_id[0] && strcmp(id, is->gauge_id) == 0) {
                        st->designer = 1; lt_dtab_select(st, (int)k, 0); break;
                    }
                }
            }
            lt_ui_pop_id(ui);
        }
    }
    lt_ui_window_end(ui);
}

/* ---- layers ---- */
void lt_dpanel_layers(lt_app *app, dash_state *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int one = -1, w[5], i, move_from = -1, move_to = -1, del = -1, hide = -1, pick = -1, scroll_ok;
    size_t n;
    if (!st->layers_open) { return; }
    if (st->layers_rect.w == 0) {
        st->layers_rect = lt_rect_make(st->area.x + st->area.w - lt_ui_px(ui, 300), st->area.y + lt_ui_px(ui, 10), lt_ui_px(ui, 290), lt_ui_px(ui, 360));
    }
    if (!lt_ui_window_begin(ui, "Layers", &st->layers_rect, &st->layers_open, LT_UI_WIN_RESIZE)) { return; }
    n = st->file->cluster.n_comps;
    lt_ui_layout_row(ui, 1, &one, 0);
    if (n == 0) { lt_ui_label_ex(ui, "No components yet", t->muted, LT_UI_ALIGN_LEFT); }
    else { lt_ui_label_ex(ui, "Top of the list is drawn on top", t->muted, LT_UI_ALIGN_LEFT); }
    w[0] = -1; w[1] = 26; w[2] = 26; w[3] = 26; w[4] = 26;
    lt_ui_layout_row(ui, 1, &one, -1);
    scroll_ok = lt_ui_scroll_begin(ui, "ly_sc");
    for (i = (int)n - 1; i >= 0; i--) {
        lt_dash_comp *c = &st->file->cluster.comps[i];
        char lab[140], nm[110];
        int hidden = lt_dash_comp_hidden(c) ? 1 : 0;
        lt_dash_comp_label(c, nm, sizeof nm);
        lt_snprintf(lab, sizeof lab, "%s%s", nm, hidden ? "  (hidden)" : "");
        lt_ui_push_id_int(ui, i);
        lt_ui_layout_row(ui, 5, w, 0);
        if (lt_ui_button_ex(ui, lab, LT_UI_BTN_FLAT | (lt_dtab_is_sel(st, i) ? LT_UI_BTN_SELECTED : 0))) { pick = i; }
        if (lt_ui_icon_button(ui, LT_UI_LUCIDE(LT_ICON_ARROW_UP), "Move up", i == (int)n - 1 ? LT_UI_BTN_DISABLED : LT_UI_BTN_FLAT)) { move_from = i; move_to = i + 1; }
        if (lt_ui_icon_button(ui, LT_UI_LUCIDE(LT_ICON_ARROW_DOWN), "Move down", i == 0 ? LT_UI_BTN_DISABLED : LT_UI_BTN_FLAT)) { move_from = i; move_to = i - 1; }
        if (lt_ui_icon_button(ui, LT_UI_LUCIDE((hidden ? LT_ICON_EYE_OFF : LT_ICON_EYE)), hidden ? "Show" : "Hide", LT_UI_BTN_FLAT)) { hide = i; }
        if (lt_ui_icon_button(ui, LT_UI_LUCIDE(LT_ICON_TRASH_2), "Delete", LT_UI_BTN_FLAT)) { del = i; }
        lt_ui_pop_id(ui);
    }
    if (scroll_ok) { lt_ui_scroll_end(ui); }
    lt_ui_window_end(ui);
    if (pick >= 0) {
        int add = (lt_ui_mods(ui) & (LT_MOD_SHIFT | LT_MOD_CTRL)) != 0;
        lt_dtab_select(st, pick, add);
    }
    if (move_from >= 0) {
        size_t ni = lt_dash_comp_move_z(st->file, (size_t)move_from, (size_t)move_to);
        st->rebuild = 1; lt_dtab_select(st, (int)ni, 0); lt_dtab_touch(st);
    }
    if (hide >= 0) {
        lt_dash_comp_set_hidden(&st->file->cluster.comps[hide], lt_dash_comp_hidden(&st->file->cluster.comps[hide]) ? LT_FALSE : LT_TRUE);
        st->rebuild = 1; lt_dtab_touch(st);
    }
    if (del >= 0) { lt_dtab_select(st, del, 0); lt_dtab_remove_sel(st); }
}

/* ---- palette ---- */
static const char *const pal_cats[5] = { "All", "Dials", "Bars", "Lines and graphs", "Readouts" };

static int painter_cat(int p)
{
    switch (p) {
    case LT_GP_ANALOG: case LT_GP_BASIC_ANALOG: case LT_GP_CIRCLE_ANALOG: case LT_GP_ASYM_SWEEP:
    case LT_GP_ROUND: case LT_GP_ROUND_DASHED: case LT_GP_FUEL_METER: case LT_GP_TACHOMETER: return 1;
    case LT_GP_HBAR: case LT_GP_VBAR: case LT_GP_ANALOG_BAR: case LT_GP_ANALOG_MOVING_BAR: return 2;
    case LT_GP_HLINE: case LT_GP_HDASHED: case LT_GP_VDASHED: case LT_GP_HISTOGRAM:
    case LT_GP_LINE_GRAPH: case LT_GP_MULTI_TREND: return 3;
    default: return 4;
    }
}

typedef struct pal_tile { lt_dash_gauge g; double value; } pal_tile;
static pal_tile *g_tiles;       /* one preview gauge per painter (draw callbacks run after the frame function) */

static void tile_cb(lt_gfx *g, lt_rect r, void *user)
{
    pal_tile *pt = (pal_tile *)user;
    lt_dash_render_gauge(g, r, &pt->g, pt->value);
}

static void tiles_init(void)
{
    int i;
    if (g_tiles != NULL) { return; }
    g_tiles = (pal_tile *)lt_calloc(DT_N_PAINTERS, sizeof(pal_tile));
    for (i = 0; i < DT_N_PAINTERS; i++) {
        lt_dash_gauge *g = &g_tiles[i].g;
        lt_dash_gauge_init(g);
        g->painter = (lt_gauge_painter)i;
        g->min = 0.0; g->max = 100.0;
        g->back_color = LT_RGB(28, 32, 40); g->font_color = LT_RGB(255, 255, 255);
        g->trim_color = LT_RGB(148, 163, 184); g->needle_color = LT_RGB(74, 158, 248);
        g->warn_color = LT_RGB(234, 179, 8); g->critical_color = LT_RGB(239, 68, 68);
        dash_set_str(&g->title, "");
        dash_set_str(&g->units, "");
        g_tiles[i].value = 62.0;
    }
}

void lt_dpanel_palette(lt_app *app, dash_state *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int one = -1, w3[3], i, col = 0, shown = 0, add_g = 0, add_i = 0, down, m_in, scroll_ok;
    lt_point m;
    size_t k;
    if (!st->palette_open) { st->pal_drag = 0; st->pal_prev = 0; return; }
    tiles_init();
    if (st->palette_rect.w == 0) {
        st->palette_rect = lt_rect_make(st->area.x + st->area.w - lt_ui_px(ui, 360), st->area.y + lt_ui_px(ui, 10), lt_ui_px(ui, 350), lt_ui_px(ui, 520));
    }
    if (!lt_ui_window_begin(ui, "Gauge Palette", &st->palette_rect, &st->palette_open, LT_UI_WIN_RESIZE)) { return; }
    m = lt_ui_mouse(ui);
    down = lt_ui_mouse_down(ui, LT_MB_LEFT);
    lt_ui_layout_row(ui, 1, &one, 0);
    (void)lt_ui_combo(ui, "pal_cat", &st->pal_cat, pal_cats, 5);
    lt_ui_layout_row(ui, 1, &one, -1);
    scroll_ok = lt_ui_scroll_begin(ui, "pal_sc");
    w3[0] = -1; w3[1] = -1; w3[2] = -1;
    for (i = 0; i < DT_N_PAINTERS; i++) {
        lt_rect tr;
        int hover;
        if (st->pal_cat > 0 && painter_cat(i) != st->pal_cat) { continue; }
        if (col == 0) { lt_ui_layout_row(ui, 3, w3, 78); }
        tr = lt_ui_layout_next(ui);
        hover = lt_rect_contains(tr, m.x, m.y) && lt_rect_contains(lt_ui_scroll_view(ui), m.x, m.y);
        lt_ui_draw_box(ui, tr, i == st->pal_sel ? t->selected : (hover ? t->hover : t->surface), i == st->pal_sel ? t->accent : t->border, 4);
        if (!lt_ui_cell_mode(ui)) {
            lt_ui_draw_custom(ui, lt_rect_make(tr.x + 4, tr.y + 3, tr.w - 8, tr.h - 20), tile_cb, &g_tiles[i]);
        }
        lt_ui_draw_text_in(ui, lt_rect_make(tr.x, tr.y + tr.h - lt_ui_px(ui, 17), tr.w, lt_ui_px(ui, 16)),
                           lt_painter_to_ts((lt_gauge_painter)i), t->text2, LT_UI_ALIGN_CENTER);
        if (hover && down && !st->pal_prev && !st->pal_drag) { st->pal_sel = i; st->pal_drag = i + 1; }
        shown++;
        col = (col + 1) % 3;
    }
    for (; col != 0 && col < 3; col++) { (void)lt_ui_layout_next(ui); }
    LT_UNUSED(shown);
    lt_ui_layout_row(ui, 1, &one, 0);
    lt_ui_label_ex(ui, "Channel (used by new gauges and indicators)", t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &one, 0);
    (void)lt_ui_text(ui, "pal_filter", st->pal_filter, sizeof st->pal_filter, 0);
    if (app->ini != NULL && app->ini->n_channels > 0) {
        const char **match = (const char **)lt_malloc(sizeof(char *) * (app->ini->n_channels + lt_datalog_virtual_count() + 1));
        int nm = 0, sel = 0, ch;
        char cur[80];
        lt_snprintf(cur, sizeof cur, "%s", st->pal_name);
        for (k = 0; k < app->ini->n_channels + lt_datalog_virtual_count(); k++) {   /* + live math channels */
            const char *nme = k < app->ini->n_channels ? app->ini->channels[k]->name
                                                       : lt_datalog_virtual_name(k - app->ini->n_channels);
            if (st->pal_filter[0] == '\0' || strstr(nme, st->pal_filter) != NULL) {
                if (strcmp(nme, st->pal_name) == 0) { sel = nm; }
                match[nm++] = nme;
            }
        }
        lt_ui_layout_row(ui, 1, &one, 110);
        if (nm > 0) {
            ch = lt_ui_listbox(ui, "pal_chans", &sel, match, nm);
            if ((ch & LT_UI_CHANGED) && sel >= 0 && sel < nm) { lt_snprintf(st->pal_name, sizeof st->pal_name, "%s", match[sel]); }
        } else { lt_ui_label_ex(ui, "No matching channels", t->muted, LT_UI_ALIGN_LEFT); }
        lt_free((void *)match);
    }
    w3[0] = -1; w3[1] = -1;
    lt_ui_layout_row(ui, 2, w3, 0);
    if (lt_ui_button_ex(ui, "Add gauge", LT_UI_BTN_PRIMARY)) { add_g = 1; }
    if (lt_ui_button(ui, "Add indicator")) { add_i = 1; }
    lt_ui_layout_row(ui, 1, &one, 0);
    lt_ui_label_ex(ui, "Drag a tile onto the dashboard to place it.", t->muted, LT_UI_ALIGN_LEFT);
    if (scroll_ok) { lt_ui_scroll_end(ui); }
    lt_ui_window_end(ui);
    m_in = lt_rect_contains(st->palette_rect, m.x, m.y);
    st->pal_prev = down;
    if (add_g) { lt_dtab_add_gauge(app, st, 0.5, 0.5, (lt_gauge_painter)st->pal_sel, st->pal_name); }
    if (add_i) { lt_dtab_add_indicator(app, st, 0.5, 0.5, st->pal_name); }
    if (st->pal_drag > 0) {
        if (!down) {
            if (!m_in && lt_rect_contains(st->area, m.x, m.y) && st->cluster.w > 0 && st->cluster.h > 0) {
                double fx = (double)(m.x - st->cluster.x) / st->cluster.w, fy = (double)(m.y - st->cluster.y) / st->cluster.h;
                lt_dtab_add_gauge(app, st, fx, fy, (lt_gauge_painter)(st->pal_drag - 1), st->pal_name);
            }
            st->pal_drag = 0;
        } else {
            lt_rect gr = lt_rect_make(m.x - lt_ui_px(ui, 40), m.y - lt_ui_px(ui, 30), lt_ui_px(ui, 80), lt_ui_px(ui, 60));
            lt_ui_draw_box(ui, gr, LT_RGBA(66, 153, 255, 90), t->accent, 4);
            lt_ui_draw_text_in(ui, gr, lt_painter_to_ts((lt_gauge_painter)(st->pal_drag - 1)), t->text, LT_UI_ALIGN_CENTER);
            lt_ui_request_redraw(ui);
        }
    }
}

/* ---- assets ---- */
void lt_dpanel_assets(lt_app *app, dash_state *st)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    lt_dash_cluster *c = &st->file->cluster;
    int one = -1, w2[2], i, bgsel = 0;
    static const char *const styles[4] = { "Tile", "Stretch", "Center", "Fit" };
    const char **items;
    if (!st->assets_open) { return; }
    if (st->assets_rect.w == 0) {
        st->assets_rect = lt_rect_make(st->area.x + lt_ui_px(ui, 60), st->area.y + lt_ui_px(ui, 30), lt_ui_px(ui, 420), lt_ui_px(ui, 380));
    }
    if (!lt_ui_window_begin(ui, "Assets", &st->assets_rect, &st->assets_open, LT_UI_WIN_RESIZE)) { return; }
    lt_ui_layout_row(ui, 1, &one, 0);
    lt_ui_label_ex(ui, "Images and fonts embedded in this dashboard", t->muted, LT_UI_ALIGN_LEFT);
    items = (const char **)lt_calloc(c->n_images + 1, sizeof(char *));
    {
        char (*labs)[100] = (char (*)[100])lt_calloc(c->n_images + 1, 100);
        for (i = 0; i < (int)c->n_images; i++) {
            const lt_dash_image *im = &c->images[i];
            lt_snprintf(labs[i], 100, "%s   [%s]%s", im->image_id ? im->image_id : "?",
                        im->type == LT_RES_TTF ? "font" : (im->type == LT_RES_GIF ? "gif" : "png"),
                        lt_dash_asset_used(st->file, (size_t)i) ? "  in use" : "");
            items[i] = labs[i];
        }
        lt_ui_layout_row(ui, 1, &one, 130);
        if (c->n_images > 0) { (void)lt_ui_listbox(ui, "as_list", &st->asset_sel, items, (int)c->n_images); }
        else { lt_ui_label_ex(ui, "No embedded assets", t->muted, LT_UI_ALIGN_LEFT); }
        lt_free(labs);
    }
    lt_free((void *)items);
    w2[0] = -1; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    if (lt_ui_button(ui, "Add file...")) { lt_dpanel_request(app, st, DP_ADD_ASSET); }
    if (lt_ui_button_ex(ui, "Delete", (st->asset_sel >= 0 && st->asset_sel < (int)c->n_images) ? LT_UI_BTN_DANGER : LT_UI_BTN_DISABLED)) {
        lt_dash_asset_remove(st->file, (size_t)st->asset_sel);
        if (st->asset_sel >= (int)c->n_images) { st->asset_sel = (int)c->n_images - 1; }
        lt_dtab_touch(st);
    }
    lt_ui_layout_row(ui, 1, &one, 0);
    lt_ui_separator(ui);
    lt_ui_label_ex(ui, "Dashboard background", t->text2, LT_UI_ALIGN_LEFT);
    {
        const char **bi = (const char **)lt_calloc(c->n_images + 2, sizeof(char *));
        int n = 1;
        bi[0] = "(none)";
        for (i = 0; i < (int)c->n_images; i++) {
            if (c->images[i].type == LT_RES_TTF) { continue; }
            bi[n] = c->images[i].image_id ? c->images[i].image_id : "?";
            if (c->background_image_file_name != NULL && strcmp(c->background_image_file_name, bi[n]) == 0) { bgsel = n; }
            n++;
        }
        w2[0] = CH * 10; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Image");
        if (lt_ui_combo(ui, "as_bg", &bgsel, bi, n)) {
            dash_set_str(&c->background_image_file_name, bgsel == 0 ? NULL : bi[bgsel]);
            lt_dtab_touch(st);
        }
        lt_free((void *)bi);
    }
    {
        int sty = (int)c->background_image_style;
        lt_ui_label(ui, "Style");
        if (lt_ui_combo(ui, "as_sty", &sty, styles, 4)) { c->background_image_style = (lt_bg_style)sty; lt_dtab_touch(st); }
    }
    lt_ui_window_end(ui);
}

/* ---- zoom ---- */
typedef struct zoom_ud { dash_state *st; int idx; } zoom_ud;
static zoom_ud g_zoom;

static void zoom_cb(lt_gfx *g, lt_rect r, void *user)
{
    zoom_ud *z = (zoom_ud *)user;
    const lt_dash_comp *c;
    const lt_dash_gstate *gs;
    lt_gfx_fill_rect(g, r, LT_RGB(0, 0, 0));
    if (z->st == NULL || z->st->file == NULL || z->idx < 0 || (size_t)z->idx >= z->st->file->cluster.n_comps) { return; }
    c = &z->st->file->cluster.comps[z->idx];
    gs = lt_dash_view_state(z->st->view, (size_t)z->idx);
    if (c->gauge != NULL) { lt_dash_render_gauge(g, r, c->gauge, gs != NULL ? gs->display : c->gauge->value); }
    else { lt_dash_render_indicator(g, r, c->indicator, gs != NULL ? gs->on : LT_FALSE); }
}

void lt_dpanel_zoom(lt_app *app, dash_state *st)
{
    lt_ui *ui = app->ui;
    int one = -1;
    char title[120];
    lt_rect body;
    if (!st->zoom_open) { return; }
    if (st->zoom_idx < 0 || (size_t)st->zoom_idx >= st->file->cluster.n_comps) { st->zoom_open = 0; return; }
    if (st->zoom_rect.w == 0) {
        st->zoom_rect = lt_rect_make(st->area.x + st->area.w / 2 - lt_ui_px(ui, 220), st->area.y + lt_ui_px(ui, 20), lt_ui_px(ui, 440), lt_ui_px(ui, 460));
    }
    lt_dash_comp_label(&st->file->cluster.comps[st->zoom_idx], title, sizeof title);
    if (!lt_ui_window_begin(ui, title, &st->zoom_rect, &st->zoom_open, LT_UI_WIN_RESIZE)) { return; }
    lt_ui_layout_row(ui, 1, &one, -1);
    body = lt_ui_layout_next(ui);
    g_zoom.st = st; g_zoom.idx = st->zoom_idx;
    lt_ui_draw_custom(ui, body, zoom_cb, &g_zoom);
    lt_ui_window_end(ui);
    lt_ui_request_redraw(ui);
}

/* ---- alignment bar ---- */
void lt_dpanel_align_bar(lt_app *app, dash_state *st)
{
    lt_ui *ui = app->ui;
    size_t idx[512], n = lt_dtab_sel_list(st, idx, 512);
    lt_vk_item it[12];
    static const int ops[10] = { LT_ALIGN_LEFT, LT_ALIGN_HCENTER, LT_ALIGN_RIGHT, LT_ALIGN_TOP, LT_ALIGN_VCENTER,
                                 LT_ALIGN_BOTTOM, LT_ALIGN_DIST_H, LT_ALIGN_DIST_V, LT_ALIGN_SAME_W, LT_ALIGN_SAME_H };
    static const int icons[10] = { LT_ICON_ALIGN_START_VERTICAL, LT_ICON_ALIGN_CENTER_VERTICAL, LT_ICON_ALIGN_END_VERTICAL,
                                   LT_ICON_ALIGN_START_HORIZONTAL, LT_ICON_ALIGN_CENTER_HORIZONTAL, LT_ICON_ALIGN_END_HORIZONTAL,
                                   LT_ICON_MOVE_HORIZONTAL, LT_ICON_MOVE_VERTICAL, LT_ICON_GRIP_HORIZONTAL, LT_ICON_GRIP_VERTICAL };
    static const char *const tips[10] = { "Align left", "Center horizontally", "Align right", "Align top", "Center vertically",
                                          "Align bottom", "Distribute horizontally", "Distribute vertically",
                                          "Same width as the first", "Same height as the first" };
    static const char *const labs[10] = { "Left", "Center", "Right", "Top", "Middle", "Bottom", "Spread H", "Spread V", "Same W", "Same H" };
    int i, hit, w1 = -1;
    lt_rect rr;
    if (n < 2) { return; }
    lt_ui_layout_row(ui, 1, &w1, lt_ui_cell_mode(ui) ? 0 : 30);
    rr = lt_ui_layout_next(ui);
    for (i = 0; i < 10; i++) {
        memset(&it[i], 0, sizeof it[i]);
        it[i].kind = LT_VK_BTN; it[i].icon = icons[i]; it[i].tip = tips[i];
        it[i].label = lt_ui_cell_mode(ui) ? labs[i] : NULL;
    }
    it[10].kind = LT_VK_END;
    hit = lt_vk_toolbar_in(ui, it, rr);
    if (hit >= 0) {
        lt_dash_align(st->file, idx, n, (lt_align_op)ops[hit]);
        lt_dtab_touch(st);
    }
}
