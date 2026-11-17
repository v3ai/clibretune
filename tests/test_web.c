/* test_web.c - tests for the web GUI backend.
 *   test_web            self-contained checks over a loopback TCP client
 *   test_web serve PORT serves a fixed canvas for tests/web_client.py; prints
 *                       URL / CHECKSUM / EVENT lines on stdout, exits 0 once
 *                       the expected input events have arrived.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lt_test.h"
#include "../src/gui/gui.h"
#include "../src/plat/plat.h"

#define CW 150
#define CH 100

static lt_rect r0 = { 0, 0, CW, CH };

static void fill_canvas(lt_canvas *c)
{
    int x, y;
    c->w = CW;
    c->h = CH;
    c->pitch = CW;
    c->px = (lt_u32 *)lt_calloc((size_t)(CW * CH), sizeof(lt_u32));
    for (y = 0; y < CH; y++)
        for (x = 0; x < CW; x++) {
            lt_u32 p = LT_RGB(x * 255 / CW, y * 255 / CH, 40);
            if (y > 20 && y < 40) p = LT_RGB(200, 30, 30);          /* long runs */
            if (x > 60 && x < 70 && (y & 1)) p = LT_RGB(255, 255, 255); /* noise */
            c->px[y * CW + x] = p;
        }
}

/* FNV-1a over R,G,B,255 bytes, row-major: the same bytes the wire carries */
static lt_u32 canvas_sum(const lt_canvas *c)
{
    lt_u32 h = 2166136261UL;
    int i, k;
    for (i = 0; i < c->w * c->h; i++) {
        lt_u32 p = c->px[i];
        lt_u8 by[4];
        by[0] = (lt_u8)((p >> 16) & 255);
        by[1] = (lt_u8)((p >> 8) & 255);
        by[2] = (lt_u8)(p & 255);
        by[3] = 255;
        for (k = 0; k < 4; k++) h = ((h ^ by[k]) * 16777619UL) & 0xFFFFFFFFUL;
    }
    return h;
}

/* Fetch a URL via raw HTTP, pumping the backend while waiting. */
static char *http(lt_backend *b, int port, const char *req, int *len_out)
{
    lt_stream *s = lt_tcp_connect("127.0.0.1", port, 2000);
    lt_buf out;
    lt_event ev;
    char tmp[4096];
    int i, n;
    if (s == NULL) return NULL;
    lt_stream_write(s, req, (int)strlen(req));
    lt_buf_init(&out);
    for (i = 0; i < 40; i++) {
        b->poll(b, &ev, 20);
        n = lt_stream_read(s, tmp, sizeof(tmp), 0);
        while (n > 0) { lt_buf_append(&out, tmp, (size_t)n); n = lt_stream_read(s, tmp, sizeof(tmp), 0); }
        if (n < 0) break;
    }
    lt_stream_close(s);
    *len_out = (int)out.len;
    return lt_buf_take(&out);
}

LT_TEST(offline)
{
    lt_backend *b;
    lt_canvas c;
    lt_event ev;
    char *r, req[512];
    int port, len, sock;
    lt_server *probe;

    /* find the port by trying the backend; token comes from stderr print, so
     * instead read it from the page by cookie-less 403 / token-less checks */
    lt_plat_setenv("LT_NO_BROWSER", "1");
    probe = lt_server_listen(18700);
    sock = probe != NULL;
    if (probe != NULL) lt_server_close(probe);
    LT_CHECK(sock);
    b = lt_backend_web_create(18700);
    LT_CHECK(b != NULL && strcmp(b->name, "web") == 0);
    LT_CHECK(b->open(b, "T", 320, 200) == 0);
    port = 18700;
    b->size(b, &len, &sock);
    LT_CHECK(len == 320 && sock == 200);
    fill_canvas(&c);
    b->present(b, &c, NULL, r0);

    lt_snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: evil.example:%d\r\n\r\n", port);
    r = http(b, port, req, &len);
    LT_CHECK(r != NULL && strstr(r, " 403 ") != NULL);
    lt_free(r);
    lt_snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n", port);
    r = http(b, port, req, &len);
    LT_CHECK(r != NULL && strstr(r, " 403 ") != NULL);    /* no token */
    lt_free(r);
    lt_snprintf(req, sizeof(req), "GET /frame?since=0 HTTP/1.1\r\nHost: localhost:%d\r\n\r\n", port);
    r = http(b, port, req, &len);
    LT_CHECK(r != NULL && strstr(r, " 403 ") != NULL);    /* no cookie */
    lt_free(r);
    lt_snprintf(req, sizeof(req), "POST /input HTTP/1.1\r\nHost: localhost:%d\r\nContent-Length: 4\r\n\r\nfo\n\n", port);
    r = http(b, port, req, &len);
    LT_CHECK(r != NULL && strstr(r, " 403 ") != NULL);
    lt_free(r);
    LT_CHECK(b->poll(b, &ev, 0) == 0);
    b->close(b);
    lt_free(b);
    lt_free(c.px);
}

static int serve(int port)
{
    lt_backend *b;
    lt_canvas c;
    lt_event ev;
    int got = 0, ok_md = 0, ok_kd = 0, ok_tx = 0, ok_wh = 0, ok_rs = 0, ok_mm = 0;
    double t0;

    LT_UNUSED(got);
    lt_plat_setenv("LT_NO_BROWSER", "1");
    b = lt_backend_web_create(port);
    if (b == NULL || b->open(b, "Serve Title", 640, 480) != 0) return 2;
    fill_canvas(&c);
    printf("CHECKSUM %08lx\n", (unsigned long)canvas_sum(&c));
    printf("SIZE %d %d\n", CW, CH);
    b->present(b, &c, NULL, r0);
    fflush(stdout);
    t0 = lt_plat_now_ms();
    while (lt_plat_now_ms() - t0 < 30000.0) {
        if (b->poll(b, &ev, 100)) {
            got++;
            printf("EVENT %d %d %d %d %d %d '%s'\n", (int)ev.type, ev.x, ev.y, ev.button, ev.wheel,
                   ev.key, ev.text);
            fflush(stdout);
            if (ev.type == LT_EV_MOUSE_DOWN && ev.x == 12 && ev.y == 34 && ev.button == 1) ok_md = 1;
            if (ev.type == LT_EV_KEY_DOWN && ev.key == 0x100 && ev.mods == 2) ok_kd = 1;
            if (ev.type == LT_EV_TEXT && strcmp(ev.text, "\xc3\xa9") == 0) ok_tx = 1;
            if (ev.type == LT_EV_WHEEL && ev.wheel == -1) ok_wh = 1;
            if (ev.type == LT_EV_RESIZE && ev.x == 800 && ev.y == 600) ok_rs = 1;
            if (ev.type == LT_EV_MOUSE_MOVE && ev.x == 7 && ev.y == 8) ok_mm = 1;
            if (ok_md && ok_kd && ok_tx && ok_wh && ok_rs && ok_mm) break;
        }
        /* second frame after the client asked for input: change one pixel block */
        if (ok_md && c.px[5 * CW + 5] != LT_RGB(1, 2, 3)) {
            c.px[5 * CW + 5] = LT_RGB(1, 2, 3);
            printf("CHECKSUM2 %08lx\n", (unsigned long)canvas_sum(&c));
            fflush(stdout);
            b->present(b, &c, NULL, r0);
        }
    }
    t0 = lt_plat_now_ms();   /* linger so the client can fetch the last frame */
    while (lt_plat_now_ms() - t0 < 2500.0) b->poll(b, &ev, 50);
    b->close(b);
    lt_free(b);
    lt_free(c.px);
    printf("DONE %d\n", ok_md && ok_kd && ok_tx && ok_wh && ok_rs && ok_mm);
    return (ok_md && ok_kd && ok_tx && ok_wh && ok_rs && ok_mm) ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "serve") == 0) return serve(atoi(argv[2]));
    LT_RUN(offline);
    LT_TEST_MAIN_END
}
