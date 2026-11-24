/* dialogs_newproj.c - New project wizard.
 * The ECU definition comes from the definitions bundled with clibretune (project/inidb.h: grouped by
 * family and release, with a search box), the local INI repository or an INI file.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dialogs.h"
#include "../project/inidb.h"

#define CH 8
#define CV 16

#define SRC_REPO   0
#define SRC_FILE   1
#define SRC_BUNDLE 2

/* ================================================================== */
/* New project wizard                                                  */
typedef struct newproj_state {
    char name[64], dir[512], ini[512];
    lt_ini_repo repo; int repo_ok;
    int source;          /* SRC_* */
    int repo_sel;
    long bundle_sel;     /* catalog index of the bundled definition, -1 none */
    char query[64];
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

/* the bundled definition as a file in the INI repository (unpacked on demand) */
static char *bundle_path(newproj_state *s)
{
    char *id = NULL, *p = NULL;
    const lt_inidb_def *d = s->bundle_sel >= 0 ? lt_inidb_at((size_t)s->bundle_sel) : NULL;
    if (d == NULL || !s->repo_ok) { return NULL; }
    if (lt_inidb_materialise(&s->repo, d, &id) == 0) { p = lt_ini_repo_path(&s->repo, id); }
    lt_free(id);
    return p;
}

static int newproj_create(lt_app *app, newproj_state *s)
{
    lt_project p;
    lt_ini *probe;
    char *ini_path = NULL, *pdir;
    int rc;
    if (s->source == SRC_REPO) {
        if (!s->repo_ok || s->repo.n == 0) { lt_strlcpy(s->err, "No definition selected", sizeof(s->err)); return -1; }
        ini_path = lt_ini_repo_path(&s->repo, s->repo.e[s->repo_sel].id);
    } else if (s->source == SRC_BUNDLE) {
        if (s->bundle_sel < 0) { lt_strlcpy(s->err, "Choose a definition", sizeof(s->err)); return -1; }
        ini_path = bundle_path(s);
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

/* one selectable definition row */
static void bundle_leaf(lt_ui *ui, newproj_state *s, size_t i, const char *text)
{
    int fl = LT_UI_TREE_LEAF;
    if ((long)i == s->bundle_sel) { fl |= LT_UI_TREE_SELECTED; }
    lt_ui_push_id_int(ui, (int)i);
    if (lt_ui_tree_node(ui, text, -1, fl) & (LT_UI_TREE_CLICKED | LT_UI_TREE_ACTIVATED)) {
        s->bundle_sel = (long)i;
    }
    lt_ui_pop_id(ui);
}

/* bundled definitions grouped by family, then (boards) release; only those matching the query */
static void bundle_tree(lt_ui *ui, newproj_state *s)
{
    static const char *const fams[] = { "speeduino", "rusefi", "fome", "miata-ecus" };
    size_t nb = lt_inidb_count(), fi, i, j;
    int searching = s->query[0] != '\0', fid = 0;
    char lab[200];
    for (fi = 0; fi < LT_COUNTOF(fams); fi++) {
        size_t cnt = 0;
        for (i = 0; i < nb; i++) {
            const lt_inidb_def *d = lt_inidb_at(i);
            if (strcmp(d->family, fams[fi]) == 0 && lt_inidb_match(d, s->query)) { cnt++; }
        }
        if (cnt == 0) { continue; }
        lt_snprintf(lab, sizeof(lab), "%s  (%lu)", lt_inidb_family_title(fams[fi]), (unsigned long)cnt);
        lt_ui_push_id_int(ui, 100000 + fid++);
        if (lt_ui_tree_node(ui, lab, -1, searching ? LT_UI_TREE_FORCE_OPEN : 0) & LT_UI_TREE_OPEN) {
            for (i = 0; i < nb; i++) {
                const lt_inidb_def *d = lt_inidb_at(i);
                int first = 1;
                if (strcmp(d->family, fams[fi]) != 0 || !lt_inidb_match(d, s->query)) { continue; }
                if (d->board[0] == '\0') {          /* Speeduino: one file per release */
                    lt_snprintf(lab, sizeof(lab), "%s   [%s]", d->version, d->signature);
                    bundle_leaf(ui, s, i, lab);
                    continue;
                }
                /* boards: a node per release, at its first entry (entries of a release are adjacent) */
                for (j = 0; j < i; j++) {
                    const lt_inidb_def *e = lt_inidb_at(j);
                    if (strcmp(e->family, d->family) == 0 && strcmp(e->version, d->version) == 0 &&
                        lt_inidb_match(e, s->query)) { first = 0; break; }
                }
                if (!first) { continue; }
                lt_ui_push_id_int(ui, 200000 + (int)i);
                if (lt_ui_tree_node(ui, d->version, -1, searching ? LT_UI_TREE_FORCE_OPEN : 0)
                    & LT_UI_TREE_OPEN) {
                    for (j = i; j < nb; j++) {
                        const lt_inidb_def *e = lt_inidb_at(j);
                        if (strcmp(e->family, d->family) != 0 || strcmp(e->version, d->version) != 0 ||
                            !lt_inidb_match(e, s->query)) { continue; }
                        lt_snprintf(lab, sizeof(lab), "%s   [%s]", e->board, e->signature);
                        bundle_leaf(ui, s, j, lab);
                    }
                    lt_ui_tree_end(ui);
                }
                lt_ui_pop_id(ui);
            }
            lt_ui_tree_end(ui);
        }
        lt_ui_pop_id(ui);
    }
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
        s->bundle_sel = -1L;
        lt_strlcpy(s->query, d->target, sizeof(s->query));     /* optional initial search */
        s->source = lt_inidb_count() > 0 ? SRC_BUNDLE : (s->repo_ok && s->repo.n > 0 ? SRC_REPO : SRC_FILE);
        if (app->signature[0] != '\0') {        /* connected: preselect the ECU's definition */
            size_t idx[1];
            if (lt_inidb_find(app->signature, idx, 1) > 0) { s->bundle_sel = (long)idx[0]; }
        }
        if (app->ini_path[0] != '\0' && s->source == SRC_FILE) { lt_strlcpy(s->ini, app->ini_path, sizeof(s->ini)); }
    }
    picked = lt_app_file_result(app, LT_DLG_FD_PROJ_INI);
    if (picked != NULL) { lt_strlcpy(s->ini, picked, sizeof(s->ini)); s->source = SRC_FILE; }
    if (lt_ui_modal_begin(ui, lt_tr("New Project"), CH * 84, 560, &open)) {
        w2[0] = CH * 16; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, lt_tr("Project name"));
        lt_ui_text(ui, "name", s->name, sizeof(s->name), 0);
        lt_ui_label(ui, lt_tr("Directory"));
        lt_ui_text(ui, "dir", s->dir, sizeof(s->dir), 0);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, lt_tr("ECU definition"), t->text2, LT_UI_ALIGN_LEFT);
        lt_ui_radio(ui, "Bundled with clibretune", &s->source, SRC_BUNDLE);
        lt_ui_radio(ui, "From the local definition repository", &s->source, SRC_REPO);
        lt_ui_radio(ui, "From an INI file", &s->source, SRC_FILE);
        if (s->source == SRC_BUNDLE) {
            lt_ui_layout_row(ui, 2, w2, 0);
            lt_ui_label(ui, lt_tr("Search"));
            lt_ui_text(ui, "defq", s->query, sizeof(s->query), 0);
            lt_ui_layout_row(ui, 1, &w1, CV * 11);
            if (lt_ui_scroll_begin(ui, "deftree")) {
                bundle_tree(ui, s);
                lt_ui_scroll_end(ui);
            }
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (s->bundle_sel >= 0) {
                char lab[200], line[300];
                const lt_inidb_def *bd = lt_inidb_at((size_t)s->bundle_sel);
                lt_inidb_label(bd, lab, sizeof(lab));
                lt_snprintf(line, sizeof(line), "%s  -  %s  (%s)", lab, bd->signature, bd->licence);
                lt_ui_label_ex(ui, line, t->text2, LT_UI_ALIGN_LEFT);
            } else {
                lt_ui_label_ex(ui, lt_tr("Pick the definition that matches your ECU firmware."),
                               t->muted, LT_UI_ALIGN_LEFT);
            }
        } else if (s->source == SRC_REPO) {
            lt_ui_layout_row(ui, 1, &w1, CV * 6);
            if (s->repo_ok && s->repo.n > 0) {
                lt_ui_listbox_cb(ui, "repo", &s->repo_sel, (int)s->repo.n, repo_get, s);
            } else {
                lt_ui_label_ex(ui, lt_tr("The local definition repository is empty. Import an INI file first."),
                               t->muted, LT_UI_ALIGN_LEFT);
            }
        } else {
            lt_ui_layout_row(ui, 2, w2, 0);
            if (lt_ui_button(ui, lt_tr("Browse..."))) {
                lt_app_file_dialog(app, LT_DLG_FD_PROJ_INI, LT_UI_FILE_OPEN, NULL, "",
                                   "ECU definitions|*.ini|All files|*");
            }
            lt_ui_text(ui, "inipath", s->ini, sizeof(s->ini), 0);
        }
        {   /* the built-in Speeduino base tunes (feat_basetune.c) */
            int wbt[2];
            wbt[0] = -1; wbt[1] = 200;
            lt_ui_layout_row(ui, 2, wbt, 0);
            lt_ui_label_ex(ui, lt_tr("Or start from a built-in Speeduino base tune:"), t->text2, LT_UI_ALIGN_LEFT);
            if (lt_ui_button(ui, lt_tr("From a Base Tune..."))) {
                (void)lt_app_dialog_push(app, "basetunes", "new", "New Project from Base Tune");
                open = 0;
            }
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_checkbox(ui, lt_tr("Open the base map wizard after creating"), &s->basemap);
        if (s->err[0] != '\0') { lt_ui_label_ex(ui, s->err, t->error, LT_UI_ALIGN_LEFT); }
        ok = s->name[0] != '\0' &&
             (s->source == SRC_FILE ? s->ini[0] != '\0'
              : s->source == SRC_BUNDLE ? s->bundle_sel >= 0 && s->repo_ok
              : (s->repo_ok && s->repo.n > 0));
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button_ex(ui, lt_tr("Create"), ok ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED) && ok) {
            int basemap = s->basemap;
            if (newproj_create(app, s) == 0) {
                /* opening the project closed this dialog (d and s are freed): touch nothing */
                lt_ui_modal_end(ui);
                if (basemap) { lt_app_basemap_wizard(app); }
                return;
            }
        }
        if (lt_ui_button(ui, lt_tr("Cancel"))) { open = 0; }
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
