/* test_gfx.c - rasterizer tests; writes build/test_gfx.ppm */
#include <stdio.h>
#include <time.h>
#include "lt_test.h"
#include "../src/gui/gui.h"

static lt_u32 P(lt_canvas *c, int x, int y) { return c->px[y * c->stride + x] & 0xFFFFFFUL; }

static void write_ppm(lt_canvas *c, const char *path)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (f == NULL) { return; }
    fprintf(f, "P6\n%d %d\n255\n", c->w, c->h);
    for (y = 0; y < c->h; y++) {
        for (x = 0; x < c->w; x++) {
            lt_u32 v = c->px[y * c->stride + x];
            fputc((int)((v >> 16) & 255), f); fputc((int)((v >> 8) & 255), f);
            fputc((int)(v & 255), f);
        }
    }
    fclose(f);
}

LT_TEST(test_rect_utils)
{
    lt_rect a = lt_rect_make(0, 0, 10, 10), b = lt_rect_make(5, 5, 10, 10), r;
    r = lt_rect_intersect(a, b);
    LT_CHECK(r.x == 5 && r.w == 5 && r.h == 5);
    r = lt_rect_intersect(a, lt_rect_make(20, 20, 3, 3));
    LT_CHECK(r.w == 0 && r.h == 0);
    LT_CHECK(lt_rect_contains(a, 9, 9) && !lt_rect_contains(a, 10, 5));
    r = lt_rect_inset(a, 2, 3);
    LT_CHECK(r.x == 2 && r.y == 3 && r.w == 6 && r.h == 4);
}

LT_TEST(test_utf8_font)
{
    const char *s = "A\xC2\xB0\xE2\x80\xA6\xFF";
    const lt_u8 *gl;
    LT_CHECK(lt_utf8_next(&s) == 'A');
    LT_CHECK(lt_utf8_next(&s) == 0xB0);
    LT_CHECK(lt_utf8_next(&s) == 0x2026);
    LT_CHECK(lt_utf8_next(&s) == 0xFFFD);
    LT_CHECK(*s == 0);
    gl = lt_font_glyph('A');
    LT_CHECK(gl[4] != 0 || gl[5] != 0 || gl[6] != 0);
    LT_CHECK(lt_font_glyph(0x1F600) == lt_font_glyph('?'));
    LT_CHECK(lt_font_glyph(0x2500) != lt_font_glyph('?'));
}

LT_TEST(test_scene)
{
    lt_canvas cv;
    lt_gfx g;
    double tri[6];
    double poly[10];
    int i;
    lt_canvas_init(&cv, 640, 480);
    lt_gfx_init_canvas(&g, &cv);
    lt_gfx_clear(&g, LT_RGB(20, 24, 32));
    LT_CHECK(P(&cv, 5, 5) == 0x141820UL);
    lt_gfx_fill_rect(&g, lt_rect_make(10, 10, 100, 60), LT_RGB(200, 40, 40));
    LT_CHECK(P(&cv, 10, 10) == 0xC82828UL && P(&cv, 109, 69) == 0xC82828UL);
    LT_CHECK(P(&cv, 110, 70) == 0x141820UL);
    lt_gfx_fill_rect(&g, lt_rect_make(60, 30, 100, 60), LT_RGBA(0, 0, 255, 128));
    LT_CHECK((P(&cv, 80, 40) & 0xFF) > 100 && ((P(&cv, 80, 40) >> 16) & 255) > 50);
    lt_gfx_rect(&g, lt_rect_make(120, 10, 60, 40), LT_RGB(255, 255, 255));
    LT_CHECK(P(&cv, 120, 10) == 0xFFFFFFUL && P(&cv, 170, 30) == 0x141820UL);
    lt_gfx_fill_round_rect(&g, lt_rect_make(190, 10, 80, 40), 12, LT_RGB(60, 160, 90));
    LT_CHECK(P(&cv, 230, 30) == 0x3CA05AUL && P(&cv, 190, 10) == 0x141820UL);
    lt_gfx_fill_rect_vgrad(&g, lt_rect_make(280, 10, 60, 60), LT_RGB(255, 255, 255), LT_RGB(0, 0, 0));
    LT_CHECK(P(&cv, 300, 10) == 0xFFFFFFUL && P(&cv, 300, 69) == 0);

    /* AA lines at angles, widths 1..8 */
    for (i = 0; i < 12; i++) {
        double a = i * 3.14159265 / 12.0;
        lt_gfx_line(&g, 80 + 0.0, 160.0, 80 + 60 * cos(a), 160 + 60 * sin(a), 1 + i % 8,
                    LT_RGB(255, 220, 80));
    }
    lt_gfx_line(&g, 200, 110, 330, 130, 1.0, LT_RGB(255, 255, 255));
    lt_gfx_line(&g, 200, 120, 330, 140, 2.5, LT_RGB(255, 255, 255));
    lt_gfx_line(&g, 200, 130, 330, 190, 4.0, LT_RGB(255, 80, 80));

    /* circles / arcs */
    lt_gfx_fill_circle(&g, 100, 300, 40, LT_RGB(80, 120, 220));
    LT_CHECK(P(&cv, 100, 300) == 0x5078DCUL && P(&cv, 100, 255) == 0x141820UL);
    lt_gfx_circle(&g, 100, 300, 50, 2, LT_RGB(255, 255, 255));
    lt_gfx_fill_arc(&g, 260, 300, 30, 50, 3.14159265 * 0.75, 3.14159265 * 2.25, LT_RGB(60, 200, 60));
    lt_gfx_fill_arc(&g, 260, 300, 30, 50, 3.14159265 * 2.25, 3.14159265 * 2.75, LT_RGB(220, 60, 60));
    lt_gfx_arc(&g, 260, 300, 60, 3.14159265 * 0.75, 3.14159265 * 2.25, 3, LT_RGB(200, 200, 200));
    LT_CHECK(P(&cv, 260, 300 - 40) == 0x3CC83CUL);   /* top of green sector */
    LT_CHECK(P(&cv, 260, 300 + 40) == 0xDC3C3CUL);   /* bottom: red range */
    /* needle */
    lt_gfx_line(&g, 260, 300, 260 + 45 * cos(-0.9), 300 + 45 * sin(-0.9), 3, LT_RGB(255, 255, 255));
    lt_gfx_fill_circle(&g, 260, 300, 5, LT_RGB(255, 255, 255));

    /* polygons */
    tri[0] = 400; tri[1] = 150; tri[2] = 470; tri[3] = 250; tri[4] = 380; tri[5] = 230;
    lt_gfx_fill_poly(&g, tri, 3, LT_RGB(240, 160, 40));
    LT_CHECK(P(&cv, 420, 200) == 0xF0A028UL);
    for (i = 0; i < 5; i++) {
        double a = -1.5708 + i * 2 * 2 * 3.14159265 / 5;  /* pentagram, even-odd */
        poly[2 * i] = 540 + 60 * cos(a); poly[2 * i + 1] = 200 + 60 * sin(a);
    }
    lt_gfx_fill_poly(&g, poly, 5, LT_RGB(180, 80, 220));
    LT_CHECK(P(&cv, 540, 200) == 0x141820UL);        /* hole in center (even-odd) */

    /* text */
    lt_gfx_text(&g, 10, 380, "Hello, LibreTune 123 \xC2\xB0 \xE2\x94\x80\xE2\x94\x82", LT_RGB(255, 255, 255), 1);
    lt_gfx_text(&g, 10, 400, "RPM 3500", LT_RGB(120, 255, 120), 2);
    lt_gfx_text(&g, 10, 440, "AFR 14.7", LT_RGB(255, 200, 100), 3);
    LT_CHECK(lt_gfx_text_width("abc", 2) == 48 && lt_gfx_text_height(3) == 48);
    {
        lt_rect pc = lt_gfx_push_clip(&g, lt_rect_make(300, 380, 40, 20));
        lt_gfx_fill_rect(&g, lt_rect_make(290, 370, 100, 100), LT_RGB(255, 0, 255));
        lt_gfx_pop_clip(&g, pc);
        LT_CHECK(P(&cv, 299, 385) == 0x141820UL && P(&cv, 300, 380) == 0xFF00FFUL &&
                 P(&cv, 340, 385) == 0x141820UL && P(&cv, 310, 400) == 0x141820UL);
    }
    /* vector text */
    lt_gfx_vtext(&g, 380, 330, "Vector Text 0123", 16, 0.0, 0, 1, 0, LT_RGB(255, 255, 255));
    lt_gfx_vtext(&g, 520, 440, "ROTATED 45\xC2\xB0", 18, -0.785, 1, 1, 2.0, LT_RGB(120, 220, 255));
    lt_gfx_vtext(&g, 625, 470, "90 deg", 16, 1.5708, 2, 1, 0, LT_RGB(255, 180, 120));
    lt_gfx_vtext(&g, 460, 360, "Gauge 6500", 10, 0.0, 0, 2, 0, LT_RGB(200, 200, 200));
    LT_CHECK(lt_gfx_vtext_width("ABC", 21) > 40.0);

    write_ppm(&cv, "build/test_gfx.ppm");
    /* offscreen / hostile coordinates must not crash */
    lt_gfx_fill_rect(&g, lt_rect_make(-50, -50, 100, 100), LT_RGB(1, 2, 3));
    lt_gfx_line(&g, -1e6, -1e6, 1e6, 1e6, 3, LT_RGB(1, 2, 3));
    lt_gfx_fill_circle(&g, -100, -100, 50, LT_RGB(1, 2, 3));
    LT_CHECK(g.dirty.w > 0);
    lt_canvas_free(&cv);
}

LT_TEST(test_huge_circle)
{
    lt_canvas cv;
    lt_gfx g;
    lt_canvas_init(&cv, 64, 64);
    lt_gfx_init_canvas(&g, &cv);
    lt_gfx_fill_circle(&g, 32, 32, 1e5, LT_RGB(5, 6, 7));
    LT_CHECK(P(&cv, 0, 0) == 0x050607UL && P(&cv, 63, 63) == 0x050607UL);
    lt_canvas_free(&cv);
}

LT_TEST(test_dirty_translate)
{
    lt_canvas cv;
    lt_gfx g;
    lt_canvas_init(&cv, 100, 100);
    lt_gfx_init_canvas(&g, &cv);
    lt_gfx_reset_dirty(&g);
    g.ox = 10; g.oy = 20;
    lt_gfx_fill_rect(&g, lt_rect_make(0, 0, 5, 5), LT_RGB(255, 0, 0));
    LT_CHECK(P(&cv, 10, 20) == 0xFF0000UL && P(&cv, 9, 20) == 0);
    LT_CHECK(g.dirty.x == 10 && g.dirty.y == 20 && g.dirty.w == 5 && g.dirty.h == 5);
    lt_canvas_free(&cv);
}

LT_TEST(test_speed)
{
    lt_canvas cv;
    lt_gfx g;
    clock_t t0;
    double ms;
    int i;
    lt_canvas_init(&cv, 1920, 1080);
    lt_gfx_init_canvas(&g, &cv);
    t0 = clock();
    for (i = 0; i < 10; i++) {
        int k;
        lt_gfx_clear(&g, LT_RGB(10, 10, 10));
        for (k = 0; k < 20; k++) {
            lt_gfx_fill_rect(&g, lt_rect_make(k * 90, 10 * k, 300, 200), LT_RGB(40, 60, 90));
        }
        for (k = 0; k < 40; k++) {
            lt_gfx_text(&g, 20, 20 + 20 * k, "The quick brown fox jumps over the lazy dog 0123456789 .....",
                        LT_RGB(255, 255, 255), 1);
        }
    }
    ms = (clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 10.0;
    printf("frame (clear+20 rects+40 text lines): %.2f ms\n", ms);
    LT_CHECK(ms < 50.0);
    lt_canvas_free(&cv);
}

static lt_cell *C(lt_cells *c, int x, int y) { return &c->cells[y * c->cols + x]; }

LT_TEST(test_cells)
{
    lt_cells cl;
    lt_gfx g;
    double tri[6];
    lt_cells_init(&cl, 40, 12);
    lt_gfx_init_cells(&g, &cl);
    LT_CHECK(lt_gfx_cell_mode(&g) && lt_gfx_width(&g) == 320 && lt_gfx_height(&g) == 192);
    lt_gfx_clear(&g, LT_RGB(0, 0, 40));
    LT_CHECK(C(&cl, 3, 3)->bg == LT_RGB(0, 0, 40) && C(&cl, 3, 3)->ch == ' ');
    lt_gfx_fill_rect(&g, lt_rect_make(16, 16, 32, 32), LT_RGB(200, 0, 0));
    LT_CHECK(C(&cl, 2, 1)->bg == LT_RGB(200, 0, 0) && C(&cl, 3, 2)->bg == LT_RGB(200, 0, 0));
    LT_CHECK(C(&cl, 6, 1)->bg == LT_RGB(0, 0, 40) && C(&cl, 2, 3)->bg == LT_RGB(0, 0, 40));
    lt_gfx_text(&g, 18, 18, "Hi!", LT_RGB(255, 255, 255), 2);
    LT_CHECK(C(&cl, 2, 1)->ch == 'H' && C(&cl, 3, 1)->ch == 'i' && C(&cl, 4, 1)->ch == '!');
    LT_CHECK(C(&cl, 2, 1)->bg == LT_RGB(200, 0, 0));      /* bg kept */
    lt_gfx_hline(&g, 0, 79, 100, LT_RGB(0, 255, 0));
    LT_CHECK(C(&cl, 5, 6)->ch == 0x2500 && C(&cl, 9, 6)->ch == 0x2500 && C(&cl, 11, 6)->ch == ' ');
    lt_gfx_vline(&g, 36, 80, 140, LT_RGB(0, 255, 0));
    LT_CHECK(C(&cl, 4, 5)->ch == 0x2502 && C(&cl, 4, 6)->ch == 0x253C);
    lt_gfx_fill_circle(&g, 200, 96, 40, LT_RGB(0, 90, 0));
    LT_CHECK(C(&cl, 25, 6)->bg == LT_RGB(0, 90, 0));
    lt_gfx_line(&g, 200, 20, 260, 20, 1, LT_RGB(255, 255, 0));
    LT_CHECK(C(&cl, 28, 1)->ch == '-');
    lt_gfx_line(&g, 280, 10, 280, 80, 1, LT_RGB(255, 255, 0));
    LT_CHECK(C(&cl, 35, 2)->ch == '|');
    tri[0] = 8; tri[1] = 100; tri[2] = 100; tri[3] = 100; tri[4] = 8; tri[5] = 180;
    lt_gfx_fill_poly(&g, tri, 3, LT_RGB(9, 9, 9));
    LT_CHECK(C(&cl, 2, 8)->bg == LT_RGB(9, 9, 9));
    lt_gfx_rect(&g, lt_rect_make(160, 112, 64, 48), LT_RGB(255, 255, 255));
    LT_CHECK(C(&cl, 20, 7)->ch == 0x250C && C(&cl, 27, 9)->ch == 0x2518);
    lt_gfx_vtext(&g, 160, 170, "ABC", 12, 0.0, 0, 1, 0, LT_RGB(255, 255, 255));
    LT_CHECK(C(&cl, 20, 10)->ch == 'A');
    {
        lt_rect p = lt_gfx_push_clip(&g, lt_rect_make(0, 0, 16, 16));
        lt_gfx_text(&g, 0, 0, "XYZ", LT_RGB(255, 255, 255), 1);
        lt_gfx_pop_clip(&g, p);
        LT_CHECK(C(&cl, 0, 0)->ch == 'X' && C(&cl, 1, 0)->ch == 'Y' && C(&cl, 2, 0)->ch != 'Z');
    }
    LT_CHECK(g.dirty.w > 0);
    lt_cells_free(&cl);
}

int main(void)
{
    LT_RUN(test_rect_utils);
    LT_RUN(test_utf8_font);
    LT_RUN(test_scene);
    LT_RUN(test_huge_circle);
    LT_RUN(test_dirty_translate);
    LT_RUN(test_speed);
    LT_RUN(test_cells);
    LT_TEST_MAIN_END
}
