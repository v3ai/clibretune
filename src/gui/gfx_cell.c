/* gfx_cell.c - character-cell drawing helpers: box-line merging, sub-cell
 * bars, sparklines, shadows. All coordinates are pixel units (a cell is 8x16)
 * translated by g->ox/g->oy like every other lt_gfx call; every function is a
 * no-op in pixel mode.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "gui.h"
#include "gfx_priv.h"

#define CELL_W 8
#define CELL_H 16
#define OPAQUE 0xFF000000UL

static int fdiv(int a, int b)
{
    int q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

/* ---- box-drawing line merging ---------------------------------------- */
/* bits: 1 left, 2 right, 4 up, 8 down (light strokes) */
static const lt_u16 line_glyph[16] = {
    0x0020, 0x2574, 0x2576, 0x2500, 0x2575, 0x2518, 0x2514, 0x2534,
    0x2577, 0x2510, 0x250C, 0x252C, 0x2502, 0x2524, 0x251C, 0x253C
};
static const lt_u16 round_glyph[16] = {
    0, 0, 0, 0, 0, 0x256F, 0x2570, 0, 0, 0x256E, 0x256D, 0, 0, 0, 0, 0
};

/* bits of a light box glyph, -1 when the glyph is not a light line */
static int glyph_bits(lt_u32 ch)
{
    int i;
    if (ch == ' ' || ch == 0) { return 0; }
    for (i = 1; i < 16; i++) {
        if (line_glyph[i] == ch || (round_glyph[i] != 0 && round_glyph[i] == ch)) { return i; }
    }
    return -1;
}

lt_u32 lt_cell_line_merge(lt_u32 old, int add, int round)
{
    int have = glyph_bits(old), bits;
    if (have < 0) { have = 0; }
    bits = have | add;
    if (round && have == 0 && round_glyph[bits] != 0) { return round_glyph[bits]; }
    if (round && round_glyph[have] != 0 && have == bits) { return old; }
    return line_glyph[bits];
}

int lt_cell_line_bits(lt_u32 ch)
{
    int b = glyph_bits(ch);
    return b < 0 ? 0 : b;
}

/* ---- helpers ---------------------------------------------------------- */
static lt_cell *cell_ptr(lt_gfx *g, int cx, int cy)
{
    if (g->cells == NULL || cx < 0 || cy < 0 || cx >= g->cells->cols || cy >= g->cells->rows) {
        return NULL;
    }
    if (!lt_rect_contains(g->clip, cx * CELL_W + CELL_W / 2, cy * CELL_H + CELL_H / 2)) {
        return NULL;
    }
    return &g->cells->cells[cy * g->cells->cols + cx];
}

static void mark_cell(lt_gfx *g, int cx, int cy)
{
    lt_rect c = lt_rect_make(cx * CELL_W, cy * CELL_H, CELL_W, CELL_H);
    if (g->dirty.w <= 0 || g->dirty.h <= 0) {
        g->dirty = c;
    } else {
        int x1 = LT_MAX(g->dirty.x + g->dirty.w, c.x + c.w);
        int y1 = LT_MAX(g->dirty.y + g->dirty.h, c.y + c.h);
        g->dirty.x = LT_MIN(g->dirty.x, c.x);
        g->dirty.y = LT_MIN(g->dirty.y, c.y);
        g->dirty.w = x1 - g->dirty.x;
        g->dirty.h = y1 - g->dirty.y;
    }
}

static lt_color mixc(lt_color a, lt_color b, int t)  /* t 0..255 toward b */
{
    int r = (LT_COLOR_R(a) * (255 - t) + LT_COLOR_R(b) * t) / 255;
    int gg = (LT_COLOR_G(a) * (255 - t) + LT_COLOR_G(b) * t) / 255;
    int bl = (LT_COLOR_B(a) * (255 - t) + LT_COLOR_B(b) * t) / 255;
    return LT_RGB(r, gg, bl);
}

/* ---- text with background --------------------------------------------- */
int lt_gfx_cell_text(lt_gfx *g, int x, int y, const char *s, lt_color fg, lt_color bg, int attr)
{
    int cx, cy, n = 0;
    lt_u32 cp;
    if (g->cells == NULL) { return 0; }
    cx = fdiv(x + g->ox, CELL_W);
    cy = fdiv(y + g->oy, CELL_H);
    while ((cp = lt_utf8_next(&s)) != 0) {
        lt_cell *k = cell_ptr(g, cx, cy);
        if (k != NULL) {
            k->ch = cp;
            k->fg = (fg | OPAQUE) & LT_U32_MASK;
            if (LT_COLOR_A(bg) != 0) { k->bg = (bg | OPAQUE) & LT_U32_MASK; }
            k->attr = (lt_u8)attr;
            mark_cell(g, cx, cy);
        }
        cx++;
        n++;
    }
    return n;
}

void lt_gfx_cell_attr(lt_gfx *g, lt_rect r, int attr, int on)
{
    int cx, cy, cx0, cx1, cy0, cy1;
    if (g->cells == NULL || r.w <= 0 || r.h <= 0) { return; }
    cx0 = fdiv(r.x + g->ox, CELL_W);
    cx1 = fdiv(r.x + g->ox + r.w - 1, CELL_W);
    cy0 = fdiv(r.y + g->oy, CELL_H);
    cy1 = fdiv(r.y + g->oy + r.h - 1, CELL_H);
    for (cy = cy0; cy <= cy1; cy++) {
        for (cx = cx0; cx <= cx1; cx++) {
            lt_cell *k = cell_ptr(g, cx, cy);
            if (k != NULL) {
                k->attr = (lt_u8)(on ? (k->attr | attr) : (k->attr & ~attr));
                mark_cell(g, cx, cy);
            }
        }
    }
}

/* ---- bars ---------------------------------------------------------------- */
/* left-aligned partial blocks: 1/8 .. 7/8 of the cell width */
static const lt_u16 hpart[8] = { 0x20, 0x258F, 0x258E, 0x258D, 0x258C, 0x258B, 0x258A, 0x2589 };
/* bottom-aligned partial blocks: 1/8 .. 7/8 of the cell height */
static const lt_u16 vpart[8] = { 0x20, 0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587 };

void lt_gfx_cell_hbar(lt_gfx *g, lt_rect r, double frac, lt_color fg, lt_color track)
{
    int cx0, cx1, cy0, cy1, cx, cy, n, full, part;
    if (g->cells == NULL || r.w <= 0 || r.h <= 0) { return; }
    if (!(frac > 0.0)) { frac = 0.0; }
    if (frac > 1.0) { frac = 1.0; }
    cx0 = fdiv(r.x + g->ox, CELL_W);
    cx1 = fdiv(r.x + g->ox + r.w - 1, CELL_W);
    cy0 = fdiv(r.y + g->oy, CELL_H);
    cy1 = fdiv(r.y + g->oy + r.h - 1, CELL_H);
    n = cx1 - cx0 + 1;
    {
        int eighths = (int)(frac * (double)(n * 8) + 0.5);
        full = eighths / 8;
        part = eighths % 8;
    }
    for (cy = cy0; cy <= cy1; cy++) {
        for (cx = cx0; cx <= cx1; cx++) {
            lt_cell *k = cell_ptr(g, cx, cy);
            int i = cx - cx0;
            if (k == NULL) { continue; }
            if (i < full) {
                k->ch = 0x2588; k->fg = (fg | OPAQUE) & LT_U32_MASK; k->bg = (track | OPAQUE) & LT_U32_MASK;
            } else if (i == full && part > 0) {
                k->ch = hpart[part]; k->fg = (fg | OPAQUE) & LT_U32_MASK; k->bg = (track | OPAQUE) & LT_U32_MASK;
            } else {
                k->ch = ' '; k->bg = (track | OPAQUE) & LT_U32_MASK;
            }
            k->attr = 0;
            mark_cell(g, cx, cy);
        }
    }
}

void lt_gfx_cell_vbar(lt_gfx *g, lt_rect r, double frac, lt_color fg, lt_color track)
{
    int cx0, cx1, cy0, cy1, cx, cy, n, full, part;
    if (g->cells == NULL || r.w <= 0 || r.h <= 0) { return; }
    if (!(frac > 0.0)) { frac = 0.0; }
    if (frac > 1.0) { frac = 1.0; }
    cx0 = fdiv(r.x + g->ox, CELL_W);
    cx1 = fdiv(r.x + g->ox + r.w - 1, CELL_W);
    cy0 = fdiv(r.y + g->oy, CELL_H);
    cy1 = fdiv(r.y + g->oy + r.h - 1, CELL_H);
    n = cy1 - cy0 + 1;
    {
        int eighths = (int)(frac * (double)(n * 8) + 0.5);
        full = eighths / 8;
        part = eighths % 8;
    }
    for (cy = cy0; cy <= cy1; cy++) {
        int i = cy1 - cy;     /* 0 = bottom row */
        for (cx = cx0; cx <= cx1; cx++) {
            lt_cell *k = cell_ptr(g, cx, cy);
            if (k == NULL) { continue; }
            if (i < full) {
                k->ch = 0x2588; k->fg = (fg | OPAQUE) & LT_U32_MASK; k->bg = (track | OPAQUE) & LT_U32_MASK;
            } else if (i == full && part > 0) {
                k->ch = vpart[part]; k->fg = (fg | OPAQUE) & LT_U32_MASK; k->bg = (track | OPAQUE) & LT_U32_MASK;
            } else {
                k->ch = ' '; k->bg = (track | OPAQUE) & LT_U32_MASK;
            }
            k->attr = 0;
            mark_cell(g, cx, cy);
        }
    }
}

/* sparkline: one cell column per sample (the newest samples if there are more
 * than columns); height of the rect in rows gives 8 levels per row */
void lt_gfx_cell_spark(lt_gfx *g, lt_rect r, const double *v, int n, double lo, double hi,
                       lt_color fg, lt_color bg)
{
    int cx0, cx1, cy0, cy1, cx, cy, cols, rows, first;
    if (g->cells == NULL || r.w <= 0 || r.h <= 0) { return; }
    cx0 = fdiv(r.x + g->ox, CELL_W);
    cx1 = fdiv(r.x + g->ox + r.w - 1, CELL_W);
    cy0 = fdiv(r.y + g->oy, CELL_H);
    cy1 = fdiv(r.y + g->oy + r.h - 1, CELL_H);
    cols = cx1 - cx0 + 1;
    rows = cy1 - cy0 + 1;
    first = n > cols ? n - cols : 0;
    for (cx = cx0; cx <= cx1; cx++) {
        int si = first + (cx - cx0);
        int eighths = -1;
        if (v != NULL && si < n) {
            double f = hi > lo ? (v[si] - lo) / (hi - lo) : 0.0;
            if (!(f > 0.0)) { f = 0.0; }
            if (f > 1.0) { f = 1.0; }
            eighths = (int)(f * (double)(rows * 8 - 1) + 0.5) + 1;
        }
        for (cy = cy0; cy <= cy1; cy++) {
            lt_cell *k = cell_ptr(g, cx, cy);
            int i = cy1 - cy, lvl;
            if (k == NULL) { continue; }
            k->bg = (bg | OPAQUE) & LT_U32_MASK;
            k->fg = (fg | OPAQUE) & LT_U32_MASK;
            k->attr = 0;
            lvl = eighths - i * 8;
            if (eighths < 0 || lvl <= 0) { k->ch = ' '; }
            else if (lvl >= 8) { k->ch = 0x2588; }
            else { k->ch = vpart[lvl]; }
            mark_cell(g, cx, cy);
        }
    }
}

/* ---- shadows ---------------------------------------------------------------- */
/* dim the cells covered by r offset by (dx, dy) cells but not by r itself: the
 * classic text-mode drop shadow (background and glyphs are darkened) */
void lt_gfx_cell_shadow(lt_gfx *g, lt_rect r, int dx, int dy)
{
    int cx0, cx1, cy0, cy1, cx, cy;
    if (g->cells == NULL || r.w <= 0 || r.h <= 0) { return; }
    cx0 = fdiv(r.x + g->ox, CELL_W);
    cx1 = fdiv(r.x + g->ox + r.w - 1, CELL_W);
    cy0 = fdiv(r.y + g->oy, CELL_H);
    cy1 = fdiv(r.y + g->oy + r.h - 1, CELL_H);
    for (cy = cy0 + dy; cy <= cy1 + dy; cy++) {
        for (cx = cx0 + dx; cx <= cx1 + dx; cx++) {
            lt_cell *k;
            if (cx >= cx0 && cx <= cx1 && cy >= cy0 && cy <= cy1) { continue; }
            k = cell_ptr(g, cx, cy);
            if (k == NULL) { continue; }
            k->bg = mixc(k->bg, LT_RGB(0, 0, 0), 150);
            k->fg = mixc(k->fg, LT_RGB(0, 0, 0), 120);
            mark_cell(g, cx, cy);
        }
    }
}
