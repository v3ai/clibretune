/* cellshot.c - snapshot of a character-cell grid: plain text and PPM image.
 *
 * The image draws every cell with its background color and its glyph in the
 * foreground color: Plex Mono (embedded) for ordinary characters, procedural
 * strokes for box drawing, block elements, braille and a few geometric
 * symbols (so the result does not depend on which glyphs the font has), and
 * the 8x16 bitmap font as the last resort. Used by the screenshot harness and
 * the tests; the terminal backend does not use it.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include "gui.h"

#define SHOT_CW 10
#define SHOT_CH 20
#define SHOT_GAP 2

/* ---- plain text ---------------------------------------------------- */
static void put_utf8(char *dst, size_t *n, lt_u32 cp)
{
    if (cp < 0x80UL) {
        dst[(*n)++] = (char)cp;
    } else if (cp < 0x800UL) {
        dst[(*n)++] = (char)(0xC0 | (cp >> 6));
        dst[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000UL) {
        dst[(*n)++] = (char)(0xE0 | (cp >> 12));
        dst[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        dst[(*n)++] = (char)(0xF0 | ((cp >> 18) & 0x07));
        dst[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
}

char *lt_cells_to_text(const lt_cells *g)
{
    size_t cap, n = 0;
    int x, y, last;
    char *out;
    if (g == NULL || g->cells == NULL) { return lt_strdup(""); }
    cap = (size_t)g->rows * ((size_t)g->cols * 4 + 1) + 1;
    out = (char *)lt_malloc(cap);
    for (y = 0; y < g->rows; y++) {
        last = -1;
        for (x = 0; x < g->cols; x++) {
            lt_u32 ch = g->cells[(size_t)y * (size_t)g->cols + (size_t)x].ch;
            if (ch != ' ' && ch != 0) { last = x; }
        }
        for (x = 0; x <= last; x++) {
            lt_u32 ch = g->cells[(size_t)y * (size_t)g->cols + (size_t)x].ch;
            if (ch < 0x20UL || ch > 0x10FFFFUL) { ch = ' '; }
            put_utf8(out, &n, ch);
        }
        out[n++] = '\n';
    }
    out[n] = '\0';
    return out;
}

/* ---- image --------------------------------------------------------- */
typedef struct shot_img {
    lt_u32 *px;
    int w, h;
} shot_img;

static void blend_px(shot_img *im, int x, int y, lt_color c, int a)
{
    lt_u32 d;
    int r, g, b;
    if (x < 0 || y < 0 || x >= im->w || y >= im->h || a <= 0) { return; }
    d = im->px[(size_t)y * (size_t)im->w + (size_t)x];
    r = (LT_COLOR_R(c) * a + (int)((d >> 16) & 255) * (255 - a)) / 255;
    g = (LT_COLOR_G(c) * a + (int)((d >> 8) & 255) * (255 - a)) / 255;
    b = (LT_COLOR_B(c) * a + (int)(d & 255) * (255 - a)) / 255;
    im->px[(size_t)y * (size_t)im->w + (size_t)x] =
        (lt_u32)(((lt_u32)r << 16) | ((lt_u32)g << 8) | (lt_u32)b);
}

/* fill [x0,x1) x [y0,y1) inside the cell whose top-left is (ox,oy) */
static void box_fill(shot_img *im, int ox, int oy, int x0, int y0, int x1, int y1, lt_color c)
{
    int x, y;
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > SHOT_CW) { x1 = SHOT_CW; }
    if (y1 > SHOT_CH) { y1 = SHOT_CH; }
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) { blend_px(im, ox + x, oy + y, c, 255); }
    }
}

/* digits: left right up down; 0 none, 1 light, 2 heavy, 3 double */
static const char *const box_tab[128] = {
    "1100", "2200", "0011", "0022", "1100", "2200", "0011", "0022",
    "1100", "2200", "0011", "0022", "0101", "0102", "0201", "0202",
    "1001", "2001", "1002", "2002", "0110", "0120", "0210", "0220",
    "1010", "2010", "1020", "2020", "0111", "0112", "0211", "0121",
    "0221", "0212", "0122", "0222", "1011", "2011", "1021", "1012",
    "1022", "2012", "2021", "2022", "1101", "2101", "1201", "2201",
    "1102", "2102", "1202", "2202", "1110", "2110", "1210", "2210",
    "1120", "2120", "1220", "2220", "1111", "2111", "1211", "2211",
    "1121", "1112", "1122", "2121", "1221", "2112", "1212", "2221",
    "2212", "2122", "1222", "2222", "1100", "2200", "0011", "0022",
    "0301", "0103", "0303", "3001", "1003", "3003", "0310", "0130",
    "0330", "3010", "1030", "3030", "0311", "0133", "0333", "3011",
    "1033", "3033", "3301", "1103", "3303", "3310", "1130", "3330",
    "3311", "1133", "3333", "0101", "1001", "1010", "0110", "0000",
    "0000", "0000", "0000", "1000", "0010", "0100", "0001", "2000",
    "0020", "0200", "0002", "1200", "0012", "2100", "0021", "0000"
};

static void draw_double(shot_img *im, int ox, int oy, int l, int r, int u, int d, lt_color fg)
{
    int cx = SHOT_CW / 2 - 1, cy = SHOT_CH / 2 - 1, x, y;
    /* horizontal strokes at cy - gap and cy + gap, vertical at cx - gap and cx + gap */
    for (x = 0; x < SHOT_CW; x++) {
        int up_on, dn_on;
        if (x < cx - SHOT_GAP) {
            up_on = dn_on = (l == 3);
        } else if (x > cx + SHOT_GAP + 1) {
            up_on = dn_on = (r == 3);
        } else {
            up_on = (u == 0) && (l == 3 || r == 3);
            dn_on = (d == 0) && (l == 3 || r == 3);
        }
        if (up_on) { box_fill(im, ox, oy, x, cy - SHOT_GAP, x + 1, cy - SHOT_GAP + 1, fg); }
        if (dn_on) { box_fill(im, ox, oy, x, cy + SHOT_GAP + 1, x + 1, cy + SHOT_GAP + 2, fg); }
    }
    for (y = 0; y < SHOT_CH; y++) {
        int lf_on, rt_on;
        if (y < cy - SHOT_GAP) {
            lf_on = rt_on = (u == 3);
        } else if (y > cy + SHOT_GAP + 1) {
            lf_on = rt_on = (d == 3);
        } else {
            lf_on = (l == 0) && (u == 3 || d == 3);
            rt_on = (r == 0) && (u == 3 || d == 3);
        }
        if (lf_on) { box_fill(im, ox, oy, cx - SHOT_GAP, y, cx - SHOT_GAP + 1, y + 1, fg); }
        if (rt_on) { box_fill(im, ox, oy, cx + SHOT_GAP + 1, y, cx + SHOT_GAP + 2, y + 1, fg); }
    }
    /* single-weight arms that meet double ones */
    if (l == 1 || l == 2) { box_fill(im, ox, oy, 0, cy, cx + 1, cy + 2, fg); }
    if (r == 1 || r == 2) { box_fill(im, ox, oy, cx, cy, SHOT_CW, cy + 2, fg); }
    if (u == 1 || u == 2) { box_fill(im, ox, oy, cx, 0, cx + 2, cy + 1, fg); }
    if (d == 1 || d == 2) { box_fill(im, ox, oy, cx, cy, cx + 2, SHOT_CH, fg); }
}

/* quarter circle for rounded corners; the arc runs between the cell center
 * (vertical arm) and the cell center (horizontal arm) with radius 4 */
static void draw_round(shot_img *im, int ox, int oy, lt_u32 cp, lt_color fg)
{
    /* quarter circle of radius 4 joining the two arms at the cell center */
    static const int arc_dx[9] = { 0, 1, 1, 2, 2, 3, 3, 4, 4 };
    static const int arc_dy[9] = { 4, 3, 3, 2, 2, 1, 1, 0, 0 };
    int cx = SHOT_CW / 2 - 1, cy = SHOT_CH / 2 - 1, i, rad = 4;
    int sx = (cp == 0x256DUL || cp == 0x2570UL) ? 1 : -1; /* horizontal arm to the right */
    int sy = (cp == 0x256DUL || cp == 0x256EUL) ? 1 : -1; /* vertical arm downward */
    for (i = 0; i < 9; i++) {
        int px = cx + sx * arc_dx[i];
        int py = cy + sy * arc_dy[i];
        box_fill(im, ox, oy, px, py, px + 2, py + 1, fg);
        box_fill(im, ox, oy, px, py, px + 1, py + 2, fg);
    }
    if (sx > 0) { box_fill(im, ox, oy, cx + rad, cy, SHOT_CW, cy + 2, fg); }
    else { box_fill(im, ox, oy, 0, cy, cx - rad + 2, cy + 2, fg); }
    if (sy > 0) { box_fill(im, ox, oy, cx, cy + rad, cx + 2, SHOT_CH, fg); }
    else { box_fill(im, ox, oy, cx, 0, cx + 2, cy - rad + 2, fg); }
}

static void draw_box(shot_img *im, int ox, int oy, lt_u32 cp, lt_color fg)
{
    const char *t = box_tab[cp - 0x2500UL];
    int l = t[0] - '0', r = t[1] - '0', u = t[2] - '0', d = t[3] - '0';
    int cx = SHOT_CW / 2 - 1, cy = SHOT_CH / 2 - 1, i;
    int hl, vl;
    if (cp >= 0x256DUL && cp <= 0x2570UL) {
        draw_round(im, ox, oy, cp, fg);
        return;
    }
    if (cp >= 0x2571UL && cp <= 0x2573UL) {
        for (i = 0; i < SHOT_CH; i++) {
            int x = i * SHOT_CW / SHOT_CH;
            if (cp != 0x2572UL) { blend_px(im, ox + SHOT_CW - 1 - x, oy + i, fg, 255); }
            if (cp != 0x2571UL) { blend_px(im, ox + x, oy + i, fg, 255); }
        }
        return;
    }
    if (l == 3 || r == 3 || u == 3 || d == 3) {
        draw_double(im, ox, oy, l, r, u, d, fg);
        return;
    }
    /* thickness of the horizontal / vertical strokes where they meet */
    hl = (l == 2 || r == 2) ? 3 : 2;
    vl = (u == 2 || d == 2) ? 3 : 2;
    if (l) {
        int th = l == 2 ? 3 : 2;
        box_fill(im, ox, oy, 0, cy - (th - 2), cx + vl, cy + 2, fg);
    }
    if (r) {
        int th = r == 2 ? 3 : 2;
        box_fill(im, ox, oy, cx, cy - (th - 2), SHOT_CW, cy + 2, fg);
    }
    if (u) {
        int th = u == 2 ? 3 : 2;
        box_fill(im, ox, oy, cx - (th - 2), 0, cx + 2, cy + hl, fg);
    }
    if (d) {
        int th = d == 2 ? 3 : 2;
        box_fill(im, ox, oy, cx - (th - 2), cy, cx + 2, SHOT_CH, fg);
    }
}

static void draw_block(shot_img *im, int ox, int oy, lt_u32 cp, lt_color fg)
{
    if (cp == 0x2580UL) {
        box_fill(im, ox, oy, 0, 0, SHOT_CW, SHOT_CH / 2, fg);
    } else if (cp >= 0x2581UL && cp <= 0x2588UL) {
        int h = (int)(cp - 0x2580UL) * SHOT_CH / 8;
        box_fill(im, ox, oy, 0, SHOT_CH - h, SHOT_CW, SHOT_CH, fg);
    } else if (cp >= 0x2589UL && cp <= 0x258FUL) {
        int w = (int)(0x2590UL - cp) * SHOT_CW / 8;
        box_fill(im, ox, oy, 0, 0, w, SHOT_CH, fg);
    } else if (cp == 0x2590UL) {
        box_fill(im, ox, oy, SHOT_CW / 2, 0, SHOT_CW, SHOT_CH, fg);
    } else if (cp >= 0x2591UL && cp <= 0x2593UL) {
        int a = cp == 0x2591UL ? 64 : (cp == 0x2592UL ? 128 : 192);
        int x, y;
        for (y = 0; y < SHOT_CH; y++) {
            for (x = 0; x < SHOT_CW; x++) { blend_px(im, ox + x, oy + y, fg, a); }
        }
    } else if (cp == 0x2594UL) {
        box_fill(im, ox, oy, 0, 0, SHOT_CW, SHOT_CH / 8, fg);
    } else if (cp == 0x2595UL) {
        box_fill(im, ox, oy, SHOT_CW - SHOT_CW / 8 - 1, 0, SHOT_CW, SHOT_CH, fg);
    } else if (cp >= 0x2596UL && cp <= 0x259FUL) {
        /* quadrants: bit0 UL, bit1 UR, bit2 LL, bit3 LR */
        static const int q[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };
        int m = q[cp - 0x2596UL], hw = SHOT_CW / 2, hh = SHOT_CH / 2;
        if (m & 1) { box_fill(im, ox, oy, 0, 0, hw, hh, fg); }
        if (m & 2) { box_fill(im, ox, oy, hw, 0, SHOT_CW, hh, fg); }
        if (m & 4) { box_fill(im, ox, oy, 0, hh, hw, SHOT_CH, fg); }
        if (m & 8) { box_fill(im, ox, oy, hw, hh, SHOT_CW, SHOT_CH, fg); }
    }
}

static void draw_braille(shot_img *im, int ox, int oy, lt_u32 cp, lt_color fg)
{
    int m = (int)(cp - 0x2800UL), k;
    /* dot n -> (col,row): 1:(0,0) 2:(0,1) 3:(0,2) 4:(1,0) 5:(1,1) 6:(1,2) 7:(0,3) 8:(1,3) */
    static const int dc[8] = { 0, 0, 0, 1, 1, 1, 0, 1 };
    static const int dr[8] = { 0, 1, 2, 0, 1, 2, 3, 3 };
    for (k = 0; k < 8; k++) {
        if (m & (1 << k)) {
            int x = 2 + dc[k] * 4, y = 2 + dr[k] * 4;
            box_fill(im, ox, oy, x, y, x + 3, y + 3, fg);
        }
    }
}

/* geometric symbols that terminals commonly draw themselves */
static int draw_symbol(shot_img *im, int ox, int oy, lt_u32 cp, lt_color fg)
{
    int x, y, cx = SHOT_CW / 2, cy = SHOT_CH / 2;
    switch (cp) {
    case 0x25B6UL: case 0x25BAUL: case 0x25B8UL: case 0x25B7UL:
        for (y = 5; y < SHOT_CH - 5; y++) {
            int half = (y < cy ? y - 5 : SHOT_CH - 6 - y);
            for (x = 2; x < 2 + (half + 1) * 7 / (cy - 4) && x < SHOT_CW - 1; x++) {
                blend_px(im, ox + x, oy + y, fg, 255);
            }
        }
        return 1;
    case 0x25C0UL: case 0x25C4UL: case 0x25C2UL: case 0x25C1UL:
        for (y = 5; y < SHOT_CH - 5; y++) {
            int half = (y < cy ? y - 5 : SHOT_CH - 6 - y);
            for (x = SHOT_CW - 3; x > SHOT_CW - 3 - (half + 1) * 7 / (cy - 4) && x > 0; x--) {
                blend_px(im, ox + x, oy + y, fg, 255);
            }
        }
        return 1;
    case 0x25BCUL: case 0x25BEUL:
        for (y = 6; y < SHOT_CH - 6; y++) {
            int w = (SHOT_CH - 7 - y) * 4 / (SHOT_CH - 12) + 1;
            for (x = cx - w; x < cx + w; x++) { blend_px(im, ox + x, oy + y, fg, 255); }
        }
        return 1;
    case 0x25B2UL: case 0x25B4UL:
        for (y = 6; y < SHOT_CH - 6; y++) {
            int w = (y - 6) * 4 / (SHOT_CH - 12) + 1;
            for (x = cx - w; x < cx + w; x++) { blend_px(im, ox + x, oy + y, fg, 255); }
        }
        return 1;
    case 0x25CFUL: case 0x2022UL: case 0x25CBUL: case 0x25C9UL:
        for (y = -4; y <= 4; y++) {
            for (x = -4; x <= 4; x++) {
                int d2 = x * x + y * y;
                if (d2 <= 16 && (cp != 0x25CBUL || d2 >= 7)) {
                    blend_px(im, ox + cx + x, oy + cy + y, fg, 255);
                }
            }
        }
        return 1;
    case 0x25A0UL: case 0x25AAUL:
        box_fill(im, ox, oy, 1, 5, SHOT_CW - 1, SHOT_CH - 5, fg);
        return 1;
    default:
        return 0;
    }
}

static void draw_bitmap(shot_img *im, int ox, int oy, lt_u32 cp, lt_color fg)
{
    const lt_u8 *gl = lt_font_glyph(cp);
    int x, y;
    for (y = 0; y < SHOT_CH; y++) {
        int sy = y * 16 / SHOT_CH;
        for (x = 0; x < SHOT_CW; x++) {
            int sx = x * 8 / SHOT_CW;
            if (gl[sy] & (0x80 >> sx)) { blend_px(im, ox + x, oy + y, fg, 255); }
        }
    }
}

static void draw_glyph(shot_img *im, int ox, int oy, lt_u32 cp, lt_color fg, int bold)
{
    lt_font *f;
    int gid;
    if (cp == ' ' || cp == 0) { return; }
    if (cp >= 0x2500UL && cp <= 0x257FUL) { draw_box(im, ox, oy, cp, fg); return; }
    if (cp >= 0x2580UL && cp <= 0x259FUL) { draw_block(im, ox, oy, cp, fg); return; }
    if (cp >= 0x2800UL && cp <= 0x28FFUL) { draw_braille(im, ox, oy, cp, fg); return; }
    if (draw_symbol(im, ox, oy, cp, fg)) { return; }
    f = lt_font_get(bold ? LT_FONT_MONO_MEDIUM : LT_FONT_MONO);
    if (f != NULL) {
        gid = lt_ttf_glyph_index(f, cp);
        if (gid != 0) {
            int adv = lt_ttf_advance(f, gid), upm = lt_ttf_units_per_em(f);
            int asc = lt_ttf_ascent(f), desc = -lt_ttf_descent(f);
            const lt_ttf_bitmap *bm;
            double px, s;
            int base, i, j;
            if (adv <= 0) { adv = upm * 6 / 10; }
            px = (double)SHOT_CW * (double)upm / (double)adv;
            s = px / (double)upm;
            base = (SHOT_CH - (int)((double)(asc + desc) * s + 0.5)) / 2 +
                   (int)((double)asc * s + 0.5);
            bm = lt_ttf_glyph(f, gid, px, 0);
            if (bm != NULL) {
                for (j = 0; j < bm->h; j++) {
                    for (i = 0; i < bm->w; i++) {
                        int a = bm->a[(size_t)j * (size_t)bm->w + (size_t)i];
                        if (a > 0) {
                            blend_px(im, ox + bm->xoff + i, oy + base + bm->yoff + j, fg, a);
                        }
                    }
                }
            }
            return;
        }
    }
    draw_bitmap(im, ox, oy, cp, fg);
}

int lt_cells_write_ppm(const lt_cells *g, const char *path)
{
    shot_img im;
    FILE *fp;
    int x, y, w, h;
    size_t i, n;
    if (g == NULL || g->cells == NULL || g->cols <= 0 || g->rows <= 0) { return -1; }
    w = g->cols * SHOT_CW;
    h = g->rows * SHOT_CH;
    im.w = w;
    im.h = h;
    n = (size_t)w * (size_t)h;
    im.px = (lt_u32 *)lt_calloc(n, sizeof(lt_u32));
    for (y = 0; y < g->rows; y++) {
        for (x = 0; x < g->cols; x++) {
            const lt_cell *c = &g->cells[(size_t)y * (size_t)g->cols + (size_t)x];
            lt_color fg = c->fg, bg = c->bg, t;
            int ox = x * SHOT_CW, oy = y * SHOT_CH;
            if (c->attr & LT_CELL_REVERSE) { t = fg; fg = bg; bg = t; }
            box_fill(&im, ox, oy, 0, 0, SHOT_CW, SHOT_CH, bg);
            draw_glyph(&im, ox, oy, c->ch, fg, (c->attr & LT_CELL_BOLD) != 0);
            if (c->attr & LT_CELL_UNDERLINE) {
                box_fill(&im, ox, oy, 0, SHOT_CH - 2, SHOT_CW, SHOT_CH - 1, fg);
            }
        }
    }
    fp = fopen(path, "wb");
    if (fp == NULL) {
        lt_free(im.px);
        return -1;
    }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < n; i++) {
        unsigned char rgb[3];
        rgb[0] = (unsigned char)((im.px[i] >> 16) & 255);
        rgb[1] = (unsigned char)((im.px[i] >> 8) & 255);
        rgb[2] = (unsigned char)(im.px[i] & 255);
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
    lt_free(im.px);
    return 0;
}
