/* gfx_text.c - TrueType text drawing on top of gfx.c: the embedded IBM Plex
 * fonts, kerned and sub-pixel positioned text, rotated text for gauges, and
 * the mapping of the legacy 8x16 bitmap / Hershey calls onto Plex.
 *
 * Text contrast: coverage is blended in sRGB space, which makes light text
 * on a dark background look thin and dark text on a light background look
 * heavy. Each draw therefore remaps coverage through a small power curve
 * chosen from the text color's luminance (see lt_gfx_contrast_lut).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>
#include "gui.h"
#include "gfx_priv.h"
#include "fonts_data.h"

#define LUT_LEVELS 16
/* legacy bitmap cell mapping: 8x16 cell, Plex Mono 13.333px has an exactly
 * 8 px advance (600/1000 em) */
#define CELL_MONO_PX (40.0 / 3.0)
#define CELL_BASELINE 13
#define UI_SUBPOS 4

static lt_font *font_tab[LT_FONT_COUNT];
static int font_tried[LT_FONT_COUNT];
static lt_u8 lut_tab[LUT_LEVELS][256];
static int lut_ready;

/* ------------------------------------------------------------------ */
/* font registry                                                       */
lt_font *lt_font_get(lt_font_id id)
{
    int n = 0, i = (int)id;
    const lt_ttf_chunk *ch;
    if (i < 0 || i >= LT_FONT_COUNT) { return NULL; }
    if (!font_tried[i]) {
        font_tried[i] = 1;
        ch = lt_font_data(i, &n);
        if (ch != NULL && n > 0) {
            font_tab[i] = lt_ttf_open_chunks(ch, n);
        }
    }
    return font_tab[i];
}

void lt_font_release_all(void)
{
    int i;
    for (i = 0; i < LT_FONT_COUNT; i++) {
        lt_ttf_close(font_tab[i]);
        font_tab[i] = NULL;
        font_tried[i] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* metrics                                                             */
static int round_px(double v)
{
    return (int)floor(v + 0.5);
}

int lt_font_ascent(lt_font *f, double px)
{
    if (f == NULL) { return 13; }
    return round_px((double)lt_ttf_ascent(f) * px / (double)lt_ttf_units_per_em(f));
}

int lt_font_descent(lt_font *f, double px)
{
    if (f == NULL) { return 3; }
    return round_px((double)-lt_ttf_descent(f) * px / (double)lt_ttf_units_per_em(f));
}

int lt_font_line_height(lt_font *f, double px)
{
    return lt_font_ascent(f, px) + lt_font_descent(f, px);
}

double lt_font_cap_height(lt_font *f, double px)
{
    if (f == NULL) { return px * 0.7; }
    return (double)lt_ttf_cap_height(f) * px / (double)lt_ttf_units_per_em(f);
}

double lt_font_x_height(lt_font *f, double px)
{
    if (f == NULL) { return px * 0.5; }
    return (double)lt_ttf_x_height(f) * px / (double)lt_ttf_units_per_em(f);
}

/* advance of the glyph for cp, kerned against prev (pixels, unrounded) */
static double glyph_step(lt_font *f, double s, lt_u32 cp, int *prev, int *gid_out)
{
    int gid = lt_ttf_glyph_index(f, cp);
    double w = (double)lt_ttf_advance(f, gid) * s;
    if (*prev != 0) { w += (double)lt_ttf_kern(f, *prev, gid) * s; }
    *prev = gid;
    *gid_out = gid;
    return w;
}

double lt_gfx_text_measure(lt_font *f, double px, const char *utf8)
{
    double w = 0.0, s;
    int prev = 0, gid;
    lt_u32 cp;
    if (utf8 == NULL) { return 0.0; }
    if (f == NULL) {
        return (double)lt_gfx_text_width(utf8, 1) * px / 16.0;
    }
    s = px / (double)lt_ttf_units_per_em(f);
    while ((cp = lt_utf8_next(&utf8)) != 0) {
        w += glyph_step(f, s, cp, &prev, &gid);
    }
    return w;
}

size_t lt_gfx_text_fit(lt_font *f, double px, const char *utf8, double max_w)
{
    const char *p = utf8;
    double w = 0.0, s;
    size_t best = 0;
    int prev = 0, gid;
    lt_u32 cp;
    if (utf8 == NULL) { return 0; }
    if (f == NULL) {
        double cw = (double)LT_FONT_W * px / 16.0;
        size_t k = 0;
        while ((cp = lt_utf8_next(&p)) != 0) {
            if ((double)(k + 1) * cw > max_w) { break; }
            k++;
            best = (size_t)(p - utf8);
        }
        return best;
    }
    s = px / (double)lt_ttf_units_per_em(f);
    while ((cp = lt_utf8_next(&p)) != 0) {
        w += glyph_step(f, s, cp, &prev, &gid);
        if (w > max_w) { break; }
        best = (size_t)(p - utf8);
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* contrast                                                            */
static void lut_init(void)
{
    int l, v;
    for (l = 0; l < LUT_LEVELS; l++) {
        /* luminance 0..1 of the level center; e < 1 boosts coverage */
        double lum = ((double)l + 0.5) / (double)LUT_LEVELS;
        double e = 1.0 + 0.6 * (0.5 - lum);
        for (v = 0; v < 256; v++) {
            double c = pow((double)v / 255.0, e) * 255.0 + 0.5;
            lut_tab[l][v] = (lt_u8)(int)(c > 255.0 ? 255.0 : c);
        }
    }
    lut_ready = 1;
}

const lt_u8 *lt_gfx_contrast_lut(lt_color c)
{
    int lum = (LT_COLOR_R(c) * 299 + LT_COLOR_G(c) * 587 + LT_COLOR_B(c) * 114) / 1000;
    if (!lut_ready) { lut_init(); }
    return lut_tab[lum * LUT_LEVELS / 256];
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
static lt_bool use_bitmap(const lt_gfx *g, lt_font *f)
{
    return f == NULL || g->canvas == NULL || g->cells != NULL || g->bitmap_text;
}

/* draw one cached glyph run; returns the unrounded advance */
static double run_draw(lt_gfx *g, lt_font *f, double px, double x, int baseline, const char *s,
                       lt_color c)
{
    double sc = px / (double)lt_ttf_units_per_em(f), pen = x, w, ya;
    const lt_u8 *lut = lt_gfx_contrast_lut(c);
    int prev = 0, gid, vis;
    lt_u32 cp;
    ya = (double)(baseline + g->oy);
    vis = ya - (double)lt_font_ascent(f, px) - 2.0 < (double)(g->clip.y + g->clip.h) &&
          ya + (double)lt_font_descent(f, px) + 2.0 > (double)g->clip.y;
    while ((cp = lt_utf8_next(&s)) != 0) {
        const lt_ttf_bitmap *bm;
        double q, adv, step;
        int ip, sub;
        step = glyph_step(f, sc, cp, &prev, &gid);
        adv = (double)lt_ttf_advance(f, gid) * sc;
        /* kerning moves the glyph itself: the pen jumps by (step - adv) first */
        pen += step - adv;
        if (vis && cp != ' ') {
            double apen = pen + (double)g->ox;
            if (apen + 1.5 * px >= (double)g->clip.x && apen - px <= (double)(g->clip.x + g->clip.w)) {
                q = floor(pen * (double)UI_SUBPOS + 0.5) / (double)UI_SUBPOS;
                ip = (int)floor(q);
                sub = (int)((q - (double)ip) * (double)UI_SUBPOS + 0.5);
                if (sub >= UI_SUBPOS) { sub = UI_SUBPOS - 1; }
                bm = lt_ttf_glyph(f, gid, px, sub);
                if (bm != NULL) {
                    lt_gfx_blit_mask(g, ip + bm->xoff, baseline + bm->yoff, bm->a, bm->w, bm->h,
                                     bm->w, c, lut);
                }
            }
        }
        pen += adv;
    }
    w = pen - x;
    return w;
}

int lt_gfx_text_bl(lt_gfx *g, lt_font *f, double px, double x, int baseline, const char *utf8,
                   lt_color c)
{
    if (utf8 == NULL) { return 0; }
    if (use_bitmap(g, f)) {
        int w = lt_gfx_text(g, (int)floor(x + 0.5), baseline - 13, utf8, c, 1);
        return w;
    }
    return round_px(run_draw(g, f, px, x, baseline, utf8, c));
}

int lt_gfx_text_ex(lt_gfx *g, lt_font *f, double px, int x, int y, const char *utf8, lt_color c)
{
    if (utf8 == NULL) { return 0; }
    if (use_bitmap(g, f)) {
        return lt_gfx_text(g, x, y, utf8, c, 1);
    }
    return round_px(run_draw(g, f, px, (double)x, y + lt_font_ascent(f, px), utf8, c));
}

void lt_gfx_text_rot(lt_gfx *g, lt_font *f, double px, double x, double y, const char *utf8,
                     double rotation, int align, int valign, lt_color c)
{
    double ca, sa, sc, tw, start, vb, pen, cap;
    const lt_u8 *lut;
    int prev = 0, gid;
    lt_u32 cp;
    if (utf8 == NULL || !(px > 0.0)) { return; }
    if (use_bitmap(g, f)) {
        lt_gfx_vtext(g, x, y, utf8, px * 0.7, rotation, align, valign, 0.0, c);
        return;
    }
    tw = lt_gfx_text_measure(f, px, utf8);
    cap = lt_font_cap_height(f, px);
    start = align == 1 ? -tw / 2.0 : (align == 2 ? -tw : 0.0);
    vb = valign == 0 ? cap : (valign == 1 ? cap / 2.0 : 0.0);
    ca = cos(rotation);
    sa = sin(rotation);
    if (fabs(sa) < 1.0e-6 && ca > 0.0) {
        /* upright: reuse the cached, grid-fitted glyphs */
        run_draw(g, f, px, x + start, round_px(y + vb), utf8, c);
        return;
    }
    sc = px / (double)lt_ttf_units_per_em(f);
    lut = lt_gfx_contrast_lut(c);
    pen = start;
    while ((cp = lt_utf8_next(&utf8)) != 0) {
        double step, adv, u, ox, oy, m[4], fx, fy;
        lt_ttf_bitmap bm;
        int ix, iy;
        step = glyph_step(f, sc, cp, &prev, &gid);
        adv = (double)lt_ttf_advance(f, gid) * sc;
        u = pen + (step - adv);
        pen += step;
        if (cp == ' ') { continue; }
        ox = x + u * ca - vb * sa;
        oy = y + u * sa + vb * ca;
        if (!(fabs(ox) < 1.0e6) || !(fabs(oy) < 1.0e6)) { continue; }
        ix = (int)floor(ox);
        iy = (int)floor(oy);
        fx = ox - (double)ix;
        fy = oy - (double)iy;
        m[0] = ca * sc; m[1] = sa * sc; m[2] = sa * sc; m[3] = -ca * sc;
        if (lt_ttf_raster(f, gid, m, fx, fy, &bm)) {
            lt_gfx_blit_mask(g, ix + bm.xoff, iy + bm.yoff, bm.a, bm.w, bm.h, bm.w, c, lut);
        }
    }
}

/* ------------------------------------------------------------------ */
/* hooks for the legacy bitmap / Hershey API                            */
int lt_gfx_text_cellfont(lt_gfx *g, int x, int y, const char *s, lt_color c, int scale)
{
    lt_font *f = lt_font_get(LT_FONT_MONO);
    const lt_u8 *lut;
    const char *p;
    double px = CELL_MONO_PX * (double)scale;
    lt_u32 cp;
    int ix = x, cw = LT_FONT_W * scale, baseline = y + CELL_BASELINE * scale;
    if (f == NULL) { return 0; }
    /* every glyph must exist in Plex Mono and not be a cell-filling box/block
     * character (those stay on the bitmap font so they tile the grid) */
    p = s;
    while ((cp = lt_utf8_next(&p)) != 0) {
        if (cp >= 0x2500UL && cp <= 0x259FUL) { return 0; }
        if (cp != ' ' && lt_ttf_glyph_index(f, cp) == 0) { return 0; }
    }
    lut = lt_gfx_contrast_lut(c);
    p = s;
    while ((cp = lt_utf8_next(&p)) != 0) {
        if (cp != ' ') {
            const lt_ttf_bitmap *bm = lt_ttf_glyph(f, lt_ttf_glyph_index(f, cp), px, 0);
            if (bm != NULL) {
                lt_gfx_blit_mask(g, ix + bm->xoff, baseline + bm->yoff, bm->a, bm->w, bm->h,
                                 bm->w, c, lut);
            }
        }
        ix += cw;
    }
    return 1;
}

int lt_gfx_vtext_plex(lt_gfx *g, double x, double y, const char *s, double height,
                      double rotation, int align, int valign, lt_color c)
{
    lt_font *f = lt_font_get(LT_FONT_SANS_MEDIUM);
    double ratio;
    if (f == NULL) { return 0; }
    ratio = (double)lt_ttf_cap_height(f) / (double)lt_ttf_units_per_em(f);
    lt_gfx_text_rot(g, f, height / ratio, x, y, s, rotation, align, valign, c);
    return 1;
}

int lt_gfx_vtext_width_plex(const char *s, double height, double *w)
{
    lt_font *f = lt_font_get(LT_FONT_SANS_MEDIUM);
    double ratio;
    if (f == NULL) { return 0; }
    ratio = (double)lt_ttf_cap_height(f) / (double)lt_ttf_units_per_em(f);
    *w = lt_gfx_text_measure(f, height / ratio, s);
    return 1;
}
