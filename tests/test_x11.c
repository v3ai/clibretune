/* test_x11.c - runs the X11 backend against tests/x11_mock.py.
 * Needs env LT_X11_MOCK (socket path) and DISPLAY set to the same path
 * (see tests/run_x11_mock.sh). Skips (exit 0) when LT_X11_MOCK is unset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/lt.h"
#include "../src/gui/gui.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int next_ev(lt_backend *b, lt_event *ev) {
    int i;
    for (i = 0; i < 50; i++) if (b->poll(b, ev, 100)) return 1;
    return 0;
}

int main(void) {
    const char *mock = getenv("LT_X11_MOCK");
    lt_backend *b;
    lt_canvas c;
    lt_event ev;
    char *logname, *log;
    int w = 0, h = 0;
    CHECK(lt_x11_parse_xft_dpi("a: b\nXft.dpi:\t192\n") > 191.9);
    CHECK(lt_x11_parse_xft_dpi("xft.dpi: 96") > 95.9);
    CHECK(lt_x11_parse_xft_dpi("Xft.dpix: 96\nfoo: 1") == 0.0);
    CHECK(lt_x11_parse_xft_dpi("Xft.dpi: abc") == 0.0);
    CHECK(lt_x11_parse_xft_dpi(NULL) == 0.0);
    if (!mock) { printf("test_x11: LT_X11_MOCK unset, skipped\n"); return 0; }
    b = lt_backend_x11_create();
    CHECK(b != NULL);
    if (!b) return 1;
    /* Xft.dpi 144 from the mock's RESOURCE_MANAGER -> 1.5 */
    CHECK(b->scale_hint != NULL);
    if (b->scale_hint) { double sh = b->scale_hint(b); CHECK(sh > 1.499 && sh < 1.501); }
    CHECK(b->screen_size != NULL && b->screen_size(b, &w, &h) == 1 && w == 640 && h == 480);
    b->set_min_size(b, 1024, 700);
    b->set_initial_geometry(b, 30, 40, LT_GEOM_POS);
    CHECK(b->open(b, "mock", 320, 200) == 0);
    {
        int gx = 0, gy = 0, gm = 1;
        CHECK(b->get_geometry(b, &gx, &gy, &gm) == 1 && gx == 7 && gy == 9 && gm == 0);
    }
    b->size(b, &w, &h);
    CHECK(w == 320 && h == 200);
    lt_canvas_init(&c, 320, 200);
    memset(c.px, 0x7f, sizeof(lt_u32) * 320 * 200);
    b->present(b, &c, NULL, lt_rect_make(0, 0, 320, 200));
    CHECK(next_ev(b, &ev) && ev.type == LT_EV_EXPOSE);
    CHECK(next_ev(b, &ev) && ev.type == LT_EV_KEY_DOWN && ev.key == 'a');
    CHECK(next_ev(b, &ev) && ev.type == LT_EV_TEXT && strcmp(ev.text, "a") == 0);
    CHECK(next_ev(b, &ev) && ev.type == LT_EV_MOUSE_DOWN && ev.button == 1 && ev.x == 10 &&
          ev.y == 20);
    CHECK(next_ev(b, &ev) && ev.type == LT_EV_QUIT);
    b->close(b);
    lt_free(b);
    lt_canvas_free(&c);
    logname = (char *)malloc(strlen(mock) + 8);
    strcpy(logname, mock); strcat(logname, ".log");
    log = lt_file_read(logname, NULL);
    CHECK(log != NULL);
    if (log) {
        CHECK(strstr(log, "op=1 ") != NULL);
        CHECK(strstr(log, "op=16 ") != NULL);
        CHECK(strstr(log, "op=18 ") != NULL);
        CHECK(strstr(log, "op=55 ") != NULL);
        CHECK(strstr(log, "op=72 ") != NULL);
        CHECK(strstr(log, "op=101 ") != NULL);
        CHECK(strstr(log, "op=8 ") != NULL);
        CHECK(strstr(log, "op=20 ") != NULL);
        CHECK(strstr(log, "op=40 ") != NULL);
        lt_free(log);
    }
    free(logname);
    printf(fails ? "test_x11: FAILED\n" : "test_x11: ok\n");
    return fails != 0;
}
