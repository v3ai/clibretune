/* main.c - LibreTune entry point.
 * Usage: libretune [--backend NAME] [--demo] [--project DIR] [--ini FILE] [--help] [--version]
 *        [PROJECT_DIR]   (parsed by lt_app_args_parse, src/app/app_args.c)
 * Env: LT_BACKEND, LT_NO_BROWSER, LT_MAX_FRAMES=N (exit after N frames),
 *      LT_SCREENSHOT=out.ppm [LT_SHOT_SIZE=WxH LT_SHOT_FRAMES=N LT_SHOT_OPEN=target
 *      LT_SHOT_CLICK=x,y LT_SHOT_THEME=name]: render offscreen, write PPM, exit.
 *      LT_SHOT_TTY=out.ppm [LT_SHOT_TEXT LT_SHOT_ANSI LT_SHOT_KEYS ...]: same for the
 *      terminal UI (see src/app/app_ttyshot.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/app.h"
#include "app/app_ttyshot.h"
#include "app/feat.h"

/* the build system passes the git commit when it compiles this file (see build.sh) */
#ifndef LT_BUILD_ID
#define LT_BUILD_ID "dev"
#endif

static void usage(void)
{
    puts("LibreTune 0.1.0 - open source ECU tuning software\n"
         "usage: libretune [options] [PROJECT_DIR]\n"
         "  PROJECT_DIR      a project folder (with project.json): same as --project");
    puts("  --backend NAME   GUI backend: auto x11 win32 cocoa web tty\n"
         "  --demo           start in demo mode (simulated ECU)\n"
         "  --project DIR    open a project directory\n"
         "  --ini FILE       open an ECU definition offline (or demo with --demo)\n"
         "  --mcp            serve read-only MCP tools (JSON-RPC) on stdin/stdout; use with\n"
         "                   --project DIR or --ini FILE\n"
         "  --help           this text\n"
         "  --version        print the version");
}

int main(int argc, char **argv)
{
    const char *backend, *project, *ini;
    int demo, mcp, rc;
    lt_app *app;
    char *shot, err[600];
    lt_app_args args;

    rc = lt_app_args_parse(argc, argv, &args, err, sizeof(err));
    if (rc == 1 && args.help) { usage(); return 0; }
    if (rc == 1) { printf("libretune 0.1.0 (build %s)\n", LT_BUILD_ID); return 0; }
    if (rc != 0) { fprintf(stderr, "libretune: %s\n", err); return 2; }
    backend = args.backend; project = args.project; ini = args.ini;
    demo = args.demo; mcp = args.mcp;
    lt_app_set_build_id(LT_BUILD_ID);
    app = lt_app_new();
    if (mcp) {
        /* no window: tools answer from the opened project / definition */
        if (project != NULL) { lt_app_open_project(app, project); }
        else if (demo) { lt_app_start_demo(app, ini); }
        else if (ini != NULL) { lt_app_open_ini(app, ini); }
        rc = lt_feat_mcp_stdio(app);
        lt_app_free(app);
        return rc;
    }
    shot = lt_plat_getenv("LT_SCREENSHOT");
    if (shot == NULL && lt_app_shot_is_tty()) { shot = lt_strdup(""); }
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
        {   /* same render on fast and slow machines; LT_PERF=1 measures real time instead */
            char *pe = lt_plat_getenv("LT_PERF");
            if (pe == NULL || pe[0] == '\0' || pe[0] == '0') { lt_plat_clock_virtual(1); }
            lt_free(pe);
        }
        if (lt_app_shot_is_tty()) { w = 120; h = 40; }
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
