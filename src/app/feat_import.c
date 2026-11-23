/* feat_import.c - TunerStudio import wizard (G-18): after the project folder is chosen, a preview
 * lists what will be imported (INI and its signature, tune, PC variables, restore points,
 * dashboards, serial settings).  A missing INI can be replaced by one from the INI library or a
 * file; the tune alone can be imported into the open project.  Wording follows the original
 * ImportProjectWizard.
 *   dialog "ts_import"   target = a path inside the TunerStudio project
 *   lt_feat_import_ts_wizard(app, path)   open it; used by the shell's file dialog
 *   lt_feat_import_ts_with_ini(...)       the import itself (tests and scripts)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "files_logic.h"
#include "../plat/plat.h"
#include "../project/inidb.h"

#define FD_TS_INI (LT_APP_FD_USER0 + 40)

/* a requested import runs from the tick hook: opening the project closes all dialogs, which must
 * not happen while one of them is being drawn */
typedef struct imp_state { int pending; char root[512], ini[512]; } imp_state;

static imp_state *impst(lt_app *app)
{
    return (imp_state *)lt_ext_state(app, "ts_import", sizeof(imp_state), NULL);
}

static char *join_pcv(const char *dir)
{
    char *a = lt_path_join(dir, "projectCfg"), *b = lt_path_join(a, "pcVariableValues.msq");
    lt_free(a);
    return b;
}

typedef struct tsi_dlg {
    char root[512];              /* TunerStudio project folder */
    lt_ts_preview pv;
    int ok;                      /* preview valid */
    char err[256];
    char ini_override[512];      /* INI chosen for a missing one */
    char ini_note[300];
    long bundle;                 /* a missing INI: the bundled definition for the tune, -1 none */
    int bundle_near;             /*   only the closest build */
    char tune_sig[96];
    int importing;
} tsi_dlg;

/* the project folder holding projectCfg/project.properties, starting from a path inside it */
void lt_feat_import_ts_root(const char *picked, char *root, size_t cap)
{
    char probe[600];
    size_t n;
    lt_strlcpy(root, picked, cap);
    n = strlen(root);
    while (n > 1 && (root[n - 1] == '/' || root[n - 1] == '\\')) { root[--n] = '\0'; }
    lt_snprintf(probe, sizeof(probe), "%s/projectCfg/project.properties", root);
    if (lt_file_exists(probe)) { return; }
    if (lt_stricmp(lt_path_base(root), "projectCfg") == 0) {
        size_t k = (size_t)(lt_path_base(root) - root);
        if (k > 1) { root[k - 1] = '\0'; }
        return;
    }
    if (!lt_plat_is_dir(root)) {             /* a file inside the folder */
        size_t k = (size_t)(lt_path_base(root) - root);
        if (k > 1) { root[k - 1] = '\0'; }
        lt_snprintf(probe, sizeof(probe), "%s/projectCfg/project.properties", root);
        if (!lt_file_exists(probe) && lt_stricmp(lt_path_base(root), "projectCfg") == 0) {
            k = (size_t)(lt_path_base(root) - root);
            if (k > 1) { root[k - 1] = '\0'; }
        }
    }
}

static int copy_restore_points(const char *root, const char *proj)
{
    char *src = lt_path_join(root, "restorePoints"), *dst = lt_path_join(proj, "restorePoints");
    char **names = NULL;
    int n, i, c = 0;
    if (lt_plat_is_dir(src)) {
        (void)lt_plat_mkdirs(dst);
        n = lt_plat_list_dir(src, &names);
        for (i = 0; i < n; i++) {
            const char *e = lt_path_ext(names[i]);
            if (e != NULL && lt_stricmp(e, ".msq") == 0) {
                char *s = lt_path_join(src, names[i]), *d = lt_path_join(dst, names[i]);
                if (lt_plat_copy_file(s, d) == 0) { c++; }
                lt_free(s); lt_free(d);
            }
        }
        if (n >= 0) { lt_plat_free_list(names, n); }
    }
    lt_free(src); lt_free(dst);
    return c;
}

/* Import the TunerStudio project at `root`.  ini_path NULL/"" = the project's own INI; else that
 * file is the definition (a missing INI).  The new project is opened.  0 or negative. */
int lt_feat_import_ts_with_ini(lt_app *app, const char *root, const char *ini_path)
{
    lt_ts_preview pv;
    int rc;
    char *dir = NULL;
    if (lt_ts_preview_read(root, &pv) < 0) { return -1; }
    if (ini_path == NULL || ini_path[0] == '\0') {
        lt_project p;
        rc = lt_project_import_ts(root, NULL, &p);
        if (rc < 0) { return rc; }
        dir = lt_strdup(p.path);
        lt_project_close(&p);
    } else {
        lt_project p;
        lt_ini *probe = lt_ini_load_file(ini_path, NULL);
        char *f, *g, *pc;
        if (probe == NULL) { return -1; }
        rc = lt_project_create(pv.name, ini_path, probe->signature != NULL ? probe->signature : "",
                               NULL, &p);
        lt_ini_free(probe);
        if (rc < 0) { return rc; }
        f = lt_path_join(root, "CurrentTune.msq");
        g = lt_path_join(p.path, "CurrentTune.msq");
        if (lt_file_exists(f)) { (void)lt_plat_copy_file(f, g); }
        lt_free(f); lt_free(g);
        f = join_pcv(root);
        pc = join_pcv(p.path);
        if (lt_file_exists(f)) { (void)lt_plat_copy_file(f, pc); }
        lt_free(f); lt_free(pc);
        (void)copy_restore_points(root, p.path);
        if (pv.port[0] != '\0') {
            lt_free(p.cfg.conn.port);
            p.cfg.conn.port = lt_strdup(pv.port);
        }
        if (pv.baud > 0) { p.cfg.conn.baud_rate = (unsigned)pv.baud; }
        (void)lt_project_save_config(&p);
        dir = lt_strdup(p.path);
        lt_project_close(&p);
    }
    rc = lt_app_open_project(app, dir);
    if (rc == 0 && pv.n_dash > 0) {
        int n = lt_ts_import_dashboards(root, app->project.path);
        if (n > 0) { lt_app_toast(app, 1, "Imported %d dashboard file(s)", n); }
    }
    lt_free(dir);
    return rc;
}

void lt_feat_import_ts_wizard(lt_app *app, const char *picked)
{
    (void)lt_app_dialog_push(app, "ts_import", picked, "Import TS Project");
}

static void load_preview(tsi_dlg *s, const char *picked)
{
    char *p;
    memset(s->err, 0, sizeof(s->err));
    lt_feat_import_ts_root(picked, s->root, sizeof(s->root));
    s->ok = lt_ts_preview_read(s->root, &s->pv) == 0;
    if (!s->ok) {
        lt_snprintf(s->err, sizeof(s->err), "Not a valid TS project: %s", lt_err_get());
        return;
    }
    s->tune_sig[0] = '\0';
    s->ini_override[0] = '\0';
    s->ini_note[0] = '\0';
    s->bundle = -1L;
    p = lt_path_join(s->root, "CurrentTune.msq");
    if (s->pv.has_tune) {
        lt_tuneinfo ti;
        if (lt_tuneinfo_read(p, &ti) == 0) { lt_strlcpy(s->tune_sig, ti.signature, sizeof(s->tune_sig)); }
    }
    lt_free(p);
    if (!s->pv.ini_found && s->tune_sig[0] != '\0') {      /* look for a library INI by signature */
        lt_ini_repo repo;
        if (lt_ini_repo_open(&repo, NULL) == 0) {
            const lt_ini_entry *e = lt_ini_repo_find_signature(&repo, s->tune_sig, LT_TRUE);
            if (e != NULL) {
                char *path = lt_ini_repo_path(&repo, e->id);
                if (path != NULL) {
                    lt_strlcpy(s->ini_override, path, sizeof(s->ini_override));
                    lt_snprintf(s->ini_note, sizeof(s->ini_note), "From the INI library: %s", e->name);
                    lt_free(path);
                }
            } else {                                        /* else one bundled with LibreTune */
                lt_inidb_pick pk;
                int q = lt_inidb_resolve(s->tune_sig, NULL, 0, &pk);
                if (q != LT_INIDB_NONE && pk.src == LT_INIDB_SRC_BUNDLE) {
                    s->bundle = pk.bundle;
                    s->bundle_near = q != LT_INIDB_EXACT;
                    lt_snprintf(s->ini_note, sizeof(s->ini_note), q == LT_INIDB_EXACT
                                ? "Will use: %s" : "Will use the closest build: %s. Some settings "
                                "may not match.", pk.label);
                }
            }
            lt_ini_repo_close(&repo);
        }
    }
}

/* the INI to import with when the project's own is missing (a bundled one is unpacked into the
 * INI library now); 0 ok */
static int pick_ini(tsi_dlg *s, char *out, size_t cap)
{
    lt_ini_repo repo;
    char *id = NULL, *path = NULL;
    const lt_inidb_def *d = s->bundle >= 0 ? lt_inidb_at((size_t)s->bundle) : NULL;
    if (s->ini_override[0] != '\0' || d == NULL) {
        lt_strlcpy(out, s->ini_override, cap);
        return out[0] != '\0' ? 0 : -1;
    }
    if (lt_ini_repo_open(&repo, NULL) != 0) { return -1; }
    if (lt_inidb_materialise(&repo, d, &id) == 0) { path = lt_ini_repo_path(&repo, id); }
    lt_ini_repo_close(&repo);
    lt_free(id);
    if (path == NULL) { return -1; }
    lt_strlcpy(out, path, cap);
    lt_free(path);
    return 0;
}

static void row(lt_ui *ui, const char *label, const char *val, lt_color c)
{
    int w2[2];
    w2[0] = 130; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label_ex(ui, label, lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, val, c, LT_UI_ALIGN_LEFT);
}

static void tsi_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    tsi_dlg *s = (tsi_dlg *)d->data;
    int open = d->open, w1 = -1, w3[4], act = 0;
    const char *picked;
    char buf[300];
    if (s == NULL) {
        s = (tsi_dlg *)lt_calloc(1, sizeof(*s));
        d->data = s;
        s->bundle = -1L;
        if (d->target[0] != '\0') { load_preview(s, d->target); }
        else {                                  /* LT_SHOT_OPEN preview of a sample project */
            lt_strlcpy(s->pv.name, "Sample Project", sizeof(s->pv.name));
            lt_strlcpy(s->pv.ini_file, "mainController.ini", sizeof(s->pv.ini_file));
            lt_strlcpy(s->pv.signature, "speeduino 202305", sizeof(s->pv.signature));
            lt_strlcpy(s->pv.port, "COM4", sizeof(s->pv.port));
            s->pv.baud = 115200; s->pv.ini_found = 1; s->pv.has_tune = 1; s->pv.has_pcv = 1;
            s->pv.n_restore = 12; s->pv.n_dash = 2; s->ok = 1;
        }
    }
    picked = lt_app_file_result(app, FD_TS_INI);
    if (picked != NULL) {
        lt_strlcpy(s->ini_override, picked, sizeof(s->ini_override));
        lt_snprintf(s->ini_note, sizeof(s->ini_note), "Chosen file: %s", lt_path_base(picked));
    }
    /* the missing-INI part needs room for its note and button */
    if (lt_ui_modal_begin(ui, "Import TS Project", 560, s->ok && !s->pv.ini_found ? 540 : 460, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Step 2 of 2: Confirm Import", t->muted, LT_UI_ALIGN_LEFT);
        if (!s->ok) {
            lt_ui_label_wrap(ui, s->err, t->error);
            lt_ui_label_wrap(ui, "Look for a folder containing projectCfg/project.properties.", t->muted);
        } else {
            lt_ui_heading(ui, s->pv.name);
            row(ui, "Folder:", s->root, t->text2);
            if (s->pv.ini_file[0] == '\0') {
                row(ui, "INI File:", "None specified", t->warning);
            } else {
                lt_snprintf(buf, sizeof(buf), "%s%s", s->pv.ini_file, s->pv.ini_found ? "" : "  (not found)");
                row(ui, "INI File:", buf, s->pv.ini_found ? t->text : t->error);
            }
            if (s->pv.signature[0] != '\0') { row(ui, "Signature:", s->pv.signature, t->text2); }
            row(ui, "Has Tune:", s->pv.has_tune ? "Yes" : "No", s->pv.has_tune ? t->success : t->warning);
            lt_snprintf(buf, sizeof(buf), "%d", s->pv.n_restore);
            row(ui, "Restore Points:", buf, t->text);
            row(ui, "PC Variables:", s->pv.has_pcv ? "Yes" : "No", s->pv.has_pcv ? t->success : t->text);
            lt_snprintf(buf, sizeof(buf), "%d", s->pv.n_dash);
            row(ui, "Dashboards:", buf, t->text);
            if (s->pv.port[0] != '\0') {
                lt_snprintf(buf, sizeof(buf), "%s @ %d", s->pv.port, s->pv.baud);
                row(ui, "Serial Port:", buf, t->text);
            }
            if (!s->pv.ini_found) {
                lt_ui_separator(ui);
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_wrap(ui, "The project's ECU definition file was not found. Choose an INI "
                                     "to use instead, or import only the tune into the open project.",
                                 t->warning);
                if (s->ini_override[0] != '\0' || s->bundle >= 0) {
                    lt_ui_label_wrap(ui, s->ini_note, s->ini_override[0] == '\0' && s->bundle_near
                                     ? t->warning : t->success);
                }
                if (lt_ui_button(ui, "Choose ECU Definition...")) {
                    lt_app_file_dialog(app, FD_TS_INI, LT_UI_FILE_OPEN, NULL, "",
                                       "ECU definitions|*.ini|All files|*");
                }
            }
            lt_ui_separator(ui);
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_wrap(ui, "The project will be imported to your LibreTune projects folder and "
                                 "opened automatically.", t->muted);
        }
        w3[0] = -1; w3[1] = 140; w3[2] = 130; w3[3] = 90;
        lt_ui_layout_row(ui, 4, w3, 0);
        lt_ui_label(ui, "");
        if (lt_ui_button_ex(ui, "Import Tune Only",
                            s->ok && s->pv.has_tune && app->ini != NULL ? 0 : LT_UI_BTN_DISABLED)) { act = 2; }
        if (lt_ui_button_ex(ui, "Import Project",
                            s->ok && (s->pv.ini_found || s->ini_override[0] != '\0' || s->bundle >= 0)
                              ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED)) { act = 1; }
        if (lt_ui_button(ui, "Cancel")) { open = 0; }
        lt_ui_modal_end(ui);
    }
    if (act == 1 || act == 2) {
        char root[512], ini[512];
        lt_strlcpy(root, s->root, sizeof(root));
        ini[0] = '\0';
        if (act == 1 && !s->pv.ini_found && pick_ini(s, ini, sizeof(ini)) != 0) {
            lt_snprintf(s->ini_note, sizeof(s->ini_note), "Cannot unpack the definition: %s", lt_err_get());
            act = 0;
        }
        if (act == 1) {
            imp_state *ist = impst(app);
            ist->pending = 1;
            lt_strlcpy(ist->root, root, sizeof(ist->root));
            lt_strlcpy(ist->ini, ini, sizeof(ist->ini));
            open = 0;
        } else {
            char *tp = lt_path_join(root, "CurrentTune.msq");
            if (lt_app_load_tune(app, tp) == 0) { open = 0; }
            lt_free(tp);
        }
    }
    d->open = open;
}

static void tsi_close(lt_app *app, lt_app_dialog *d)
{
    LT_UNUSED(app);
    lt_free(d->data);
    d->data = NULL;
}

static void imp_tick(lt_app *app)
{
    imp_state *ist = impst(app);
    char root[512], ini[512];
    if (!ist->pending) { return; }
    ist->pending = 0;
    lt_strlcpy(root, ist->root, sizeof(root));
    lt_strlcpy(ini, ist->ini, sizeof(ini));
    if (lt_feat_import_ts_with_ini(app, root, ini) < 0) {
        lt_app_toast(app, 3, "Import failed: %s", lt_err_get());
    }
}

void lt_feat_import_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    lt_ext_add_tick(reg, imp_tick);
    dk.kind = "ts_import"; dk.draw = tsi_draw; dk.close = tsi_close;
    lt_ext_add_dialog_kind(reg, &dk);
}
