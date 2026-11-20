/* feat_sync.c - what happens after the ECU link comes up (G-01, G-02, G-03):
 *   - signature mismatch dialog with matching INIs from the local INI library
 *   - project tune vs ECU tune choice (Use LibreTune / Ignore / Use ECU settings)
 *   - Connect ECU wizard: scan, pick the INI by signature, name the project, connect
 *   - automatic definition choice (project/inidb.h): on connect, a signature that differs from
 *     the open definition is resolved against the ECU's USB drive, the INI library and the
 *     definitions bundled with the program: exact = switch silently (one toast), near = switch
 *     with a one-line warning and "Find Exact INI...", none = the mismatch dialog as before
 *   - lt_feat_sync_connect_auto: connect, identify, pick the INI, open or create the project
 * Wording follows the original SignatureMismatchDialog / TuneMismatchDialog.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "feat.h"
#include "sync_logic.h"
#include "ports_logic.h"
#include "../project/inidb.h"

#define CH 8
#define CV 16
#define MAX_MATCH 24
#define FD_SYNC_INI (LT_APP_FD_USER0 + 20)
#define FD_WIZ_INI  (LT_APP_FD_USER0 + 21)

#define BUNDLE_ID "bundle:"

/* shared (per app) state: the pending tune choice, the automatic definition switch */
typedef struct sync_state {
    lt_sync_cmp *cmp;
    int sync_after_sig;             /* run the tune check when the signature dialog is left */
    char auto_ecu[128];             /* ECU signature the definition was resolved for */
    char auto_pick[160];            /* signature of the definition switched to */
    int  auto_near;                 /* that definition is only the nearest build */
    char near_label[200];           /* warning dialog: definition in use */
    char near_ecu[128];             /*                 ECU signature */
} sync_state;

static void sync_cleanup(lt_app *app, void *p)
{
    sync_state *st = (sync_state *)p;
    LT_UNUSED(app);
    lt_sync_cmp_free(st->cmp);
    st->cmp = NULL;
}

static sync_state *get_state(lt_app *app)
{
    return (sync_state *)lt_ext_state(app, "sync", sizeof(sync_state), sync_cleanup);
}

/* ------------------------------------------------------------------ */
/* tune choice after connecting                                        */
static int project_has_saved_tune(lt_app *app)
{
    char *p;
    int ok;
    if (!app->has_project) { return 0; }
    p = lt_project_file(&app->project, "CurrentTune.msq");
    ok = p != NULL && lt_file_exists(p);
    lt_free(p);
    return ok;
}

static void restore_point(lt_app *app)
{
    char *rp = NULL;
    if (app->has_project && app->project.tune != NULL && lt_restore_create(&app->project, &rp) == 0) {
        lt_free(rp);
    }
}

/* "Ignore" in the Difference Report: keep the project tune as it is (a restore point is saved) and
 * do not copy the ECU into it on disconnect either */
void lt_feat_sync_ignore(lt_app *app)
{
    restore_point(app);
    app->keep_project_tune = 1;
}

/* decide which tune the session continues with; may open the tune_sync dialog */
static void tune_check(lt_app *app)
{
    sync_state *st = get_state(app);
    if (app->tune == NULL || app->ini == NULL || app->mem == NULL || !app->has_project) { return; }
    lt_sync_cmp_free(st->cmp);
    st->cmp = NULL;
    if (!project_has_saved_tune(app)) {
        /* nothing offline worth protecting: the ECU is the truth */
        lt_sync_apply_ecu(app->tune, app->ini, app->mem);
        return;
    }
    st->cmp = lt_sync_compare(app->ini, app->tune, app->mem);
    if (lt_sync_cmp_identical(st->cmp)) {
        lt_sync_cmp_free(st->cmp);
        st->cmp = NULL;
        return;
    }
    (void)lt_app_dialog_push(app, "tune_sync", "", "Difference Report");
}

/* ------------------------------------------------------------------ */
/* automatic definition choice                                         */
static int switch_ini(lt_app *app, const char *ini_path);

static lt_bool same_sig(const char *a, const char *b)
{
    return lt_inidb_distance(a, b) == 0;
}

/* the warning shown while a near-match definition is in use */
static void show_near(lt_app *app, const char *label, const char *ecu_sig)
{
    sync_state *st = get_state(app);
    if (label != st->near_label) { lt_strlcpy(st->near_label, label, sizeof(st->near_label)); }
    lt_strlcpy(st->near_ecu, ecu_sig, sizeof(st->near_ecu));
    (void)lt_app_dialog_push(app, "inidb_near", ecu_sig, "Closest ECU Definition");
}

/* The ECU's signature differs from the open definition: resolve it (USB drive, INI library,
 * bundle). Returns 0 = not handled (ask the user), 1 = switching (the reconnect runs the checks),
 * 2 = handled, continue with the tune check. */
static int auto_resolve(lt_app *app)
{
    sync_state *st = get_state(app);
    const char *ecu = app->signature;
    const char *cur = app->ini != NULL && app->ini->signature != NULL ? app->ini->signature : "";
    lt_ini_repo repo;
    lt_inidb_pick pk;
    char *path = NULL;
    int q, rc;
    if (ecu[0] == '\0') { return 0; }
    if (strcmp(st->auto_ecu, ecu) == 0) {
        /* the reconnect after our own switch; a near match still differs, which is expected */
        if (st->auto_pick[0] != '\0' && same_sig(cur, st->auto_pick)) {
            if (st->auto_near && strcmp(st->near_ecu, ecu) != 0) {
                show_near(app, st->near_label, ecu);
            }
            return 2;
        }
        return 0;                       /* tried for this ECU already: the user decides */
    }
    lt_strlcpy(st->auto_ecu, ecu, sizeof(st->auto_ecu));
    st->auto_pick[0] = '\0';
    st->auto_near = 0;
    if (lt_ini_repo_open(&repo, NULL) != 0) { return 0; }
    q = lt_inidb_resolve(ecu, &repo, LT_INIDB_USE_DRIVE, &pk);
    if (q == LT_INIDB_NONE) { lt_ini_repo_close(&repo); return 0; }
    if (q == LT_INIDB_NEAR) {
        /* keep the open definition when it is at least as close as the pick */
        long dc = lt_inidb_distance(ecu, cur);
        if (dc > 0 && dc <= pk.distance) {
            lt_ini_repo_close(&repo);
            lt_snprintf(st->near_label, sizeof(st->near_label), "%s (the open definition)", cur);
            lt_strlcpy(st->auto_pick, cur, sizeof(st->auto_pick));
            st->auto_near = 1;
            show_near(app, st->near_label, ecu);
            return 2;
        }
    }
    rc = lt_inidb_pick_path(&repo, &pk, &path);
    lt_ini_repo_close(&repo);
    if (rc != 0 || path == NULL) { lt_free(path); return 0; }
    lt_strlcpy(st->auto_pick, pk.signature, sizeof(st->auto_pick));
    lt_strlcpy(st->near_label, pk.label, sizeof(st->near_label));
    st->auto_near = q == LT_INIDB_NEAR;
    st->near_ecu[0] = '\0';
    st->sync_after_sig = 0;
    if (q == LT_INIDB_EXACT) {
        lt_app_toast(app, 1, "ECU definition: %s", pk.label);
    } else {
        lt_app_toast(app, 2, "No exact definition for %s: using the closest, %s", ecu, pk.label);
    }
    rc = switch_ini(app, path);
    lt_free(path);
    return rc == 0 ? 1 : 0;
}

static void on_connect(lt_app *app)
{
    sync_state *st = get_state(app);
    if (app->proto != NULL && lt_proto_signature_mismatch(app->proto)) {
        int h = auto_resolve(app);
        if (h == 1) { return; }
        if (h == 2) { tune_check(app); return; }
        st->sync_after_sig = 1;
        (void)lt_app_dialog_push(app, "sig_mismatch", app->signature, "INI Signature Mismatch");
        return;
    }
    tune_check(app);
}

/* "Closest ECU Definition": one line of warning and the way to the exact file */
static void near_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    sync_state *st = get_state(app);
    int open = d->open, w1 = -1, w2[2];
    char line[480];
    const char *ecu = st->near_ecu[0] != '\0' ? st->near_ecu : d->target;
    if (lt_ui_modal_begin(ui, "Closest ECU Definition", CH * 76, CV * 11, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_snprintf(line, sizeof(line), "No exact definition for this firmware (%s): using the "
                    "closest build, %s. Some settings may not match.", ecu, st->near_label);
        lt_ui_label_wrap(ui, line, t->warning);
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button(ui, "Find Exact INI...")) {
            char sig[128];
            lt_strlcpy(sig, ecu, sizeof(sig));
            lt_ui_modal_end(ui);
            d->open = 0;
            lt_feat_online_open(app, sig);
            return;
        }
        if (lt_ui_button_ex(ui, "Continue", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

/* the definitions worth offering for ecu_sig: the resolve pick first (preselected), then the
 * INI library's exact/partial matches, then other bundled builds of the same board, closest first */
static int collect_defs(lt_ini_repo *repo, int repo_ok, const char *sig, lt_sync_ini_match *m,
                        int cap, lt_inidb_pick *pk)
{
    lt_sync_ini_match tmp[MAX_MATCH];
    int n = 0, i, k, nt;
    size_t bi, nb = lt_inidb_count();
    long *dist;
    (void)lt_inidb_resolve(sig, repo_ok ? repo : NULL, LT_INIDB_USE_DRIVE, pk);
    if (pk->quality != LT_INIDB_NONE && n < cap) {
        m[n].id[0] = '\0';
        if (pk->src == LT_INIDB_SRC_BUNDLE) {
            lt_snprintf(m[n].id, sizeof(m[n].id), "%s%ld", BUNDLE_ID, pk->bundle);
        } else if (pk->src == LT_INIDB_SRC_REPO) {
            lt_strlcpy(m[n].id, pk->repo_id, sizeof(m[n].id));
        } else if (repo_ok) {           /* the ECU's own drive: keep a copy in the library */
            char *id = NULL;
            if (lt_ini_repo_import(repo, pk->path, &id) == 0) {
                lt_strlcpy(m[n].id, id, sizeof(m[n].id));
            }
            lt_free(id);
        }
        if (m[n].id[0] != '\0') {
            lt_strlcpy(m[n].name, pk->label, sizeof(m[n].name));
            lt_strlcpy(m[n].signature, pk->signature, sizeof(m[n].signature));
            m[n].kind = pk->quality == LT_INIDB_EXACT ? LT_SIG_EXACT : LT_SIG_PARTIAL;
            n++;
        }
    }
    nt = repo_ok ? lt_sync_find_inis(repo, sig, tmp, MAX_MATCH) : 0;
    for (i = 0; i < nt && n < cap; i++) {
        for (k = 0; k < n; k++) {
            if (strcmp(m[k].id, tmp[i].id) == 0) { break; }
        }
        if (k == n) { m[n++] = tmp[i]; }
    }
    if (nb == 0) { return n; }
    dist = (long *)lt_malloc(nb * sizeof(long));
    for (bi = 0; bi < nb; bi++) { dist[bi] = lt_inidb_distance(sig, lt_inidb_at(bi)->signature); }
    while (n < cap) {                   /* bundled builds of the same board, closest first */
        long best = -1L;
        size_t bsel = 0;
        char lab[160];
        for (bi = 0; bi < nb; bi++) {
            if (dist[bi] >= 0 && (best < 0 || dist[bi] < best)) { best = dist[bi]; bsel = bi; }
        }
        if (best < 0) { break; }
        dist[bsel] = -1L;
        lt_snprintf(m[n].id, sizeof(m[n].id), "%s%lu", BUNDLE_ID, (unsigned long)bsel);
        for (k = 0; k < n; k++) {
            if (strcmp(m[k].id, m[n].id) == 0) { break; }
        }
        if (k < n) { continue; }
        lt_inidb_label(lt_inidb_at(bsel), lab, sizeof(lab));
        lt_snprintf(m[n].name, sizeof(m[n].name), "%s (bundled)", lab);
        lt_strlcpy(m[n].signature, lt_inidb_at(bsel)->signature, sizeof(m[n].signature));
        m[n].kind = best == 0 ? LT_SIG_EXACT : LT_SIG_PARTIAL;
        n++;
    }
    lt_free(dist);
    return n;
}

/* path of an offered definition: bundled ones are unpacked into the INI library first */
static char *match_path(lt_ini_repo *repo, const lt_sync_ini_match *m)
{
    char *id = NULL, *path = NULL;
    size_t bl = strlen(BUNDLE_ID);
    if (strncmp(m->id, BUNDLE_ID, bl) == 0) {
        long idx = atol(m->id + bl);
        if (idx >= 0 && lt_inidb_materialise(repo, lt_inidb_at((size_t)idx), &id) == 0) {
            path = lt_ini_repo_path(repo, id);
        }
        lt_free(id);
        return path;
    }
    return lt_ini_repo_path(repo, m->id);
}

/* ------------------------------------------------------------------ */
/* tune_sync dialog                                                    */
static void sync_close_dlg(lt_app *app, lt_app_dialog *d)
{
    sync_state *st = get_state(app);
    LT_UNUSED(d);
    /* the comparison belongs to this dialog: drop it when the dialog goes */
    lt_sync_cmp_free(st->cmp);
    st->cmp = NULL;
}

/* Preview for LT_SHOT_OPEN=dlg:tune_sync without a real mismatch: the ECU image is the
 * tune's own pages with a few bytes changed. */
static lt_sync_cmp *preview_cmp(lt_app *app)
{
    lt_ecu_mem *m;
    lt_sync_cmp *c = NULL;
    size_t np, i, len = 0;
    if (app->ini == NULL || app->tune == NULL) { return NULL; }
    m = lt_ecu_mem_from_ini(app->ini);
    if (m == NULL) { return NULL; }
    lt_tune_to_mem(app->tune, m);
    np = lt_ecu_mem_page_count(m);
    for (i = 0; i < np && i < 3; i++) {
        const lt_u8 *pg = lt_ecu_mem_page(m, (int)i, &len);
        size_t k;
        if (pg == NULL) { continue; }
        for (k = 8; k < 40 && k < len; k += 3) {
            lt_u8 b = (lt_u8)(pg[k] ^ 0x15);
            lt_ecu_mem_write(m, (int)i, k, &b, 1);
        }
    }
    c = lt_sync_compare(app->ini, app->tune, m);
    lt_ecu_mem_free(m);
    return c;
}

static void tune_sync_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    sync_state *st = get_state(app);
    int open = d->open, w1 = -1, w3[3], preview = 0, act = 0;
    size_t i, n;
    char sum[160], a[64], b[64];
    if (st->cmp == NULL) {
        st->cmp = preview_cmp(app);
        preview = 1;
    } else if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) {
        open = 0;                         /* link went away while the dialog was open */
    }
    if (st->cmp == NULL) { d->open = 0; return; }
    if (lt_ui_modal_begin(ui, "Difference Report", CH * 84, CV * 31, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui,
            "There are differences between the settings currently in LibreTune and the settings "
            "found in the ECU. Review the list, then choose which settings to keep.", t->text2);
        lt_sync_cmp_summary(st->cmp, sum, sizeof(sum));
        lt_ui_label_ex(ui, sum, t->warning, LT_UI_ALIGN_LEFT);
        w3[0] = CH * 30; w3[1] = (CH * 84 - CH * 30 - 40) / 2; w3[2] = -1;
        lt_ui_layout_row(ui, 3, w3, 0);
        lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
        lt_ui_label_ex(ui, "Setting", t->text, LT_UI_ALIGN_LEFT);
        lt_ui_label_ex(ui, "Current LibreTune Settings", t->text, LT_UI_ALIGN_LEFT);
        lt_ui_label_ex(ui, "Settings in ECU", t->text, LT_UI_ALIGN_LEFT);
        lt_ui_text_style_reset(ui);
        lt_ui_layout_row(ui, 1, &w1, CV * 14);
        if (lt_ui_scroll_begin(ui, "diffs")) {
            n = st->cmp->diff != NULL ? st->cmp->diff->n : 0;
            if (n > 400) { n = 400; }
            for (i = 0; i < n; i++) {
                const lt_tune_difference *df = &st->cmp->diff->diffs[i];
                lt_ui_layout_row(ui, 3, w3, 0);
                lt_ui_label_ex(ui, df->name, t->text, LT_UI_ALIGN_LEFT);
                lt_sync_diff_values(st->cmp, i, a, sizeof(a), b, sizeof(b));
                lt_ui_label_ex(ui, a, t->text2, LT_UI_ALIGN_LEFT);
                lt_ui_label_ex(ui, b, t->text2, LT_UI_ALIGN_LEFT);
            }
            if (n == 0) {
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_wrap(ui,
                    "No named INI settings differ. Unused page bytes may still differ; Use "
                    "LibreTune Settings will not invent those bytes.", t->muted);
            }
            lt_ui_scroll_end(ui);
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui,
            "Use LibreTune Settings writes the project tune to the ECU (burn to keep it). Use ECU "
            "Settings replaces the project tune with what the ECU holds; a restore point of the "
            "project tune is saved first.", t->muted);
        w3[0] = -1; w3[1] = -1; w3[2] = -1;
        lt_ui_layout_row(ui, 3, w3, 0);
        if (lt_ui_button_ex(ui, "Use LibreTune Settings", LT_UI_BTN_PRIMARY)) { act = 1; }
        if (lt_ui_button(ui, "Ignore")) { act = 2; }
        if (lt_ui_button(ui, "Use ECU Settings")) { act = 3; }
        lt_ui_modal_end(ui);
    }
    if (act == 1 && !preview) {
        long nb = lt_sync_apply_project(st->cmp, app->mem);
        if (nb < 0 || lt_proto_write_dirty(app->proto, app->mem) < 0) {
            lt_app_toast(app, 3, "Writing the project tune to the ECU failed: %s", lt_err_get());
        } else {
            lt_app_toast(app, 1, "Project tune written to the ECU (%ld bytes). Burn to make it permanent.", nb);
        }
        app->modified = 1;
        open = 0;
    } else if (act == 3 && !preview) {
        restore_point(app);
        lt_sync_apply_ecu(app->tune, app->ini, app->mem);
        app->modified = 1;
        lt_app_toast(app, 1, "Project tune replaced by the ECU settings");
        open = 0;
    } else if (act != 0) {
        if (act == 2 && !preview) {
            lt_feat_sync_ignore(app);
        }
        open = 0;
    }
    d->open = open;
}

/* ------------------------------------------------------------------ */
/* signature mismatch dialog                                           */
typedef struct sig_state {
    char ecu_sig[128], ini_sig[128], note[200];
    lt_ini_repo repo; int repo_ok;
    lt_sync_ini_match m[MAX_MATCH]; int n_m, sel;
    int preview, online_gen;
    lt_inidb_pick pk;
} sig_state;

static void sig_reload(sig_state *s)
{
    if (s->repo_ok) { lt_ini_repo_close(&s->repo); s->repo_ok = 0; }
    s->repo_ok = lt_ini_repo_open(&s->repo, NULL) == 0;
    s->n_m = collect_defs(&s->repo, s->repo_ok, s->ecu_sig, s->m, MAX_MATCH, &s->pk);
    s->sel = 0;
}

static const char *sig_item(void *ud, int i)
{
    static char buf[300];
    sig_state *s = (sig_state *)ud;
    lt_snprintf(buf, sizeof(buf), "%s   [%s]   %s", s->m[i].name, s->m[i].signature,
                s->m[i].kind == LT_SIG_EXACT ? "Exact" : "Near");
    return buf;
}

/* use `ini_path` for this project/session and reconnect to the same ECU */
static int switch_ini(lt_app *app, const char *ini_path)
{
    char port[128], dir[512];
    long baud = app->settings.baud;
    int demo = app->conn == LT_APP_DEMO, rc;
    lt_strlcpy(port, app->settings.port, sizeof(port));
    dir[0] = '\0';
    if (app->has_project) {
        char *dst, *txt;
        size_t len = 0;
        lt_strlcpy(dir, app->project.path, sizeof(dir));
        txt = lt_file_read(ini_path, &len);
        dst = lt_project_ini_path(&app->project);
        if (txt == NULL || dst == NULL || lt_plat_write_atomic(dst, txt, len) < 0) {
            lt_free(txt); lt_free(dst);
            lt_app_toast(app, 3, "Cannot switch the project definition: %s", lt_err_get());
            return -1;
        }
        lt_free(txt); lt_free(dst);
        {
            lt_ini *probe = lt_ini_load_file(ini_path, NULL);
            if (probe != NULL) {
                lt_free(app->project.cfg.signature);
                app->project.cfg.signature = lt_strdup(probe->signature != NULL ? probe->signature : "");
                lt_ini_free(probe);
            }
        }
        lt_project_save_config(&app->project);
        rc = lt_app_open_project(app, dir);
    } else {
        rc = lt_app_open_ini(app, ini_path);
    }
    if (rc < 0) { return rc; }
    if (demo) { return lt_app_connect_demo(app); }
    if (port[0] != '\0') { return lt_app_connect_serial(app, port, baud); }
    return 0;
}

static void save_ecu_tune(lt_app *app, sig_state *s)
{
    lt_tune *t;
    char name[160], *path;
    if (app->mem == NULL || app->ini == NULL) { return; }
    t = app->tune != NULL ? lt_tune_clone(app->tune) : lt_tune_from_defaults(app->ini);
    lt_sync_apply_ecu(t, app->ini, app->mem);
    lt_snprintf(name, sizeof(name), "ECU backup %s.msq", s->ecu_sig);
    path = lt_path_join(app->has_project ? app->project.path : (app->config_dir != NULL ? app->config_dir : "."),
                        name);
    if (path != NULL && lt_tune_save(t, path) == 0) {
        lt_snprintf(s->note, sizeof(s->note), "ECU tune saved to %s", path);
    } else {
        lt_snprintf(s->note, sizeof(s->note), "Could not save the ECU tune: %s", lt_err_get());
    }
    lt_free(path);
    lt_tune_free(t);
}

static void sig_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    sig_state *s = (sig_state *)d->data;
    int open = d->open, w1 = -1, w2[2], w3[3], act = 0;
    const char *picked;
    if (s == NULL) {
        s = (sig_state *)lt_calloc(1, sizeof(*s));
        d->data = s;
        lt_strlcpy(s->ini_sig, app->ini != NULL && app->ini->signature != NULL ? app->ini->signature : "",
                   sizeof(s->ini_sig));
        if (d->target[0] != '\0') {
            lt_strlcpy(s->ecu_sig, d->target, sizeof(s->ecu_sig));
        } else {                          /* LT_SHOT_OPEN preview: a neighbouring firmware build */
            size_t n;
            s->preview = 1;
            lt_strlcpy(s->ecu_sig, s->ini_sig, sizeof(s->ecu_sig));
            n = strlen(s->ecu_sig);
            if (n > 0) { s->ecu_sig[n - 1] = s->ecu_sig[n - 1] == '9' ? '8' : '9'; }
        }
        sig_reload(s);
        s->online_gen = lt_feat_online_generation(app);
    }
    if (s->online_gen != lt_feat_online_generation(app)) {     /* INI imported by Search Online */
        s->online_gen = lt_feat_online_generation(app);
        sig_reload(s);
    }
    picked = lt_app_file_result(app, FD_SYNC_INI);
    if (picked != NULL) {
        char *id = NULL;
        if (lt_ini_repo_import(&s->repo, picked, &id) == 0) {
            lt_free(id);
            sig_reload(s);
            lt_snprintf(s->note, sizeof(s->note), "Imported %s", picked);
        } else {
            lt_snprintf(s->note, sizeof(s->note), "Import failed: %s", lt_err_get());
        }
    }
    if (lt_ui_modal_begin(ui, "INI Signature Mismatch", CH * 80, CV * 28, &open)) {
        w2[0] = CH * 16; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "ECU Signature:");
        lt_ui_label_ex(ui, s->ecu_sig, t->warning, LT_UI_ALIGN_LEFT);
        lt_ui_label(ui, "INI Signature:");
        lt_ui_label_ex(ui, s->ini_sig, t->text2, LT_UI_ALIGN_LEFT);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui,
            lt_sync_sig_match(s->ini_sig, s->ecu_sig) == LT_SIG_PARTIAL
              ? "The ECU signature partially matches the loaded INI file. This usually means the ECU "
                "firmware and INI file are from different versions of the same platform (e.g., "
                "different release dates). Using a partial match may work, but some features might "
                "not display correctly."
              : "The ECU signature does not match the loaded INI file. Using the wrong INI may cause "
                "read/write errors or incorrect data display.", t->warning);
        lt_ui_separator(ui);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, "Save your current ECU tune to your PC before switching INI files. "
                             "This protects your existing tune.", t->text2);
        if (lt_ui_button(ui, "Save Current ECU Tune First")) { save_ecu_tune(app, s); }
        lt_ui_separator(ui);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_heading(ui, s->n_m > 0 && s->m[0].kind == LT_SIG_EXACT ? "Matching INI files found:"
                                                                      : "Similar INI files found:");
        if (s->n_m > 0) {
            lt_ui_layout_row(ui, 1, &w1, CV * 5);
            lt_ui_listbox_cb(ui, "inis", &s->sel, s->n_m, sig_item, s);
        } else {
            lt_ui_label_ex(ui, "No matching INI files in the INI library or among the definitions "
                               "bundled with LibreTune.", t->muted, LT_UI_ALIGN_LEFT);
        }
        if (s->note[0] != '\0') { lt_ui_label_wrap(ui, s->note, t->text2); }
        w3[0] = -1; w3[1] = -1; w3[2] = -1;
        lt_ui_layout_row(ui, 3, w3, 0);
        if (lt_ui_button_ex(ui, "Switch to Selected INI", s->n_m > 0 ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED)
            && s->n_m > 0) { act = 1; }
        if (lt_ui_button(ui, "Browse for INI File...")) {
            lt_app_file_dialog(app, FD_SYNC_INI, LT_UI_FILE_OPEN, NULL, "",
                               "ECU definitions|*.ini|All files|*");
        }
        if (lt_ui_button(ui, "Search Online")) { lt_feat_online_open(app, s->ecu_sig); }
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button(ui, "Continue Anyway")) { act = 2; }
        if (lt_ui_button(ui, "Disconnect")) { act = 3; }
        lt_ui_modal_end(ui);
    }
    if (act == 1 && !s->preview) {
        char *path = s->repo_ok ? match_path(&s->repo, &s->m[s->sel]) : NULL;
        if (path != NULL) {
            sync_state *st = get_state(app);
            st->sync_after_sig = 0;      /* the reconnect runs its own checks */
            /* switching closes the project and with it this dialog: do not touch d/s after */
            (void)switch_ini(app, path);
            lt_free(path);
            return;
        }
    } else if (act == 2 || (act == 1 && s->preview)) {
        sync_state *st = get_state(app);
        open = 0;
        if (st->sync_after_sig && !s->preview) { st->sync_after_sig = 0; d->open = 0; tune_check(app); }
    } else if (act == 3) {
        lt_app_disconnect(app);
        open = 0;
    }
    d->open = open;
}

static void sig_close(lt_app *app, lt_app_dialog *d)
{
    sig_state *s = (sig_state *)d->data;
    LT_UNUSED(app);
    if (s != NULL) {
        if (s->repo_ok) { lt_ini_repo_close(&s->repo); }
        lt_free(s);
    }
    d->data = NULL;
}

/* ------------------------------------------------------------------ */
/* Connect ECU wizard                                                  */
typedef struct wiz_state {
    int step;                        /* 0 scan, 1 choose, 2 failed */
    lt_ecuport *ports; int n_ports, n_hidden;   /* USB ports first (ports_logic.h) */
    lt_ecuport_scan *scan;           /* stepped from the UI loop, never blocks */
    char port[128], sig[128], note[480], why[96], name[64], err[200];
    long baud;
    lt_ini_repo repo; int repo_ok;
    lt_sync_ini_match m[MAX_MATCH]; int n_m, sel;
    lt_inidb_pick pk;
} wiz_state;

/* default project name for a picked definition: "rusEFI proteus_f4", "Speeduino 202501" */
static void pick_name(const lt_inidb_pick *pk, const char *sig, char *out, size_t cap)
{
    lt_sig_info si;
    const lt_inidb_def *d = pk->src == LT_INIDB_SRC_BUNDLE ? lt_inidb_at((size_t)pk->bundle) : NULL;
    if (d != NULL && d->board[0] != '\0') {
        lt_snprintf(out, cap, "%s %s", lt_inidb_family_title(d->family), d->board);
    } else if (lt_sig_parse(sig, &si) == 0 && si.family == LT_SIGFAM_RUSEFI) {
        lt_snprintf(out, cap, "%s %s", si.variant[0] != '\0' ? si.variant : "rusEFI", si.board);
    } else if (lt_sig_parse(sig, &si) == 0 && si.family == LT_SIGFAM_SPEEDUINO) {
        lt_snprintf(out, cap, "Speeduino %ld%02ld", si.when / 12L, si.when % 12L + 1L);
    } else {
        lt_strlcpy(out, "My ECU", cap);
    }
}

/* create a project named `name` (or "name 2", "name 3", ... when taken) with ini_path and open it */
static int create_project(lt_app *app, const char *name, const char *ini_path, char *dir_out,
                          size_t cap)
{
    lt_project p;
    lt_ini *probe = lt_ini_load_file(ini_path, NULL);
    char nm[96], *dir;
    int rc = LT_EEXIST, k;
    memset(&p, 0, sizeof(p));
    if (probe == NULL) { return LT_EDATA; }
    for (k = 1; k < 100 && rc == LT_EEXIST; k++) {
        if (k == 1) { lt_strlcpy(nm, name, sizeof(nm)); }
        else { lt_snprintf(nm, sizeof(nm), "%s %d", name, k); }
        rc = lt_project_create(nm, ini_path, probe->signature != NULL ? probe->signature : "", NULL,
                               &p);
    }
    lt_ini_free(probe);
    if (rc < 0) { return rc; }
    dir = lt_strdup(p.path);
    lt_project_close(&p);
    if (dir_out != NULL) { lt_strlcpy(dir_out, dir, cap); }
    rc = lt_app_open_project(app, dir);
    lt_free(dir);
    return rc;
}

static void wiz_match(wiz_state *w)
{
    if (w->repo_ok) { lt_ini_repo_close(&w->repo); w->repo_ok = 0; }
    w->repo_ok = lt_ini_repo_open(&w->repo, NULL) == 0;
    w->n_m = collect_defs(&w->repo, w->repo_ok, w->sig, w->m, MAX_MATCH, &w->pk);
    w->sel = 0;
    if (strcmp(w->name, "My ECU") == 0 || w->name[0] == '\0') {
        pick_name(&w->pk, w->sig, w->name, sizeof(w->name));
    }
}

static const char *wiz_item(void *ud, int i)
{
    static char buf[300];
    wiz_state *w = (wiz_state *)ud;
    lt_snprintf(buf, sizeof(buf), "%s   [%s]   %s", w->m[i].name, w->m[i].signature,
                w->m[i].kind == LT_SIG_EXACT ? "Exact" : "Near");
    return buf;
}

static void wiz_scan_start(lt_app *app, wiz_state *w)
{
    lt_free(w->ports);
    lt_ecuport_scan_free(w->scan);
    w->n_ports = lt_ecuport_list(0, lt_feat_devices_mem(app), &w->ports, &w->n_hidden);
    w->scan = lt_ecuport_scan_new(w->ports, w->n_ports);
    w->step = 0;
}

static void wiz_scan_step(wiz_state *w)
{
    int r = lt_ecuport_scan_step(w->scan, lt_plat_now_ms());
    if (r == LT_ECUSCAN_FOUND) {
        const lt_ecuport_hit *h = lt_ecuport_scan_hit(w->scan);
        lt_strlcpy(w->port, h->port, sizeof(w->port));
        lt_strlcpy(w->sig, h->signature, sizeof(w->sig));
        w->baud = h->baud;
        w->step = 1;
        wiz_match(w);
    } else if (r == LT_ECUSCAN_DONE) {
        lt_ecuport_scan_report(w->scan, w->n_hidden, w->why, sizeof(w->why), w->note, sizeof(w->note));
        w->step = 2;
    }
}

/* the definition status line of step 2 */
static void wiz_pick_line(lt_app *app, wiz_state *w)
{
    const lt_theme *t = lt_ui_theme(app->ui);
    char line[400];
    int w1 = -1;
    lt_ui_layout_row(app->ui, 1, &w1, 0);
    if (w->pk.quality == LT_INIDB_EXACT) {
        lt_snprintf(line, sizeof(line), "Exact definition found: %s", w->pk.label);
        lt_ui_label_wrap(app->ui, line, t->success);
    } else if (w->pk.quality == LT_INIDB_NEAR) {
        lt_snprintf(line, sizeof(line), "No exact definition for this firmware; the closest is %s. "
                    "Some settings may not match.", w->pk.label);
        lt_ui_label_wrap(app->ui, line, t->warning);
    } else if (w->n_m == 0) {
        lt_ui_label_wrap(app->ui, "No definition for this firmware in the INI library or among the "
                                  "bundled definitions. Browse for the INI that belongs to it, or "
                                  "search online.", t->warning);
    }
    if (w->pk.drive_compressed) {
        lt_ui_label_wrap(app->ui, "The ECU's USB drive only holds a compressed INI (rusefi.ini.7z), "
                                  "which LibreTune cannot unpack.", t->muted);
    }
}

static void wiz_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    wiz_state *w = (wiz_state *)d->data;
    int open = d->open, w1 = -1, w2[2], w3[3], create = 0;
    const char *picked;
    if (w == NULL) {
        w = (wiz_state *)lt_calloc(1, sizeof(*w));
        d->data = w;
        lt_strlcpy(w->name, "My ECU", sizeof(w->name));
        if (d->target[0] != '\0') {
            /* "port\tbaud\tsignature": an ECU already identified (lt_feat_sync_connect_auto) */
            char tmp[300], *b, *g;
            lt_strlcpy(tmp, d->target, sizeof(tmp));
            b = strchr(tmp, '\t');
            g = b != NULL ? strchr(b + 1, '\t') : NULL;
            if (g != NULL) {
                *b = '\0';
                *g = '\0';
                lt_strlcpy(w->port, tmp, sizeof(w->port));
                w->baud = atol(b + 1);
                lt_strlcpy(w->sig, g + 1, sizeof(w->sig));
                w->step = 1;
                wiz_match(w);
            }
        }
        if (w->step == 0) { wiz_scan_start(app, w); }
    }
    if (w->step == 0) { wiz_scan_step(w); }
    if (w->step == 0) { lt_ui_request_redraw(ui); }
    picked = lt_app_file_result(app, FD_WIZ_INI);
    if (picked != NULL) {
        char *id = NULL;
        if (w->repo_ok && lt_ini_repo_import(&w->repo, picked, &id) == 0) {
            lt_free(id);
            wiz_match(w);
        } else {
            lt_snprintf(w->err, sizeof(w->err), "Import failed: %s", lt_err_get());
        }
    }
    if (lt_ui_modal_begin(ui, "Connect ECU", CH * 76, CV * 24, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (w->step == 0) {
            char b[240];
            lt_ui_heading(ui, "Step 1 of 2: Find your ECU");
            lt_ecuport_scan_status(w->scan, b, sizeof(b));
            lt_ui_progress(ui, lt_ecuport_scan_progress(w->scan), b);
            if (lt_ui_button(ui, "Cancel scan")) { lt_ecuport_scan_cancel(w->scan); wiz_scan_step(w); }
        } else if (w->step == 2) {
            lt_ui_heading(ui, w->why[0] != '\0' ? w->why : "No ECU detected");
            lt_ui_label_wrap(ui, w->note, t->text2);
            lt_ui_label_wrap(ui, "You can also connect manually from the connection dialog.", t->muted);
            w2[0] = -1; w2[1] = -1;
            lt_ui_layout_row(ui, 2, w2, 0);
            if (lt_ui_button(ui, "Scan again")) { wiz_scan_start(app, w); }
            if (lt_ui_button(ui, "Manual connection...")) { (void)lt_app_connect_dialog(app); open = 0; }
        } else {
            int ok;
            lt_ui_heading(ui, "Step 2 of 2: Choose the definition and name the project");
            w2[0] = CH * 18; w2[1] = -1;
            lt_ui_layout_row(ui, 2, w2, 0);
            lt_ui_label(ui, "ECU found on");
            {
                char b[200];
                lt_snprintf(b, sizeof(b), "%s at %ld baud", w->port, w->baud);
                lt_ui_label_ex(ui, b, t->success, LT_UI_ALIGN_LEFT);
            }
            lt_ui_label(ui, "Signature");
            lt_ui_label_ex(ui, w->sig, t->text, LT_UI_ALIGN_LEFT);
            wiz_pick_line(app, w);
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (w->n_m > 0) {
                lt_ui_label_ex(ui, w->m[0].kind == LT_SIG_EXACT ? "Matching definitions:"
                                                                 : "Similar definitions:",
                               t->text2, LT_UI_ALIGN_LEFT);
                lt_ui_layout_row(ui, 1, &w1, CV * 5);
                lt_ui_listbox_cb(ui, "inis", &w->sel, w->n_m, wiz_item, w);
            }
            w3[0] = -1; w3[1] = -1; w3[2] = -1;
            lt_ui_layout_row(ui, 3, w3, 0);
            if (lt_ui_button(ui, "Browse for INI File...")) {
                lt_app_file_dialog(app, FD_WIZ_INI, LT_UI_FILE_OPEN, NULL, "",
                                   "ECU definitions|*.ini|All files|*");
            }
            if (lt_ui_button(ui, w->pk.quality == LT_INIDB_NEAR ? "Find Exact INI..." : "Search Online")) {
                lt_feat_online_open(app, w->sig);
            }
            lt_ui_label(ui, "");
            w2[0] = CH * 18; w2[1] = -1;
            lt_ui_layout_row(ui, 2, w2, 0);
            lt_ui_label(ui, "Project name");
            lt_ui_text(ui, "pname", w->name, sizeof(w->name), 0);
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (w->err[0] != '\0') { lt_ui_label_wrap(ui, w->err, t->error); }
            ok = w->n_m > 0 && w->name[0] != '\0';
            w2[0] = -1; w2[1] = -1;
            lt_ui_layout_row(ui, 2, w2, 0);
            if (lt_ui_button_ex(ui, "Create Project and Connect", ok ? LT_UI_BTN_DEFAULT : LT_UI_BTN_DISABLED) && ok) {
                create = 1;
            }
            if (lt_ui_button(ui, "Cancel")) { open = 0; }
        }
        lt_ui_modal_end(ui);
    }
    if (create) {
        char *ini_path = w->repo_ok ? match_path(&w->repo, &w->m[w->sel]) : NULL;
        char port[128], name[64];
        long baud = w->baud;
        lt_strlcpy(port, w->port, sizeof(port));
        lt_strlcpy(name, w->name, sizeof(name));
        if (ini_path != NULL && create_project(app, name, ini_path, NULL, 0) == 0) {
            /* the new project closed this dialog (and freed d/w): nothing may touch them now */
            lt_free(ini_path);
            if (strcmp(port, "demo") == 0) { (void)lt_app_connect_demo(app); }
            else { (void)lt_app_connect_serial(app, port, baud); }
            return;
        }
        lt_free(ini_path);
        lt_snprintf(w->err, sizeof(w->err), "Cannot create the project: %s", lt_err_get());
    }
    d->open = open;
}

static void wiz_close(lt_app *app, lt_app_dialog *d)
{
    wiz_state *w = (wiz_state *)d->data;
    LT_UNUSED(app);
    if (w != NULL) {
        lt_ecuport_scan_free(w->scan);
        lt_free(w->ports);
        if (w->repo_ok) { lt_ini_repo_close(&w->repo); }
        lt_free(w);
    }
    d->data = NULL;
}

static void act_wizard(lt_app *app)
{
    (void)lt_app_dialog_push(app, "connect_wizard", "", "Connect ECU");
}

/* ------------------------------------------------------------------ */
/* connect, identify, pick the INI, open or create the project          */

/* the newest project whose definition has exactly signature sig; 1 found (dir in out) */
static int find_project(const char *sig, char *out, size_t cap)
{
    lt_project_info *v = NULL;
    size_t n = 0, i;
    int found = 0;
    if (lt_project_list(&v, &n, NULL) != 0) { return 0; }
    for (i = 0; i < n && !found; i++) {
        if (v[i].signature != NULL && v[i].path != NULL && same_sig(v[i].signature, sig)) {
            lt_strlcpy(out, v[i].path, cap);
            found = 1;
        }
    }
    lt_project_info_free(v, n);
    return found;
}

static int probe_port(const char *port, long baud, char *sig, size_t cap)
{
    lt_disc_cfg cfg;
    lt_disc_result *res = NULL;
    size_t n = 0, k;
    int rc = LT_ETIMEOUT;
    long b = baud;
    lt_disc_cfg_default(&cfg);
    if (b > 0) {
        cfg.bauds = &b;
        cfg.n_bauds = 1;
    }
    if (lt_disc_run(&cfg, &port, 1, &res, &n) == 0 && res != NULL) {
        for (k = 0; k < n; k++) {
            if (res[k].kind == LT_DISC_SIGNATURE) {
                lt_strlcpy(sig, res[k].signature, cap);
                rc = 0;
                break;
            }
        }
    }
    lt_free(res);
    if (rc != 0) { lt_err_set("No ECU answered on %s", port); }
    return rc;
}

int lt_feat_sync_connect_auto(lt_app *app, const char *port, long baud, const char *signature,
                              lt_feat_auto *res)
{
    lt_feat_auto tmp;
    lt_feat_auto *r = res != NULL ? res : &tmp;
    sync_state *st = get_state(app);
    char sig[128], dir[512], name[96], *path = NULL;
    lt_ini_repo repo;
    lt_inidb_pick pk;
    int q, rc, demo;
    memset(r, 0, sizeof(*r));
    if (port == NULL || port[0] == '\0') {
        lt_err_set("No port given");
        return LT_EINVAL;
    }
    demo = strcmp(port, "demo") == 0;
    if (signature != NULL && signature[0] != '\0') {
        lt_strlcpy(sig, signature, sizeof(sig));
    } else if (demo) {
        lt_err_set("The demo ECU needs a signature to answer with");
        return LT_EINVAL;
    } else {
        rc = probe_port(port, baud, sig, sizeof(sig));
        if (rc < 0) { return rc; }
    }
    lt_strlcpy(r->signature, sig, sizeof(r->signature));
    if (lt_ini_repo_open(&repo, NULL) != 0) { return LT_EIO; }
    q = lt_inidb_resolve(sig, &repo, LT_INIDB_USE_DRIVE, &pk);
    if (q == LT_INIDB_NONE) {
        char tgt[300];
        lt_ini_repo_close(&repo);
        lt_snprintf(tgt, sizeof(tgt), "%s\t%ld\t%s", port, baud, sig);
        (void)lt_app_dialog_push(app, "connect_wizard", tgt, "Connect ECU");
        lt_err_set("No ECU definition fits %s", sig);
        return LT_ENOENT;
    }
    r->quality = q;
    lt_strlcpy(r->label, pk.label, sizeof(r->label));
    dir[0] = '\0';
    if (find_project(pk.signature, dir, sizeof(dir))) {
        lt_ini_repo_close(&repo);
        rc = lt_app_open_project(app, dir);
    } else {
        rc = lt_inidb_pick_path(&repo, &pk, &path);
        lt_ini_repo_close(&repo);
        if (rc == 0) {
            pick_name(&pk, sig, name, sizeof(name));
            rc = create_project(app, name, path, dir, sizeof(dir));
            r->created = rc == 0;
        }
        lt_free(path);
    }
    if (rc < 0) { return rc; }
    lt_strlcpy(r->project, dir, sizeof(r->project));
    /* the on-connect check must not resolve this ECU again */
    lt_strlcpy(st->auto_ecu, sig, sizeof(st->auto_ecu));
    lt_strlcpy(st->auto_pick, pk.signature, sizeof(st->auto_pick));
    lt_strlcpy(st->near_label, pk.label, sizeof(st->near_label));
    st->auto_near = q == LT_INIDB_NEAR;
    st->near_ecu[0] = '\0';
    if (q == LT_INIDB_EXACT) { lt_app_toast(app, 1, "ECU definition: %s", pk.label); }
    rc = demo ? lt_app_connect_demo(app) : lt_app_connect_serial(app, port, baud);
    if (rc < 0) { return rc; }
    if (q == LT_INIDB_NEAR && strcmp(st->near_ecu, sig) != 0) { show_near(app, pk.label, sig); }
    return q;
}

void lt_feat_sync_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "tune_sync"; dk.draw = tune_sync_draw; dk.close = sync_close_dlg;
    lt_ext_add_dialog_kind(reg, &dk);
    dk.kind = "sig_mismatch"; dk.draw = sig_draw; dk.close = sig_close;
    lt_ext_add_dialog_kind(reg, &dk);
    dk.kind = "connect_wizard"; dk.draw = wiz_draw; dk.close = wiz_close;
    lt_ext_add_dialog_kind(reg, &dk);
    dk.kind = "inidb_near"; dk.draw = near_draw; dk.close = NULL;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_on_connect(reg, on_connect);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "Connect ECU &Wizard...", NULL, act_wizard, NULL);
}
