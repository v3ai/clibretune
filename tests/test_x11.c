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
    {
        /* XSETTINGS blob, little endian: Net/ThemeName "Adwaita", Xft/DPI 196608 (192),
         * Gdk/WindowScalingFactor 2 */
        static const unsigned char xs[] = {
            0, 0, 0, 0,  1, 0, 0, 0,  3, 0, 0, 0,
            1, 0, 13, 0, 'N', 'e', 't', '/', 'T', 'h', 'e', 'm', 'e', 'N', 'a', 'm', 'e', 0, 0, 0,
            0, 0, 0, 0,  7, 0, 0, 0, 'A', 'd', 'w', 'a', 'i', 't', 'a', 0,
            0, 0, 7, 0, 'X', 'f', 't', '/', 'D', 'P', 'I', 0,  0, 0, 0, 0,  0, 0, 3, 0,
            0, 0, 23, 0, 'G', 'd', 'k', '/', 'W', 'i', 'n', 'd', 'o', 'w', 'S', 'c', 'a', 'l',
            'i', 'n', 'g', 'F', 'a', 'c', 't', 'o', 'r', 0,  0, 0, 0, 0,  2, 0, 0, 0
        };
        double dpi = 0.0;
        int ws = 0;
        CHECK(lt_x11_parse_xsettings(xs, sizeof xs, &dpi, &ws) == 0);
        CHECK(dpi > 191.9 && dpi < 192.1 && ws == 2);
        dpi = 0.0; ws = 0;
        CHECK(lt_x11_parse_xsettings(xs, sizeof xs - 6, &dpi, &ws) == -1);   /* truncated */
        CHECK(lt_x11_parse_xsettings(NULL, 0, &dpi, &ws) == -1);
    }
    /* scale picking: largest source wins; physical DPI only as a last resort */
    CHECK(lt_x11_pick_scale(96.0, 192.0, 0, 0.0, 0.0, 0, 0) > 1.99);
    CHECK(lt_x11_pick_scale(0.0, 0.0, 2, 0.0, 0.0, 0, 0) > 1.99);
    CHECK(lt_x11_pick_scale(144.0, 0.0, 0, 0.0, 0.0, 0, 0) > 1.49);
    CHECK(lt_x11_pick_scale(0.0, 0.0, 0, 0.0, 0.0, 2880, 344) > 1.99);   /* 14" 2880 px: 213 dpi */
    CHECK(lt_x11_pick_scale(0.0, 0.0, 0, 0.0, 0.0, 1920, 508) < 1.01);   /* ordinary 96 dpi */
    CHECK(lt_x11_pick_scale(0.0, 0.0, 0, 0.0, 0.0, 0, 0) < 1.01);
    /* physical density per monitor (RandR) */
    CHECK(lt_x11_dpi_scale(2880, 1800, 302, 189) > 1.99 && lt_x11_dpi_scale(2880, 1800, 302, 189) < 2.01);
    CHECK(lt_x11_dpi_scale(2560, 1600, 286, 179) > 1.99 && lt_x11_dpi_scale(2560, 1600, 286, 179) < 2.01);
    CHECK(lt_x11_dpi_scale(1800, 2880, 189, 302) > 1.99);                    /* rotated */
    CHECK(lt_x11_dpi_scale(1920, 1200, 301, 188) > 1.24 && lt_x11_dpi_scale(1920, 1200, 301, 188) < 1.26);
    CHECK(lt_x11_dpi_scale(1920, 1080, 531, 299) > 0.99 && lt_x11_dpi_scale(1920, 1080, 531, 299) < 1.01);
    CHECK(lt_x11_dpi_scale(2560, 1440, 597, 336) > 0.99 && lt_x11_dpi_scale(2560, 1440, 597, 336) < 1.01);
    CHECK(lt_x11_dpi_scale(1920, 1080, 0, 0) == 0.0);                        /* no EDID */
    CHECK(lt_x11_dpi_scale(1920, 1080, 16, 9) == 0.0);                       /* bogus size */
    if (!mock) { printf("test_x11: LT_X11_MOCK unset, skipped\n"); return 0; }
    b = lt_backend_x11_create();
    CHECK(b != NULL);
    if (!b) return 1;
    /* the mock's RESOURCE_MANAGER says Xft.dpi 144 (1.5) but its XSETTINGS manager says
     * Xft/DPI 192 and window scale 2: the largest wins -> 2.0 */
    CHECK(b->scale_hint != NULL);
    if (b->scale_hint) { double sh = b->scale_hint(b); CHECK(sh > 1.999 && sh < 2.001); }
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
