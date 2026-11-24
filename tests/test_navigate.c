/* test_navigate.c - finding things and getting around: the single File > Connect entry, Alt
 * access keys of the ECU's own menus, the resizable sidebar, one-column INI panels in narrow
 * windows and the settings search (field captions, keyboard).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/app_ext.h"
#include "../src/app/feat.h"
#include "../src/app/hotkeys.h"
#include "../src/plat/plat.h"

static void sandbox(const char *tag)
{
    char cfg[160], ts[32];
    int i;
    lt_plat_timestamp(ts, sizeof(ts));
    for (i = 0; ts[i] != '\0'; i++) { if (ts[i] < '0' || ts[i] > '9') { ts[i] = '_'; } }
    lt_snprintf(cfg, sizeof(cfg), "build/test_navigate_%s_%s_%lu", tag, ts, (unsigned long)lt_plat_now_ms());
    (void)lt_plat_mkdirs(cfg);
    LT_CHECK(lt_plat_setenv("XDG_CONFIG_HOME", cfg) == 0);
    LT_CHECK(lt_plat_setenv("LIBRETUNE_CONFIG_DIR", "") == 0);
}

static int has_dialog(const lt_app *app, const char *kind)
{
    int i;
    for (i = 0; i < app->n_dialogs; i++) {
        if (strcmp(app->dialogs[i]->kind, kind) == 0 && app->dialogs[i]->open) { return 1; }
    }
    return 0;
}

static void drop_dialogs(lt_app *app)
{
    int i;
    for (i = 0; i < app->n_dialogs; i++) { app->dialogs[i]->open = 0; }
    LT_CHECK(lt_app_screenshot(app, "build/test_navigate_tmp.ppm", 400, 300, 2) == 0);
}

/* ---- one connect entry ---- */
LT_TEST(connect_entries)
{
    char ini[512], dir[300];
    lt_app *app;
    lt_project p;
    int i, n;
    sandbox("conn");
    app = lt_app_new();
    /* the wizard is no separate File entry any more: File > Connect opens it when needed */
    n = lt_ext_n_menu_items(app, LT_EXT_MENU_FILE);
    for (i = 0; i < n; i++) {
        LT_CHECK(strstr(lt_ext_menu_item_label(app, LT_EXT_MENU_FILE, i), "Wizard") == NULL);
    }
    LT_CHECK(n > 3);
    /* the old shortcut id still connects; the settings dialog has its own (unbound) id */
    LT_CHECK(lt_hk_core_find("conn.connect") != NULL);
    LT_CHECK(lt_hk_core_find("conn.settings") != NULL && lt_hk_core_find("conn.settings")->key == LT_HK_NONE);
    /* definition open but no project (demo, disconnected): the wizard */
    LT_CHECK(lt_app_start_demo(app, lt_app_find_demo_ini(ini, sizeof(ini))) == 0);
    lt_app_connect(app);                                /* already online: nothing happens */
    LT_CHECK(app->n_dialogs == 0);
    lt_app_disconnect(app);
    lt_app_connect(app);
    LT_CHECK(has_dialog(app, "connect_wizard"));
    drop_dialogs(app);
    /* a project with a saved port connects there; when that fails the settings dialog opens */
    lt_snprintf(dir, sizeof(dir), "build/test_navigate_proj_%lu", (unsigned long)lt_plat_now_ms());
    (void)lt_plat_mkdirs(dir);
    LT_CHECK(lt_project_create("navconn", ini, app->ini->signature, dir, &p) == 0);
    lt_strlcpy(dir, p.path, sizeof(dir));
    lt_project_close(&p);
    LT_CHECK(lt_app_open_project(app, dir) == 0);
    LT_CHECK(app->has_project && app->conn == LT_APP_OFFLINE);
    lt_app_connect(app);                                /* no saved port: the wizard */
    LT_CHECK(has_dialog(app, "connect_wizard") && !has_dialog(app, "connect"));
    drop_dialogs(app);
    lt_free(app->project.cfg.conn.port);
    app->project.cfg.conn.port = lt_strdup("/nonexistent/ttyNAV0");
    app->project.cfg.conn.baud_rate = 115200;
    lt_app_connect(app);
    LT_CHECK(app->conn == LT_APP_OFFLINE);
    LT_CHECK(has_dialog(app, "connect") && !has_dialog(app, "connect_wizard"));
    drop_dialogs(app);
    LT_CHECK(lt_app_connect_dialog(app) == 0 && has_dialog(app, "connect"));
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(connect_entries);
    LT_TEST_MAIN_END
}
