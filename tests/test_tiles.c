/* test_tiles.c - retained rendering (gfx_rec.c): a recorded frame rasterized tile by tile must
 * be pixel-identical to drawing every call directly, unchanged tiles must not be re-rendered,
 * and the exact fast paths of the rasterizer (two-lane blending, one-pass shadow) must match
 * their per-channel / per-ring references. */
#include <stdio.h>
#include <string.h>
#include "lt_test.h"
#include "../src/gui/gui.h"

#define CW 320
#define CH 200

static int ref_div255(int x) { return ((x + 128) + ((x + 128) >> 8)) >> 8; }

/* first differing pixel index or -1 */
static long canvas_cmp(const lt_canvas *a, const lt_canvas *b)
{
    long i, n = (long)a->w * (long)a->h;
    if (a->w != b->w || a->h != b->h) { return 0; }
    for (i = 0; i < n; i++) {
        int x = (int)(i % a->w), y = (int)(i / a->w);
        if (a->px[y * a->pitch + x] != b->px[y * b->pitch + x]) { return i; }
    }
    return -1;
}

static void make_premul(lt_canvas *s, int seed)
{
    int x, y;
    for (y = 0; y < s->h; y++) {
        for (x = 0; x < s->w; x++) {
            lt_u32 a = (lt_u32)((x * 9 + y * 5 + seed * 31) & 0xFF);
            lt_u32 r = a * (lt_u32)((x * 7 + seed) & 0xFF) / 255U;
            lt_u32 gg = a * (lt_u32)((y * 11) & 0xFF) / 255U;
            lt_u32 b = a / 2U;
            s->px[y * s->pitch + x] = (a << 24) | (r << 16) | (gg << 8) | b;
        }
    }
}

/* a frame using every recorded primitive; `phase` moves one small element, `src` is blitted
 * premultiplied */
static void scene(lt_gfx *g, int phase, const lt_canvas *src)
{
    static const double tri[8] = { 150.5, 120.25, 210.0, 135.0, 190.75, 190.5, 140.0, 170.0 };
    static lt_u32 img[16 * 12];
    lt_u8 mask[24 * 10];
    lt_font *f = lt_font_get(LT_FONT_SANS);
    lt_rect prev;
    int i;
    for (i = 0; i < 16 * 12; i++) {
        img[i] = ((lt_u32)(i * 13 & 0xFF) << 24) | (((lt_u32)i * 2654435761UL) & 0xFFFFFFUL);
    }
    for (i = 0; i < 24 * 10; i++) { mask[i] = (lt_u8)(i * 37); }
    lt_gfx_clear(g, LT_RGB(20, 22, 26));
    lt_gfx_fill_rect(g, lt_rect_make(10, 10, 120, 60), LT_RGB(40, 60, 90));
    lt_gfx_fill_rect(g, lt_rect_make(30, 30, 140, 50), LT_RGBA(200, 80, 40, 120));
    lt_gfx_shadow(g, lt_rect_make(180, 20, 100, 50), 6, 9, 90);
    lt_gfx_fill_round_rect(g, lt_rect_make(180, 20, 100, 50), 8, LT_RGBA(230, 230, 240, 230));
    lt_gfx_fill_rect_vgrad(g, lt_rect_make(5, 90, 60, 80), LT_RGB(10, 200, 30), LT_RGBA(250, 0, 90, 140));
    lt_gfx_line(g, 3.5, 190.25, 300.75, 95.5, 2.5, LT_RGBA(255, 255, 0, 200));
    lt_gfx_fill_circle(g, 100.3, 140.7, 22.4, LT_RGBA(0, 160, 255, 180));
    lt_gfx_circle(g, 100.3, 140.7, 30.0, 1.5, LT_RGB(255, 255, 255));
    lt_gfx_arc(g, 260.0, 140.0, 40.0, 0.3, 2.9, 4.0, LT_RGB(255, 120, 0));
    lt_gfx_fill_arc(g, 260.0, 140.0, 20.0, 34.0, 3.5, 5.9, LT_RGBA(120, 255, 120, 160));
    lt_gfx_fill_poly(g, tri, 4, LT_RGBA(255, 0, 255, 100));
    lt_gfx_text(g, 12, 14, "Bitmap 123", LT_RGB(255, 255, 255), 1);
    lt_gfx_text_ex(g, f, 15.0, 70, 74, "Retained tiles", LT_RGB(250, 250, 250));
    lt_gfx_text_bl(g, f, 11.5, 190.25, 186, "baseline", LT_RGBA(255, 220, 160, 200));
    lt_gfx_text_rot(g, f, 12.0, 230.0, 100.0, "rotated", 0.7, 1, 1, LT_RGB(200, 255, 200));
    lt_gfx_vtext(g, 20.0, 180.0, "VEC", 14.0, -0.3, 0, 0, 1.2, LT_RGB(255, 200, 255));
    lt_gfx_icon(g, 5, 290.0, 4.0, 24.0, 0.0, LT_RGB(255, 255, 255));
    lt_gfx_blit(g, 140, 4, img, 16, 12, 16);
    lt_gfx_blit_mask(g, 150, 60, mask, 24, 10, 24, LT_RGBA(255, 255, 255, 210), NULL);
    if (src != NULL) { lt_gfx_blit_premul(g, 60, 110, src); }
    /* clipped and translated part */
    prev = lt_gfx_push_clip(g, lt_rect_make(200, 150, 70, 40));
    g->ox += 7; g->oy -= 3;
    lt_gfx_fill_round_rect(g, lt_rect_make(190, 145, 60, 40), 12, LT_RGBA(90, 90, 255, 200));
    lt_gfx_text(g, 200, 160, "clip", LT_RGB(0, 0, 0), 2);
    g->ox -= 7; g->oy += 3;
    lt_gfx_pop_clip(g, prev);
    /* the moving element */
    lt_gfx_fill_rect(g, lt_rect_make(20 + phase * 3, 150, 10, 10), LT_RGB(255, 40, 40));
}

static void render_immediate(lt_canvas *c, int phase, const lt_canvas *src)
{
    lt_gfx g;
    lt_gfx_init_canvas(&g, c);
    scene(&g, phase, src);
}

static int render_retained(lt_tiles *t, lt_gfx *g, int phase, const lt_canvas *src, lt_rect *out)
{
    int n;
    lt_tiles_begin(t, g);
    LT_CHECK(lt_gfx_recording(g));
    scene(g, phase, src);
    n = lt_tiles_end(t, g, out, 16);
    LT_CHECK(!lt_gfx_recording(g));
    return n;
}

LT_TEST(test_tiles_identical_and_incremental)
{
    lt_canvas ref, cur, src;
    lt_gfx g;
    lt_tiles *t = lt_tiles_new();
    lt_rect out[16];
    long ncmd;
    int ntiles, ndirty, n, i, vx, vy;
    double pixels;
    lt_canvas_init(&ref, CW, CH);
    lt_canvas_init(&cur, CW, CH);
    lt_canvas_init(&src, 40, 30);
    make_premul(&src, 1);
    lt_gfx_init_canvas(&g, &cur);

    /* first frame: everything rendered and reported, identical to direct drawing */
    n = render_retained(t, &g, 0, &src, out);
    render_immediate(&ref, 0, &src);
    LT_CHECK(canvas_cmp(&ref, &cur) < 0);
    lt_tiles_stats(t, &ncmd, &ntiles, &ndirty, &pixels);
    LT_CHECK(ncmd > 20 && ntiles == 5 * 7 && ndirty == ntiles);
    LT_CHECK(n >= 1 && g.dirty.w == CW && g.dirty.h == CH);
    LT_CHECK(lt_tiles_verify(t, &g, &vx, &vy) == 0);

    /* the same frame again: nothing to render or present */
    n = render_retained(t, &g, 0, &src, out);
    lt_tiles_stats(t, &ncmd, &ntiles, &ndirty, &pixels);
    LT_CHECK(n == 0 && ndirty == 0 && pixels == 0.0);
    LT_CHECK(g.dirty.w == 0 || g.dirty.h == 0);
    LT_CHECK(canvas_cmp(&ref, &cur) < 0);

    /* the small rect moves 3 px inside tile column 0, row 4 (y 128..159): only that tile */
    n = render_retained(t, &g, 1, &src, out);
    render_immediate(&ref, 1, &src);
    lt_tiles_stats(t, &ncmd, &ntiles, &ndirty, &pixels);
    LT_CHECK(ndirty == 1 && n == 1);
    LT_CHECK(out[0].x == 0 && out[0].y == 128 && out[0].w == 64 && out[0].h == 32);
    LT_CHECK(canvas_cmp(&ref, &cur) < 0);

    /* across a tile border: the old and the new tile */
    n = render_retained(t, &g, 13, &src, out);
    render_immediate(&ref, 13, &src);
    lt_tiles_stats(t, &ncmd, &ntiles, &ndirty, &pixels);
    LT_CHECK(ndirty == 2 && n >= 1);
    LT_CHECK(canvas_cmp(&ref, &cur) < 0);
    for (i = 0; i < n; i++) { LT_CHECK(out[i].y == 128 && out[i].h == 32); }

    /* the premultiplied source changes: only the tiles under the blit (keyed on its generation) */
    make_premul(&src, 2);
    lt_canvas_touch(&src);
    n = render_retained(t, &g, 13, &src, out);
    render_immediate(&ref, 13, &src);
    lt_tiles_stats(t, &ncmd, &ntiles, &ndirty, &pixels);
    LT_CHECK(ndirty >= 2 && ndirty <= 4 && n >= 1);
    for (i = 0; i < n; i++) { LT_CHECK(out[i].x < 100 && out[i].x + out[i].w > 60); }
    LT_CHECK(canvas_cmp(&ref, &cur) < 0);

    /* invalidate: everything again */
    lt_tiles_invalidate(t);
    n = render_retained(t, &g, 13, &src, out);
    lt_tiles_stats(t, &ncmd, &ntiles, &ndirty, &pixels);
    LT_CHECK(ndirty == ntiles && n >= 1);
    LT_CHECK(canvas_cmp(&ref, &cur) < 0);

    /* resize: a new grid, everything rendered */
    lt_canvas_resize(&cur, CW + 40, CH - 30);
    lt_canvas_resize(&ref, CW + 40, CH - 30);
    lt_gfx_init_canvas(&g, &cur);
    n = render_retained(t, &g, 5, &src, out);
    render_immediate(&ref, 5, &src);
    lt_tiles_stats(t, &ncmd, &ntiles, &ndirty, &pixels);
    LT_CHECK(ntiles == 6 * 6 && ndirty == ntiles && n >= 1);
    LT_CHECK(canvas_cmp(&ref, &cur) < 0);
    LT_CHECK(lt_tiles_verify(t, &g, &vx, &vy) == 0);

    lt_tiles_free(t);
    lt_canvas_free(&src);
    lt_canvas_free(&cur);
    lt_canvas_free(&ref);
}

/* calls appended on top of an unchanged frame (a dialog opening) are drawn over the pixels
 * already there; removing them again re-renders the tiles from scratch */
LT_TEST(test_tiles_appended_overlay)
{
    lt_canvas ref, cur;
    lt_gfx g, gr;
    lt_tiles *t = lt_tiles_new();
    lt_rect out[16];
    int k, ndirty, ntiles, vx, vy;
    lt_canvas_init(&ref, CW, CH);
    lt_canvas_init(&cur, CW, CH);
    lt_gfx_init_canvas(&g, &cur);
    lt_gfx_init_canvas(&gr, &ref);
    for (k = 0; k < 6; k++) {
        int overlay = k == 1 || k == 2 || k == 4, moved = k == 4 ? 2 : 0;
        lt_tiles_begin(t, &g);
        scene(&g, moved, NULL);
        if (overlay) {
            lt_gfx_fill_rect(&g, lt_rect_make(0, 0, CW, CH), LT_RGBA(0, 0, 0, 100));
            lt_gfx_shadow(&g, lt_rect_make(90 + k, 50, 140, 90), 8, 12, 120);
            lt_gfx_fill_round_rect(&g, lt_rect_make(90 + k, 50, 140, 90), 8, LT_RGB(240, 240, 240));
        }
        (void)lt_tiles_end(t, &g, out, 16);
        scene(&gr, moved, NULL);
        if (overlay) {
            lt_gfx_fill_rect(&gr, lt_rect_make(0, 0, CW, CH), LT_RGBA(0, 0, 0, 100));
            lt_gfx_shadow(&gr, lt_rect_make(90 + k, 50, 140, 90), 8, 12, 120);
            lt_gfx_fill_round_rect(&gr, lt_rect_make(90 + k, 50, 140, 90), 8, LT_RGB(240, 240, 240));
        }
        LT_CHECK(canvas_cmp(&ref, &cur) < 0);
        LT_CHECK(lt_tiles_verify(t, &g, &vx, &vy) == 0);
        lt_tiles_stats(t, NULL, &ntiles, &ndirty, NULL);
        if (k == 1) { LT_CHECK(ndirty == ntiles); }
    }
    lt_tiles_free(t);
    lt_canvas_free(&cur);
    lt_canvas_free(&ref);
}

/* a source drawn into or freed after it was recorded still shows what it held at record time,
 * like a direct blit would */
LT_TEST(test_tiles_source_copy_on_change)
{
    lt_canvas ref, cur, src, old;
    lt_gfx g, gs;
    lt_tiles *t = lt_tiles_new();
    lt_rect out[16];
    int k;
    lt_canvas_init(&ref, CW, CH);
    lt_canvas_init(&cur, CW, CH);
    lt_canvas_init(&old, 40, 30);
    lt_gfx_init_canvas(&g, &cur);
    for (k = 0; k < 2; k++) {
        lt_canvas_init(&src, 40, 30);
        make_premul(&src, 3 + k);
        memcpy(old.px, src.px, sizeof(lt_u32) * 40U * 30U);
        lt_tiles_begin(t, &g);
        scene(&g, 0, &src);
        if (k == 0) {
            lt_gfx_init_canvas(&gs, &src);          /* drawing into the source after the blit */
            lt_gfx_fill_rect(&gs, lt_rect_make(0, 0, 40, 30), LT_RGB(255, 0, 0));
            lt_canvas_free(&src);
        } else {
            lt_canvas_free(&src);                   /* freed after the blit */
        }
        (void)lt_tiles_end(t, &g, out, 16);
        render_immediate(&ref, 0, &old);
        LT_CHECK(canvas_cmp(&ref, &cur) < 0);
    }
    lt_tiles_free(t);
    lt_canvas_free(&old);
    lt_canvas_free(&cur);
    lt_canvas_free(&ref);
}

/* opaque covers (fill, gradient, round rect, clipped and translated) over changing content:
 * the tiles they hide skip the calls below them and the result still equals direct drawing */
static void cover_scene(lt_gfx *g, int k)
{
    lt_rect prev;
    scene(g, k % 5, NULL);
    lt_gfx_fill_rect(g, lt_rect_make(0, 0, CW, CH), LT_RGBA(k * 30, 50, 80, 128));
    lt_gfx_fill_round_rect(g, lt_rect_make(k * 2, 0, CW - k * 2, CH), 60 + k, LT_RGB(60, 60, 70));
    lt_gfx_fill_rect(g, lt_rect_make(64, 32, 128, 64), LT_RGB(10, 20, 30));
    lt_gfx_fill_rect_vgrad(g, lt_rect_make(0, 100 + k, 140, 70), LT_RGB(200, 0, 0), LT_RGB(0, 0, 200));
    lt_gfx_fill_round_rect(g, lt_rect_make(130 + k, 10, 180, 180), 70 + k, LT_RGB(240, 240, 220));
    lt_gfx_fill_rect(g, lt_rect_make(170, 60 + k, 40, 20), LT_RGBA(0, 0, 0, 90));
    prev = lt_gfx_push_clip(g, lt_rect_make(5, 5, 200 - k * 7, 90));
    g->ox += 3; g->oy += k;
    lt_gfx_fill_round_rect(g, lt_rect_make(-20, 0, 300, 70), 12, LT_RGB(30, 90, 30));
    lt_gfx_text(g, 10, 10, "covered", LT_RGB(255, 255, 255), 2);
    g->ox -= 3; g->oy -= k;
    lt_gfx_pop_clip(g, prev);
    if (k & 1) { lt_gfx_fill_rect(g, lt_rect_make(0, 0, CW, CH), LT_RGBA(0, 0, 0, 120)); }
}

LT_TEST(test_tiles_occlusion)
{
    lt_canvas ref, cur;
    lt_gfx g, gr;
    lt_tiles *t = lt_tiles_new();
    lt_rect out[16];
    int k, vx, vy;
    lt_canvas_init(&ref, CW, CH);
    lt_canvas_init(&cur, CW, CH);
    lt_gfx_init_canvas(&g, &cur);
    lt_gfx_init_canvas(&gr, &ref);
    for (k = 0; k < 8; k++) {
        lt_tiles_begin(t, &g);
        cover_scene(&g, k);
        (void)lt_tiles_end(t, &g, out, 16);
        cover_scene(&gr, k);
        LT_CHECK(canvas_cmp(&ref, &cur) < 0);
        LT_CHECK(lt_tiles_verify(t, &g, &vx, &vy) == 0);
    }
    lt_tiles_free(t);
    lt_canvas_free(&cur);
    lt_canvas_free(&ref);
}

/* icon masks come from a cache with eviction: a mask from the cache draws the same pixels as a
 * freshly rasterised one (more icons than the cache holds, drawn twice more after a reference
 * pass that rasterises every icon from scratch) */
LT_TEST(test_icon_cache)
{
    lt_canvas a, b;
    lt_gfx ga, gb;
    int pass, i;
    lt_canvas_init(&a, CW, CH);
    lt_canvas_init(&b, CW, CH);
    lt_gfx_init_canvas(&ga, &a);
    lt_gfx_init_canvas(&gb, &b);
    for (pass = 0; pass < 3; pass++) {
        lt_gfx *g = pass == 0 ? &ga : &gb;
        lt_gfx_clear(g, LT_RGB(30, 30, 40));
        for (i = 0; i < 700; i++) {
            double x = (double)(i % 20) * 15.0 + (double)(i % 16) / 16.0;
            double y = (double)(i / 20 % 12) * 15.0 + (double)(i % 3) / 4.0;
            if (pass == 0) { lt_icon_cache_release(); }
            lt_gfx_icon(g, 1 + i % 37, x, y, 12.0 + (double)(i % 2), 0.0, LT_RGB(255, 255 - i % 200, 200));
        }
        if (pass > 0) { LT_CHECK(canvas_cmp(&a, &b) < 0); }
    }
    lt_canvas_free(&a);
    lt_canvas_free(&b);
}

/* a cancelled (discarded) frame leaves the canvas and the hashes alone: the next frame is
 * compared with the last one that was shown */
LT_TEST(test_tiles_cancel)
{
    lt_canvas ref, cur;
    lt_gfx g;
    lt_tiles *t = lt_tiles_new();
    lt_rect out[16];
    int n, ndirty, vx, vy;
    lt_canvas_init(&ref, CW, CH);
    lt_canvas_init(&cur, CW, CH);
    lt_gfx_init_canvas(&g, &cur);
    (void)render_retained(t, &g, 0, NULL, out);
    render_immediate(&ref, 0, NULL);
    /* a discarded frame that drew something else */
    lt_tiles_begin(t, &g);
    scene(&g, 9, NULL);
    lt_gfx_fill_rect(&g, lt_rect_make(0, 0, CW, CH), LT_RGB(255, 0, 0));
    lt_tiles_cancel(t, &g);
    LT_CHECK(!lt_gfx_recording(&g));
    LT_CHECK(canvas_cmp(&ref, &cur) < 0);
    LT_CHECK(lt_tiles_verify(t, &g, &vx, &vy) == 0);
    /* the same frame as the last shown one: nothing to do */
    n = render_retained(t, &g, 0, NULL, out);
    lt_tiles_stats(t, NULL, NULL, &ndirty, NULL);
    LT_CHECK(n == 0 && ndirty == 0);
    LT_CHECK(canvas_cmp(&ref, &cur) < 0);
    lt_tiles_free(t);
    lt_canvas_free(&cur);
    lt_canvas_free(&ref);
}

/* reference: lt_ui_draw_shadow's ring loop */
static void shadow_ref(lt_gfx *g, lt_rect r, int radius, int n, int alpha)
{
    int i;
    for (i = n; i >= 1; i--) {
        int a = alpha * (n - i + 1) * (n - i + 1) / (n * n * 3 / 2 + 1);
        lt_rect q = lt_rect_make(r.x - i, r.y - i + n / 2, r.w + 2 * i, r.h + 2 * i);
        if (a < 1) { a = 1; }
        lt_gfx_fill_round_rect(g, q, radius + i, LT_RGBA(0, 0, 0, a));
    }
}

static void pattern(lt_canvas *c)
{
    int x, y;
    for (y = 0; y < c->h; y++) {
        for (x = 0; x < c->w; x++) {
            c->px[y * c->pitch + x] = 0xFF000000UL | ((lt_u32)((x * 3) & 0xFF) << 16) |
                                      ((lt_u32)((y * 5) & 0xFF) << 8) | (lt_u32)((x + y) & 0xFF);
        }
    }
}

LT_TEST(test_shadow_matches_rings)
{
    static const int cases[][7] = {     /* x, y, w, h, radius, spread, alpha */
        { 30, 30, 100, 60, 8, 18, 120 }, { 40, 20, 80, 90, 0, 4, 40 }, { 50, 50, 3, 2, 2, 7, 255 },
        { 10, 70, 140, 30, 20, 36, 160 }, { 60, 40, 20, 20, 10, 1, 90 }, { -20, -10, 90, 60, 6, 12, 200 },
        { 120, 80, 70, 70, 4, 25, 60 }, { 70, 50, 1, 1, 0, 9, 255 }, { 30, 30, 0, 40, 5, 6, 100 }
    };
    lt_canvas a, b;
    lt_gfx ga, gb;
    int k, clip;
    lt_canvas_init(&a, 180, 140);
    lt_canvas_init(&b, 180, 140);
    for (clip = 0; clip < 2; clip++) {
        for (k = 0; k < (int)LT_COUNTOF(cases); k++) {
            lt_rect r = lt_rect_make(cases[k][0], cases[k][1], cases[k][2], cases[k][3]);
            pattern(&a); pattern(&b);
            lt_gfx_init_canvas(&ga, &a); lt_gfx_init_canvas(&gb, &b);
            if (clip) {
                lt_gfx_set_clip(&ga, lt_rect_make(37, 25, 61, 70));
                lt_gfx_set_clip(&gb, lt_rect_make(37, 25, 61, 70));
                ga.ox = gb.ox = 5; ga.oy = gb.oy = -4;
            }
            shadow_ref(&ga, r, cases[k][4], cases[k][5], cases[k][6]);
            lt_gfx_shadow(&gb, r, cases[k][4], cases[k][5], cases[k][6]);
            LT_CHECK(canvas_cmp(&a, &b) < 0);
        }
    }
    lt_canvas_free(&a);
    lt_canvas_free(&b);
}

/* two-lane blending == per-channel DIV255 blending, for every alpha and channel value */
LT_TEST(test_blend_exact)
{
    lt_canvas c;
    lt_gfx g;
    lt_u8 cov[256];
    int a, i, bad = 0;
    lt_canvas_init(&c, 256, 1);
    lt_gfx_init_canvas(&g, &c);
    for (i = 0; i < 256; i++) { cov[i] = (lt_u8)i; }
    for (a = 0; a < 256; a++) {
        lt_color col = LT_RGBA(17, 250, 128, a);
        for (i = 0; i < 256; i++) { c.px[i] = LT_RGB(i, 255 - i, (i * 7) & 0xFF); }
        lt_gfx_fill_rect(&g, lt_rect_make(0, 0, 256, 1), col);
        for (i = 0; i < 256 && a > 0; i++) {
            int ia = 255 - a;
            lt_u32 want = a == 255 ? LT_RGB(17, 250, 128) :
                LT_RGB(ref_div255(17 * a + i * ia), ref_div255(250 * a + (255 - i) * ia),
                       ref_div255(128 * a + ((i * 7) & 0xFF) * ia));
            if ((c.px[i] & 0xFFFFFFUL) != (want & 0xFFFFFFUL)) { bad++; }
        }
        /* over runs of one colour (1 to 40 pixels long): the same result */
        for (i = 0; i < 256; i++) {
            int q = i / (1 + (i >> 4) % 12) + (i > 200 ? 3 : 0);
            c.px[i] = LT_RGB((q * 13) & 0xFF, 255 - q, (q * 7) & 0xFF);
        }
        lt_gfx_fill_rect(&g, lt_rect_make(0, 0, 256, 1), col);
        for (i = 0; i < 256 && a > 0; i++) {
            int ia = 255 - a, q = i / (1 + (i >> 4) % 12) + (i > 200 ? 3 : 0);
            lt_u32 want = a == 255 ? LT_RGB(17, 250, 128) :
                LT_RGB(ref_div255(17 * a + ((q * 13) & 0xFF) * ia), ref_div255(250 * a + (255 - q) * ia),
                       ref_div255(128 * a + ((q * 7) & 0xFF) * ia));
            if ((c.px[i] & 0xFFFFFFUL) != (want & 0xFFFFFFUL)) { bad++; }
        }
        /* coverage mask: alpha = a * coverage */
        for (i = 0; i < 256; i++) { c.px[i] = LT_RGB(i, 255 - i, (i * 7) & 0xFF); }
        lt_gfx_blit_mask(&g, 0, 0, cov, 256, 1, 256, col, NULL);
        for (i = 0; i < 256; i++) {
            int al = a >= 255 ? i : ref_div255(a * i), ia = 255 - al;
            lt_u32 want = al >= 255 ? LT_RGB(17, 250, 128) : al <= 0 ? LT_RGB(i, 255 - i, (i * 7) & 0xFF) :
                LT_RGB(ref_div255(17 * al + i * ia), ref_div255(250 * al + (255 - i) * ia),
                       ref_div255(128 * al + ((i * 7) & 0xFF) * ia));
            if ((c.px[i] & 0xFFFFFFUL) != (want & 0xFFFFFFUL)) { bad++; }
        }
    }
    LT_CHECK(bad == 0);
    {
        /* premultiplied source over a destination, saturating per channel */
        lt_canvas s;
        lt_canvas_init(&s, 256, 1);
        for (a = 1; a < 255; a += 3) {
            for (i = 0; i < 256; i++) {
                s.px[i] = ((lt_u32)a << 24) | ((lt_u32)i << 16) | ((lt_u32)(255 - i) << 8) | (lt_u32)(a / 2);
                c.px[i] = LT_RGB((i * 3) & 0xFF, i, 255 - i);
            }
            lt_gfx_blit_premul(&g, 0, 0, &s);
            for (i = 0; i < 256; i++) {
                int ia = 255 - a, r = i + ref_div255(((i * 3) & 0xFF) * ia);
                int gg = (255 - i) + ref_div255(i * ia), b = a / 2 + ref_div255((255 - i) * ia);
                lt_u32 want = LT_RGB(r > 255 ? 255 : r, gg > 255 ? 255 : gg, b > 255 ? 255 : b);
                if ((c.px[i] & 0xFFFFFFUL) != (want & 0xFFFFFFUL)) { bad++; }
            }
        }
        /* runs of opaque, empty and translucent pixels (gauge layers), clipped at both ends */
        for (a = 0; a < 4; a++) {
            static const int runs[] = { 255, 255, 0, 0, 0, 90, 255, 0, 255, 255, 255, 1, 254, 0 };
            lt_rect prev;
            for (i = 0; i < 256; i++) {
                lt_u32 al = (lt_u32)runs[(i / (a + 1)) % (int)LT_COUNTOF(runs)];
                s.px[i] = (al << 24) | ((al * (lt_u32)i / 255U) << 16) | ((al / 3U) << 8) | (al / 2U);
                c.px[i] = LT_RGB((i * 5) & 0xFF, 255 - i, i);
            }
            prev = lt_gfx_push_clip(&g, lt_rect_make(a, 0, 256 - 2 * a, 1));
            lt_gfx_blit_premul(&g, 0, 0, &s);
            lt_gfx_pop_clip(&g, prev);
            for (i = 0; i < 256; i++) {
                int al = (int)((s.px[i] >> 24) & 0xFF), ia = 255 - al;
                int sr = (int)((s.px[i] >> 16) & 0xFF), sg = (int)((s.px[i] >> 8) & 0xFF);
                int sb = (int)(s.px[i] & 0xFF);
                int r = sr + ref_div255(((i * 5) & 0xFF) * ia), gg = sg + ref_div255((255 - i) * ia);
                int b = sb + ref_div255(i * ia);
                lt_u32 want = LT_RGB(r > 255 ? 255 : r, gg > 255 ? 255 : gg, b > 255 ? 255 : b);
                if (i < a || i >= 256 - a) { want = LT_RGB((i * 5) & 0xFF, 255 - i, i); }
                if ((c.px[i] & 0xFFFFFFUL) != (want & 0xFFFFFFUL)) { bad++; }
            }
        }
        lt_canvas_free(&s);
    }
    LT_CHECK(bad == 0);
    lt_canvas_free(&c);
}

int main(void)
{
    LT_RUN(test_tiles_identical_and_incremental);
    LT_RUN(test_tiles_appended_overlay);
    LT_RUN(test_tiles_source_copy_on_change);
    LT_RUN(test_tiles_cancel);
    LT_RUN(test_tiles_occlusion);
    LT_RUN(test_icon_cache);
    LT_RUN(test_shadow_matches_rings);
    LT_RUN(test_blend_exact);
    lt_font_release_all();
    LT_TEST_MAIN_END
}
