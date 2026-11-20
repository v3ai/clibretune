/* test_x11.c - runs the X11 backend against tests/x11_mock.py.
 * Needs env LT_X11_MOCK (socket path) and DISPLAY set to the same path
 * (see tests/run_x11_mock.sh). Skips (exit 0) when LT_X11_MOCK is unset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/lt.h"
#include "../src/gui/gui.h"
#include "../src/plat/plat.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int next_ev(lt_backend *b, lt_event *ev) {
    int i;
    for (i = 0; i < 50; i++) if (b->poll(b, ev, 100)) return 1;
    return 0;
}

/* a pattern where every pixel differs; the top byte is junk the server must ignore */
static void fill_pattern(lt_canvas *c, lt_rect r, unsigned salt) {
    int x, y;
    for (y = r.y; y < r.y + r.h; y++)
        for (x = r.x; x < r.x + r.w; x++)
            c->px[(size_t)y * (size_t)c->pitch + (size_t)x] = 0xA5000000UL |
                ((lt_u32)(((unsigned)x * 3U + salt) & 0xFFU) << 16) |
                ((lt_u32)(((unsigned)y * 5U + salt) & 0xFFU) << 8) |
                (lt_u32)(((unsigned)(x + y) + salt) & 0xFFU);
}

static int count_str(const char *hay, const char *needle) {
    int n = 0;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) { n++; p += strlen(needle); }
    return n;
}

int main(void) {
    const char *mock = getenv("LT_X11_MOCK");
    lt_backend *b;
    lt_canvas c;
    lt_event ev;
    char *logname, *log;
    lt_u32 *expect = NULL;
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
    CHECK(b->scale_hint != NULL);
    {
        const char *mode = getenv("LT_X11_MOCK_MODE");
        double sh = b->scale_hint ? b->scale_hint(b) : 0.0;
        if (mode != NULL && strcmp(mode, "bare") == 0) {
            /* nothing published by the desktop: the RandR panel (2880x1800, 302x189 mm,
             * 242 dpi) decides -> 2.0; the disconnected output is skipped */
            CHECK(sh > 1.999 && sh < 2.001);
        } else {
            /* RESOURCE_MANAGER says Xft.dpi 144 (1.5) but the XSETTINGS manager says
             * Xft/DPI 192 and window scale 2: the largest wins -> 2.0 */
            CHECK(sh > 1.999 && sh < 2.001);
        }
    }
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
    fill_pattern(&c, lt_rect_make(0, 0, 320, 200), 0);
    /* Bell first: the mock answers the image with events, so once they arrive the Bell
     * request ahead of it is in the log (sent after it, the log could be read too early) */
    CHECK(b->beep != NULL);
    if (b->beep != NULL) b->beep(b);                /* core protocol Bell request */
    b->present(b, &c, NULL, lt_rect_make(0, 0, 320, 200));      /* queued, sent by poll */
    /* the first Expose (whole window) is answered by the backend from the presented canvas;
     * the one after the window grew to 400x300 reaches the app */
    CHECK(next_ev(b, &ev) && ev.type == LT_EV_KEY_DOWN && ev.key == 'a');
    CHECK(next_ev(b, &ev) && ev.type == LT_EV_TEXT && strcmp(ev.text, "a") == 0);
    CHECK(next_ev(b, &ev) && ev.type == LT_EV_MOUSE_DOWN && ev.button == 1 && ev.x == 10 &&
          ev.y == 20);
    CHECK(next_ev(b, &ev) && ev.type == LT_EV_RESIZE && ev.x == 400 && ev.y == 300);
    CHECK(next_ev(b, &ev) && ev.type == LT_EV_EXPOSE);
    CHECK(next_ev(b, &ev) && ev.type == LT_EV_QUIT);
    {
        /* zero copy where MIT-SHM works, the app's own canvas otherwise; frames presented
         * through present_rects (one write per frame) must land in the window exactly */
        const char *mode = getenv("LT_X11_MOCK_MODE");
        int want_shm = mode == NULL || strcmp(mode, "desktop") == 0;
        lt_u32 *zc, *zc2;
        lt_canvas d;
        lt_rect rs[2];
        CHECK(b->present_rects != NULL && b->canvas_pixels != NULL);
        zc = b->canvas_pixels(b, 320, 200);
        CHECK((zc != NULL) == want_shm);
        d = c;
        if (zc != NULL) { d.px = zc; d.w = 320; d.h = 200; d.pitch = 320; }
        fill_pattern(&d, lt_rect_make(0, 0, 320, 200), 77);
        rs[0] = lt_rect_make(0, 0, 320, 100);
        rs[1] = lt_rect_make(0, 100, 320, 100);
        b->present_rects(b, &d, rs, 2);
        (void)b->poll(b, &ev, 50);
        zc2 = b->canvas_pixels(b, 320, 200);       /* next frame: same memory, contents kept */
        CHECK(zc2 == zc);
        fill_pattern(&d, lt_rect_make(50, 60, 30, 20), 200);
        rs[0] = lt_rect_make(50, 60, 30, 20);
        b->present_rects(b, &d, rs, 1);
        expect = (lt_u32 *)malloc(sizeof(lt_u32) * 320 * 200);
        if (expect != NULL) memcpy(expect, d.px, sizeof(lt_u32) * 320 * 200);
    }
    b->close(b);                                    /* the segment goes with close */
    lt_free(b);
    lt_canvas_free(&c);
    logname = (char *)malloc(strlen(mock) + 8);
    strcpy(logname, mock); strcat(logname, ".log");
    {   /* the mock logs asynchronously: wait until it has seen the DestroyWindow of close() */
        int tries;
        log = NULL;
        for (tries = 0; tries < 100; tries++) {
            log = lt_file_read(logname, NULL);
            if (log != NULL && strstr(log, "op=4 ") != NULL) break;
            lt_free(log);
            log = NULL;
            lt_plat_sleep_ms(20);
        }
        if (log == NULL) log = lt_file_read(logname, NULL);
    }
    CHECK(log != NULL);
    if (log) {
        CHECK(strstr(log, "op=1 ") != NULL);
        CHECK(strstr(log, "op=16 ") != NULL);
        CHECK(strstr(log, "op=18 ") != NULL);
        CHECK(strstr(log, "op=55 ") != NULL);
        CHECK(strstr(log, "op=101 ") != NULL);
        {
            const char *mode = getenv("LT_X11_MOCK_MODE");
            if (mode == NULL || strcmp(mode, "desktop") == 0) {
                /* MIT-SHM: attach once, verified by a pattern through a pixmap (GetImage),
                 * then 5 ShmPutImage (frame, Expose repaint, 2 + 1 rects); no pixels through
                 * the socket; detach at close */
                CHECK(count_str(log, "shm=1 ") == 1);
                CHECK(count_str(log, "op=73 ") == 1);
                CHECK(count_str(log, "shm=3 ") == 6);
                CHECK(count_str(log, "shm=2 ") == 1);
                CHECK(strstr(log, "op=72 ") == NULL);
            } else if (strcmp(mode, "shmfail") == 0 || strcmp(mode, "shmforeign") == 0) {
                /* attach refused (remote server) or attached to another segment (other IPC
                 * namespace): the verification put fails, PutImage from then on */
                CHECK(count_str(log, "shm=1 ") == 1);
                CHECK(count_str(log, "shm=3 ") == 1);
                CHECK(strstr(log, "op=72 ") != NULL);
            } else {
                CHECK(strstr(log, "op=130 ") == NULL);  /* no extension: never tried */
                CHECK(strstr(log, "op=72 ") != NULL);
            }
        }
        CHECK(strstr(log, "op=8 ") != NULL);
        CHECK(strstr(log, "op=20 ") != NULL);
        CHECK(strstr(log, "op=40 ") != NULL);
        CHECK(strstr(log, "op=104 len=1") != NULL);
        lt_free(log);
    }
    {   /* the window contents the mock assembled from the image requests */
        char *fbname = (char *)malloc(strlen(mock) + 8);
        size_t n = 0;
        char *fb;
        strcpy(fbname, mock); strcat(fbname, ".fb");
        fb = lt_file_read(fbname, &n);
        CHECK(fb != NULL && expect != NULL);
        if (fb != NULL && expect != NULL) {
            const char *hdr = "320 200\n";
            size_t hl = strlen(hdr), i, bad = 0;
            CHECK(n == hl + 320U * 200U * 4U && memcmp(fb, hdr, hl) == 0);
            if (n == hl + 320U * 200U * 4U) {
                const unsigned char *q = (const unsigned char *)fb + hl;
                for (i = 0; i < 320U * 200U; i++) {
                    lt_u32 e = expect[i];
                    if (q[4 * i] != (e & 0xFF) || q[4 * i + 1] != ((e >> 8) & 0xFF) ||
                        q[4 * i + 2] != ((e >> 16) & 0xFF)) bad++;
                }
                CHECK(bad == 0);
                if (bad) printf("framebuffer: %lu pixels differ\n", (unsigned long)bad);
            }
        }
        lt_free(fb);
        free(fbname);
    }
    free(expect);
    free(logname);
    printf(fails ? "test_x11: FAILED\n" : "test_x11: ok\n");
    return fails != 0;
}
