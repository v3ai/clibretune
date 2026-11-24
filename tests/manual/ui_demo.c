/* ui_demo.c - interactive demo of the widget gallery (NOT run by build.sh:
 * build.sh only picks up tests/test_*.c).
 *
 * Compile (from the repository root, any C89 compiler):
 *   cc -O2 -Isrc -Itests tests/manual/ui_demo.c \
 *      $(find src -name '*.c' ! -path src/main.c) -o build/ui_demo -lm
 *
 * Run:
 *   ./build/ui_demo                       auto backend (x11 / win32 / tty / web)
 *   LT_BACKEND=web LT_NO_BROWSER=1 ./build/ui_demo
 *        prints "clibretune UI: http://127.0.0.1:PORT/?t=TOKEN" on stderr;
 *        open it in a browser (or curl the URL, then /frame?since=0 with the
 *        cookie) -- see be_web.c for the protocol.
 *   LT_DEMO_FRAMES=N                      exit after N rendered frames
 * Quit with Ctrl+Q, the window close button or Ctrl+C.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/gui/ui.h"
#include "../../src/plat/plat.h"
#include "../ui_gallery.h"

int main(void)
{
    lt_backend *be = lt_backend_select("auto");
    lt_ui *ui;
    lt_canvas cv;
    lt_cells cells;
    lt_gfx g;
    gallery gal;
    lt_event ev;
    int w = 1280, h = 800, cell, frames = 0, max_frames = 0, quit = 0, got, hint, timeout;
    char *fenv;

    memset(&cv, 0, sizeof(cv));
    memset(&cells, 0, sizeof(cells));
    if (be == NULL) {
        fprintf(stderr, "ui_demo: no backend available (set LT_BACKEND=web|x11|tty)\n");
        return 1;
    }
    if (be->open(be, "clibretune UI gallery", w, h) != 0) {
        fprintf(stderr, "ui_demo: cannot open backend '%s': %s\n", be->name, lt_err_get());
        lt_backend_destroy(be);
        return 1;
    }
    fenv = lt_plat_getenv("LT_DEMO_FRAMES");
    if (fenv != NULL) { max_frames = atoi(fenv); lt_free(fenv); }
    cell = be->cell_mode ? 1 : 0;
    be->size(be, &w, &h);
    if (cell) { lt_cells_init(&cells, w, h); lt_gfx_init_cells(&g, &cells); }
    else { lt_canvas_init(&cv, w, h); lt_gfx_init_canvas(&g, &cv); }

    ui = lt_ui_create();
    lt_ui_set_backend(ui, be);
    gallery_init(&gal);

    while (!quit) {
        hint = lt_ui_wait_hint_ms(ui);
        timeout = lt_ui_needs_redraw(ui) ? 0 : hint;
        got = be->poll(be, &ev, timeout);
        while (got) {
            if (ev.type == LT_EV_QUIT) { quit = 1; }
            else if (ev.type == LT_EV_RESIZE) {
                if (cell) {
                    lt_cells_resize(&cells, ev.x, ev.y);
                    lt_gfx_init_cells(&g, &cells);
                } else if (ev.x > 0 && ev.y > 0) {
                    lt_canvas_resize(&cv, ev.x, ev.y);
                    lt_gfx_init_canvas(&g, &cv);
                }
                lt_ui_request_redraw(ui);
            } else {
                lt_ui_input(ui, &ev);
            }
            got = be->poll(be, &ev, 0);
        }
        /* a frame runs for input, pending animation or a timer wake-up */
        if (lt_ui_needs_redraw(ui) || timeout >= 0) {
            lt_ui_begin(ui, &g, lt_plat_now_ms());
            gallery_frame(ui, &gal);
            if (lt_ui_key_pressed(ui, 'q', LT_MOD_CTRL)) { quit = 1; }
            lt_ui_end(ui);
            be->present(be, cell ? NULL : &cv, cell ? &cells : NULL, g.dirty);
            lt_gfx_reset_dirty(&g);
            frames++;
            if (max_frames > 0 && frames >= max_frames) { quit = 1; }
        }
        if (lt_ui_quit_requested(ui)) { quit = 1; }
    }
    gallery_free(&gal);
    lt_ui_destroy(ui);
    if (cell) { lt_cells_free(&cells); } else { lt_canvas_free(&cv); }
    lt_backend_destroy(be);
    fprintf(stderr, "ui_demo: %d frames rendered\n", frames);
    return 0;
}
