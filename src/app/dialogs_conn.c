/* dialogs_conn.c - Connect, Settings and New Project dialogs.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dialogs.h"

#define CH 8
#define CV 16

/* ================================================================== */
/* Connect                                                             */
static const char *const bauds[] = { "9600", "19200", "38400", "57600", "115200", "230400",
                                     "460800", "921600" };
#define N_BAUDS 8
static const long scan_bauds[] = { 115200L, 921600L, 460800L, 230400L, 57600L, 9600L };
#define N_SCAN 6

typedef struct conn_state {
    char **ports; int n_ports, sel;
    int baud_sel;
    char host[128];
    /* autodetect */
    int scanning, armed, step, steps;
    int found;                   /* 1 signature found */
    char found_port[128], found_sig[96], note[160];
    long found_baud;
} conn_state;

static const char *port_get(void *ud, int i) { return ((conn_state *)ud)->ports[i]; }

static void conn_refresh(lt_app *app, conn_state *s)
{
    int i;
    if (s->ports != NULL) { lt_plat_free_list(s->ports, s->n_ports); }
    s->ports = NULL;
    s->n_ports = lt_serial_list(&s->ports);
    if (s->n_ports < 0) { s->n_ports = 0; s->ports = NULL; }
    s->sel = 0;
    for (i = 0; i < s->n_ports; i++) {
        if (strcmp(s->ports[i], app->settings.port) == 0) { s->sel = i; }
    }
}

static int sig_mismatch(lt_app *app, const char *sig)
{
    const char *want = app->ini != NULL ? app->ini->signature : NULL;
    if (want == NULL || want[0] == '\0' || sig == NULL || sig[0] == '\0') { return 0; }
    if (strcmp(want, sig) == 0) { return 0; }
    if (app->ini->signature_prefix != NULL && lt_str_starts(sig, app->ini->signature_prefix)) { return 0; }
    return 1;
}

static void scan_step(lt_app *app, conn_state *s)
{
    lt_disc_cfg cfg;
    lt_disc_result *res = NULL;
    size_t n = 0;
    int pi = s->step / N_SCAN, bi = s->step % N_SCAN;
    const char *port;
    LT_UNUSED(app);
    if (pi >= s->n_ports) { s->scanning = 0; lt_strlcpy(s->note, "No ECU found on any port", sizeof(s->note)); return; }
    port = s->ports[pi];
    lt_disc_cfg_default(&cfg);
    cfg.bauds = &scan_bauds[bi];
    cfg.n_bauds = 1;
    if (lt_disc_run(&cfg, &port, 1, &res, &n) == 0 && res != NULL) {
        size_t i;
        for (i = 0; i < n; i++) {
            if (res[i].kind == LT_DISC_SIGNATURE) {
                s->found = 1;
                s->scanning = 0;
                lt_strlcpy(s->found_port, res[i].port, sizeof(s->found_port));
                lt_strlcpy(s->found_sig, res[i].signature, sizeof(s->found_sig));
                s->found_baud = res[i].baud;
                s->sel = pi;
                lt_snprintf(s->note, sizeof(s->note), "Found %s on %s at %ld baud",
                            res[i].signature, res[i].port, res[i].baud);
                break;
            }
        }
    }
    lt_free(res);
    if (s->scanning) { s->step++; }
}

static void connect_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    conn_state *s = (conn_state *)d->data;
    int w2[2], w3[3], w1 = -1, open = d->open, i;
    int online = app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO;
    if (s == NULL) {
        s = (conn_state *)lt_calloc(1, sizeof(*s));
        d->data = s;
        conn_refresh(app, s);
        s->baud_sel = 4;
        for (i = 0; i < N_BAUDS; i++) { if (atoi(bauds[i]) == app->settings.baud) { s->baud_sel = i; } }
        lt_strlcpy(s->host, "127.0.0.1:29001", sizeof(s->host));
    }
    if (s->scanning && s->armed) { scan_step(app, s); }
    s->armed = s->scanning;
    if (s->scanning) { lt_ui_request_redraw(ui); }
    if (lt_ui_modal_begin(ui, "ECU Connection", CH * 70, CV * 21, &open)) {
        w2[0] = CH * 14; w2[1] = -1;
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_heading(ui, "Serial / USB");
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Port");
        if (s->n_ports > 0) { lt_ui_combo_cb(ui, "port", &s->sel, s->n_ports, port_get, s); }
        else { lt_ui_label_ex(ui, "no serial ports found", t->muted, LT_UI_ALIGN_LEFT); }
        lt_ui_label(ui, "Baud rate");
        lt_ui_combo(ui, "baud", &s->baud_sel, bauds, N_BAUDS);
        w3[0] = -1; w3[1] = -1; w3[2] = -1;
        lt_ui_layout_row(ui, 3, w3, 0);
        if (lt_ui_button_ex(ui, "Connect", (s->n_ports > 0 && !s->scanning) ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED)
            && s->n_ports > 0 && !s->scanning) {
            if (lt_app_connect_serial(app, s->ports[s->sel], atol(bauds[s->baud_sel])) == 0) {
                if (sig_mismatch(app, app->signature)) {
                    lt_app_toast(app, 2, "ECU signature '%s' differs from the loaded definition '%s'",
                                 app->signature, app->ini->signature);
                }
                open = 0;
            }
        }
        if (lt_ui_button_ex(ui, "Autodetect", (s->n_ports > 0 && !s->scanning) ? 0 : LT_UI_BTN_DISABLED)
            && s->n_ports > 0 && !s->scanning) {
            s->scanning = 1; s->armed = 0; s->step = 0; s->found = 0;
            s->steps = s->n_ports * N_SCAN;
            lt_snprintf(s->note, sizeof(s->note), "Scanning...");
        }
        if (lt_ui_button(ui, "Refresh ports")) { conn_refresh(app, s); }
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (s->scanning) {
            int pi = s->step / N_SCAN, bi = s->step % N_SCAN;
            char b[160];
            lt_snprintf(b, sizeof(b), "Probing %s at %ld baud",
                        pi < s->n_ports ? s->ports[pi] : "-", scan_bauds[bi]);
            lt_ui_progress(ui, s->steps > 0 ? (double)s->step / (double)s->steps : -1.0, b);
            if (lt_ui_button(ui, "Cancel scan")) { s->scanning = 0; lt_strlcpy(s->note, "Scan cancelled", sizeof(s->note)); }
        } else if (s->note[0] != '\0') {
            lt_ui_label_ex(ui, s->note, s->found ? t->success : t->text2, LT_UI_ALIGN_LEFT);
            if (s->found) {
                for (i = 0; i < N_BAUDS; i++) { if (atol(bauds[i]) == s->found_baud) { s->baud_sel = i; } }
                if (sig_mismatch(app, s->found_sig)) {
                    char b[256];
                    lt_snprintf(b, sizeof(b), "Warning: the ECU reports '%s' but the loaded definition is '%s'.",
                                s->found_sig, app->ini->signature);
                    lt_ui_label_wrap(ui, b, t->warning);
                }
            }
        }
        lt_ui_separator(ui);
        lt_ui_heading(ui, "TCP / WiFi");
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Host:port");
        lt_ui_text(ui, "host", s->host, sizeof(s->host), 0);
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button(ui, "TCP connect")) {
            char tmp[140];
            lt_snprintf(tmp, sizeof(tmp), "tcp:%s", s->host);
            if (lt_app_connect_serial(app, tmp, 0) == 0) { open = 0; }
        }
        if (lt_ui_button(ui, "Demo (simulator)")) { if (lt_app_connect_demo(app) == 0) { open = 0; } }
        lt_ui_separator(ui);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, lt_app_conn_label(app), t->text2, LT_UI_ALIGN_LEFT);
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button_ex(ui, "Disconnect", online ? 0 : LT_UI_BTN_DISABLED) && online) { lt_app_disconnect(app); }
        if (lt_ui_button(ui, "Close")) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void connect_close(lt_app *app, lt_app_dialog *d)
{
    conn_state *s = (conn_state *)d->data;
    LT_UNUSED(app);
    if (s != NULL) {
        if (s->ports != NULL) { lt_plat_free_list(s->ports, s->n_ports); }
        lt_free(s);
    }
    d->data = NULL;
}

/* ================================================================== */
/* Settings                                                            */
static const char *const themes[] = { "dark", "light", "midnight", "carbon", "synthwave",
                                      "solarized", "nord", "dracula", "highcontrast" };
static const char *const backends[] = { "auto", "x11", "win32", "web", "tty" };
static const char *const heatmaps[] = { "tunerstudio", "grayscale", "pastel", "thermal", "viridis", "plasma" };
static const char *const temps[] = { "Celsius", "Fahrenheit" };
static const char *const press[] = { "kPa", "psi", "bar" };
static const char *const afrs[] = { "AFR", "Lambda" };
static const char *const scales[] = { "1x", "2x", "3x" };

static int find_str(const char *const *a, int n, const char *s)
{
    int i;
    for (i = 0; i < n; i++) { if (strcmp(a[i], s) == 0) { return i; } }
    return 0;
}

static void swatch_row(lt_ui *ui, const char *name, const int *w2)
{
    lt_theme t;
    lt_rect r;
    lt_color cs[5];
    int i;
    if (lt_theme_preset(&t, name) != 0) { return; }
    lt_ui_layout_row(ui, 2, w2, 20);
    lt_ui_label(ui, "");
    r = lt_ui_layout_next(ui);
    cs[0] = t.bg_app; cs[1] = t.surface; cs[2] = t.primary; cs[3] = t.accent; cs[4] = t.text;
    for (i = 0; i < 5; i++) {
        lt_rect q = r;
        q.x = r.x + i * 48; q.w = 44;
        lt_ui_draw_box(ui, q, cs[i], t.border, 2);
    }
}

/* bold caption above a full-width field (stock settings form) */
static void form_label(lt_ui *ui, const char *text)
{
    int w1 = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
    lt_ui_label_ex(ui, text, lt_ui_theme(ui)->text, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
}

static void form_note(lt_ui *ui, const char *text)
{
    int w1 = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_text_style(ui, (int)LT_FONT_SANS, LT_UI_FS_SM);
    lt_ui_label_ex(ui, text, lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
}

static const char *const set_tabs[] = { "General", "Appearance", "Connection", "Keyboard Shortcuts" };

static void settings_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    lt_app_settings *st = &app->settings;
    int open = d->open, w1 = -1, sel, ch = 0, wf[2], fh;
    double hz;
    int *tab = &app->settings_tab;
    const lt_theme *th = lt_ui_theme(ui);
    if (lt_ui_modal_begin(ui, "Settings", 600, 560, &open)) {
        fh = lt_ui_cell_mode(ui) ? 16 : 30;
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_layout_gap(ui, 0, 0);
        lt_ui_tabs_flat(ui, "settabs", set_tabs, 4, tab);
        lt_ui_layout_row(ui, 1, &w1, -(fh + 8));
        if (lt_ui_scroll_begin(ui, "setbody")) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            switch (*tab) {
            case 0:
                form_label(ui, "Unit system");
                lt_ui_layout_row(ui, 1, &w1, 0);
                if (lt_ui_checkbox(ui, "Metric (\xC2\xB0" "C, kPa)", &st->metric)) {
                    st->temp_unit = st->metric ? 0 : 1;
                    st->press_unit = st->metric ? 0 : 1;
                    ch = 1;
                }
                lt_ui_layout_space(ui, 8);
                form_label(ui, "Temperature");
                lt_ui_layout_row(ui, 1, &w1, 0);
                sel = LT_CLAMP(st->temp_unit, 0, 1);
                if (lt_ui_combo(ui, "temp", &sel, temps, 2)) { st->temp_unit = sel; ch = 1; }
                form_label(ui, "Pressure");
                lt_ui_layout_row(ui, 1, &w1, 0);
                sel = LT_CLAMP(st->press_unit, 0, 2);
                if (lt_ui_combo(ui, "press", &sel, press, 3)) { st->press_unit = sel; ch = 1; }
                form_label(ui, "Air-Fuel Ratio");
                lt_ui_layout_row(ui, 1, &w1, 0);
                sel = LT_CLAMP(st->afr_unit, 0, 1);
                if (lt_ui_combo(ui, "afr", &sel, afrs, 2)) { st->afr_unit = sel; ch = 1; }
                break;
            case 1:
                form_label(ui, "Theme");
                lt_ui_layout_row(ui, 1, &w1, 0);
                sel = find_str(themes, 9, st->theme);
                if (lt_ui_combo(ui, "theme", &sel, themes, 9)) { lt_app_apply_theme(app, themes[sel]); ch = 1; }
                wf[0] = 0; wf[1] = -1;
                swatch_row(ui, themes[sel], wf);
                lt_ui_layout_space(ui, 8);
                form_label(ui, "Heat map");
                lt_ui_layout_row(ui, 1, &w1, 0);
                sel = find_str(heatmaps, 6, st->heatmap);
                if (lt_ui_combo(ui, "heat", &sel, heatmaps, 6)) { lt_strlcpy(st->heatmap, heatmaps[sel], sizeof(st->heatmap)); ch = 1; }
                form_label(ui, "UI scale");
                lt_ui_layout_row(ui, 1, &w1, 0);
                sel = LT_CLAMP(st->scale, 1, 3) - 1;
                if (lt_ui_combo(ui, "scale", &sel, scales, 3)) { st->scale = sel + 1; lt_ui_set_scale(ui, st->scale); ch = 1; }
                lt_ui_layout_space(ui, 8);
                lt_ui_layout_row(ui, 1, &w1, 0);
                if (lt_ui_checkbox(ui, "Show ECU menus in the menu bar", &st->show_ecu_menus)) { ch = 1; }
                break;
            case 2:
                form_label(ui, "Realtime rate (Hz)");
                lt_ui_layout_row(ui, 1, &w1, 0);
                hz = st->poll_hz;
                if (lt_ui_number(ui, "hz", &hz, 5, 100, 1, 0) & LT_UI_CHANGED) { st->poll_hz = (int)(hz + 0.5); ch = 1; }
                lt_ui_layout_space(ui, 8);
                lt_ui_layout_row(ui, 1, &w1, 0);
                if (lt_ui_checkbox(ui, "Connect automatically when a project opens", &st->auto_connect)) { ch = 1; }
                lt_ui_layout_space(ui, 8);
                form_label(ui, "Graphics backend");
                lt_ui_layout_row(ui, 1, &w1, 0);
                sel = find_str(backends, 5, st->backend);
                if (lt_ui_combo(ui, "backend", &sel, backends, 5)) { lt_strlcpy(st->backend, backends[sel], sizeof(st->backend)); ch = 1; }
                form_note(ui, "The backend change applies at the next start.");
                break;
            default:
                form_label(ui, "Global shortcuts");
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_wrap(ui, "Ctrl+N new project    Ctrl+O load tune    Ctrl+S save    Ctrl+Shift+S save as\n"
                                 "Ctrl+B burn    Ctrl+Q quit    Ctrl+Shift+C connect    Ctrl+Shift+D disconnect\n"
                                 "Ctrl+D dashboard    Ctrl+L log    Ctrl+Shift+A autotune    Ctrl+, settings\n"
                                 "Ctrl+W close tab    Ctrl+Tab next tab    Ctrl+1..9 tab    Ctrl+K search    F9 sidebar",
                                 th->text2);
                break;
            }
            lt_ui_scroll_end(ui);
        }
        wf[0] = -1; wf[1] = 96;
        lt_ui_layout_row(ui, 2, wf, fh);
        lt_ui_layout_next(ui);
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        if (ch) { lt_app_settings_save(app); }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

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

void lt_dlg_register_conn(lt_app *app)
{
    lt_app_dialog_kind dk;
    dk.kind = "connect"; dk.draw = connect_draw; dk.close = connect_close;
    lt_app_register_dialog_kind(app, &dk);
    dk.kind = "settings"; dk.draw = settings_draw; dk.close = NULL;
    lt_app_register_dialog_kind(app, &dk);
    dk.kind = "newproject"; dk.draw = newproj_draw; dk.close = newproj_close;
    lt_app_register_dialog_kind(app, &dk);
}
