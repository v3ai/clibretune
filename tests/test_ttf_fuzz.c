/* test_ttf_fuzz.c - malformed-font fuzzing of the TrueType loader.
 * Random truncation and mutation of the embedded fonts must never crash,
 * hang, read out of bounds (run under ASan/UBSan) or yield bitmaps larger than
 * the documented caps. Deterministic: seeded xorshift32. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lt_test.h"
#include "../src/gui/gui.h"
#include "../src/gui/fonts_data.h"

static lt_u32 rng_state = 0x1234567UL;

static lt_u32 rnd(void)
{
    lt_u32 x = rng_state;
    x ^= (x << 13) & LT_U32_MASK;
    x ^= x >> 17;
    x ^= (x << 5) & LT_U32_MASK;
    rng_state = x;
    return x;
}

static size_t rnd_below(size_t n)
{
    return n == 0 ? 0 : (size_t)(rnd() % (lt_u32)n);
}

static lt_u8 *font_bytes(int id, size_t *len)
{
    int n = 0, k;
    const lt_ttf_chunk *ch = lt_font_data(id, &n);
    size_t total = 0, pos = 0;
    lt_u8 *buf;
    for (k = 0; k < n; k++) { total += ch[k].len; }
    buf = (lt_u8 *)lt_malloc(total);
    for (k = 0; k < n; k++) {
        memcpy(buf + pos, ch[k].data, ch[k].len);
        pos += ch[k].len;
    }
    *len = total;
    return buf;
}

static lt_u32 be16(const lt_u8 *p) { return ((lt_u32)p[0] << 8) | (lt_u32)p[1]; }
static lt_u32 be32(const lt_u8 *p) { return (be16(p) << 16) | be16(p + 2); }

/* offset/length of table i from the (pristine) directory, 0 if absent */
static int table_range(const lt_u8 *d, size_t len, int i, size_t *off, size_t *tl)
{
    int nt = (int)be16(d + 4);
    if (i >= nt || 12 + (size_t)i * 16 + 16 > len) { return 0; }
    *off = (size_t)be32(d + 12 + (size_t)i * 16 + 8);
    *tl = (size_t)be32(d + 12 + (size_t)i * 16 + 12);
    return *off <= len && *tl <= len - *off && *tl > 0;
}

static void exercise(lt_ttf *f)
{
    int i, gids[6], k;
    lt_u32 cp;
    double sizes[3];
    sizes[0] = 11.0; sizes[1] = 17.5; sizes[2] = 60.0;
    for (cp = 0; cp < 0x200; cp++) { (void)lt_ttf_glyph_index(f, cp); }
    for (i = 0; i < 40; i++) {
        (void)lt_ttf_glyph_index(f, rnd());
        (void)lt_ttf_glyph_index(f, rnd() & 0xFFFFUL);
    }
    for (i = 0; i < 6; i++) { gids[i] = (int)(rnd() % 1200UL) - 20; }
    for (i = 0; i < 6; i++) {
        for (k = 0; k < 6; k++) {
            int a = lt_ttf_kern(f, gids[i], gids[k]), b = lt_ttf_kern(f, gids[i], gids[k]);
            LT_CHECK(a == b);
        }
        (void)lt_ttf_advance(f, gids[i]);
    }
    for (i = 0; i < 90; i++) {
        int gid = (int)(rnd() % (lt_u32)(lt_ttf_num_glyphs(f) + 4));
        const lt_ttf_bitmap *bm = lt_ttf_glyph(f, gid, sizes[i % 3], (int)(rnd() % 4UL));
        if (bm != NULL) {
            LT_CHECK(bm->w >= 1 && bm->h >= 1 && (long)bm->w * (long)bm->h <= 1048576L);
            LT_CHECK(bm->a != NULL);
        }
        if (i % 10 == 0) {
            double m[4];
            lt_ttf_bitmap rb;
            m[0] = 0.01; m[1] = 0.007; m[2] = 0.007; m[3] = -0.01;
            (void)lt_ttf_raster(f, gid, m, 0.25, 0.5, &rb);
        }
    }
    (void)lt_ttf_ascent(f); (void)lt_ttf_descent(f); (void)lt_ttf_x_height(f);
}

static void run_one(const lt_u8 *orig, size_t len, int mode, int *opened)
{
    lt_u8 *buf = (lt_u8 *)lt_malloc(len);
    size_t n = len, k, nmut;
    lt_ttf *f;
    memcpy(buf, orig, len);
    switch (mode) {
    case 0:   /* random byte flips anywhere */
        nmut = 1 + rnd_below(12);
        for (k = 0; k < nmut; k++) { buf[rnd_below(len)] = (lt_u8)rnd(); }
        break;
    case 1: { /* flips inside a random table */
        size_t off, tl;
        nmut = 1 + rnd_below(24);
        for (k = 0; k < nmut; k++) {
            if (table_range(orig, len, (int)rnd_below(9), &off, &tl)) {
                buf[off + rnd_below(tl)] = (lt_u8)rnd();
            }
        }
        break;
    }
    case 2:   /* 16-bit words to extreme values inside tables */
        nmut = 1 + rnd_below(16);
        for (k = 0; k < nmut; k++) {
            size_t off, tl;
            if (table_range(orig, len, (int)rnd_below(9), &off, &tl) && tl >= 2) {
                size_t p = off + (rnd_below(tl - 1) & ~(size_t)1);
                lt_u32 v = (rnd() & 1UL) ? 0xFFFFUL : ((rnd() & 1UL) ? 0UL : 0x8000UL);
                buf[p] = (lt_u8)(v >> 8);
                buf[p + 1] = (lt_u8)v;
            }
        }
        break;
    case 3:   /* truncation */
        n = rnd_below(len + 1);
        break;
    case 4:   /* corrupt the table directory */
        nmut = 1 + rnd_below(6);
        for (k = 0; k < nmut; k++) { buf[rnd_below(12 + 16 * 8)] = (lt_u8)rnd(); }
        break;
    default:  /* truncate and flip */
        n = len / 2 + rnd_below(len / 2 + 1);
        buf[rnd_below(n == 0 ? 1 : n)] = (lt_u8)rnd();
        break;
    }
    f = lt_ttf_open(buf, n, LT_TRUE);
    if (f != NULL) {
        (*opened)++;
        exercise(f);
        lt_ttf_close(f);
    }
    lt_free(buf);
}

LT_TEST(fuzz_mutations)
{
    int id, it, opened = 0, ran = 0;
    for (id = 0; id < LT_FONT_DATA_COUNT; id += 2) {      /* sans, sans semibold, mono */
        size_t len = 0;
        lt_u8 *orig = font_bytes(id, &len);
        for (it = 0; it < 500; it++) {
            run_one(orig, len, it % 6, &opened);
            ran++;
        }
        lt_free(orig);
    }
    LT_CHECK(ran == 1500);
    LT_CHECK(opened > 100);                 /* many mutants stay loadable: they were exercised */
}

LT_TEST(fuzz_random_bytes)
{
    int it, opened = 0;
    for (it = 0; it < 400; it++) {
        size_t n = 12 + rnd_below(2000), k;
        lt_u8 *buf = (lt_u8 *)lt_malloc(n);
        lt_ttf *f;
        for (k = 0; k < n; k++) { buf[k] = (lt_u8)rnd(); }
        if (it % 2 == 0) { buf[0] = 0; buf[1] = 1; buf[2] = 0; buf[3] = 0; }   /* plausible sfnt header */
        f = lt_ttf_open(buf, n, LT_TRUE);
        if (f != NULL) { opened++; exercise(f); lt_ttf_close(f); }
        lt_free(buf);
    }
    LT_CHECK(opened <= 400);
}

LT_TEST(fuzz_all_prefixes_of_small_font)
{
    /* every possible truncation point of the mono font header + directory */
    size_t len = 0, cut;
    lt_u8 *orig = font_bytes(LT_FONT_MONO, &len);
    int opened = 0;
    for (cut = 0; cut < 400; cut++) {
        lt_ttf *f = lt_ttf_open(orig, cut, LT_TRUE);
        if (f != NULL) { opened++; lt_ttf_close(f); }
    }
    LT_CHECK(opened == 0);
    lt_free(orig);
}

int main(void)
{
    LT_RUN(fuzz_mutations);
    LT_RUN(fuzz_random_bytes);
    LT_RUN(fuzz_all_prefixes_of_small_font);
    LT_TEST_MAIN_END
}
