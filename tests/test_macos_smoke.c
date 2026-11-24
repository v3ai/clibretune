/* test_macos_smoke.c - native macOS (Cocoa) backend smoke test.
 *
 * Creates the backend, opens a window, presents a frame, polls for about a
 * second, and closes. On every other OS the cocoa constructor is a stub that
 * returns NULL, and the same happens on a Mac without a window-server session
 * (ssh login, headless daemon); in both cases the test reports "skipped" and
 * passes. It cannot hang: every poll has a timeout and the whole run is
 * bounded.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt_test.h"
#include "../src/lt.h"
#include "../src/gui/gui.h"
#include "../src/plat/plat.h"

#define SMOKE_W 480
#define SMOKE_H 300
#define SMOKE_RUN_MS 1000.0
#define SMOKE_HARD_LIMIT_MS 8000.0

LT_TEST(cocoa_smoke)
{
    lt_backend *b = lt_backend_cocoa_create();
    lt_canvas c;
    lt_event ev;
    int w = 0, h = 0, x, y, got_expose = 0, quit = 0;
    double t0, now;
    char *clip;

    if (b == NULL) {
        printf("test_macos_smoke: cocoa backend unavailable here, skipped\n");
        return;
    }
    LT_CHECK(strcmp(b->name, "cocoa") == 0);
    LT_CHECK(b->cell_mode == LT_FALSE);
    if (b->open(b, "clibretune smoke test", SMOKE_W, SMOKE_H) != 0) {
        printf("test_macos_smoke: cannot open a window (%s), skipped\n", lt_err_get());
        lt_backend_destroy(b);
        return;
    }
    b->size(b, &w, &h);
    LT_CHECK(w == SMOKE_W && h == SMOKE_H);

    lt_canvas_init(&c, w, h);
    for (y = 0; y < c.h; y++) {
        for (x = 0; x < c.w; x++) {
            /* gradient so a wrong channel order or flipped image is visible */
            c.px[y * c.pitch + x] = LT_RGB(x * 255 / c.w, y * 255 / c.h, 0x40);
        }
    }
    b->present(b, &c, NULL, lt_rect_make(0, 0, w, h));

    t0 = lt_plat_now_ms();
    for (;;) {
        now = lt_plat_now_ms();
        if (now - t0 >= SMOKE_RUN_MS || now - t0 >= SMOKE_HARD_LIMIT_MS) {
            break;
        }
        if (b->poll(b, &ev, 50)) {
            if (ev.type == LT_EV_EXPOSE) {
                got_expose = 1;
            } else if (ev.type == LT_EV_QUIT) {
                quit = 1;
                break;
            } else if (ev.type == LT_EV_RESIZE && ev.x > 0 && ev.y > 0) {
                lt_canvas_resize(&c, ev.x, ev.y);
            }
        }
        b->present(b, &c, NULL, lt_rect_make(0, 0, c.w, c.h));
    }
    LT_CHECK(got_expose);
    (void)quit;

    /* partial updates: two disjoint rectangles in one display pass (setNeedsDisplayInRect:,
     * drawRect: copies only those parts of the canvas) */
    LT_CHECK(b->present_rects != NULL);
    if (b->present_rects != NULL && c.w >= 64 && c.h >= 64) {
        lt_rect rs[2];
        for (y = 0; y < 32; y++) {
            for (x = 0; x < 32; x++) {
                c.px[y * c.pitch + x] = LT_RGB(0xFF, 0, 0);
                c.px[(c.h - 1 - y) * c.pitch + (c.w - 1 - x)] = LT_RGB(0, 0, 0xFF);
            }
        }
        rs[0] = lt_rect_make(0, 0, 32, 32);
        rs[1] = lt_rect_make(c.w - 32, c.h - 32, 32, 32);
        b->present_rects(b, &c, rs, 2);
        (void)b->poll(b, &ev, 50);
    }

    b->set_title(b, "clibretune smoke test (renamed)");
    if (b->set_clipboard != NULL && b->get_clipboard != NULL) {
        b->set_clipboard(b, "clibretune \xC3\xA9 clip");
        clip = b->get_clipboard(b);
        if (clip != NULL) {
            LT_CHECK(strcmp(clip, "clibretune \xC3\xA9 clip") == 0);
            lt_free(clip);
        }
    }
    lt_canvas_free(&c);
    b->close(b);
    b->close(b);                    /* must be safe twice */
    lt_backend_destroy(b);
}

int main(void)
{
    LT_RUN(cocoa_smoke);
    LT_TEST_MAIN_END
}
