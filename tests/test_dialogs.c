/* test_dialogs.c - smoke tests of the dialogs owner's screens: every INI
 * dialog renders, the modal dialogs open and draw, the log viewer loads an MLG.
 * Frames are rendered offscreen with lt_app_screenshot (PPMs go to build/).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/dialogs.h"
#include "../src/app/feat.h"
#include <string.h>
#include <stdlib.h>

static int shot(lt_app *app, const char *name)
{
    char path[200];
    lt_snprintf(path, sizeof(path), "build/test_dialogs_%s.ppm", name);
    return lt_app_screenshot(app, path, 1100, 760, 3);
}

static void close_all(lt_app *app)
{
    int i;
    for (i = 0; i < app->n_dialogs; i++) { app->dialogs[i]->open = 0; }
    lt_app_screenshot(app, "build/test_dialogs_tmp.ppm", 400, 300, 2);
}

LT_TEST(ini_dialogs_render)
{
    char buf[512];
    lt_app *app = lt_app_new();
    size_t i;
    int n = 0;
    LT_CHECK(lt_app_start_demo(app, lt_app_find_demo_ini(buf, sizeof(buf))) == 0);
    for (i = 0; i < app->ini->n_dialogs && n < 6; i++) {
        const lt_ini_dialog *d = app->ini->dialogs[i];
        if (d->name == NULL || d->n_comps == 0) { continue; }
        LT_CHECK(lt_app_dialog_open(app, d->name) == 0);
        LT_CHECK(shot(app, "dlg") == 0);
        n++;
    }
    LT_CHECK(n > 0);
    LT_CHECK(lt_app_special_open(app, "och-status") == 0);
    LT_CHECK(lt_app_special_open(app, "console") == 0);
    LT_CHECK(shot(app, "special") == 0);
    lt_app_free(app);
}

LT_TEST(modals_and_log_viewer)
{
    char buf[512], *pdir;
    lt_app *app = lt_app_new();
    lt_project p;
    lt_log log;
    static const char *const names[] = { "RPMValue", "TPSValue" };
    double row[2];
    int i;
    LT_CHECK(lt_app_start_demo(app, lt_app_find_demo_ini(buf, sizeof(buf))) == 0);
    lt_plat_mkdirs("build/test_dialogs_proj");
    if (lt_project_create("dlgtest", buf, app->ini->signature != NULL ? app->ini->signature : "",
                          "build/test_dialogs_proj", &p) < 0) {
        LT_CHECK(lt_project_open("build/test_dialogs_proj/dlgtest", &p) == 0);
    }
    pdir = lt_strdup(p.path);
    lt_project_close(&p);
    LT_CHECK(lt_app_open_project(app, pdir) == 0);
    LT_CHECK(lt_app_connect_demo(app) == 0);
    {
        char *rp = NULL;
        LT_CHECK(lt_restore_create(&app->project, &rp) == 0);
        lt_free(rp);
    }
    LT_CHECK(lt_app_restore_points_dialog(app) == 0);
    LT_CHECK(shot(app, "restore") == 0);
    close_all(app);
    LT_CHECK(lt_app_basemap_wizard(app) == 0);
    LT_CHECK(shot(app, "basemap") == 0);
    close_all(app);
    LT_CHECK(lt_app_connect_dialog(app) == 0);
    LT_CHECK(shot(app, "connect") == 0);
    close_all(app);
    LT_CHECK(lt_app_settings_dialog(app) == 0);
    LT_CHECK(shot(app, "settings") == 0);
    close_all(app);
    LT_CHECK(lt_app_new_project_dialog(app) == 0);
    LT_CHECK(shot(app, "newproject") == 0);
    close_all(app);
    /* log viewer with a generated log */
    lt_log_init(&log);
    lt_log_set_channels(&log, names, NULL, 2);
    for (i = 0; i < 200; i++) {
        row[0] = 800.0 + 20.0 * i; row[1] = (i % 50) * 2.0;
        lt_log_append(&log, i * 0.1, row);
    }
    LT_CHECK(lt_mlg_write("build/test_dialogs.mlg", &log, NULL) == 0);
    lt_log_free(&log);
    LT_CHECK(lt_app_datalog_open(app, 1) == 0);
    lt_strlcpy(app->fd.path, "build/test_dialogs.mlg", sizeof(app->fd.path));
    app->fd_ready = LT_DLG_FD_LOG_OPEN;
    LT_CHECK(shot(app, "viewer") == 0);
    LT_CHECK(lt_app_datalog_open(app, 0) == 0);
    LT_CHECK(shot(app, "logger") == 0);
    lt_free(pdir);
    lt_app_free(app);
}

/* quitting while a firmware update runs would cut the flash off: the app refuses and says why */
LT_TEST(firmware_flash_blocks_quit)
{
    char buf[512];
    lt_app *app;
    int i;
    lt_plat_clock_virtual(1);
    app = lt_app_new();
    LT_CHECK(lt_app_start_demo(app, lt_app_find_demo_ini(buf, sizeof(buf))) == 0);
    LT_CHECK(lt_plat_setenv("LT_SHOT_FW_START", "1") == 0);   /* reboot the demo ECU, then flash */
    LT_CHECK(lt_app_dialog_push(app, "firmware", "", "Firmware Update") != NULL);
    LT_CHECK(shot(app, "fw") == 0);
    for (i = 0; i < 400 && lt_feat_firmware_busy(app) != 1; i++) {
        lt_plat_sleep_ms(60);
        lt_app_service(app);
        lt_ui_begin(app->ui, &app->gfx, lt_plat_now_ms());
        lt_app_frame(app);
        lt_ui_end(app->ui);
    }
    LT_CHECK_INT(lt_feat_firmware_busy(app), 1);
    lt_app_request_quit(app);
    LT_CHECK(!app->quit);
    LT_CHECK(strstr(app->status, "firmware update is running") != NULL);
    (void)lt_plat_setenv("LT_SHOT_FW_START", "");
    lt_app_free(app);
    lt_plat_clock_virtual(0);
}

int main(void)
{
    LT_RUN(firmware_flash_blocks_quit);
    LT_RUN(ini_dialogs_render);
    LT_RUN(modals_and_log_viewer);
    LT_TEST_MAIN_END
}
