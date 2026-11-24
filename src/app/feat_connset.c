/* feat_connset.c - per-project connection settings and protocol tuning (G-05, G-06).
 *   Project connection   port, baud and auto-connect are stored in project.json and restored when
 *                        the project opens (lt_feat_connset_apply, called by lt_app_open_project);
 *                        changes made in the Connection dialogs are written back by a tick hook.
 *   Protocol tuning      runtime packet mode (Auto / Force Burst / Force OCH / Disabled), adaptive
 *                        timing; applied at the next connect. (The original's "FOME fast comms"
 *                        toggle is not offered: it had no faster path behind it.)
 *   Connection metrics   live frame counts, rate, bytes/packets and the last protocol error:
 *                        in the Connection settings page and in the dialog "conn_metrics" (click
 *                        the connection state in the status bar, or Tools menu).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"

typedef struct cs_state {
    char path[512];                  /* project the snapshot belongs to */
    char port[128];
    int baud, auto_connect;
    int valid;
} cs_state;

static cs_state *cs(lt_app *app)
{
    return (cs_state *)lt_ext_state(app, "connset", sizeof(cs_state), NULL);
}

static void snapshot(lt_app *app, cs_state *s)
{
    lt_strlcpy(s->path, app->project.path != NULL ? app->project.path : "", sizeof(s->path));
    lt_strlcpy(s->port, app->settings.port, sizeof(s->port));
    s->baud = app->settings.baud;
    s->auto_connect = app->settings.auto_connect;
    s->valid = 1;
}

/* project -> settings, once per opened project */
void lt_feat_connset_apply(lt_app *app)
{
    cs_state *s = cs(app);
    const lt_proj_config *c;
    lt_feat_prefs_project_opened(app);
    if (!app->has_project) { s->valid = 0; return; }
    c = &app->project.cfg;
    if (c->conn.port != NULL && c->conn.port[0] != '\0') {
        lt_strlcpy(app->settings.port, c->conn.port, sizeof(app->settings.port));
        if (c->conn.baud_rate > 0) { app->settings.baud = (int)c->conn.baud_rate; }
        app->settings.auto_connect = c->settings.auto_connect ? 1 : 0;
    }
    snapshot(app, s);
}

/* settings -> project when the user changed them while the project is open */
static void tick(lt_app *app)
{
    cs_state *s = cs(app);
    lt_proj_config *c;
    int changed;
    if (!app->has_project) { s->valid = 0; return; }
    if (!s->valid || strcmp(s->path, app->project.path) != 0) { snapshot(app, s); return; }
    changed = strcmp(s->port, app->settings.port) != 0 || s->baud != app->settings.baud ||
              s->auto_connect != app->settings.auto_connect;
    if (!changed) { return; }
    c = &app->project.cfg;
    if (app->settings.port[0] != '\0') {
        lt_free(c->conn.port);
        c->conn.port = lt_strdup(app->settings.port);
    }
    if (app->settings.baud > 0) { c->conn.baud_rate = (unsigned)app->settings.baud; }
    c->settings.auto_connect = app->settings.auto_connect ? LT_TRUE : LT_FALSE;
    (void)lt_project_save_config(&app->project);
    snapshot(app, s);
}

/* ------------------------------------------------------------------ */
/* metrics                                                             */
static void kv(lt_ui *ui, const char *k, const char *v, lt_color c)
{
    int w2[2];
    w2[0] = 150; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label_ex(ui, k, lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, v, c, LT_UI_ALIGN_LEFT);
}

/* the live numbers (also used by the dialog) */
static void metrics_rows(lt_app *app)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    char b[96];
    int online = app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO;
    kv(ui, "State:", lt_app_conn_label(app), online ? t->success : t->muted);
    if (!online) {
        kv(ui, "", "Connect to an ECU to see live metrics.", t->muted);
        return;
    }
    kv(ui, "Signature:", app->signature, t->text);
    lt_snprintf(b, sizeof(b), "%.1f frames/s", app->data_rate);
    kv(ui, "Realtime rate:", b, t->text);
    lt_snprintf(b, sizeof(b), "%lu ok, %lu failed", app->frames_ok, app->frames_bad);
    kv(ui, "Realtime frames:", b, app->frames_bad > 0 ? t->warning : t->text);
    if (app->frames_ok + app->frames_bad > 0) {
        double pct = 100.0 * (double)app->frames_bad / (double)(app->frames_ok + app->frames_bad);
        lt_snprintf(b, sizeof(b), "%.2f %%", pct);
        kv(ui, "Frame error rate:", b, pct > 1.0 ? t->warning : t->text);
    }
    if (app->proto != NULL) {
        const lt_proto_stats *st = lt_proto_stats_get(app->proto);
        const lt_proto_err *e = lt_proto_errinfo(app->proto);
        lt_snprintf(b, sizeof(b), "%lu sent / %lu received", st->tx_packets, st->rx_packets);
        kv(ui, "Packets:", b, t->text);
        lt_snprintf(b, sizeof(b), "%lu sent / %lu received", st->tx_bytes, st->rx_bytes);
        kv(ui, "Bytes:", b, t->text);
        lt_snprintf(b, sizeof(b), "%d ms", lt_proto_effective_timeout_ms(app->proto));
        kv(ui, "Read timeout:", b, t->text);
        kv(ui, "Protocol:", lt_proto_is_modern(app->proto) ? "CRC envelope" : "legacy", t->text);
        if (e->code != LT_PE_NONE) { kv(ui, "Last error:", e->msg, t->warning); }
    }
}

static void conn_metrics_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    int open = d->open, w1 = -1, w2[2];
    if (lt_ui_modal_begin(ui, "Connection Metrics", 480, 360, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        metrics_rows(app);
        lt_ui_layout_space(ui, 8);
        w2[0] = -1; w2[1] = 96;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "");
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

void lt_feat_connset_metrics(lt_app *app)
{
    (void)lt_app_dialog_push(app, "conn_metrics", "", "Connection Metrics");
}

static void act_metrics(lt_app *app) { lt_feat_connset_metrics(app); }

/* ------------------------------------------------------------------ */
/* settings sections (drawn inside the built-in Connection page)        */
static void caption(lt_ui *ui, const char *text)
{
    int w1 = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
    lt_ui_label_ex(ui, text, lt_ui_theme(ui)->text, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
}

void lt_feat_connset_draw(lt_app *app)
{
    static const char *const modes[] = { "Auto", "Force Burst", "Force OCH", "Disabled" };
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    lt_prefs *p = &app->settings.prefs;
    int w1 = -1, ch = 0;
    lt_ui_layout_space(ui, 8);
    caption(ui, "Project connection");
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (app->has_project) {
        char b[256];
        lt_snprintf(b, sizeof(b), "Port, baud rate and auto-connect are remembered for the project \"%s\".",
                    app->project.cfg.name != NULL ? app->project.cfg.name : "");
        lt_ui_label_wrap(ui, b, t->muted);
    } else {
        lt_ui_label_wrap(ui, "Without a project these settings are global. With a project open they are "
                             "stored in the project and restored when it opens.", t->muted);
    }
    lt_ui_layout_space(ui, 8);
    caption(ui, "Protocol tuning");
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "Runtime packet mode", t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_combo(ui, "pktmode", &p->packet_mode, modes, 4)) { ch = 1; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, "Adaptive timing (shorten waits on fast links)", &p->adaptive_timing)) { ch = 1; }
    lt_ui_label_wrap(ui, "Packet mode: Auto picks burst or output-channel reads from the INI. These "
                         "options apply at the next connect.", t->muted);
    lt_ui_layout_space(ui, 8);
    caption(ui, "Connection metrics");
    lt_ui_layout_row(ui, 1, &w1, 0);
    metrics_rows(app);
    if (ch) { lt_app_settings_save(app); }
}

void lt_feat_connset_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "conn_metrics"; dk.draw = conn_metrics_draw; dk.close = NULL;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_tick(reg, tick);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "Connection &Metrics...", NULL, act_metrics, NULL);
}
