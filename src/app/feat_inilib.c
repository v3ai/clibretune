/* feat_inilib.c - INI library management: Settings -> "ECU Definitions" lists the imported
 * ECU definitions (INI files) with signature, imports a file, scans a folder, removes entries and
 * shows the storage path and the open project's INI.  (G-11)
 * Lookup by signature for other screens: project/inilib.h (lt_inilib_find).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "../project/inilib.h"

#define FD_IMPORT (LT_APP_FD_USER0 + 40)
#define FD_SCAN   (LT_APP_FD_USER0 + 41)

typedef struct inilib_state {
    lt_ini_repo repo;
    int  loaded;          /* repo is open */
    int  drawn;           /* page was drawn during this frame */
    int  confirm;         /* entry index awaiting "Confirm Remove", -1 none */
    char msg[200];
    int  msg_err;
} inilib_state;

static void st_close(inilib_state *s)
{
    if (s->loaded) { lt_ini_repo_close(&s->repo); }
    s->loaded = 0;
}

static void st_free(lt_app *app, void *p)
{
    LT_UNUSED(app);
    st_close((inilib_state *)p);
}

static inilib_state *get_state(lt_app *app)
{
    return (inilib_state *)lt_ext_state(app, "inilib", sizeof(inilib_state), st_free);
}

static void st_load(inilib_state *s)
{
    st_close(s);
    s->confirm = -1;
    if (lt_ini_repo_open(&s->repo, NULL) == 0) { s->loaded = 1; }
    else { lt_snprintf(s->msg, sizeof(s->msg), "Cannot open the INI library: %s", lt_err_get()); s->msg_err = 1; }
}

/* the library is reloaded the next time the page is shown */
static void inilib_tick(lt_app *app)
{
    inilib_state *s = get_state(app);
    if (!s->drawn && s->loaded) { st_close(s); }
    s->drawn = 0;
}

static void dir_of(const char *path, char *out, size_t cap)
{
    const char *base = lt_path_base(path);
    size_t n = (size_t)(base - path);
    if (n == 0) { lt_strlcpy(out, ".", cap); return; }
    if (n >= cap) { n = cap - 1; }
    memcpy(out, path, n);
    out[n] = '\0';
    if (n > 1 && (out[n - 1] == '/' || out[n - 1] == '\\')) { out[n - 1] = '\0'; }
}

static void do_import(inilib_state *s, const char *path)
{
    char *id = NULL;
    size_t before = s->repo.n;
    if (lt_ini_repo_import(&s->repo, path, &id) == 0) {
        lt_snprintf(s->msg, sizeof(s->msg), s->repo.n > before ? "Imported %s" : "%s is already in the library",
                    lt_path_base(path));
        s->msg_err = 0;
    } else {
        lt_snprintf(s->msg, sizeof(s->msg), "Failed to import INI: %s", lt_err_get());
        s->msg_err = 1;
    }
    lt_free(id);
}

static void do_scan(inilib_state *s, const char *file)
{
    char dir[512], **ids = NULL;
    size_t n = 0, i;
    dir_of(file, dir, sizeof(dir));
    if (lt_ini_repo_scan_dir(&s->repo, dir, &ids, &n) == 0) {
        lt_snprintf(s->msg, sizeof(s->msg), "Scanned %s: %lu definition%s found", dir, (unsigned long)n,
                    n == 1 ? "" : "s");
        s->msg_err = 0;
    } else {
        lt_snprintf(s->msg, sizeof(s->msg), "Failed to scan folder: %s", lt_err_get());
        s->msg_err = 1;
    }
    for (i = 0; i < n; i++) { lt_free(ids[i]); }
    lt_free(ids);
}

static void inilib_page(lt_app *app)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    inilib_state *s = get_state(app);
    const char *picked;
    int w1 = -1, wb[3], i, rm = -1;
    char line[600];
    s->drawn = 1;
    if (!s->loaded) { st_load(s); }
    picked = lt_app_file_result(app, FD_IMPORT);
    if (picked != NULL && s->loaded) { do_import(s, picked); }
    picked = lt_app_file_result(app, FD_SCAN);
    if (picked != NULL && s->loaded) { do_scan(s, picked); }

    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
    lt_ui_label_ex(ui, "Imported ECU Definitions (INI Files)", t->text, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "Manage the ECU definition files available for projects.", t->muted, LT_UI_ALIGN_LEFT);
    wb[0] = 150; wb[1] = 150; wb[2] = -1;
    lt_ui_layout_row(ui, 3, wb, 0);
    if (lt_ui_button_ex(ui, "Import INI File...", LT_UI_BTN_DEFAULT)) {
        lt_app_file_dialog(app, FD_IMPORT, LT_UI_FILE_OPEN, NULL, "", "INI Files|*.ini|All Files|*");
    }
    if (lt_ui_button(ui, "Scan Folder...")) {
        lt_app_file_dialog(app, FD_SCAN, LT_UI_FILE_OPEN, NULL, "",
                           "INI Files (pick any file in the folder)|*.ini|All Files|*");
    }
    lt_ui_label(ui, "");
    if (s->msg[0] != '\0') {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, s->msg, s->msg_err ? t->error : t->success, LT_UI_ALIGN_LEFT);
    }
    if (s->loaded) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_snprintf(line, sizeof(line), "Library folder: %s", s->repo.dir != NULL ? s->repo.dir : "");
        lt_ui_label_ex(ui, line, t->text2, LT_UI_ALIGN_LEFT);
    }
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (app->has_project) {
        lt_snprintf(line, sizeof(line), "Project INI: %s  [%s]", app->ini_path,
                    app->project.cfg.signature != NULL ? app->project.cfg.signature : "");
    } else if (app->ini_path[0] != '\0') {
        lt_snprintf(line, sizeof(line), "Open definition: %s", app->ini_path);
    } else {
        lt_strlcpy(line, "No project open.", sizeof(line));
    }
    lt_ui_label_ex(ui, line, t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_separator(ui);

    if (!s->loaded || s->repo.n == 0) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "No ECU definitions imported yet.", t->muted, LT_UI_ALIGN_CENTER);
        return;
    }
    for (i = 0; i < (int)s->repo.n; i++) {
        const lt_ini_entry *e = &s->repo.e[i];
        int wr[2];
        lt_ui_push_id_int(ui, i);
        wr[0] = -1; wr[1] = 130;
        lt_ui_layout_row(ui, 2, wr, 0);
        lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
        lt_ui_label_ex(ui, e->name != NULL ? e->name : e->id, t->text, LT_UI_ALIGN_LEFT);
        lt_ui_text_style_reset(ui);
        if (lt_ui_button_ex(ui, s->confirm == i ? "Confirm Remove" : "Remove",
                            s->confirm == i ? LT_UI_BTN_DANGER : 0)) {
            if (s->confirm == i) { rm = i; } else { s->confirm = i; }
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, e->signature != NULL ? e->signature : "", t->muted, LT_UI_ALIGN_LEFT);
        lt_ui_pop_id(ui);
    }
    if (rm >= 0) {
        char *id = lt_strdup(s->repo.e[rm].id);
        if (lt_ini_repo_remove(&s->repo, id) == 0) {
            lt_snprintf(s->msg, sizeof(s->msg), "Removed %s", id);
            s->msg_err = 0;
        } else {
            lt_snprintf(s->msg, sizeof(s->msg), "Failed to remove INI: %s", lt_err_get());
            s->msg_err = 1;
        }
        lt_free(id);
        s->confirm = -1;
    }
}

void lt_feat_inilib_register(lt_app_ext *reg)
{
    lt_ext_add_settings_page(reg, "ECU Definitions", inilib_page);
    lt_ext_add_tick(reg, inilib_tick);
}
