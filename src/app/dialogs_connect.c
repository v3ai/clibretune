/* dialogs_connect.c - Connect dialog: USB ports first and named after the device behind them
 * (ports_logic.h), legacy on-board ports behind "Show all ports", a non-blocking autodetect
 * stepped from the UI loop, and errors that say what to do (port busy, permission, ...).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dialogs.h"
#include "feat.h"
#include "ports_logic.h"

#define CH 8
#define CV 16

/* ================================================================== */
/* Connect                                                             */
static const char *const bauds[] = { "9600", "19200", "38400", "57600", "115200", "230400",
                                     "460800", "921600" };
#define N_BAUDS 8

typedef struct conn_state {
    lt_ecuport *ports; int n_ports, n_hidden, sel, last_sel, show_all;
    char **items;                /* combo texts */
    int baud_sel;
    char host[128];
    lt_ecuport_scan *scan;       /* autodetect running */
    int found;                   /* 1 signature found */
    char found_port[128], found_sig[96], note[160];
    char err_title[96], err[480];   /* why the last connect / search failed */
    long found_baud;
} conn_state;

static const char *port_get(void *ud, int i) { return ((conn_state *)ud)->items[i]; }

static void set_baud(conn_state *s, long b)
{
    int i;
    for (i = 0; i < N_BAUDS; i++) { if (atol(bauds[i]) == b) { s->baud_sel = i; } }
}

static void free_ports(conn_state *s)
{
    int i;
    for (i = 0; i < s->n_ports; i++) { lt_free(s->items[i]); }
    lt_free(s->items);
    lt_free(s->ports);
    s->items = NULL; s->ports = NULL; s->n_ports = 0;
}

static void conn_refresh(lt_app *app, conn_state *s)
{
    int i;
    free_ports(s);
    s->n_ports = lt_ecuport_list(s->show_all, lt_feat_devices_mem(app), &s->ports, &s->n_hidden);
    s->items = s->n_ports > 0 ? (char **)lt_calloc((size_t)s->n_ports, sizeof(char *)) : NULL;
    s->sel = 0;
    for (i = 0; i < s->n_ports; i++) {
        char b[260];
        const lt_ecuport *p = &s->ports[i];
        lt_snprintf(b, sizeof(b), "%s  -  %s%s", p->info.name, p->label, p->remembered ? "  (used before)" : "");
        s->items[i] = lt_strdup(b);
        if (strcmp(p->info.name, app->settings.port) == 0) { s->sel = i; }
    }
    s->last_sel = -1;
}

static int sig_mismatch(lt_app *app, const char *sig)
{
    const char *want = app->ini != NULL ? app->ini->signature : NULL;
    if (want == NULL || want[0] == '\0' || sig == NULL || sig[0] == '\0') { return 0; }
    if (strcmp(want, sig) == 0) { return 0; }
    if (app->ini->signature_prefix != NULL && lt_str_starts(sig, app->ini->signature_prefix)) { return 0; }
    return 1;
}

static void scan_tick(conn_state *s)
{
    int r = lt_ecuport_scan_step(s->scan, lt_plat_now_ms());
    if (r == LT_ECUSCAN_FOUND) {
        const lt_ecuport_hit *h = lt_ecuport_scan_hit(s->scan);
        s->found = 1;
        lt_strlcpy(s->found_port, h->port, sizeof(s->found_port));
        lt_strlcpy(s->found_sig, h->signature, sizeof(s->found_sig));
        s->found_baud = h->baud;
        if (h->index >= 0 && h->index < s->n_ports) { s->sel = h->index; s->last_sel = h->index; }
        set_baud(s, h->baud);
        lt_snprintf(s->note, sizeof(s->note), "Found %s on %s at %ld baud", h->signature, h->port, h->baud);
    } else if (r == LT_ECUSCAN_DONE) {
        lt_ecuport_scan_report(s->scan, s->n_hidden, s->err_title, sizeof(s->err_title), s->err, sizeof(s->err));
        s->note[0] = '\0';
    }
    if (r != LT_ECUSCAN_RUNNING) { lt_ecuport_scan_free(s->scan); s->scan = NULL; }
}

static void connect_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    conn_state *s = (conn_state *)d->data;
    int w2[2], w3[3], w1 = -1, open = d->open;
    int online = app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO;
    int busy;
    if (s == NULL) {
        s = (conn_state *)lt_calloc(1, sizeof(*s));
        d->data = s;
        conn_refresh(app, s);
        s->baud_sel = 4;
        set_baud(s, app->settings.baud);
        if (s->n_ports > 0 && strcmp(s->ports[s->sel].info.name, app->settings.port) == 0) { s->last_sel = s->sel; }
        lt_strlcpy(s->host, "127.0.0.1:29001", sizeof(s->host));
    }
    if (s->scan != NULL) { scan_tick(s); }
    busy = s->scan != NULL;
    if (busy) { lt_ui_request_redraw(ui); }
    /* picking a port picks the baud rate its kind of device uses */
    if (s->n_ports > 0 && s->sel != s->last_sel) {
        set_baud(s, s->ports[s->sel].bauds[0]);
        s->last_sel = s->sel;
    }
    if (lt_ui_modal_begin(ui, lt_tr("ECU Connection"), CH * 80, CV * 26, &open)) {
        w2[0] = CH * 14; w2[1] = -1;
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_heading(ui, lt_tr("Serial / USB"));
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, lt_tr("Port"));
        if (s->n_ports > 0) { lt_ui_combo_cb(ui, "port", &s->sel, s->n_ports, port_get, s); }
        else {
            lt_ui_label_ex(ui, lt_tr(s->n_hidden > 0 ? "no USB serial device found" : "no serial ports found"),
                           t->muted, LT_UI_ALIGN_LEFT);
        }
        lt_ui_label(ui, lt_tr("Baud rate"));
        lt_ui_combo(ui, "baud", &s->baud_sel, bauds, N_BAUDS);
        lt_ui_label(ui, "");
        if (lt_ui_checkbox(ui, lt_tr("Show all ports (also built-in serial ports)"), &s->show_all)) {
            conn_refresh(app, s);
        }
        w3[0] = -1; w3[1] = -1; w3[2] = -1;
        lt_ui_layout_row(ui, 3, w3, 0);
        if (lt_ui_button_ex(ui, lt_tr("Connect"), (s->n_ports > 0 && !busy) ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED)
            && s->n_ports > 0 && !busy) {
            char port[128];
            lt_strlcpy(port, s->ports[s->sel].info.name, sizeof(port));
            s->err[0] = '\0';
            if (app->ini == NULL) {
                /* no definition yet (start screen): identify the ECU, pick its definition and
                 * open or create its project, like the Connect ECU wizard */
                int rc = lt_feat_sync_connect_auto(app, port, atol(bauds[s->baud_sel]),
                                                   s->found && strcmp(s->found_port, port) == 0 ? s->found_sig : NULL,
                                                   NULL);
                if (app->ini != NULL) { return; }  /* a project opened and closed this dialog */
                if (rc == LT_ENOENT) {
                    open = 0;                        /* the wizard asks for the definition */
                } else {
                    lt_strlcpy(s->err_title, "The ECU did not answer", sizeof(s->err_title));
                    lt_snprintf(s->err, sizeof(s->err), "%s: %s. Check power and firmware, or use Find my "
                                "ECU to try every speed.", port, lt_err_get());
                }
            } else if (lt_app_connect_serial(app, port, atol(bauds[s->baud_sel])) == 0) {
                open = 0;
            } else if (lt_serial_last_error() != LT_SERIAL_EOK) {
                lt_strlcpy(s->err_title, "Cannot open the port", sizeof(s->err_title));
                lt_ecuport_open_advice(lt_serial_last_error(), port, s->err, sizeof(s->err));
            } else {
                lt_strlcpy(s->err_title, "The ECU did not answer", sizeof(s->err_title));
                lt_snprintf(s->err, sizeof(s->err), "%s opened, but no ECU answered at %s baud. Check "
                            "power and firmware, or use Find my ECU to try every speed.", port,
                            bauds[s->baud_sel]);
            }
        }
        if (lt_ui_button_ex(ui, lt_tr("Find my ECU"), (s->n_ports > 0 && !busy) ? 0 : LT_UI_BTN_DISABLED)
            && s->n_ports > 0 && !busy) {
            s->scan = lt_ecuport_scan_new(s->ports, s->n_ports);
            s->found = 0; s->err[0] = '\0';
            lt_snprintf(s->note, sizeof(s->note), "Searching...");
        }
        if (lt_ui_button_ex(ui, lt_tr("Refresh ports"), busy ? LT_UI_BTN_DISABLED : 0) && !busy) {
            conn_refresh(app, s);
            s->err[0] = '\0';
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (s->scan != NULL) {
            char b[240];
            lt_ecuport_scan_status(s->scan, b, sizeof(b));
            lt_ui_progress(ui, lt_ecuport_scan_progress(s->scan), b);
            if (lt_ui_button(ui, lt_tr("Stop search"))) {
                lt_ecuport_scan_free(s->scan);
                s->scan = NULL;
                lt_strlcpy(s->note, "Search stopped", sizeof(s->note));
            }
        } else if (s->err[0] != '\0') {
            lt_ui_label_ex(ui, s->err_title, t->warning, LT_UI_ALIGN_LEFT);
            lt_ui_label_wrap(ui, s->err, t->text2);
        } else if (s->note[0] != '\0') {
            lt_ui_label_ex(ui, s->note, s->found ? t->success : t->text2, LT_UI_ALIGN_LEFT);
            if (s->found && sig_mismatch(app, s->found_sig)) {
                char b[256];
                lt_snprintf(b, sizeof(b), "Warning: the ECU reports '%s' but the loaded definition is '%s'.",
                            s->found_sig, app->ini->signature);
                lt_ui_label_wrap(ui, b, t->warning);
            }
        } else if (s->n_ports > 0) {
            lt_ui_label_wrap(ui, "USB devices are listed first. Not sure which port? Press Find my ECU.", t->muted);
        } else {
            lt_ui_label_wrap(ui, "Plug in the ECU's USB cable and switch the ignition on, then press Refresh "
                                 "ports.", t->muted);
        }
        lt_ui_separator(ui);
        lt_ui_heading(ui, lt_tr("TCP / WiFi"));
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, lt_tr("Host:port"));
        lt_ui_text(ui, "host", s->host, sizeof(s->host), 0);
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button(ui, lt_tr("TCP connect"))) {
            char tmp[140];
            lt_snprintf(tmp, sizeof(tmp), "tcp:%s", s->host);
            if (app->ini == NULL) {
                lt_strlcpy(s->err_title, "No ECU definition yet", sizeof(s->err_title));
                lt_strlcpy(s->err, "A TCP connection cannot identify the ECU by itself: set up the project "
                                   "with its definition first (File > New Project...), then connect.",
                           sizeof(s->err));
            } else if (lt_app_connect_serial(app, tmp, 0) == 0) {
                open = 0;
            }
        }
        if (lt_ui_button(ui, lt_tr("Demo (simulator)"))) {
            if (app->ini == NULL) {
                /* nothing loaded: the demo definition opens (and closes this dialog) */
                if (lt_app_start_demo(app, NULL) == 0 || app->ini != NULL) { return; }
            } else if (lt_app_connect_demo(app) == 0) {
                open = 0;
            }
        }
        lt_ui_separator(ui);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, lt_app_conn_label(app), t->text2, LT_UI_ALIGN_LEFT);
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button_ex(ui, lt_tr("Disconnect"), online ? 0 : LT_UI_BTN_DISABLED) && online) {
            lt_feat_guard_disconnect(app);
        }
        if (lt_ui_button(ui, lt_tr("Close"))) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void connect_close(lt_app *app, lt_app_dialog *d)
{
    conn_state *s = (conn_state *)d->data;
    LT_UNUSED(app);
    if (s != NULL) {
        lt_ecuport_scan_free(s->scan);
        free_ports(s);
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
