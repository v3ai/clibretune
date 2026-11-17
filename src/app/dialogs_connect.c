/* dialogs_connect.c - Connect dialog (manual ports, autodetect scan).
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

void lt_dlg_register_connect(lt_app *app)
{
    lt_app_dialog_kind dk;
    dk.kind = "connect"; dk.draw = connect_draw; dk.close = connect_close;
    lt_app_register_dialog_kind(app, &dk);
}
