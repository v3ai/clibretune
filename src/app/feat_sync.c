/* feat_sync.c - what happens after the ECU link comes up (G-01, G-02, G-03):
 *   - signature mismatch dialog with matching INIs from the local INI library
 *   - project tune vs ECU tune choice (Use LibreTune / Ignore / Use ECU settings)
 *   - Connect ECU wizard: scan, pick the INI by signature, name the project, connect
 * Wording follows the original SignatureMismatchDialog / TuneMismatchDialog.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "sync_logic.h"

#define CH 8
#define CV 16
#define MAX_MATCH 24
#define FD_SYNC_INI (LT_APP_FD_USER0 + 20)
#define FD_WIZ_INI  (LT_APP_FD_USER0 + 21)

/* shared (per app) state: the pending tune choice */
typedef struct sync_state {
    lt_sync_cmp *cmp;
    int sync_after_sig;             /* run the tune check when the signature dialog is left */
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

static void on_connect(lt_app *app)
{
    sync_state *st = get_state(app);
    if (app->proto != NULL && lt_proto_signature_mismatch(app->proto)) {
        st->sync_after_sig = 1;
        (void)lt_app_dialog_push(app, "sig_mismatch", app->signature, "INI Signature Mismatch");
        return;
    }
    tune_check(app);
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
} sig_state;

static void sig_reload(sig_state *s)
{
    if (s->repo_ok) { lt_ini_repo_close(&s->repo); s->repo_ok = 0; }
    s->repo_ok = lt_ini_repo_open(&s->repo, NULL) == 0;
    s->n_m = s->repo_ok ? lt_sync_find_inis(&s->repo, s->ecu_sig, s->m, MAX_MATCH) : 0;
    s->sel = 0;
}

static const char *sig_item(void *ud, int i)
{
    static char buf[300];
    sig_state *s = (sig_state *)ud;
    lt_snprintf(buf, sizeof(buf), "%s   [%s]   %s", s->m[i].name, s->m[i].signature,
                s->m[i].kind == LT_SIG_EXACT ? "Exact" : "Partial");
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
        if (txt == NULL || dst == NULL || lt_file_write(dst, txt, len) < 0) {
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
            lt_ui_label_ex(ui, "No matching INI files found in the local repository.", t->muted,
                           LT_UI_ALIGN_LEFT);
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
        char *path = lt_ini_repo_path(&s->repo, s->m[s->sel].id);
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
static const long wiz_bauds[] = { 115200L, 921600L, 460800L, 230400L, 57600L, 9600L };
#define N_WBAUD 6

typedef struct wiz_state {
    int step;                        /* 0 scan, 1 choose, 2 failed */
    char **ports; int n_ports;
    int scan_i, armed;
    char port[128], sig[128], note[200], name[64], err[200];
    long baud;
    lt_ini_repo repo; int repo_ok;
    lt_sync_ini_match m[MAX_MATCH]; int n_m, sel;
} wiz_state;

static void wiz_match(wiz_state *w)
{
    if (w->repo_ok) { lt_ini_repo_close(&w->repo); w->repo_ok = 0; }
    w->repo_ok = lt_ini_repo_open(&w->repo, NULL) == 0;
    w->n_m = w->repo_ok ? lt_sync_find_inis(&w->repo, w->sig, w->m, MAX_MATCH) : 0;
    w->sel = 0;
}

static const char *wiz_item(void *ud, int i)
{
    static char buf[300];
    wiz_state *w = (wiz_state *)ud;
    lt_snprintf(buf, sizeof(buf), "%s   [%s]   %s", w->m[i].name, w->m[i].signature,
                w->m[i].kind == LT_SIG_EXACT ? "Exact" : "Partial");
    return buf;
}

static void wiz_scan_step(wiz_state *w)
{
    int pi = w->scan_i / N_WBAUD, bi = w->scan_i % N_WBAUD;
    lt_disc_cfg cfg;
    lt_disc_result *res = NULL;
    size_t n = 0, k;
    const char *port;
    if (pi >= w->n_ports) { w->step = 2; lt_strlcpy(w->note, "No ECU found on any port.", sizeof(w->note)); return; }
    port = w->ports[pi];
    lt_disc_cfg_default(&cfg);
    cfg.bauds = &wiz_bauds[bi];
    cfg.n_bauds = 1;
    if (lt_disc_run(&cfg, &port, 1, &res, &n) == 0 && res != NULL) {
        for (k = 0; k < n; k++) {
            if (res[k].kind == LT_DISC_SIGNATURE) {
                lt_strlcpy(w->port, res[k].port, sizeof(w->port));
                lt_strlcpy(w->sig, res[k].signature, sizeof(w->sig));
                w->baud = res[k].baud;
                w->step = 1;
                wiz_match(w);
                break;
            }
        }
    }
    lt_free(res);
    if (w->step == 0) { w->scan_i++; }
}

static void wiz_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    wiz_state *w = (wiz_state *)d->data;
    int open = d->open, w1 = -1, w2[2], create = 0;
    const char *picked;
    if (w == NULL) {
        w = (wiz_state *)lt_calloc(1, sizeof(*w));
        d->data = w;
        w->n_ports = lt_serial_list(&w->ports);
        if (w->n_ports < 0) { w->n_ports = 0; w->ports = NULL; }
        lt_strlcpy(w->name, "My ECU", sizeof(w->name));
        if (w->n_ports == 0) { w->step = 2; lt_strlcpy(w->note, "No serial ports found.", sizeof(w->note)); }
    }
    if (w->step == 0 && w->armed) { wiz_scan_step(w); }
    w->armed = (w->step == 0);
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
    if (lt_ui_modal_begin(ui, "Connect ECU", CH * 76, CV * 22, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (w->step == 0) {
            char b[160];
            int pi = w->scan_i / N_WBAUD, bi = w->scan_i % N_WBAUD;
            lt_ui_heading(ui, "Step 1 of 2: Find your ECU");
            lt_snprintf(b, sizeof(b), "Probing %s at %ld baud", pi < w->n_ports ? w->ports[pi] : "-",
                        wiz_bauds[bi]);
            lt_ui_progress(ui, w->n_ports > 0 ? (double)w->scan_i / (double)(w->n_ports * N_WBAUD) : -1.0, b);
            if (lt_ui_button(ui, "Cancel scan")) { w->step = 2; lt_strlcpy(w->note, "Scan cancelled.", sizeof(w->note)); }
        } else if (w->step == 2) {
            lt_ui_heading(ui, "No ECU detected");
            lt_ui_label_wrap(ui, w->note, t->text2);
            lt_ui_label_wrap(ui, "Check the cable and power, then scan again. You can also connect "
                                 "manually from the connection dialog.", t->muted);
            w2[0] = -1; w2[1] = -1;
            lt_ui_layout_row(ui, 2, w2, 0);
            if (lt_ui_button(ui, "Scan again") && w->n_ports > 0) {
                w->step = 0; w->scan_i = 0; w->armed = 0;
            }
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
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (w->n_m > 0) {
                lt_ui_label_ex(ui, w->m[0].kind == LT_SIG_EXACT ? "Matching INI files found:"
                                                                 : "Similar INI files found:",
                               t->text2, LT_UI_ALIGN_LEFT);
                lt_ui_layout_row(ui, 1, &w1, CV * 5);
                lt_ui_listbox_cb(ui, "inis", &w->sel, w->n_m, wiz_item, w);
            } else {
                lt_ui_label_wrap(ui, "No matching INI files found in the local repository. Browse "
                                     "for the INI that belongs to this firmware.", t->warning);
            }
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (lt_ui_button(ui, "Browse for INI File...")) {
                lt_app_file_dialog(app, FD_WIZ_INI, LT_UI_FILE_OPEN, NULL, "",
                                   "ECU definitions|*.ini|All files|*");
            }
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
        char *ini_path = lt_ini_repo_path(&w->repo, w->m[w->sel].id);
        char port[128], name[64];
        long baud = w->baud;
        lt_strlcpy(port, w->port, sizeof(port));
        lt_strlcpy(name, w->name, sizeof(name));
        if (ini_path != NULL && lt_app_new_project(app, name, ini_path) == 0) {
            /* the new project closed this dialog (and freed d/w): nothing may touch them now */
            lt_free(ini_path);
            (void)lt_app_connect_serial(app, port, baud);
            return;
        }
        lt_free(ini_path);
        lt_strlcpy(w->err, "Cannot create the project.", sizeof(w->err));
    }
    d->open = open;
}

static void wiz_close(lt_app *app, lt_app_dialog *d)
{
    wiz_state *w = (wiz_state *)d->data;
    LT_UNUSED(app);
    if (w != NULL) {
        if (w->ports != NULL) { lt_plat_free_list(w->ports, w->n_ports); }
        if (w->repo_ok) { lt_ini_repo_close(&w->repo); }
        lt_free(w);
    }
    d->data = NULL;
}

static void act_wizard(lt_app *app)
{
    (void)lt_app_dialog_push(app, "connect_wizard", "", "Connect ECU");
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
    lt_ext_add_on_connect(reg, on_connect);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "Connect ECU &Wizard...", NULL, act_wizard, NULL);
}
