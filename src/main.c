/* main.c - LibreTune entry point.
 * Usage: libretune [--backend NAME] [--demo] [--project DIR] [--ini FILE] [--help] [--version]
 * Env: LT_BACKEND, LT_NO_BROWSER, LT_MAX_FRAMES=N (exit after N frames),
 *      LT_SCREENSHOT=out.ppm [LT_SHOT_SIZE=WxH LT_SHOT_FRAMES=N LT_SHOT_OPEN=target
 *      LT_SHOT_CLICK=x,y LT_SHOT_THEME=name]: render offscreen, write PPM, exit.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/app.h"

static void usage(void)
{
    puts("LibreTune 0.1.0 - open source ECU tuning software\n"
         "usage: libretune [options]\n"
         "  --backend NAME   GUI backend: auto x11 win32 web tty\n"
         "  --demo           start in demo mode (simulated ECU)\n"
         "  --project DIR    open a project directory\n"
         "  --ini FILE       open an ECU definition offline (or demo with --demo)\n"
         "  --help           this text\n"
         "  --version        print the version");
}

int main(int argc, char **argv)
{
    const char *backend = NULL, *project = NULL, *ini = NULL;
    int demo = 0, i, rc = 0;
    lt_app *app;
    char *shot;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(); return 0; }
        if (strcmp(argv[i], "--version") == 0) { puts("libretune 0.1.0"); return 0; }
        if (strcmp(argv[i], "--demo") == 0) { demo = 1; }
        else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) { backend = argv[++i]; }
        else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) { project = argv[++i]; }
        else if (strcmp(argv[i], "--ini") == 0 && i + 1 < argc) { ini = argv[++i]; }
        else { fprintf(stderr, "libretune: unknown option '%s' (try --help)\n", argv[i]); return 2; }
    }
    app = lt_app_new();
    shot = lt_plat_getenv("LT_SCREENSHOT");
    if (shot == NULL) {
        if (lt_app_open_window(app, backend, 1280, 800) != 0) {
            fprintf(stderr, "libretune: %s\n", lt_err_get());
            lt_app_free(app);
            return 1;
        }
    }
    if (shot != NULL) {
        int w = 1280, h = 800, frames = 8;
        char *e = lt_plat_getenv("LT_SHOT_SIZE");
        char *th, *op;
        if (e != NULL) { sscanf(e, "%dx%d", &w, &h); lt_free(e); }
        e = lt_plat_getenv("LT_SHOT_FRAMES");
        if (e != NULL) { frames = atoi(e); lt_free(e); }
        th = lt_plat_getenv("LT_SHOT_THEME");
        if (th != NULL) { lt_strlcpy(app->settings.theme, th, sizeof(app->settings.theme)); lt_free(th); }
        if (project != NULL) { lt_app_open_project(app, project); }
        else if (demo) { lt_app_start_demo(app, ini); }
        else if (ini != NULL) { lt_app_open_ini(app, ini); }
        op = lt_plat_getenv("LT_SHOT_OPEN");
        if (op != NULL) {
            if (strncmp(op, "dlg:", 4) == 0) { lt_app_dialog_push(app, op + 4, "", op + 4); }
            else { lt_app_open_target(app, op, op); }
            lt_free(op);
        }
        rc = lt_app_screenshot(app, shot, w, h, frames);
        lt_free(shot);
        lt_app_free(app);
        return rc;
    }
    if (project != NULL) { lt_app_open_project(app, project); }
    else if (demo) { lt_app_start_demo(app, ini); }
    else if (ini != NULL) { lt_app_open_ini(app, ini); }
    rc = lt_app_run(app);
    lt_app_free(app);
    return rc;
}
