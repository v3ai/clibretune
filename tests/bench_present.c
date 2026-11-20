/* bench_present.c - times a pixel backend's present path (see tools/bench_present.sh, which
 * builds and runs it): X11 against tests/x11_mock.py in bench mode, Win32 under wine.
 *
 *   bench_present W H FRAMES [zc] [win32]
 *
 * For each scenario (a full frame, one 256x64 rectangle, 24 scattered 128x64 rectangles) it
 * presents FRAMES frames and polls once after each (as the app's main loop does: that is where
 * queued requests are written and completion events read), then makes one round trip so the
 * server has consumed everything, and prints the mean wall time per frame and the client CPU
 * time per frame (clock(): what presenting costs the app itself). "zc" draws into the
 * backend's canvas_pixels memory (X11: the MIT-SHM segment, Win32: a DIB section). The canvas is
 * changed before every frame, outside the timed part. Not a test: build.sh does not run it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/lt.h"
#include "../src/gui/gui.h"
#include "../src/plat/plat.h"

static void bench_touch(lt_canvas *c, const lt_rect *rs, int n, lt_u32 v)
{
    int i, x, y;
    for (i = 0; i < n; i++) {
        for (y = rs[i].y; y < rs[i].y + rs[i].h; y++) {
            lt_u32 *row = c->px + (size_t)y * (size_t)c->pitch;
            for (x = rs[i].x; x < rs[i].x + rs[i].w; x++) { row[x] = v ^ (lt_u32)x; }
        }
    }
}

static void bench_drain(lt_backend *b)
{
    lt_event ev;
    while (b->poll(b, &ev, 0)) { }
}

typedef struct bench_res { double wall, cpu; } bench_res;

static double bench_cpu_ms(void)
{
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

/* mean ms per frame: wall clock and this process's CPU time */
static bench_res bench_run(lt_backend *b, lt_canvas *c, int zc, const lt_rect *rs, int n,
                           int frames)
{
    bench_res res;
    double t0, c0;
    int f, i, gx, gy, gm;
    res.wall = 0.0;
    res.cpu = 0.0;
    for (f = 0; f < frames; f++) {
        if (zc) {
            t0 = lt_plat_now_ms();
            c0 = bench_cpu_ms();
            c->px = b->canvas_pixels(b, c->w, c->h);   /* waits until the server read it */
            res.wall += lt_plat_now_ms() - t0;
            res.cpu += bench_cpu_ms() - c0;
            if (c->px == NULL) { res.wall = res.cpu = -1.0; return res; }
        }
        bench_touch(c, rs, n, (lt_u32)f * 0x010203UL);
        t0 = lt_plat_now_ms();
        c0 = bench_cpu_ms();
        if (b->present_rects != NULL) {
            b->present_rects(b, c, rs, n);
        } else {
            for (i = 0; i < n; i++) { b->present(b, c, NULL, rs[i]); }
        }
        bench_drain(b);
        res.wall += lt_plat_now_ms() - t0;
        res.cpu += bench_cpu_ms() - c0;
    }
    t0 = lt_plat_now_ms();
    c0 = bench_cpu_ms();
    (void)b->get_geometry(b, &gx, &gy, &gm);           /* round trip: all consumed */
    res.wall += lt_plat_now_ms() - t0;
    res.cpu += bench_cpu_ms() - c0;
    res.wall /= (double)frames;
    res.cpu /= (double)frames;
    return res;
}

int main(int argc, char **argv)
{
    lt_backend *b;
    lt_canvas c, own;
    lt_rect rs[24];
    int w, h, frames, zc = 0, win32 = 0, i;
    bench_res full, one, many;
    if (argc < 4) {
        fprintf(stderr, "usage: bench_present W H FRAMES [zc] [win32]\n");
        return 2;
    }
    w = atoi(argv[1]);
    h = atoi(argv[2]);
    frames = atoi(argv[3]);
    for (i = 4; i < argc; i++) {
        if (strcmp(argv[i], "zc") == 0) { zc = 1; }
        if (strcmp(argv[i], "win32") == 0) { win32 = 1; }
    }
    if (w < 320 || h < 200 || w > 8192 || h > 8192 || frames < 1) { return 2; }
    b = win32 ? lt_backend_win32_create() : lt_backend_x11_create();
    if (b == NULL || b->open(b, "bench", w, h) != 0) {
        fprintf(stderr, "bench: cannot open the %s backend\n", win32 ? "win32" : "x11");
        return 1;
    }
    b->size(b, &w, &h);             /* the window manager may have clamped it */
    bench_drain(b);
    lt_canvas_init(&own, w, h);
    c = own;
    if (zc) {
        if (b->canvas_pixels == NULL || b->canvas_pixels(b, w, h) == NULL) {
            printf("zero copy unavailable\n");
            b->close(b);
            lt_canvas_free(&own);
            return 0;
        }
    }
    rs[0] = lt_rect_make(0, 0, w, h);
    full = bench_run(b, &c, zc, rs, 1, frames);
    rs[0] = lt_rect_make(w / 3, h / 3, 256, 64);
    one = bench_run(b, &c, zc, rs, 1, frames * 10);
    for (i = 0; i < 24; i++) {      /* 6 x 4 grid of value readouts / needles */
        rs[i] = lt_rect_make((i % 6) * (w / 6) + 8, (i / 6) * (h / 4) + 8, 128, 64);
    }
    many = bench_run(b, &c, zc, rs, 24, frames * 10);
    printf("%dx%d  full %7.2f / %6.2f   1x256x64 %6.3f / %6.3f   24x128x64 %6.3f / %6.3f\n",
           w, h, full.wall, full.cpu, one.wall, one.cpu, many.wall, many.cpu);
    b->close(b);
    lt_canvas_free(&own);
    return 0;
}
