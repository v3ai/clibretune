/* dialogs_newproj.c - New project wizard.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dialogs.h"

#define CH 8
#define CV 16

/* ================================================================== */
/* New project wizard                                                  */
typedef struct newproj_state {
    char name[64], dir[512], ini[512];
    lt_ini_repo repo; int repo_ok;
    int source;          /* 0 repository, 1 file */
    int repo_sel;
    int basemap;
    char err[160];
} newproj_state;

static const char *repo_get(void *ud, int i)
{
    newproj_state *s = (newproj_state *)ud;
    static char buf[256];
    lt_snprintf(buf, sizeof(buf), "%s   [%s]", s->repo.e[i].name != NULL ? s->repo.e[i].name : s->repo.e[i].id,
                s->repo.e[i].signature != NULL ? s->repo.e[i].signature : "");
    return buf;
}

static int newproj_create(lt_app *app, newproj_state *s)
{
    lt_project p;
    lt_ini *probe;
    char *ini_path = NULL, *pdir;
    int rc;
    if (s->source == 0) {
        if (!s->repo_ok || s->repo.n == 0) { lt_strlcpy(s->err, "No definition selected", sizeof(s->err)); return -1; }
        ini_path = lt_ini_repo_path(&s->repo, s->repo.e[s->repo_sel].id);
    } else {
        if (s->ini[0] == '\0') { lt_strlcpy(s->err, "Choose an INI file", sizeof(s->err)); return -1; }
        ini_path = lt_strdup(s->ini);
    }
    if (ini_path == NULL) { lt_strlcpy(s->err, "Cannot resolve the definition file", sizeof(s->err)); return -1; }
    probe = lt_ini_load_file(ini_path, NULL);
    if (probe == NULL) {
        lt_snprintf(s->err, sizeof(s->err), "Cannot load INI: %s", lt_err_get());
        lt_free(ini_path);
        return -1;
    }
    if (s->dir[0] != '\0') { lt_plat_mkdirs(s->dir); }
    rc = lt_project_create(s->name, ini_path, probe->signature != NULL ? probe->signature : "",
                           s->dir[0] != '\0' ? s->dir : NULL, &p);
    lt_ini_free(probe);
    lt_free(ini_path);
    if (rc < 0) { lt_snprintf(s->err, sizeof(s->err), "Cannot create project: %s", lt_err_get()); return rc; }
    pdir = lt_strdup(p.path);
    lt_project_close(&p);
    rc = lt_app_open_project(app, pdir);
    lt_free(pdir);
    return rc;
}

static void newproj_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    newproj_state *s = (newproj_state *)d->data;
    int open = d->open, w2[2], w1 = -1, ok;
    const char *picked;
    if (s == NULL) {
        char *pd;
        s = (newproj_state *)lt_calloc(1, sizeof(*s));
        d->data = s;
        lt_strlcpy(s->name, "My Project", sizeof(s->name));
        pd = lt_projects_dir();
        if (pd != NULL) { lt_strlcpy(s->dir, pd, sizeof(s->dir)); lt_free(pd); }
        s->repo_ok = lt_ini_repo_open(&s->repo, NULL) == 0;
        if (s->repo_ok && s->repo.n == 0) { s->source = 1; }
        if (app->ini_path[0] != '\0' && s->source == 1) { lt_strlcpy(s->ini, app->ini_path, sizeof(s->ini)); }
    }
    picked = lt_app_file_result(app, LT_DLG_FD_PROJ_INI);
    if (picked != NULL) { lt_strlcpy(s->ini, picked, sizeof(s->ini)); s->source = 1; }
    if (lt_ui_modal_begin(ui, "New Project", CH * 76, 420, &open)) {
        w2[0] = CH * 16; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Project name");
        lt_ui_text(ui, "name", s->name, sizeof(s->name), 0);
        lt_ui_label(ui, "Directory");
        lt_ui_text(ui, "dir", s->dir, sizeof(s->dir), 0);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "ECU definition", t->text2, LT_UI_ALIGN_LEFT);
        lt_ui_radio(ui, "From the local definition repository", &s->source, 0);
        lt_ui_radio(ui, "From an INI file", &s->source, 1);
        if (s->source == 0) {
            lt_ui_layout_row(ui, 1, &w1, CV * 6);
            if (s->repo_ok && s->repo.n > 0) {
                lt_ui_listbox_cb(ui, "repo", &s->repo_sel, (int)s->repo.n, repo_get, s);
            } else {
                lt_ui_label_ex(ui, "The local definition repository is empty. Import an INI file first.",
                               t->muted, LT_UI_ALIGN_LEFT);
            }
        } else {
            lt_ui_layout_row(ui, 2, w2, 0);
            if (lt_ui_button(ui, "Browse...")) {
                lt_app_file_dialog(app, LT_DLG_FD_PROJ_INI, LT_UI_FILE_OPEN, NULL, "",
                                   "ECU definitions|*.ini|All files|*");
            }
            lt_ui_text(ui, "inipath", s->ini, sizeof(s->ini), 0);
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_checkbox(ui, "Open the base map wizard after creating", &s->basemap);
        if (s->err[0] != '\0') { lt_ui_label_ex(ui, s->err, t->error, LT_UI_ALIGN_LEFT); }
        ok = s->name[0] != '\0' && (s->source == 1 ? s->ini[0] != '\0' : (s->repo_ok && s->repo.n > 0));
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button_ex(ui, "Create", ok ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED) && ok) {
            if (newproj_create(app, s) == 0) {
                open = 0;
                if (s->basemap) { lt_app_basemap_wizard(app); }
            }
        }
        if (lt_ui_button(ui, "Cancel")) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void newproj_close(lt_app *app, lt_app_dialog *d)
{
    newproj_state *s = (newproj_state *)d->data;
    LT_UNUSED(app);
    if (s != NULL) {
        if (s->repo_ok) { lt_ini_repo_close(&s->repo); }
        lt_free(s);
    }
    d->data = NULL;
}

void lt_dlg_register_newproj(lt_app *app)
{
    lt_app_dialog_kind dk;
    dk.kind = "newproject"; dk.draw = newproj_draw; dk.close = newproj_close;
    lt_app_register_dialog_kind(app, &dk);
}
