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

/* ---- ellipsis ----------------------------------------------------------- */
/* cut the middle out of s so that at most cols columns remain and both the start and the end
 * stay readable ("Target AFR Warm...ent(CLT)"); ASCII only, others are cut at the end */
void lt_cell_ellipsize_mid(const char *s, int cols, char *out, size_t cap)
{
    size_t n = 0, o = 0;
    int ascii = 1, head, tail, i;
    const char *p;
    if (cap == 0) { return; }
    for (p = s; *p != '\0'; p++) {
        if ((unsigned char)*p >= 0x80) { ascii = 0; }
        n++;
    }
    if (!ascii) {
        /* count code points */
        const char *q = s;
        n = 0;
        while (lt_utf8_next(&q) != 0) { n++; }
    }
    if ((int)n <= cols || cols < 3) {
        if ((int)n <= cols) { lt_strlcpy(out, s, cap); return; }
    }
    if (cap < n + 8 || cols < 1) { if (cap > 0) { out[0] = '\0'; } return; }
    if (!ascii || cols < 5) {
        const char *q = s;
        int k = 0;
        while (k < cols - 1 && *q != '\0') {
            const char *b = q;
            (void)lt_utf8_next(&q);
            while (b < q) { out[o++] = *b++; }
            k++;
        }
        out[o++] = '\xE2'; out[o++] = '\x80'; out[o++] = '\xA6';
        out[o] = '\0';
        return;
    }
    tail = (cols - 1) / 2;
    head = cols - 1 - tail;
    for (i = 0; i < head; i++) { out[o++] = s[i]; }
    out[o++] = '\xE2'; out[o++] = '\x80'; out[o++] = '\xA6';
    for (i = 0; i < tail; i++) { out[o++] = s[(int)n - tail + i]; }
    out[o] = '\0';
}

/* ---- braille plots ------------------------------------------------------ */
/* braille dot bit for (dot column 0..1, dot row 0..3) */
static const lt_u8 braille_bit[2][4] = { { 0x01, 0x02, 0x04, 0x40 }, { 0x08, 0x10, 0x20, 0x80 } };

static void braille_dot(lt_u8 *m, int cols, int x, int y)
{
    m[(y / 4) * cols + x / 2] = (lt_u8)(m[(y / 4) * cols + x / 2] | braille_bit[x & 1][y & 3]);
}

/* line plot (2x4 dots per cell) of v[0..n-1] scaled lo..hi into r; without LT_PLOT_STRETCH one
 * sample takes one dot column (newest samples if there are more than columns), with it the
 * samples are spread over the whole width. LT_PLOT_CLEAR resets the rect's cells to bg first,
 * otherwise dots merge into the braille already there (several series in one plot). */
void lt_gfx_cell_plot(lt_gfx *g, lt_rect r, const double *v, int n, double lo, double hi,
                      lt_color fg, lt_color bg, int flags)
{
    int cx0, cx1, cy0, cy1, cols, rows, dw, dh, x, y, first, prev_y = -1, cx, cy;
    lt_u8 *m;
    if (g->cells == NULL || r.w <= 0 || r.h <= 0) { return; }
    cx0 = fdiv(r.x + g->ox, CELL_W);
    cx1 = fdiv(r.x + g->ox + r.w - 1, CELL_W);
    cy0 = fdiv(r.y + g->oy, CELL_H);
    cy1 = fdiv(r.y + g->oy + r.h - 1, CELL_H);
    cols = cx1 - cx0 + 1;
    rows = cy1 - cy0 + 1;
    if (cols > 400) { cols = 400; }
    if (rows > 120) { rows = 120; }
    dw = cols * 2;
    dh = rows * 4;
    m = (lt_u8 *)lt_calloc((size_t)(cols * rows) + 1, 1);
    first = (n > dw && !(flags & LT_PLOT_STRETCH)) ? n - dw : 0;
    for (x = 0; x < dw && v != NULL && n > 0; x++) {
        double val, f;
        if (flags & LT_PLOT_STRETCH) {
            double pos = n > 1 ? (double)x * (double)(n - 1) / (double)(dw - 1) : 0.0;
            int i0 = (int)pos;
            double fr = pos - (double)i0;
            if (i0 >= n - 1) { i0 = n - 1; fr = 0.0; }
            val = i0 < n - 1 ? v[i0] * (1.0 - fr) + v[i0 + 1] * fr : v[i0];
        } else {
            if (first + x >= n) { break; }
            val = v[first + x];
        }
        f = hi > lo ? (val - lo) / (hi - lo) : 0.5;
        if (!(f > 0.0)) { f = 0.0; }
        if (f > 1.0) { f = 1.0; }
        y = (int)((1.0 - f) * (double)(dh - 1) + 0.5);
        if (flags & LT_PLOT_FILL) {
            int yy;
            for (yy = y; yy < dh; yy++) { braille_dot(m, cols, x, yy); }
        } else {
            int a = y, b = y, yy;
            if (prev_y >= 0) { a = LT_MIN(y, prev_y); b = LT_MAX(y, prev_y); }
            for (yy = a; yy <= b; yy++) { braille_dot(m, cols, x, yy); }
        }
        prev_y = y;
    }
    for (cy = 0; cy < rows; cy++) {
        for (cx = 0; cx < cols; cx++) {
            lt_cell *k = cell_ptr(g, cx0 + cx, cy0 + cy);
            lt_u8 bits = m[cy * cols + cx];
            if (k == NULL) { continue; }
            if (flags & LT_PLOT_CLEAR) {
                k->bg = (bg | OPAQUE) & LT_U32_MASK;
                k->ch = ' ';
                k->attr = 0;
            }
            if (bits != 0) {
                if (!(flags & LT_PLOT_CLEAR) && k->ch >= 0x2800UL && k->ch <= 0x28FFUL) {
                    bits = (lt_u8)(bits | (lt_u8)(k->ch - 0x2800UL));
                }
                k->ch = 0x2800UL + (lt_u32)bits;
                k->fg = (fg | OPAQUE) & LT_U32_MASK;
            }
            mark_cell(g, cx0 + cx, cy0 + cy);
        }
    }
    lt_free(m);
}

/* ---- thin lines as braille dots ------------------------------------------ */
static void braille_put(lt_gfx *g, int dx, int dy, lt_color c)
{
    int cx = fdiv(dx, 2), cy = fdiv(dy, 4), bx = dx - cx * 2, by = dy - cy * 4;
    lt_cell *k = cell_ptr(g, cx, cy);
    lt_u32 bits;
    if (k == NULL) { return; }
    if (k->ch >= 0x2800UL && k->ch <= 0x28FFUL) { bits = (lt_u32)(k->ch - 0x2800UL); }
    else if (k->ch == ' ' || k->ch == 0 || (k->ch >= 0x2500UL && k->ch <= 0x257FUL)) { bits = 0; }
    else { return; }
    bits |= (lt_u32)braille_bit[bx][by];
    k->ch = 0x2800UL + bits;
    k->fg = (c | OPAQUE) & LT_U32_MASK;
    mark_cell(g, cx, cy);
}

void lt_cell_braille_line(lt_gfx *g, double x0, double y0, double x1, double y1, lt_color c)
{
    int ax = (int)(x0 / 4.0 - (x0 < 0.0 ? 1.0 : 0.0)), ay = (int)(y0 / 4.0 - (y0 < 0.0 ? 1.0 : 0.0));
    int bx = (int)(x1 / 4.0 - (x1 < 0.0 ? 1.0 : 0.0)), by = (int)(y1 / 4.0 - (y1 < 0.0 ? 1.0 : 0.0));
    int dx = bx > ax ? bx - ax : ax - bx, dy = by > ay ? by - ay : ay - by;
    int sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1, err = dx - dy, guard = 0;
    if (LT_COLOR_A(c) == 0) { return; }
    for (;;) {
        braille_put(g, ax, ay, c);
        if ((ax == bx && ay == by) || ++guard > 4000) { break; }
        {
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; ax += sx; }
            if (e2 < dx) { err += dx; ay += sy; }
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
