/* feat_reconnect.c - automatic reconnect (G-04): after the link drops, after controller commands
 * that reboot the ECU, after a firmware update (other features call lt_app_reconnect_request),
 * and auto-connect when a project opens.  Port selection and retry budget follow the original
 * useReconnectHandler: the last good port if it is listed, else the only listed port.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "sync_logic.h"

typedef struct rc_state {
    lt_reconnect rc;
    char auto_for[512];             /* project path auto-connect was already tried for */
} rc_state;

static rc_state *get_rc(lt_app *app)
{
    return (rc_state *)lt_ext_state(app, "reconnect", sizeof(rc_state), NULL);
}

int lt_app_reconnect_request(lt_app *app, const char *source, int delay_ms, int retries)
{
    rc_state *st = get_rc(app);
    return lt_reconnect_arm(&st->rc, &app->settings, source, app->settings.port, delay_ms, retries,
                            lt_plat_now_ms());
}

static void on_link_lost(lt_app *app)
{
    if (app->conn == LT_APP_ONLINE) { (void)lt_app_reconnect_request(app, "link-lost", -1, -1); }
}

static void on_connect(lt_app *app)
{
    get_rc(app)->rc.armed = 0;
}

/* port to try: the wanted one when listed (tcp: always), else the only listed port */
static int pick_port(const lt_reconnect *rc, char *out, size_t cap)
{
    char **ports = NULL;
    int n, i, found = 0;
    if (strncmp(rc->port, "tcp:", 4) == 0) { lt_strlcpy(out, rc->port, cap); return 1; }
    n = lt_serial_list(&ports);
    if (n < 0) { return 0; }
    for (i = 0; i < n; i++) {
        if (rc->port[0] != '\0' && strcmp(ports[i], rc->port) == 0) { found = 1; }
    }
    if (found) { lt_strlcpy(out, rc->port, cap); }
    else if (n == 1) { lt_strlcpy(out, ports[0], cap); found = 1; }
    if (ports != NULL) { lt_plat_free_list(ports, n); }
    return found;
}

static void tick(lt_app *app)
{
    rc_state *st = get_rc(app);
    int online = app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO, act;
    /* auto-connect once per opened project */
    if (!st->rc.armed && !online && app->conn == LT_APP_OFFLINE && app->has_project &&
        app->settings.auto_connect && !app->headless && app->settings.port[0] != '\0' &&
        strcmp(st->auto_for, app->project.path) != 0) {
        lt_strlcpy(st->auto_for, app->project.path, sizeof(st->auto_for));
        (void)lt_reconnect_arm(&st->rc, NULL, "auto-connect", app->settings.port, 0, 1, lt_plat_now_ms());
    }
    if (!st->rc.armed) { return; }
    act = lt_reconnect_step(&st->rc, lt_plat_now_ms(), online);
    if (act == 1) {
        char port[128];
        if (pick_port(&st->rc, port, sizeof(port)) &&
            lt_app_connect_serial(app, port, app->settings.baud > 0 ? app->settings.baud : 115200) == 0) {
            if (st->rc.tries > 1 || strstr(st->rc.source, "firmware") != NULL) {
                lt_app_toast(app, 1, "Reconnected on %s", port);
            }
        }
    } else if (act == 2) {
        lt_app_toast(app, 2, "Automatic reconnect failed - open Connection and connect when the ECU is ready.");
    }
}

static void settings_page(lt_app *app)
{
    lt_ui *ui = app->ui;
    int w1 = -1, ch = 0;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_heading(ui, "Reconnect");
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, "Reconnect automatically after controller commands", &app->settings.reconnect_cmd)) { ch = 1; }
    if (lt_ui_checkbox(ui, "Reconnect automatically after a firmware update", &app->settings.reconnect_fw)) { ch = 1; }
    lt_ui_label_wrap(ui, "A lost connection is always retried a few times. The last working port is "
                         "remembered between sessions.", lt_ui_theme(ui)->muted);
    if (ch) { lt_app_settings_save(app); }
}

void lt_feat_reconnect_register(lt_app_ext *reg)
{
    lt_ext_add_on_link_lost(reg, on_link_lost);
    lt_ext_add_on_connect(reg, on_connect);
    lt_ext_add_tick(reg, tick);
    lt_ext_add_settings_page(reg, "Reconnect", settings_page);
}
