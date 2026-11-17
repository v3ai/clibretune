/* test_ttf.c - TrueType loader, rasterizer, text/icon drawing.
 * Also renders a typography showcase to build/typography.ppm (convert with
 * tools/ppm2png.py to look at it). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lt_test.h"
#include "../src/gui/gui.h"
#include "../src/gui/fonts_data.h"

static lt_ttf *open_font(int id)
{
    int n = 0;
    const lt_ttf_chunk *ch = lt_font_data(id, &n);
    return lt_ttf_open_chunks(ch, n);
}

static double coverage_sum(const lt_ttf_bitmap *b)
{
    double s = 0.0;
    int i;
    for (i = 0; i < b->w * b->h; i++) { s += (double)b->a[i]; }
    return s / 255.0;
}

/* centroid x of coverage (pixels, relative to bitmap origin incl. xoff) */
static double centroid_x(const lt_ttf_bitmap *b)
{
    double sx = 0.0, s = 0.0;
    int x, y;
    for (y = 0; y < b->h; y++) {
        for (x = 0; x < b->w; x++) {
            double v = (double)b->a[y * b->w + x];
            sx += v * ((double)(x + b->xoff) + 0.5);
            s += v;
        }
    }
    return s > 0.0 ? sx / s : 0.0;
}

LT_TEST(test_chunks_and_open)
{
    int id, n;
    for (id = 0; id < LT_FONT_DATA_COUNT; id++) {
        const lt_ttf_chunk *ch = lt_font_data(id, &n);
        int i;
        lt_ttf *f;
        LT_CHECK(ch != NULL && n >= 1);
        for (i = 0; i < n; i++) { LT_CHECK(ch[i].len > 0 && ch[i].len <= 32000); }
        f = open_font(id);
        LT_CHECK(f != NULL);
        if (f == NULL) { continue; }
        LT_CHECK(lt_ttf_units_per_em(f) == 1000);
        LT_CHECK(lt_ttf_num_glyphs(f) > 400);
        LT_CHECK(lt_ttf_ascent(f) > 800 && lt_ttf_ascent(f) < 1200);
        LT_CHECK(lt_ttf_descent(f) < -150 && lt_ttf_descent(f) > -400);
        LT_CHECK(lt_ttf_x_height(f) > 450 && lt_ttf_x_height(f) < 560);
        LT_CHECK(lt_ttf_cap_height(f) > 650 && lt_ttf_cap_height(f) < 750);
        lt_ttf_close(f);
    }
    LT_CHECK(lt_font_data(-1, &n) == NULL && n == 0);
    LT_CHECK(lt_font_data(LT_FONT_DATA_COUNT, &n) == NULL);
}

LT_TEST(test_cmap)
{
    lt_ttf *f = open_font(LT_FONT_SANS);
    lt_u32 cp;
    int distinct = 0, last = -1;
    if (f == NULL) { LT_CHECK(0); return; }
    for (cp = 0x21; cp < 0x7F; cp++) {
        int g = lt_ttf_glyph_index(f, cp);
        LT_CHECK(g > 0);
        if (g != last) { distinct++; }
        last = g;
    }
    LT_CHECK(distinct == 0x7F - 0x21);
    LT_CHECK(lt_ttf_glyph_index(f, 0xE9) > 0);      /* e acute */
    LT_CHECK(lt_ttf_glyph_index(f, 0xB0) > 0);      /* degree */
    LT_CHECK(lt_ttf_glyph_index(f, 0x3BB) > 0);     /* lambda */
    LT_CHECK(lt_ttf_glyph_index(f, 0x20AC) > 0);    /* euro */
    LT_CHECK(lt_ttf_glyph_index(f, 0x1F600) == 0);  /* beyond the BMP subset */
    LT_CHECK(lt_ttf_glyph_index(f, 0x4E2D) == 0);   /* CJK not embedded */
    LT_CHECK(lt_ttf_glyph_index(f, 0xFFFF) == 0);
    LT_CHECK(lt_ttf_glyph_index(f, 0x10FFFF) == 0);
    LT_CHECK(lt_ttf_glyph_index(f, 'A') != lt_ttf_glyph_index(f, 'B'));
    lt_ttf_close(f);
}

LT_TEST(test_metrics_mono)
{
    lt_ttf *f = open_font(LT_FONT_MONO);
    lt_u32 cp;
    if (f == NULL) { LT_CHECK(0); return; }
    for (cp = 0x20; cp < 0x7F; cp++) {
        LT_CHECK(lt_ttf_advance(f, lt_ttf_glyph_index(f, cp)) == 600);
    }
    /* a monospace font has no kerning */
    LT_CHECK(lt_ttf_kern(f, lt_ttf_glyph_index(f, 'A'), lt_ttf_glyph_index(f, 'V')) == 0);
    lt_ttf_close(f);
}

LT_TEST(test_advance_and_kerning)
{
    lt_ttf *f = open_font(LT_FONT_SANS);
    int gA, gV, gT, go, gi, gl, nneg = 0, a, b;
    if (f == NULL) { LT_CHECK(0); return; }
    gA = lt_ttf_glyph_index(f, 'A'); gV = lt_ttf_glyph_index(f, 'V');
    gT = lt_ttf_glyph_index(f, 'T'); go = lt_ttf_glyph_index(f, 'o');
    gi = lt_ttf_glyph_index(f, 'i'); gl = lt_ttf_glyph_index(f, 'l');
    LT_CHECK(lt_ttf_advance(f, gi) < lt_ttf_advance(f, gA));
    LT_CHECK(lt_ttf_advance(f, gl) == lt_ttf_advance(f, gi) || lt_ttf_advance(f, gl) < 400);
    LT_CHECK(lt_ttf_advance(f, gA) > 500 && lt_ttf_advance(f, gA) < 800);
    LT_CHECK(lt_ttf_kern(f, gA, gV) < -20);
    LT_CHECK(lt_ttf_kern(f, gT, go) < -20);
    LT_CHECK(lt_ttf_kern(f, 0, gV) == 0 && lt_ttf_kern(f, gA, 0) == 0);
    LT_CHECK(lt_ttf_kern(f, -1, 99999) == 0);
    LT_CHECK(lt_ttf_kern(f, gi, gl) == 0);
    for (a = 'A'; a <= 'Z'; a++) {
        for (b = 'a'; b <= 'z'; b++) {
            if (lt_ttf_kern(f, lt_ttf_glyph_index(f, (lt_u32)a), lt_ttf_glyph_index(f, (lt_u32)b)) < 0) {
                nneg++;
            }
        }
    }
    LT_CHECK(nneg > 40);
    /* out of range glyph ids are tolerated */
    LT_CHECK(lt_ttf_advance(f, -5) == lt_ttf_advance(f, 0));
    LT_CHECK(lt_ttf_advance(f, 100000) >= 0);
    lt_ttf_close(f);
}

LT_TEST(test_composite_glyphs)
{
    lt_ttf *f = open_font(LT_FONT_SANS);
    int ge, gE, gacc, nc = 0;
    lt_u32 cp;
    double m[4];
    lt_ttf_bitmap be, bacc;
    int h_e, h_acc, yoff_e;
    double sum_e;
    if (f == NULL) { LT_CHECK(0); return; }
    for (cp = 0xC0; cp < 0x100; cp++) {
        if (lt_ttf_glyph_composite(f, lt_ttf_glyph_index(f, cp))) { nc++; }
    }
    LT_CHECK(nc > 30);                       /* Latin-1 accents are composites */
    LT_CHECK(!lt_ttf_glyph_composite(f, lt_ttf_glyph_index(f, 'e')));
    ge = lt_ttf_glyph_index(f, 'e');
    gE = lt_ttf_glyph_index(f, 0xE9);
    gacc = lt_ttf_glyph_index(f, 0xC9);
    LT_CHECK(lt_ttf_glyph_composite(f, gE) && lt_ttf_glyph_composite(f, gacc));
    m[0] = 0.1; m[1] = 0.0; m[2] = 0.0; m[3] = -0.1;   /* 100 px em */
    LT_CHECK(lt_ttf_raster(f, ge, m, 0.0, 0.0, &be));
    /* the bitmap lives in a scratch buffer: copy what we need before the next raster */
    h_e = be.h; yoff_e = be.yoff; sum_e = coverage_sum(&be);
    LT_CHECK(lt_ttf_raster(f, gE, m, 0.0, 0.0, &bacc));
    h_acc = bacc.h;
    LT_CHECK(h_acc > h_e + 10);              /* accent sits above the e */
    LT_CHECK(bacc.yoff < yoff_e - 10);
    LT_CHECK(coverage_sum(&bacc) > sum_e * 1.05);
    lt_ttf_close(f);
}

LT_TEST(test_raster_scaling_and_aa)
{
    lt_ttf *f = open_font(LT_FONT_SANS);
    int g, i, partial = 0;
    double m50[4], m100[4], s50, s100;
    lt_ttf_bitmap b;
    if (f == NULL) { LT_CHECK(0); return; }
    g = lt_ttf_glyph_index(f, 'H');
    m50[0] = 0.05; m50[1] = 0.0; m50[2] = 0.0; m50[3] = -0.05;
    m100[0] = 0.1; m100[1] = 0.0; m100[2] = 0.0; m100[3] = -0.1;
    LT_CHECK(lt_ttf_raster(f, g, m50, 0.0, 0.0, &b));
    s50 = coverage_sum(&b);
    LT_CHECK(b.yoff < 0 && b.yoff + b.h <= 1);    /* sits on the baseline */
    LT_CHECK(b.h >= 34 && b.h <= 36);             /* cap height 0.698 * 50 */
    LT_CHECK(lt_ttf_raster(f, g, m100, 0.0, 0.0, &b));
    s100 = coverage_sum(&b);
    LT_CHECK(b.h >= 69 && b.h <= 71);
    LT_CHECK_NEAR(s100 / s50, 4.0, 0.08);         /* area scales quadratically */
    for (i = 0; i < b.w * b.h; i++) {
        if (b.a[i] > 0 && b.a[i] < 255) { partial++; }
    }
    LT_CHECK(partial > 100);                      /* edges are anti-aliased */
    lt_ttf_close(f);
}

LT_TEST(test_subpixel_and_rotation)
{
    lt_ttf *f = open_font(LT_FONT_SANS);
    int g;
    double m[4], s0, s2, c0, c2, mr[4], sr;
    lt_ttf_bitmap b, br;
    int w0, h0;
    if (f == NULL) { LT_CHECK(0); return; }
    g = lt_ttf_glyph_index(f, 'B');
    m[0] = 0.02; m[1] = 0.0; m[2] = 0.0; m[3] = -0.02;     /* 20 px */
    LT_CHECK(lt_ttf_raster(f, g, m, 0.0, 0.0, &b));
    s0 = coverage_sum(&b); c0 = centroid_x(&b);
    w0 = b.w; h0 = b.h;
    LT_CHECK(lt_ttf_raster(f, g, m, 0.5, 0.0, &b));
    s2 = coverage_sum(&b); c2 = centroid_x(&b);
    LT_CHECK_NEAR(s0, s2, s0 * 0.01);
    LT_CHECK_NEAR(c2 - c0, 0.5, 0.05);
    /* rotate by 90 degrees clockwise on screen: X = -v*s... (u->down, v->right) */
    mr[0] = 0.0; mr[1] = 0.02; mr[2] = 0.02; mr[3] = 0.0;
    LT_CHECK(lt_ttf_raster(f, g, mr, 0.0, 0.0, &br));
    sr = coverage_sum(&br);
    LT_CHECK_NEAR(sr, s0, s0 * 0.01);
    LT_CHECK(br.w >= h0 - 1 && br.w <= h0 + 1);
    LT_CHECK(br.h >= w0 - 1 && br.h <= w0 + 1);
    lt_ttf_close(f);
}

LT_TEST(test_glyph_cache)
{
    lt_ttf *f = open_font(LT_FONT_SANS);
    int g, i, entries;
    size_t bytes;
    long hits, misses;
    const lt_ttf_bitmap *b1, *b2;
    if (f == NULL) { LT_CHECK(0); return; }
    g = lt_ttf_glyph_index(f, 'g');
    b1 = lt_ttf_glyph(f, g, 12.0, 0);
    LT_CHECK(b1 != NULL && b1->w > 4 && b1->h > 8);
    b2 = lt_ttf_glyph(f, g, 12.0, 0);
    LT_CHECK(b2 != NULL && b2->w == b1->w && b2->h == b1->h);
    lt_ttf_cache_stats(f, &entries, &bytes, &hits, &misses);
    LT_CHECK(hits == 1 && misses == 1 && entries == 1);
    LT_CHECK(lt_ttf_glyph(f, lt_ttf_glyph_index(f, ' '), 12.0, 0) == NULL);
    LT_CHECK(lt_ttf_glyph(f, g, 0.5, 0) == NULL);        /* too small */
    LT_CHECK(lt_ttf_glyph(f, g, 5000.0, 0) == NULL);     /* too large */
    /* different sub-pixel positions are different cache entries */
    LT_CHECK(lt_ttf_glyph(f, g, 12.0, 1) != NULL);
    lt_ttf_cache_stats(f, &entries, &bytes, &hits, &misses);
    LT_CHECK(entries == 3);
    /* bounded memory: thrash with many sizes under a tiny limit */
    lt_ttf_cache_clear(f);
    lt_ttf_cache_limit(f, 20000);
    for (i = 0; i < 3000; i++) {
        int gl = 3 + i % 300;
        lt_ttf_glyph(f, gl, 10.0 + (double)(i % 23), i % LT_TTF_SUBPOS);
        lt_ttf_cache_stats(f, &entries, &bytes, NULL, NULL);
        if (bytes > 20000) { LT_CHECK(0); break; }
        if (entries > 768) { LT_CHECK(0); break; }
    }
    LT_CHECK(bytes <= 20000);
    lt_ttf_close(f);
}

LT_TEST(test_open_rejects_garbage)
{
    static const lt_u8 junk[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    lt_u8 hdr[12];
    LT_CHECK(lt_ttf_open(NULL, 0, LT_FALSE) == NULL);
    LT_CHECK(lt_ttf_open(junk, sizeof(junk), LT_FALSE) == NULL);
    LT_CHECK(lt_ttf_open(junk, 3, LT_TRUE) == NULL);
    memset(hdr, 0, sizeof(hdr));
    hdr[1] = 1;                                   /* sfnt version 1.0, zero tables */
    LT_CHECK(lt_ttf_open(hdr, sizeof(hdr), LT_FALSE) == NULL);
    LT_CHECK(lt_ttf_open_chunks(NULL, 0) == NULL);
}

LT_TEST(test_truncated_font)
{
    int n, k;
    const lt_ttf_chunk *ch = lt_font_data(LT_FONT_SANS, &n);
    size_t total = 0, pos = 0;
    lt_u8 *buf;
    for (k = 0; k < n; k++) { total += ch[k].len; }
    buf = (lt_u8 *)lt_malloc(total);
    for (k = 0; k < n; k++) {
        memcpy(buf + pos, ch[k].data, ch[k].len);
        pos += ch[k].len;
    }
    /* every prefix is either rejected or safe to use */
    for (k = 0; k < 400; k++) {
        size_t cut = (total * (size_t)k) / 400;
        lt_ttf *f = lt_ttf_open(buf, cut, LT_TRUE);
        if (f != NULL) {
            int g;
            for (g = 0; g < 200; g++) { lt_ttf_glyph(f, g, 14.0, 0); }
            lt_ttf_close(f);
        }
    }
    {
        lt_ttf *f = lt_ttf_open(buf, total, LT_FALSE);
        LT_CHECK(f != NULL);
        lt_ttf_close(f);
    }
    lt_free(buf);
}


/* ---- synthetic font: cmap format 12, kern format 0, composite transform ---- */
typedef struct sbuf { lt_u8 d[2048]; size_t n; } sbuf;

static void sput16(sbuf *b, long v)
{
    lt_u32 u = (lt_u32)(v & 0xFFFFL);
    b->d[b->n++] = (lt_u8)(u >> 8);
    b->d[b->n++] = (lt_u8)(u & 0xFFU);
}

static void sput32(sbuf *b, lt_u32 v)
{
    sput16(b, (long)((v >> 16) & 0xFFFFUL));
    sput16(b, (long)(v & 0xFFFFUL));
}

static void spad4(sbuf *b)
{
    while (b->n % 4 != 0) { b->d[b->n++] = 0; }
}

typedef struct stab { const char *tag; sbuf data; } stab;

static void build_font(sbuf *out)
{
    stab t[8];
    size_t i, off;
    int k;
    for (i = 0; i < 8; i++) { t[i].data.n = 0; }
    t[0].tag = "cmap";
    t[1].tag = "glyf";
    t[2].tag = "head";
    t[3].tag = "hhea";
    t[4].tag = "hmtx";
    t[5].tag = "kern";
    t[6].tag = "loca";
    t[7].tag = "maxp";
    /* cmap: one (3,10) format 12 subtable, 2 groups */
    sput16(&t[0].data, 0); sput16(&t[0].data, 1);
    sput16(&t[0].data, 3); sput16(&t[0].data, 10); sput32(&t[0].data, 12);
    sput16(&t[0].data, 12); sput16(&t[0].data, 0); sput32(&t[0].data, 16 + 24);
    sput32(&t[0].data, 0); sput32(&t[0].data, 2);
    sput32(&t[0].data, 0x41UL); sput32(&t[0].data, 0x42UL); sput32(&t[0].data, 2);
    sput32(&t[0].data, 0x10000UL); sput32(&t[0].data, 0x10000UL); sput32(&t[0].data, 1);
    /* glyf: gid1 square 500x500, gid2 composite (square, scale .5, dx 600), gid3 all-off */
    sput16(&t[1].data, 1); sput16(&t[1].data, 0); sput16(&t[1].data, 0);
    sput16(&t[1].data, 500); sput16(&t[1].data, 500);
    sput16(&t[1].data, 3); sput16(&t[1].data, 0);
    for (k = 0; k < 4; k++) { t[1].data.d[t[1].data.n++] = 1; }
    sput16(&t[1].data, 0); sput16(&t[1].data, 500); sput16(&t[1].data, 0); sput16(&t[1].data, -500);
    sput16(&t[1].data, 0); sput16(&t[1].data, 0); sput16(&t[1].data, 500); sput16(&t[1].data, 0);
    spad4(&t[1].data);
    sput16(&t[1].data, -1); sput16(&t[1].data, 600); sput16(&t[1].data, 0);
    sput16(&t[1].data, 850); sput16(&t[1].data, 250);
    sput16(&t[1].data, 0x000B); sput16(&t[1].data, 1); sput16(&t[1].data, 600); sput16(&t[1].data, 0);
    sput16(&t[1].data, 0x2000);
    spad4(&t[1].data);
    sput16(&t[1].data, 1); sput16(&t[1].data, 0); sput16(&t[1].data, 0);
    sput16(&t[1].data, 1000); sput16(&t[1].data, 1000);
    sput16(&t[1].data, 3); sput16(&t[1].data, 0);
    for (k = 0; k < 4; k++) { t[1].data.d[t[1].data.n++] = 0; }
    sput16(&t[1].data, 0); sput16(&t[1].data, 1000); sput16(&t[1].data, 0); sput16(&t[1].data, -1000);
    sput16(&t[1].data, 0); sput16(&t[1].data, 0); sput16(&t[1].data, 1000); sput16(&t[1].data, 0);
    spad4(&t[1].data);
    /* head */
    for (i = 0; i < 54; i++) { t[2].data.d[t[2].data.n++] = 0; }
    t[2].data.d[18] = (lt_u8)(1000 >> 8); t[2].data.d[19] = (lt_u8)(1000 & 255);
    t[2].data.d[51] = 1;                                   /* long loca */
    /* hhea */
    for (i = 0; i < 36; i++) { t[3].data.d[t[3].data.n++] = 0; }
    t[3].data.d[4] = (lt_u8)(800 >> 8); t[3].data.d[5] = (lt_u8)(800 & 255);
    t[3].data.d[6] = 0xFF; t[3].data.d[7] = 0x38;          /* descent -200 */
    t[3].data.d[35] = 4;
    /* hmtx */
    sput16(&t[4].data, 500); sput16(&t[4].data, 0);
    sput16(&t[4].data, 700); sput16(&t[4].data, 0);
    sput16(&t[4].data, 1000); sput16(&t[4].data, 0);
    sput16(&t[4].data, 1200); sput16(&t[4].data, 0);
    /* kern: version 0, 1 subtable format 0 with pairs (2,3) -> -50 and (3,2) -> +20 */
    sput16(&t[5].data, 0); sput16(&t[5].data, 1);
    sput16(&t[5].data, 0); sput16(&t[5].data, 14 + 12); sput16(&t[5].data, 1);
    sput16(&t[5].data, 2); sput16(&t[5].data, 0); sput16(&t[5].data, 0); sput16(&t[5].data, 0);
    sput16(&t[5].data, 2); sput16(&t[5].data, 3); sput16(&t[5].data, -50);
    sput16(&t[5].data, 3); sput16(&t[5].data, 2); sput16(&t[5].data, 20);
    /* loca (long): glyph sizes 0, 36, 20, 36 */
    sput32(&t[6].data, 0); sput32(&t[6].data, 0); sput32(&t[6].data, 36);
    sput32(&t[6].data, 56); sput32(&t[6].data, 92);
    /* maxp */
    sput32(&t[7].data, 0x00010000UL); sput16(&t[7].data, 4);
    /* directory */
    out->n = 0;
    sput32(out, 0x00010000UL); sput16(out, 8); sput16(out, 128); sput16(out, 3); sput16(out, 0);
    off = 12 + 16 * 8;
    for (i = 0; i < 8; i++) {
        size_t len = t[i].data.n;
        int j;
        for (j = 0; j < 4; j++) { out->d[out->n++] = (lt_u8)t[i].tag[j]; }
        sput32(out, 0); sput32(out, (lt_u32)off); sput32(out, (lt_u32)len);
        off += (len + 3) / 4 * 4;
    }
    for (i = 0; i < 8; i++) {
        memcpy(out->d + out->n, t[i].data.d, t[i].data.n);
        out->n += t[i].data.n;
        spad4(out);
    }
}

LT_TEST(test_synthetic_font)
{
    sbuf *fb = (sbuf *)lt_malloc(sizeof(sbuf));
    lt_ttf *f;
    lt_ttf_bitmap b;
    double m[4], sum;
    int x0, y0, x1, y1;
    build_font(fb);
    f = lt_ttf_open(fb->d, fb->n, LT_TRUE);
    LT_CHECK(f != NULL);
    if (f == NULL) { lt_free(fb); return; }
    /* cmap format 12, including a code point outside the BMP */
    LT_CHECK(lt_ttf_glyph_index(f, 0x41) == 2 && lt_ttf_glyph_index(f, 0x42) == 3);
    LT_CHECK(lt_ttf_glyph_index(f, 0x10000UL) == 1);
    LT_CHECK(lt_ttf_glyph_index(f, 0x43) == 0 && lt_ttf_glyph_index(f, 0x40) == 0);
    LT_CHECK(lt_ttf_glyph_index(f, 0x10001UL) == 0 && lt_ttf_glyph_index(f, 0x1FFFFUL) == 0);
    /* hmtx: numberOfHMetrics = 4 */
    LT_CHECK(lt_ttf_advance(f, 1) == 700 && lt_ttf_advance(f, 3) == 1200);
    /* legacy kern table, format 0 */
    LT_CHECK(lt_ttf_kern(f, 2, 3) == -50 && lt_ttf_kern(f, 3, 2) == 20);
    LT_CHECK(lt_ttf_kern(f, 1, 2) == 0 && lt_ttf_kern(f, 2, 2) == 0);
    LT_CHECK(lt_ttf_glyph_box(f, 1, &x0, &y0, &x1, &y1) && x1 == 500 && y1 == 500);
    LT_CHECK(!lt_ttf_glyph_box(f, 0, &x0, &y0, &x1, &y1));
    LT_CHECK(lt_ttf_glyph_composite(f, 2) && !lt_ttf_glyph_composite(f, 1));
    /* exact area: 500x500 square at 0.01 px/unit = 25 px^2, any sub-pixel offset */
    m[0] = 0.01; m[1] = 0.0; m[2] = 0.0; m[3] = -0.01;
    LT_CHECK(lt_ttf_raster(f, 1, m, 0.0, 0.0, &b));
    LT_CHECK_NEAR(coverage_sum(&b), 25.0, 0.02);
    LT_CHECK(b.w == 5 && b.h == 5 && b.xoff == 0 && b.yoff == -5);
    LT_CHECK(lt_ttf_raster(f, 1, m, 0.37, 0.61, &b));
    LT_CHECK_NEAR(coverage_sum(&b), 25.0, 0.02);
    LT_CHECK(lt_ttf_raster(f, 1, m, 0.5, 0.5, &b));
    LT_CHECK(b.w == 6 && b.h == 6 && b.a != NULL);
    if (b.a != NULL) { LT_CHECK(b.a[0] == 64 || b.a[0] == 63); }   /* corner pixel: 0.25 coverage */
    /* composite: unit square scaled by 0.5 and moved 600 right: 250x250 at x 600..850 */
    LT_CHECK(lt_ttf_raster(f, 2, m, 0.0, 0.0, &b));
    LT_CHECK_NEAR(coverage_sum(&b), 6.25, 0.02);
    LT_CHECK(b.xoff == 6 && b.w == 3 && b.h == 3);
    /* all-off-curve contour: quadratic splines through edge midpoints.
     * area = 500000 + 4 * 2/3 * 125000 = 833333 units^2 -> 8333.3 px^2 at 0.1 */
    m[0] = 0.1; m[3] = -0.1;
    LT_CHECK(lt_ttf_raster(f, 3, m, 0.0, 0.0, &b));
    sum = coverage_sum(&b);
    LT_CHECK_NEAR(sum, 8333.3, 8333.3 * 0.002);
    /* the whole thing also works through the cached path and the text layout */
    LT_CHECK(lt_ttf_glyph(f, 1, 50.0, 0) != NULL);
    LT_CHECK(lt_ttf_glyph(f, 0, 50.0, 0) == NULL);       /* empty .notdef */
    lt_ttf_close(f);
    lt_free(fb);
}

/* ---- gfx integration ------------------------------------------------ */
static int count_changed(const lt_canvas *c, lt_u32 bg)
{
    int i, n = 0;
    for (i = 0; i < c->w * c->h; i++) {
        if ((c->px[i] & 0xFFFFFFUL) != (bg & 0xFFFFFFUL)) { n++; }
    }
    return n;
}

LT_TEST(test_gfx_text)
{
    lt_canvas cv;
    lt_gfx g;
    lt_font *sans = lt_font_get(LT_FONT_SANS), *mono = lt_font_get(LT_FONT_MONO);
    int adv, i;
    double w1, w2;
    LT_CHECK(sans != NULL && mono != NULL);
    LT_CHECK(lt_font_get(LT_FONT_COUNT) == NULL);
    lt_canvas_init(&cv, 200, 60);
    lt_gfx_init_canvas(&g, &cv);
    lt_gfx_clear(&g, LT_RGB(0, 0, 0));
    LT_CHECK(lt_font_ascent(sans, 12.0) + lt_font_descent(sans, 12.0) == lt_font_line_height(sans, 12.0));
    LT_CHECK(lt_font_line_height(sans, 12.0) >= 14 && lt_font_line_height(sans, 12.0) <= 17);
    LT_CHECK_NEAR(lt_font_cap_height(sans, 100.0), 69.8, 0.5);
    adv = lt_gfx_text_ex(&g, sans, 12.0, 10, 10, "Hello", LT_RGB(255, 255, 255));
    w1 = lt_gfx_text_measure(sans, 12.0, "Hello");
    LT_CHECK(adv > 20 && adv < 40);
    LT_CHECK_NEAR((double)adv, w1, 0.51);
    LT_CHECK(count_changed(&cv, 0xFF000000UL) > 80);
    /* nothing drawn above the line box top or left of x */
    for (i = 0; i < cv.w; i++) { LT_CHECK((cv.px[8 * cv.w + i] & 0xFFFFFFUL) == 0); }
    /* measuring is additive up to kerning */
    w2 = lt_gfx_text_measure(sans, 12.0, "He") + lt_gfx_text_measure(sans, 12.0, "llo");
    LT_CHECK_NEAR(w1, w2, 1.0);
    LT_CHECK(lt_gfx_text_measure(sans, 12.0, "") == 0.0);
    LT_CHECK(lt_gfx_text_measure(sans, 24.0, "Hello") > 1.99 * w1 * 0.99);
    /* mono: every character is 0.6 em wide */
    LT_CHECK_NEAR(lt_gfx_text_measure(mono, 20.0, "0123456789"), 120.0, 0.01);
    /* kerning is applied: AV narrower than A + V */
    LT_CHECK(lt_gfx_text_measure(sans, 20.0, "AV") <
             lt_gfx_text_measure(sans, 20.0, "A") + lt_gfx_text_measure(sans, 20.0, "V") - 0.5);
    /* fit */
    LT_CHECK(lt_gfx_text_fit(sans, 12.0, "Hello world", 1000.0) == 11);
    LT_CHECK(lt_gfx_text_fit(sans, 12.0, "Hello world", w1) >= 4);
    LT_CHECK(lt_gfx_text_fit(sans, 12.0, "Hello world", 1.0) == 0);
    LT_CHECK(lt_gfx_text_fit(sans, 12.0, "\xC2\xB0" "CC", 14.0) <= 3);
    /* clipping */
    lt_gfx_clear(&g, LT_RGB(0, 0, 0));
    lt_gfx_set_clip(&g, lt_rect_make(0, 0, 20, 60));
    lt_gfx_reset_dirty(&g);
    lt_gfx_text_ex(&g, sans, 12.0, 5, 5, "WWWWWWWWWWWWWW", LT_RGB(255, 255, 255));
    for (i = 0; i < cv.h; i++) { LT_CHECK((cv.px[i * cv.w + 25] & 0xFFFFFFUL) == 0); }
    LT_CHECK(g.dirty.x + g.dirty.w <= 20);
    /* NULL text, NULL font fall back without crashing */
    LT_CHECK(lt_gfx_text_ex(&g, sans, 12.0, 0, 0, NULL, 0) == 0);
    lt_gfx_set_clip(&g, lt_rect_make(0, 0, cv.w, cv.h));
    LT_CHECK(lt_gfx_text_ex(&g, NULL, 12.0, 0, 0, "ab", LT_RGB(255, 255, 255)) == 16);
    lt_canvas_free(&cv);
}

LT_TEST(test_gfx_legacy_mapping)
{
    lt_canvas cv;
    lt_gfx g;
    int n1, n2;
    lt_canvas_init(&cv, 160, 40);
    lt_gfx_init_canvas(&g, &cv);
    lt_gfx_clear(&g, LT_RGB(0, 0, 0));
    lt_gfx_reset_dirty(&g);
    LT_CHECK(lt_gfx_text(&g, 4, 4, "Test 123", LT_RGB(255, 255, 255), 1) == 64);
    n1 = count_changed(&cv, 0xFF000000UL);
    LT_CHECK(n1 > 100);
    /* the text stays inside its 8x16 cells */
    LT_CHECK(g.dirty.x >= 4 - 1 && g.dirty.x + g.dirty.w <= 4 + 64 + 1);
    LT_CHECK(g.dirty.y >= 4 && g.dirty.y + g.dirty.h <= 4 + 16);
    /* bitmap mode draws the old font */
    lt_gfx_clear(&g, LT_RGB(0, 0, 0));
    g.bitmap_text = 1;
    LT_CHECK(lt_gfx_text(&g, 4, 4, "Test 123", LT_RGB(255, 255, 255), 1) == 64);
    n2 = count_changed(&cv, 0xFF000000UL);
    LT_CHECK(n2 > 100 && n2 != n1);
    /* box drawing characters stay on the bitmap font (cell-filling) */
    g.bitmap_text = 0;
    lt_gfx_clear(&g, LT_RGB(0, 0, 0));
    lt_gfx_text(&g, 0, 0, "\xE2\x96\x88", LT_RGB(255, 255, 255), 1);
    LT_CHECK((cv.px[0] & 0xFFFFFFUL) == 0xFFFFFFUL && (cv.px[15 * cv.w + 7] & 0xFFFFFFUL) == 0xFFFFFFUL);
    /* vtext width and drawing go through Plex */
    LT_CHECK(lt_gfx_vtext_width("Hello", 10.0) > 20.0);
    lt_gfx_clear(&g, LT_RGB(0, 0, 0));
    lt_gfx_vtext(&g, 80.0, 20.0, "88.8", 12.0, 0.0, 1, 1, 1.0, LT_RGB(255, 255, 255));
    LT_CHECK(count_changed(&cv, 0xFF000000UL) > 50);
    lt_canvas_free(&cv);
}

LT_TEST(test_gfx_text_rot)
{
    lt_canvas cv;
    lt_gfx g;
    lt_font *sans = lt_font_get(LT_FONT_SANS);
    int n0, n90, i, x0, x1, y0, y1;
    lt_canvas_init(&cv, 120, 120);
    lt_gfx_init_canvas(&g, &cv);
    lt_gfx_clear(&g, LT_RGB(0, 0, 0));
    lt_gfx_text_rot(&g, sans, 16.0, 60.0, 60.0, "Rotate", 0.0, 1, 1, LT_RGB(255, 255, 255));
    n0 = count_changed(&cv, 0xFF000000UL);
    LT_CHECK(n0 > 100);
    x0 = g.dirty.x; x1 = g.dirty.x + g.dirty.w;
    LT_CHECK(x0 < 60 && x1 > 60);                      /* centered on the anchor */
    LT_CHECK_NEAR((double)(x0 + x1) / 2.0, 60.0, 3.0);
    lt_gfx_clear(&g, LT_RGB(0, 0, 0));
    lt_gfx_reset_dirty(&g);
    lt_gfx_text_rot(&g, sans, 16.0, 60.0, 60.0, "Rotate", LT_PI / 2.0, 1, 1, LT_RGB(255, 255, 255));
    n90 = count_changed(&cv, 0xFF000000UL);
    LT_CHECK_NEAR((double)n90, (double)n0, (double)n0 * 0.12);
    y0 = g.dirty.y; y1 = g.dirty.y + g.dirty.h;
    LT_CHECK(g.dirty.h > g.dirty.w * 2);               /* now vertical */
    LT_CHECK_NEAR((double)(y0 + y1) / 2.0, 60.0, 3.0);
    /* arbitrary angle is drawn, and alpha-translucent colors do not saturate */
    lt_gfx_clear(&g, LT_RGB(0, 0, 0));
    lt_gfx_text_rot(&g, sans, 14.0, 60.0, 60.0, "Gauge", 0.6, 0, 2, LT_RGBA(255, 255, 255, 128));
    LT_CHECK(count_changed(&cv, 0xFF000000UL) > 60);
    for (i = 0; i < cv.w * cv.h; i++) { LT_CHECK((cv.px[i] & 0xFF) <= 130); }
    lt_canvas_free(&cv);
}

LT_TEST(test_icons)
{
    lt_canvas cv;
    lt_gfx g;
    int id, i, total = 0, blank = 0;
    lt_canvas_init(&cv, 64, 64);
    lt_gfx_init_canvas(&g, &cv);
    LT_CHECK(lt_icon_count() >= 190);
    LT_CHECK(lt_icon_find("folder") == LT_ICON_FOLDER && LT_ICON_FOLDER != 0);
    LT_CHECK(lt_icon_find("folder-open") == LT_ICON_FOLDER_OPEN);
    LT_CHECK(lt_icon_find("FolderOpen") == LT_ICON_FOLDER_OPEN);
    LT_CHECK(lt_icon_find("FOLDER_OPEN") == LT_ICON_FOLDER_OPEN);
    LT_CHECK(lt_icon_find("AlertTriangle") == LT_ICON_TRIANGLE_ALERT);
    LT_CHECK(lt_icon_find("Wand2") == LT_ICON_WAND_SPARKLES);
    LT_CHECK(lt_icon_find("Loader2") == LT_ICON_LOADER_CIRCLE);
    LT_CHECK(lt_icon_find("nonsense-icon") == LT_ICON_NONE);
    LT_CHECK(lt_icon_find("") == LT_ICON_NONE && lt_icon_find(NULL) == LT_ICON_NONE);
    LT_CHECK_STR(lt_icon_name(LT_ICON_GAUGE), "gauge");
    LT_CHECK(lt_icon_name(0) == NULL && lt_icon_name(LT_ICON_COUNT) == NULL);
    /* every icon draws something inside its box, nothing outside */
    for (id = 1; id < LT_ICON_COUNT; id++) {
        int n;
        lt_gfx_clear(&g, LT_RGB(0, 0, 0));
        lt_gfx_reset_dirty(&g);
        lt_gfx_icon(&g, id, 20.0, 20.0, 24.0, 2.0, LT_RGB(255, 255, 255));
        n = count_changed(&cv, 0xFF000000UL);
        total += n;
        if (n < 8) { blank++; }
        LT_CHECK(g.dirty.x >= 16 && g.dirty.y >= 16 && g.dirty.x + g.dirty.w <= 48 &&
                 g.dirty.y + g.dirty.h <= 48);
    }
    LT_CHECK(blank == 0);
    LT_CHECK(total > 10000);
    /* a filled-ish stroke: the check mark has no pixel outside its strokes */
    lt_gfx_clear(&g, LT_RGB(0, 0, 0));
    lt_gfx_icon(&g, LT_ICON_X, 8.0, 8.0, 48.0, 0.0, LT_RGB(255, 0, 0));
    LT_CHECK((cv.px[32 * cv.w + 32] & 0xFF0000UL) == 0xFF0000UL);   /* crossing */
    LT_CHECK((cv.px[14 * cv.w + 32] & 0xFFFFFFUL) == 0);            /* above it */
    /* tiny and huge sizes are safe */
    lt_gfx_icon(&g, LT_ICON_X, 0.0, 0.0, 0.2, 1.0, 0);
    lt_gfx_icon(&g, LT_ICON_X, 0.0, 0.0, 9999.0, 1.0, 0);
    lt_gfx_icon(&g, 99999, 0.0, 0.0, 16.0, 1.0, 0);
    lt_gfx_icon(&g, -1, 0.0, 0.0, 16.0, 1.0, 0);
    /* stroke width matters */
    lt_gfx_clear(&g, LT_RGB(0, 0, 0));
    lt_gfx_icon(&g, LT_ICON_MINUS, 8.0, 8.0, 48.0, 2.0, LT_RGB(255, 255, 255));
    i = count_changed(&cv, 0xFF000000UL);
    lt_gfx_clear(&g, LT_RGB(0, 0, 0));
    lt_gfx_icon(&g, LT_ICON_MINUS, 8.0, 8.0, 48.0, 8.0, LT_RGB(255, 255, 255));
    LT_CHECK(count_changed(&cv, 0xFF000000UL) > 2 * i);
    lt_canvas_free(&cv);
}

/* ---- showcase ------------------------------------------------------- */
static void write_ppm(lt_canvas *c, const char *path)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (f == NULL) { return; }
    fprintf(f, "P6\n%d %d\n255\n", c->w, c->h);
    for (y = 0; y < c->h; y++) {
        for (x = 0; x < c->w; x++) {
            lt_u32 v = c->px[y * c->pitch + x];
            fputc((int)((v >> 16) & 255), f);
            fputc((int)((v >> 8) & 255), f);
            fputc((int)(v & 255), f);
        }
    }
    fclose(f);
}

static void gauge(lt_gfx *g, double cx, double cy, double r, lt_color fg, lt_color dim,
                  lt_color accent, lt_font *sans, lt_font *mono, double value)
{
    double a0 = LT_PI * 0.75, a1 = LT_PI * 2.25, a;
    int i;
    char buf[32];
    lt_gfx_arc(g, cx, cy, r, a0, a1, 4.0, dim);
    lt_gfx_arc(g, cx, cy, r, a0, a0 + (a1 - a0) * value, 4.0, accent);
    for (i = 0; i <= 10; i++) {
        double ca, sa, r0 = r - 9.0, r1 = r - 4.0, tr = r - 20.0;
        a = a0 + (a1 - a0) * (double)i / 10.0;
        ca = cos(a); sa = sin(a);
        lt_gfx_line(g, cx + ca * r0, cy + sa * r0, cx + ca * r1, cy + sa * r1, 1.0, fg);
        if (i % 2 == 0) {
            lt_snprintf(buf, sizeof(buf), "%d", i * 10);
            /* rotated labels read along the dial */
            lt_gfx_text_rot(g, sans, 10.0, cx + ca * tr, cy + sa * tr, buf, a + LT_PI / 2.0, 1, 1, fg);
        }
    }
    lt_snprintf(buf, sizeof(buf), "%.1f", value * 100.0);
    {
        double w = lt_gfx_text_measure(mono, 22.0, buf);
        lt_gfx_text_bl(g, mono, 22.0, cx - w / 2.0, (int)(cy + 8.0), buf, fg);
    }
    lt_gfx_text_rot(g, sans, 11.0, cx, cy + 26.0, "AFR", 0.0, 1, 1, dim);
}

static void panel(lt_gfx *g, int x0, int w, lt_color bg, lt_color fg, lt_color dim, lt_color accent)
{
    lt_font *sans = lt_font_get(LT_FONT_SANS), *med = lt_font_get(LT_FONT_SANS_MEDIUM);
    lt_font *semi = lt_font_get(LT_FONT_SANS_SEMIBOLD), *mono = lt_font_get(LT_FONT_MONO);
    lt_font *monom = lt_font_get(LT_FONT_MONO_MEDIUM);
    static const int sizes[] = { 10, 11, 12, 13, 14, 16 };
    int i, y = 8, k;
    char buf[96];
    lt_gfx_fill_rect(g, lt_rect_make(x0, 0, w, g->canvas->h), bg);
    lt_gfx_text_ex(g, semi, 16.0, x0 + 12, y, "Fuel Table - VE (%)", fg);
    y += lt_font_line_height(semi, 16.0) + 6;
    for (i = 0; i < 6; i++) {
        lt_snprintf(buf, sizeof(buf), "%dpx  Ignition advance, AFR target 14.7:1 (WAV)", sizes[i]);
        lt_gfx_text_ex(g, sans, (double)sizes[i], x0 + 12, y, buf, fg);
        y += lt_font_line_height(sans, (double)sizes[i]) + 3;
    }
    y += 4;
    lt_gfx_text_ex(g, med, 12.0, x0 + 12, y, "Medium: Engine speed (RPM) / Manifold pressure", fg);
    y += 18;
    lt_gfx_text_ex(g, semi, 12.0, x0 + 12, y, "SemiBold: Tuning, Settings, Datalog", fg);
    y += 20;
    for (k = 0; k < 3; k++) {
        int j, x = x0 + 12;
        for (j = 0; j < 6; j++) {
            lt_snprintf(buf, sizeof(buf), "%.1f", 38.5 + (double)(j * 7 + k * 13) * 0.9);
            lt_gfx_text_ex(g, k == 1 ? monom : mono, 12.0, x, y, buf, j == 2 && k == 1 ? accent : fg);
            x += 46;
        }
        y += 16;
    }
    y += 6;
    lt_gfx_text_ex(g, mono, 11.0, x0 + 12, y, "0x1F4A  14.70  -2.5%  1234 rpm", dim);
    y += 20;
    /* legacy 8x16 calls remapped to Plex Mono, then the original bitmap font */
    lt_gfx_text(g, x0 + 12, y, "Legacy bitmap call -> Plex Mono", fg, 1);
    y += 18;
    g->bitmap_text = 1;
    lt_gfx_text(g, x0 + 12, y, "Legacy bitmap call (old font)", fg, 1);
    g->bitmap_text = 0;
    y += 20;
    gauge(g, (double)(x0 + 100), (double)(y + 92), 72.0, fg, dim, accent, sans, mono, 0.62);
    gauge(g, (double)(x0 + 270), (double)(y + 78), 56.0, fg, dim, accent, sans, mono, 0.35);
}

LT_TEST(test_showcase)
{
    lt_canvas cv;
    lt_gfx g;
    int id, x, y, n = 0;
    static const char *const names[] = {
        "folder-open", "save", "undo-2", "redo-2", "search", "settings", "gauge", "table-2",
        "line-chart", "plug", "unplug", "flame", "play", "pause", "circle-dot", "lock",
        "lock-open", "refresh-cw", "box", "grid-3x3", "wand-2", "cpu", "zap", "alert-triangle",
        "info", "check", "x", "chevron-down", "chevron-right", "file-text", "thermometer",
        "fuel", "wind", "droplets", "activity", "usb", "wifi", "trash-2", "copy", "download"
    };
    lt_font *sans = lt_font_get(LT_FONT_SANS);
    lt_canvas_init(&cv, 920, 600);
    lt_gfx_init_canvas(&g, &cv);
    panel(&g, 0, 460, LT_RGB(0x1b, 0x1d, 0x23), LT_RGB(0xe4, 0xe6, 0xeb), LT_RGB(0x8b, 0x93, 0xa3),
          LT_RGB(0x3f, 0xb9, 0x50));
    panel(&g, 460, 460, LT_RGB(0xf4, 0xf5, 0xf7), LT_RGB(0x1f, 0x23, 0x2a), LT_RGB(0x6b, 0x72, 0x80),
          LT_RGB(0x1a, 0x7f, 0x37));
    /* icons strip along the bottom of both panels */
    for (y = 0; y < 2; y++) {
        lt_color fg = y == 0 ? LT_RGB(0xe4, 0xe6, 0xeb) : LT_RGB(0x1f, 0x23, 0x2a);
        for (x = 0; x < 20; x++) {
            id = lt_icon_find(names[(size_t)(y * 20 + x) % (sizeof(names) / sizeof(names[0]))]);
            LT_CHECK(id != LT_ICON_NONE);
            lt_gfx_icon(&g, id, (double)(y * 460 + 12 + x * 22), 536.0, 16.0, 0.0, fg);
            lt_gfx_icon(&g, id, (double)(y * 460 + 12 + x * 22), 560.0, 16.0, 1.5, fg);
            n++;
        }
    }
    (void)sans;
    LT_CHECK(n == 40);
    write_ppm(&cv, "build/typography.ppm");
    lt_canvas_free(&cv);
}

int main(void)
{
    LT_RUN(test_chunks_and_open);
    LT_RUN(test_cmap);
    LT_RUN(test_metrics_mono);
    LT_RUN(test_advance_and_kerning);
    LT_RUN(test_composite_glyphs);
    LT_RUN(test_raster_scaling_and_aa);
    LT_RUN(test_subpixel_and_rotation);
    LT_RUN(test_glyph_cache);
    LT_RUN(test_open_rejects_garbage);
    LT_RUN(test_truncated_font);
    LT_RUN(test_synthetic_font);
    LT_RUN(test_gfx_text);
    LT_RUN(test_gfx_legacy_mapping);
    LT_RUN(test_gfx_text_rot);
    LT_RUN(test_icons);
    LT_RUN(test_showcase);
    lt_font_release_all();
    LT_TEST_MAIN_END
}
