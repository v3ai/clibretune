/* gfx.c - software rasterizer: pixel canvas and character-cell back ends.
 *
 * Coordinate conventions
 *   - All drawing coordinates are translated by (g->ox, g->oy).
 *   - g->clip and g->dirty are in absolute surface coordinates (NOT
 *     translated); set_clip/push_clip take absolute coordinates too.
 *   - Pixel centers are at (x+0.5, y+0.5); anti-aliasing is distance based.
 * Cell mode: a cell is 8x16 pixel units; a cell is "covered" by a shape when
 * its center is inside the shape and inside the clip.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>
#include "gui.h"
#include "gfx_priv.h"

/* vector font data from font_vec.c (generated) */
extern const lt_u8 lt_vfont_adv[96];
extern const lt_u16 lt_vfont_off[97];
extern const lt_s8 lt_vfont_data[];

#define CELL_W 8
#define CELL_H 16
#define DIV255(x) ((((x) + 128) + (((x) + 128) >> 8)) >> 8)
#define OPAQUE_MASK 0xFF000000UL
#define TWO_PI (2.0 * LT_PI)

/* ------------------------------------------------------------------ */
/* rect helpers                                                        */
lt_rect lt_rect_make(int x, int y, int w, int h)
{
    lt_rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

lt_rect lt_rect_intersect(lt_rect a, lt_rect b)
{
    lt_rect r;
    int x0 = LT_MAX(a.x, b.x), y0 = LT_MAX(a.y, b.y);
    int x1 = LT_MIN(a.x + a.w, b.x + b.w), y1 = LT_MIN(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) {
        r.x = x0; r.y = y0; r.w = 0; r.h = 0;
    } else {
        r.x = x0; r.y = y0; r.w = x1 - x0; r.h = y1 - y0;
    }
    return r;
}

lt_bool lt_rect_contains(lt_rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

lt_rect lt_rect_inset(lt_rect r, int dx, int dy)
{
    r.x += dx; r.y += dy; r.w -= 2 * dx; r.h -= 2 * dy;
    if (r.w < 0) { r.w = 0; }
    if (r.h < 0) { r.h = 0; }
    return r;
}

/* ------------------------------------------------------------------ */
/* canvas / cells storage                                              */
void lt_canvas_init(lt_canvas *c, int w, int h)
{
    if (w < 1) { w = 1; }
    if (h < 1) { h = 1; }
    c->w = w; c->h = h; c->pitch = w;
    c->px = (lt_u32 *)lt_calloc((size_t)w * (size_t)h, sizeof(lt_u32));
}

void lt_canvas_resize(lt_canvas *c, int w, int h)
{
    if (c->px != NULL && c->w == w && c->h == h) {
        return;
    }
    lt_canvas_free(c);
    lt_canvas_init(c, w, h);
}

void lt_canvas_free(lt_canvas *c)
{
    if (c->px != NULL) {
        lt_free(c->px);
    }
    c->px = NULL;
    c->w = c->h = c->pitch = 0;
}

void lt_cells_init(lt_cells *g, int cols, int rows)
{
    int i, n;
    if (cols < 1) { cols = 1; }
    if (rows < 1) { rows = 1; }
    g->cols = cols; g->rows = rows;
    n = cols * rows;
    g->cells = (lt_cell *)lt_calloc((size_t)n, sizeof(lt_cell));
    for (i = 0; i < n; i++) {
        g->cells[i].ch = ' ';
        g->cells[i].fg = 0xFFC0C0C0UL;
        g->cells[i].bg = 0xFF000000UL;
        g->cells[i].attr = 0;
    }
}

void lt_cells_resize(lt_cells *g, int cols, int rows)
{
    if (g->cells != NULL && g->cols == cols && g->rows == rows) {
        return;
    }
    lt_cells_free(g);
    lt_cells_init(g, cols, rows);
}

void lt_cells_free(lt_cells *g)
{
    if (g->cells != NULL) {
        lt_free(g->cells);
    }
    g->cells = NULL;
    g->cols = g->rows = 0;
}

/* ------------------------------------------------------------------ */
/* context                                                             */
void lt_gfx_init_canvas(lt_gfx *g, lt_canvas *c)
{
    g->canvas = c;
    g->cells = NULL;
    g->cell_w = 1; g->cell_h = 1;
    g->ox = g->oy = 0;
    g->clip = lt_rect_make(0, 0, c->w, c->h);
    g->dirty = lt_rect_make(0, 0, 0, 0);
    g->bitmap_text = 0;
}

void lt_gfx_init_cells(lt_gfx *g, lt_cells *cells)
{
    g->canvas = NULL;
    g->cells = cells;
    g->cell_w = CELL_W; g->cell_h = CELL_H;
    g->ox = g->oy = 0;
    g->clip = lt_rect_make(0, 0, cells->cols * CELL_W, cells->rows * CELL_H);
    g->dirty = lt_rect_make(0, 0, 0, 0);
    g->bitmap_text = 0;
}

lt_bool lt_gfx_cell_mode(const lt_gfx *g)
{
    return g->cells != NULL;
}

int lt_gfx_width(const lt_gfx *g)
{
    if (g->cells != NULL) { return g->cells->cols * CELL_W; }
    return g->canvas != NULL ? g->canvas->w : 0;
}

int lt_gfx_height(const lt_gfx *g)
{
    if (g->cells != NULL) { return g->cells->rows * CELL_H; }
    return g->canvas != NULL ? g->canvas->h : 0;
}

void lt_gfx_set_clip(lt_gfx *g, lt_rect r)
{
    g->clip = lt_rect_intersect(r, lt_rect_make(0, 0, lt_gfx_width(g), lt_gfx_height(g)));
}

lt_rect lt_gfx_push_clip(lt_gfx *g, lt_rect r)
{
    lt_rect prev = g->clip;
    g->clip = lt_rect_intersect(prev, r);
    return prev;
}

void lt_gfx_pop_clip(lt_gfx *g, lt_rect prev)
{
    g->clip = prev;
}

void lt_gfx_reset_dirty(lt_gfx *g)
{
    g->dirty = lt_rect_make(0, 0, 0, 0);
}

static void mark(lt_gfx *g, int x, int y, int w, int h)
{
    int x1, y1, dx1, dy1;
    if (w <= 0 || h <= 0) { return; }
    if (g->dirty.w <= 0 || g->dirty.h <= 0) {
        g->dirty = lt_rect_make(x, y, w, h);
        return;
    }
    x1 = x + w; y1 = y + h;
    dx1 = g->dirty.x + g->dirty.w; dy1 = g->dirty.y + g->dirty.h;
    if (x < g->dirty.x) { g->dirty.x = x; }
    if (y < g->dirty.y) { g->dirty.y = y; }
    if (x1 > dx1) { dx1 = x1; }
    if (y1 > dy1) { dy1 = y1; }
    g->dirty.w = dx1 - g->dirty.x;
    g->dirty.h = dy1 - g->dirty.y;
}

/* mark a pixel rectangle (absolute) clipped to the clip */
static void mark_clipped(lt_gfx *g, int x, int y, int w, int h)
{
    lt_rect r = lt_rect_intersect(lt_rect_make(x, y, w, h), g->clip);
    mark(g, r.x, r.y, r.w, r.h);
}

/* ------------------------------------------------------------------ */
/* small numeric helpers                                               */
static int ifloor(double v)
{
    int i;
    if (v > 1.0e8) { v = 1.0e8; }
    if (v < -1.0e8) { v = -1.0e8; }
    i = (int)v;
    return ((double)i > v) ? i - 1 : i;
}

static int iceil(double v)
{
    return ifloor(v) + ((double)ifloor(v) < v ? 1 : 0);
}

static int cdiv_floor(int a, int b)
{
    int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/* clamp a cell index range to the grid */
static int cclamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static double clamp01(double v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

static int cov255(double c)
{
    int v = (int)(c * 255.0 + 0.5);
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static lt_color blend_color(lt_color dst, lt_color src, int a)
{
    int ia, r, gg, b;
    if (a >= 255) { return (src | OPAQUE_MASK) & LT_U32_MASK; }
    if (a <= 0) { return dst; }
    ia = 255 - a;
    r = LT_COLOR_R(src) * a + LT_COLOR_R(dst) * ia;
    gg = LT_COLOR_G(src) * a + LT_COLOR_G(dst) * ia;
    b = LT_COLOR_B(src) * a + LT_COLOR_B(dst) * ia;
    return LT_RGB(DIV255(r), DIV255(gg), DIV255(b));
}

/* ------------------------------------------------------------------ */
/* pixel span primitive: horizontal run with constant coverage          */
static void put_span(lt_gfx *g, int x, int y, int w, lt_color col, int cov)
{
    lt_canvas *c = g->canvas;
    int x0, y0, x1, y1, a, ia, sr, sg, sb, i;
    lt_u32 *p;
    lt_u32 d;

    x0 = LT_MAX(g->clip.x, 0); y0 = LT_MAX(g->clip.y, 0);
    x1 = LT_MIN(g->clip.x + g->clip.w, c->w); y1 = LT_MIN(g->clip.y + g->clip.h, c->h);
    if (y < y0 || y >= y1) { return; }
    if (x < x0) { w -= x0 - x; x = x0; }
    if (x + w > x1) { w = x1 - x; }
    if (w <= 0) { return; }
    a = LT_COLOR_A(col);
    if (cov < 255) { a = DIV255(a * cov); }
    if (a <= 0) { return; }
    p = c->px + (size_t)y * (size_t)c->pitch + x;
    if (a >= 255) {
        lt_u32 v = (col | OPAQUE_MASK) & LT_U32_MASK;
        for (i = 0; i < w; i++) { p[i] = v; }
        return;
    }
    ia = 255 - a;
    sr = LT_COLOR_R(col) * a; sg = LT_COLOR_G(col) * a; sb = LT_COLOR_B(col) * a;
    for (i = 0; i < w; i++) {
        int r, gg, b;
        d = p[i];
        r = sr + (int)((d >> 16) & 0xFF) * ia;
        gg = sg + (int)((d >> 8) & 0xFF) * ia;
        b = sb + (int)(d & 0xFF) * ia;
        p[i] = (lt_u32)(0xFF000000UL | ((lt_u32)DIV255(r) << 16) |
                        ((lt_u32)DIV255(gg) << 8) | (lt_u32)DIV255(b));
    }
}

/* ------------------------------------------------------------------ */
/* cell primitives                                                     */
static lt_cell *cell_at(lt_gfx *g, int cx, int cy)
{
    if (cx < 0 || cy < 0 || cx >= g->cells->cols || cy >= g->cells->rows) { return NULL; }
    return &g->cells->cells[cy * g->cells->cols + cx];
}

/* returns the cell if it exists and its center is inside the clip */
static lt_cell *cell_vis(lt_gfx *g, int cx, int cy)
{
    if (!lt_rect_contains(g->clip, cx * CELL_W + CELL_W / 2, cy * CELL_H + CELL_H / 2)) {
        return NULL;
    }
    return cell_at(g, cx, cy);
}

static void cell_dirty(lt_gfx *g, int cx, int cy)
{
    mark(g, cx * CELL_W, cy * CELL_H, CELL_W, CELL_H);
}

/* set background (blend if translucent); clear glyph when opaque */
static void cell_bg(lt_gfx *g, int cx, int cy, lt_color c)
{
    lt_cell *k = cell_vis(g, cx, cy);
    int a = LT_COLOR_A(c);
    if (k == NULL || a == 0) { return; }
    if (a >= 255) {
        k->bg = (c | OPAQUE_MASK) & LT_U32_MASK;
        k->ch = ' ';
        k->attr = 0;
    } else {
        k->bg = blend_color(k->bg, c, a);
    }
    cell_dirty(g, cx, cy);
}

static void cell_ch(lt_gfx *g, int cx, int cy, lt_u32 ch, lt_color fg)
{
    lt_cell *k = cell_vis(g, cx, cy);
    if (k == NULL || LT_COLOR_A(fg) == 0) { return; }
    k->ch = ch;
    k->fg = (fg | OPAQUE_MASK) & LT_U32_MASK;
    cell_dirty(g, cx, cy);
}

/* put a char in the cell containing pixel point (px,py) (already translated) */
static void cell_put_at(lt_gfx *g, double px, double py, lt_u32 ch, lt_color fg)
{
    cell_ch(g, ifloor(px / CELL_W), ifloor(py / CELL_H), ch, fg);
}

typedef int (*inside_fn)(const void *ctx, double x, double y);

/* fill bg of cells whose center satisfies inside() within pixel bbox */
static void cell_fill_shape(lt_gfx *g, double bx0, double by0, double bx1, double by1,
                            inside_fn fn, const void *ctx, lt_color c)
{
    int cx0 = ifloor(bx0 / CELL_W), cx1 = ifloor(bx1 / CELL_W);
    int cy0 = ifloor(by0 / CELL_H), cy1 = ifloor(by1 / CELL_H);
    int cx, cy;
    if (cx0 < 0) { cx0 = 0; }
    if (cy0 < 0) { cy0 = 0; }
    if (cx1 >= g->cells->cols) { cx1 = g->cells->cols - 1; }
    if (cy1 >= g->cells->rows) { cy1 = g->cells->rows - 1; }
    for (cy = cy0; cy <= cy1; cy++) {
        for (cx = cx0; cx <= cx1; cx++) {
            if (fn(ctx, cx * CELL_W + CELL_W / 2.0, cy * CELL_H + CELL_H / 2.0)) {
                cell_bg(g, cx, cy, c);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* clear / rect                                                        */
void lt_gfx_clear(lt_gfx *g, lt_color c)
{
    if (g->cells != NULL) {
        int i, n = g->cells->cols * g->cells->rows;
        for (i = 0; i < n; i++) {
            lt_cell *k = &g->cells->cells[i];
            k->ch = ' ';
            k->bg = (c | OPAQUE_MASK) & LT_U32_MASK;
            k->attr = 0;
        }
        mark(g, 0, 0, lt_gfx_width(g), lt_gfx_height(g));
        return;
    }
    if (g->canvas == NULL) { return; }
    {
        lt_rect r = lt_rect_intersect(g->clip, lt_rect_make(0, 0, g->canvas->w, g->canvas->h));
        int y;
        for (y = r.y; y < r.y + r.h; y++) {
            put_span(g, r.x, y, r.w, (c | OPAQUE_MASK) & LT_U32_MASK, 255);
        }
        mark(g, r.x, r.y, r.w, r.h);
    }
}

typedef struct rect_ctx { lt_rect r; } rect_ctx;

static void fill_rect_abs(lt_gfx *g, lt_rect r, lt_color c)
{
    int y, cx, cy, cx0, cx1, cy0, cy1;
    if (g->cells != NULL) {
        if (r.w <= 0 || r.h <= 0) { return; }
        cx0 = cdiv_floor(r.x, CELL_W); cx1 = cdiv_floor(r.x + r.w, CELL_W);
        cy0 = cdiv_floor(r.y, CELL_H); cy1 = cdiv_floor(r.y + r.h, CELL_H);
        cx0 = cclamp(cx0, 0, g->cells->cols); cx1 = cclamp(cx1, 0, g->cells->cols);
        cy0 = cclamp(cy0, 0, g->cells->rows); cy1 = cclamp(cy1, 0, g->cells->rows);
        for (cy = cy0; cy <= cy1; cy++) {
            for (cx = cx0; cx <= cx1; cx++) {
                if (lt_rect_contains(r, cx * CELL_W + CELL_W / 2, cy * CELL_H + CELL_H / 2)) {
                    cell_bg(g, cx, cy, c);
                }
            }
        }
        return;
    }
    r = lt_rect_intersect(r, g->clip);
    if (r.w <= 0 || r.h <= 0 || g->canvas == NULL) { return; }
    for (y = r.y; y < r.y + r.h; y++) {
        put_span(g, r.x, y, r.w, c, 255);
    }
    mark_clipped(g, r.x, r.y, r.w, r.h);
}

void lt_gfx_fill_rect(lt_gfx *g, lt_rect r, lt_color c)
{
    r.x += g->ox; r.y += g->oy;
    fill_rect_abs(g, r, c);
}

void lt_gfx_hline(lt_gfx *g, int x0, int x1, int y, lt_color c)
{
    int cx, cx0, cx1, cy;
    if (x0 > x1) { cx = x0; x0 = x1; x1 = cx; }
    x0 += g->ox; x1 += g->ox; y += g->oy;
    if (g->cells == NULL) {
        fill_rect_abs(g, lt_rect_make(x0, y, x1 - x0 + 1, 1), c);
        return;
    }
    cy = cdiv_floor(y, CELL_H);
    cx0 = cclamp(cdiv_floor(x0, CELL_W), 0, g->cells->cols);
    cx1 = cclamp(cdiv_floor(x1, CELL_W), -1, g->cells->cols);
    for (cx = cx0; cx <= cx1; cx++) {
        lt_cell *k = cell_at(g, cx, cy);
        lt_u32 ch = 0x2500;
        if (k != NULL && k->ch == 0x2502) { ch = 0x253C; }
        cell_ch(g, cx, cy, ch, c);
    }
}

void lt_gfx_vline(lt_gfx *g, int x, int y0, int y1, lt_color c)
{
    int cy, cy0, cy1, cx;
    if (y0 > y1) { cy = y0; y0 = y1; y1 = cy; }
    x += g->ox; y0 += g->oy; y1 += g->oy;
    if (g->cells == NULL) {
        fill_rect_abs(g, lt_rect_make(x, y0, 1, y1 - y0 + 1), c);
        return;
    }
    cx = cdiv_floor(x, CELL_W);
    cy0 = cclamp(cdiv_floor(y0, CELL_H), 0, g->cells->rows);
    cy1 = cclamp(cdiv_floor(y1, CELL_H), -1, g->cells->rows);
    for (cy = cy0; cy <= cy1; cy++) {
        lt_cell *k = cell_at(g, cx, cy);
        lt_u32 ch = 0x2502;
        if (k != NULL && k->ch == 0x2500) { ch = 0x253C; }
        cell_ch(g, cx, cy, ch, c);
    }
}

void lt_gfx_rect(lt_gfx *g, lt_rect r, lt_color c)
{
    if (r.w <= 0 || r.h <= 0) { return; }
    if (g->cells == NULL) {
        lt_gfx_fill_rect(g, lt_rect_make(r.x, r.y, r.w, 1), c);
        if (r.h > 1) {
            lt_gfx_fill_rect(g, lt_rect_make(r.x, r.y + r.h - 1, r.w, 1), c);
        }
        if (r.h > 2) {
            lt_gfx_fill_rect(g, lt_rect_make(r.x, r.y + 1, 1, r.h - 2), c);
            if (r.w > 1) {
                lt_gfx_fill_rect(g, lt_rect_make(r.x + r.w - 1, r.y + 1, 1, r.h - 2), c);
            }
        }
        return;
    }
    {
        int x0 = cdiv_floor(r.x + g->ox, CELL_W), x1 = cdiv_floor(r.x + r.w - 1 + g->ox, CELL_W);
        int y0 = cdiv_floor(r.y + g->oy, CELL_H), y1 = cdiv_floor(r.y + r.h - 1 + g->oy, CELL_H);
        int x, y;
        int xa = cclamp(x0, 0, g->cells->cols), xb = cclamp(x1, -1, g->cells->cols);
        int ya = cclamp(y0, 0, g->cells->rows), yb = cclamp(y1, -1, g->cells->rows);
        for (x = xa; x <= xb; x++) {
            cell_ch(g, x, y0, 0x2500, c);
            cell_ch(g, x, y1, 0x2500, c);
        }
        for (y = ya; y <= yb; y++) {
            cell_ch(g, x0, y, 0x2502, c);
            cell_ch(g, x1, y, 0x2502, c);
        }
        if (x0 != x1 && y0 != y1) {
            cell_ch(g, x0, y0, 0x250C, c);
            cell_ch(g, x1, y0, 0x2510, c);
            cell_ch(g, x0, y1, 0x2514, c);
            cell_ch(g, x1, y1, 0x2518, c);
        }
    }
}

void lt_gfx_fill_round_rect(lt_gfx *g, lt_rect r, int radius, lt_color c)
{
    int y, i, rad, x0, rx;
    double rr;
    if (r.w <= 0 || r.h <= 0) { return; }
    rad = radius;
    if (rad > r.w / 2) { rad = r.w / 2; }
    if (rad > r.h / 2) { rad = r.h / 2; }
    if (rad <= 1 || g->cells != NULL) {
        lt_gfx_fill_rect(g, r, c);
        return;
    }
    r.x += g->ox; r.y += g->oy;
    rr = (double)rad;
    for (y = LT_MAX(0, g->clip.y - r.y); y < r.h && r.y + y < g->clip.y + g->clip.h; y++) {
        double dy = -1.0;
        if (y < rad) {
            dy = rr - (y + 0.5);
        } else if (y >= r.h - rad) {
            dy = (y + 0.5) - (r.h - rr);
        }
        if (dy < 0.0) {
            put_span(g, r.x, r.y + y, r.w, c, 255);
            continue;
        }
        x0 = r.x;
        rx = r.x + r.w;
        put_span(g, x0 + rad, r.y + y, r.w - 2 * rad, c, 255);
        for (i = 0; i < rad; i++) {
            double dx = rr - (i + 0.5);
            double d = sqrt(dx * dx + dy * dy);
            int cv = cov255(clamp01(rr + 0.5 - d));
            if (cv > 0) {
                put_span(g, x0 + i, r.y + y, 1, c, cv);
                put_span(g, rx - 1 - i, r.y + y, 1, c, cv);
            }
        }
    }
    mark_clipped(g, r.x, r.y, r.w, r.h);
}

void lt_gfx_fill_rect_vgrad(lt_gfx *g, lt_rect r, lt_color top, lt_color bottom)
{
    int i, a, rr, gg, bb;
    lt_rect v;
    if (r.w <= 0 || r.h <= 0) { return; }
    if (g->cells != NULL) {
        int cx, cy;
        int cy0, cy1, cx0, cx1;
        r.x += g->ox; r.y += g->oy;
        cy0 = cclamp(cdiv_floor(r.y, CELL_H), 0, g->cells->rows);
        cy1 = cclamp(cdiv_floor(r.y + r.h, CELL_H), 0, g->cells->rows);
        cx0 = cclamp(cdiv_floor(r.x, CELL_W), 0, g->cells->cols);
        cx1 = cclamp(cdiv_floor(r.x + r.w, CELL_W), 0, g->cells->cols);
        for (cy = cy0; cy <= cy1; cy++) {
            for (cx = cx0; cx <= cx1; cx++) {
                int py = cy * CELL_H + CELL_H / 2;
                if (lt_rect_contains(r, cx * CELL_W + CELL_W / 2, py)) {
                    double t = (double)(py - r.y) / (double)r.h;
                    int ta = (int)(LT_COLOR_A(top) + (LT_COLOR_A(bottom) - LT_COLOR_A(top)) * t);
                    lt_color col = LT_RGBA(
                        (int)(LT_COLOR_R(top) + (LT_COLOR_R(bottom) - LT_COLOR_R(top)) * t),
                        (int)(LT_COLOR_G(top) + (LT_COLOR_G(bottom) - LT_COLOR_G(top)) * t),
                        (int)(LT_COLOR_B(top) + (LT_COLOR_B(bottom) - LT_COLOR_B(top)) * t), ta);
                    cell_bg(g, cx, cy, col);
                }
            }
        }
        return;
    }
    r.x += g->ox; r.y += g->oy;
    v = lt_rect_intersect(r, g->clip);
    for (i = LT_MAX(0, v.y - r.y); i < r.h && r.y + i < v.y + v.h; i++) {
        int y = r.y + i;
        double t = r.h > 1 ? (double)i / (double)(r.h - 1) : 0.0;
        a = (int)(LT_COLOR_A(top) + (LT_COLOR_A(bottom) - LT_COLOR_A(top)) * t + 0.5);
        rr = (int)(LT_COLOR_R(top) + (LT_COLOR_R(bottom) - LT_COLOR_R(top)) * t + 0.5);
        gg = (int)(LT_COLOR_G(top) + (LT_COLOR_G(bottom) - LT_COLOR_G(top)) * t + 0.5);
        bb = (int)(LT_COLOR_B(top) + (LT_COLOR_B(bottom) - LT_COLOR_B(top)) * t + 0.5);
        put_span(g, r.x, y, r.w, LT_RGBA(rr, gg, bb, a), 255);
    }
    mark_clipped(g, r.x, r.y, r.w, r.h);
}

/* ------------------------------------------------------------------ */
/* anti-aliased line (capsule)                                         */
static char line_char(double dx, double dy, double width)
{
    double ax = fabs(dx), ay = fabs(dy);
    if (width >= 3.0) { return '#'; }
    if (ay < ax * 0.4142) { return '-'; }
    if (ax < ay * 0.4142) { return '|'; }
    return (dx * dy > 0.0) ? '\\' : '/';
}

void lt_gfx_line(lt_gfx *g, double x0, double y0, double x1, double y1, double width,
                 lt_color c)
{
    double dx, dy, len2, len, rad, rr, wcap;
    int py, ya, yb;
    x0 += g->ox; x1 += g->ox; y0 += g->oy; y1 += g->oy;
    if (width < 0.0) { width = 0.0; }
    dx = x1 - x0; dy = y1 - y0;
    len2 = dx * dx + dy * dy;
    len = sqrt(len2);
    if (g->cells != NULL) {
        char ch = line_char(dx, dy, width);
        int n, i;
        if (len > 1.0e6) { return; }
        n = (int)(len / 4.0) + 1;
        for (i = 0; i <= n; i++) {
            double t = (double)i / (double)n;
            cell_put_at(g, x0 + dx * t, y0 + dy * t, (lt_u32)(unsigned char)ch, c);
        }
        return;
    }
    rad = width / 2.0;
    rr = rad + 0.5;
    wcap = width < 1.0 ? width : 1.0;
    ya = ifloor((y0 < y1 ? y0 : y1) - rr);
    yb = iceil((y0 > y1 ? y0 : y1) + rr);
    if (ya < g->clip.y) { ya = g->clip.y; }
    if (yb > g->clip.y + g->clip.h) { yb = g->clip.y + g->clip.h; }
    for (py = ya; py < yb; py++) {
        double yc = py + 0.5, tlo = 0.0, thi = 1.0, xa, xb, t;
        int pxa, pxb, px;
        if (fabs(dy) > 1.0e-12) {
            double t0 = (yc - rr - y0) / dy, t1 = (yc + rr - y0) / dy;
            if (t0 > t1) { t = t0; t0 = t1; t1 = t; }
            tlo = t0 < 0.0 ? 0.0 : t0;
            thi = t1 > 1.0 ? 1.0 : t1;
            if (tlo > thi) { continue; }
        } else if (fabs(yc - y0) > rr) {
            continue;
        }
        xa = x0 + dx * tlo; xb = x0 + dx * thi;
        if (xa > xb) { t = xa; xa = xb; xb = t; }
        pxa = ifloor(xa - rr); pxb = iceil(xb + rr);
        if (pxa < g->clip.x) { pxa = g->clip.x; }
        if (pxb > g->clip.x + g->clip.w) { pxb = g->clip.x + g->clip.w; }
        for (px = pxa; px < pxb; px++) {
            double qx = px + 0.5 - x0, qy = yc - y0, d, cv;
            double tt = len2 > 0.0 ? (qx * dx + qy * dy) / len2 : 0.0;
            if (tt < 0.0) { tt = 0.0; }
            if (tt > 1.0) { tt = 1.0; }
            qx -= dx * tt; qy -= dy * tt;
            d = sqrt(qx * qx + qy * qy);
            cv = clamp01(rr - d);
            if (cv > wcap) { cv = wcap; }
            if (cv > 0.0) { put_span(g, px, py, 1, c, cov255(cv)); }
        }
    }
    mark_clipped(g, ifloor(LT_MIN(x0, x1) - rr), ifloor(LT_MIN(y0, y1) - rr),
                 iceil(fabs(dx) + 2.0 * rr) + 1, iceil(fabs(dy) + 2.0 * rr) + 1);
}

/* ------------------------------------------------------------------ */
/* annulus / sector engine: circles, rings, arcs, annular sectors      */
typedef struct arc_ctx {
    double cx, cy, rin, rout, a0, sweep;
    int full;
    int bx0, by0, bx1, by1; /* pixel bounding box of the sector (inclusive) */
} arc_ctx;

static double arc_cov(const arc_ctx *a, double x, double y)
{
    double dx = x - a->cx, dy = y - a->cy, d = sqrt(dx * dx + dy * dy), cv, th, m, ac;
    cv = clamp01(a->rout + 0.5 - d);
    if (a->rin > 0.0) {
        cv = cv + clamp01(d - a->rin + 0.5) - 1.0;
        if (cv < 0.0) { cv = 0.0; }
    }
    if (cv <= 0.0 || a->full) { return cv; }
    th = atan2(dy, dx) - a->a0;
    th = fmod(th, TWO_PI);
    if (th < 0.0) { th += TWO_PI; }
    if (th <= a->sweep) {
        m = LT_MIN(th, a->sweep - th) * d;
        ac = clamp01(m + 0.5);
    } else {
        m = LT_MIN(th - a->sweep, TWO_PI - th) * d;
        ac = clamp01(0.5 - m);
    }
    return cv * ac;
}

static void arc_pixels(lt_gfx *g, const arc_ctx *a, int py, int xa, int xb, lt_color c)
{
    int px;
    double yc = py + 0.5;
    if (xa < g->clip.x) { xa = g->clip.x; }
    if (xb >= g->clip.x + g->clip.w) { xb = g->clip.x + g->clip.w - 1; }
    if (xa < a->bx0) { xa = a->bx0; }
    if (xb > a->bx1) { xb = a->bx1; }
    for (px = xa; px <= xb; px++) {
        double cv = arc_cov(a, px + 0.5, yc);
        if (cv > 0.0) { put_span(g, px, py, 1, c, cov255(cv)); }
    }
}

static void arc_draw(lt_gfx *g, const arc_ctx *a, lt_color c)
{
    int py, ya, yb;
    double ro = a->rout + 1.0, ri = a->rin - 1.0;
    ya = ifloor(a->cy - ro); yb = iceil(a->cy + ro);
    if (ya < a->by0) { ya = a->by0; }
    if (yb > a->by1 + 1) { yb = a->by1 + 1; }
    if (ya < g->clip.y) { ya = g->clip.y; }
    if (yb > g->clip.y + g->clip.h) { yb = g->clip.y + g->clip.h; }
    for (py = ya; py < yb; py++) {
        double dy = py + 0.5 - a->cy, o2 = ro * ro - dy * dy, xo, xi;
        int xl, xr;
        if (o2 < 0.0) { continue; }
        xo = sqrt(o2);
        xl = ifloor(a->cx - xo); xr = iceil(a->cx + xo);
        if (a->rin <= 0.0 && a->full) {
            double s2 = (a->rout - 0.5) * (a->rout - 0.5) - dy * dy;
            if (s2 > 0.0) {
                double xs = sqrt(s2);
                int sl = iceil(a->cx - xs - 0.5), sr = ifloor(a->cx + xs - 0.5);
                if (sl <= sr) {
                    put_span(g, sl, py, sr - sl + 1, c, 255);
                    arc_pixels(g, a, py, xl, sl - 1, c);
                    arc_pixels(g, a, py, sr + 1, xr, c);
                    continue;
                }
            }
            arc_pixels(g, a, py, xl, xr, c);
        } else if (ri > 0.0 && ri * ri > dy * dy) {
            xi = sqrt(ri * ri - dy * dy);
            arc_pixels(g, a, py, xl, ifloor(a->cx - xi), c);
            arc_pixels(g, a, py, iceil(a->cx + xi), xr, c);
        } else {
            arc_pixels(g, a, py, xl, xr, c);
        }
    }
    mark_clipped(g, ifloor(a->cx - ro), ifloor(a->cy - ro), iceil(2.0 * ro) + 1,
                 iceil(2.0 * ro) + 1);
}

/* Bounding box of the annular sector: endpoints at both radii plus every
 * axis extreme (0, 90, 180, 270 deg) the sweep crosses, padded for AA. */
static void arc_bbox(arc_ctx *a)
{
    double x0, y0, x1, y1, px, py, ang;
    int i, k;
    double r[2];
    if (a->full) {
        a->bx0 = ifloor(a->cx - a->rout - 2.0); a->bx1 = iceil(a->cx + a->rout + 2.0);
        a->by0 = ifloor(a->cy - a->rout - 2.0); a->by1 = iceil(a->cy + a->rout + 2.0);
        return;
    }
    r[0] = a->rin; r[1] = a->rout;
    x0 = x1 = a->cx + a->rout * cos(a->a0);
    y0 = y1 = a->cy + a->rout * sin(a->a0);
    for (i = 0; i < 2; i++) {
        for (k = 0; k < 2; k++) {
            ang = k ? a->a0 + a->sweep : a->a0;
            px = a->cx + r[i] * cos(ang); py = a->cy + r[i] * sin(ang);
            if (px < x0) { x0 = px; } if (px > x1) { x1 = px; }
            if (py < y0) { y0 = py; } if (py > y1) { y1 = py; }
        }
    }
    /* axis extremes crossed by the sweep */
    ang = ceil(a->a0 / (LT_PI / 2.0)) * (LT_PI / 2.0);
    for (k = 0; k < 5 && ang <= a->a0 + a->sweep; k++, ang += LT_PI / 2.0) {
        px = a->cx + a->rout * cos(ang); py = a->cy + a->rout * sin(ang);
        if (px < x0) { x0 = px; } if (px > x1) { x1 = px; }
        if (py < y0) { y0 = py; } if (py > y1) { y1 = py; }
    }
    /* pad for anti-aliased edges (radial and angular) */
    a->bx0 = ifloor(x0 - 2.0); a->bx1 = iceil(x1 + 2.0);
    a->by0 = ifloor(y0 - 2.0); a->by1 = iceil(y1 + 2.0);
}

static void arc_setup(arc_ctx *a, double cx, double cy, double rin, double rout,
                      double a0, double a1)
{
    double t;
    if (a1 < a0) { t = a0; a0 = a1; a1 = t; }
    a->cx = cx; a->cy = cy;
    a->rin = rin < 0.0 ? 0.0 : rin;
    a->rout = rout < 0.0 ? 0.0 : rout;
    a->a0 = a0;
    a->sweep = a1 - a0;
    a->full = a->sweep >= TWO_PI - 1.0e-9;
    arc_bbox(a);
}

static int arc_inside(const void *ctx, double x, double y)
{
    const arc_ctx *a = (const arc_ctx *)ctx;
    double dx = x - a->cx, dy = y - a->cy, d = sqrt(dx * dx + dy * dy), th;
    if (d > a->rout || d < a->rin) { return 0; }
    if (a->full) { return 1; }
    th = fmod(atan2(dy, dx) - a->a0, TWO_PI);
    if (th < 0.0) { th += TWO_PI; }
    return th <= a->sweep;
}

/* mark cells along an arc with a glyph */
static void cell_arc_marks(lt_gfx *g, double cx, double cy, double r, double a0, double a1,
                           double width, lt_color c)
{
    double t, sweep;
    int n, i;
    lt_u32 ch = width >= 3.0 ? '#' : '*';
    if (a1 < a0) { t = a0; a0 = a1; a1 = t; }
    sweep = a1 - a0;
    if (sweep > TWO_PI) { sweep = TWO_PI; }
    if (r < 0.0 || r > 1.0e6) { return; }
    n = (int)(sweep * r / 3.0) + 2;
    for (i = 0; i <= n; i++) {
        double an = a0 + sweep * (double)i / (double)n;
        cell_put_at(g, cx + cos(an) * r, cy + sin(an) * r, ch, c);
    }
}

void lt_gfx_fill_circle(lt_gfx *g, double cx, double cy, double r, lt_color c)
{
    arc_ctx a;
    cx += g->ox; cy += g->oy;
    arc_setup(&a, cx, cy, 0.0, r, 0.0, TWO_PI);
    if (g->cells != NULL) {
        cell_fill_shape(g, cx - r, cy - r, cx + r, cy + r, arc_inside, &a, c);
        return;
    }
    arc_draw(g, &a, c);
}

void lt_gfx_circle(lt_gfx *g, double cx, double cy, double r, double width, lt_color c)
{
    arc_ctx a;
    cx += g->ox; cy += g->oy;
    if (g->cells != NULL) {
        cell_arc_marks(g, cx, cy, r, 0.0, TWO_PI, width, c);
        return;
    }
    arc_setup(&a, cx, cy, r - width / 2.0, r + width / 2.0, 0.0, TWO_PI);
    arc_draw(g, &a, c);
}

void lt_gfx_arc(lt_gfx *g, double cx, double cy, double r, double a0, double a1,
                double width, lt_color c)
{
    arc_ctx a;
    cx += g->ox; cy += g->oy;
    if (g->cells != NULL) {
        cell_arc_marks(g, cx, cy, r, a0, a1, width, c);
        return;
    }
    arc_setup(&a, cx, cy, r - width / 2.0, r + width / 2.0, a0, a1);
    arc_draw(g, &a, c);
}

void lt_gfx_fill_arc(lt_gfx *g, double cx, double cy, double r_in, double r_out,
                     double a0, double a1, lt_color c)
{
    arc_ctx a;
    cx += g->ox; cy += g->oy;
    arc_setup(&a, cx, cy, r_in, r_out, a0, a1);
    if (g->cells != NULL) {
        cell_fill_shape(g, cx - r_out, cy - r_out, cx + r_out, cy + r_out, arc_inside, &a, c);
        return;
    }
    arc_draw(g, &a, c);
}

/* ------------------------------------------------------------------ */
/* polygon                                                             */
typedef struct poly_ctx { const double *xy; int n; double ox, oy; } poly_ctx;

static int poly_inside(const void *ctx, double x, double y)
{
    const poly_ctx *p = (const poly_ctx *)ctx;
    int i, j, in = 0;
    for (i = 0, j = p->n - 1; i < p->n; j = i++) {
        double xi = p->xy[2 * i] + p->ox, yi = p->xy[2 * i + 1] + p->oy;
        double xj = p->xy[2 * j] + p->ox, yj = p->xy[2 * j + 1] + p->oy;
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) {
            in = !in;
        }
    }
    return in;
}

typedef struct edge { double x0, y0, y1, dxdy; } edge;

void lt_gfx_fill_poly(lt_gfx *g, const double *xy, int n, lt_color c)
{
    int i, ne = 0, py, ya, yb, cx0, cx1, width;
    double miny = 1e30, maxy = -1e30, minx = 1e30, maxx = -1e30;
    edge *edges;
    double *xs, *acc;
    if (n < 3) { return; }
    if (g->cells != NULL) {
        poly_ctx pc;
        pc.xy = xy; pc.n = n; pc.ox = g->ox; pc.oy = g->oy;
        for (i = 0; i < n; i++) {
            double x = xy[2 * i] + g->ox, y = xy[2 * i + 1] + g->oy;
            if (x < minx) { minx = x; }
            if (x > maxx) { maxx = x; }
            if (y < miny) { miny = y; }
            if (y > maxy) { maxy = y; }
        }
        cell_fill_shape(g, minx, miny, maxx, maxy, poly_inside, &pc, c);
        return;
    }
    edges = (edge *)lt_malloc((size_t)n * sizeof(edge));
    xs = (double *)lt_malloc((size_t)n * sizeof(double));
    for (i = 0; i < n; i++) {
        int j = (i + 1) % n;
        double ax = xy[2 * i] + g->ox, ay = xy[2 * i + 1] + g->oy;
        double bx = xy[2 * j] + g->ox, by = xy[2 * j + 1] + g->oy;
        if (ax < minx) { minx = ax; }
        if (ax > maxx) { maxx = ax; }
        if (ay < miny) { miny = ay; }
        if (ay > maxy) { maxy = ay; }
        if (ay == by) { continue; }
        if (ay < by) {
            edges[ne].x0 = ax; edges[ne].y0 = ay; edges[ne].y1 = by;
            edges[ne].dxdy = (bx - ax) / (by - ay);
        } else {
            edges[ne].x0 = bx; edges[ne].y0 = by; edges[ne].y1 = ay;
            edges[ne].dxdy = (ax - bx) / (ay - by);
        }
        ne++;
    }
    cx0 = LT_MAX(g->clip.x, ifloor(minx));
    cx1 = LT_MIN(g->clip.x + g->clip.w, iceil(maxx) + 1);
    ya = LT_MAX(g->clip.y, ifloor(miny));
    yb = LT_MIN(g->clip.y + g->clip.h, iceil(maxy) + 1);
    width = cx1 - cx0;
    if (ne == 0 || width <= 0 || yb <= ya) {
        lt_free(edges); lt_free(xs);
        return;
    }
    acc = (double *)lt_calloc((size_t)width + 2, sizeof(double));
    for (py = ya; py < yb; py++) {
        int k, lo = width, hi = -1, x;
        for (k = 0; k < 4; k++) {
            double sy = py + (k + 0.5) / 4.0;
            int m = 0, e, a, b;
            for (e = 0; e < ne; e++) {
                if (sy >= edges[e].y0 && sy < edges[e].y1) {
                    double xv = edges[e].x0 + (sy - edges[e].y0) * edges[e].dxdy;
                    int q = m++;
                    while (q > 0 && xs[q - 1] > xv) { xs[q] = xs[q - 1]; q--; }
                    xs[q] = xv;
                }
            }
            for (a = 0; a + 1 < m; a += 2) {
                double xa = xs[a] - cx0, xb = xs[a + 1] - cx0;
                int ia, ib;
                if (xa < 0.0) { xa = 0.0; }
                if (xb > (double)width) { xb = (double)width; }
                if (xb <= xa) { continue; }
                ia = ifloor(xa); ib = ifloor(xb);
                if (ib >= width) { ib = width - 1; xb = (double)width; }
                if (ia == ib) {
                    acc[ia] += (xb - xa) * 0.25;
                } else {
                    acc[ia] += (ia + 1 - xa) * 0.25;
                    for (b = ia + 1; b < ib; b++) { acc[b] += 0.25; }
                    acc[ib] += (xb - ib) * 0.25;
                }
                if (ia < lo) { lo = ia; }
                if (ib > hi) { hi = ib; }
            }
        }
        x = lo;
        while (x <= hi) {
            int cv = cov255(acc[x]);
            if (cv >= 255) {
                int s = x;
                while (x <= hi && cov255(acc[x]) >= 255) { acc[x] = 0.0; x++; }
                put_span(g, cx0 + s, py, x - s, c, 255);
                continue;
            }
            if (cv > 0) { put_span(g, cx0 + x, py, 1, c, cv); }
            acc[x] = 0.0;
            x++;
        }
    }
    mark_clipped(g, cx0, ya, width, yb - ya);
    lt_free(acc); lt_free(edges); lt_free(xs);
}

/* ------------------------------------------------------------------ */
/* blit                                                                */
void lt_gfx_blit(lt_gfx *g, int x, int y, const lt_u32 *px, int w, int h, int pitch)
{
    int j, i, ww;
    x += g->ox; y += g->oy;
    if (w <= 0 || h <= 0) { return; }
    if (g->cells != NULL) {
        int cx, cy;
        int cy0 = cclamp(cdiv_floor(y, CELL_H), 0, g->cells->rows);
        int cy1 = cclamp(cdiv_floor(y + h, CELL_H), 0, g->cells->rows);
        int cx0 = cclamp(cdiv_floor(x, CELL_W), 0, g->cells->cols);
        int cx1 = cclamp(cdiv_floor(x + w, CELL_W), 0, g->cells->cols);
        for (cy = cy0; cy <= cy1; cy++) {
            for (cx = cx0; cx <= cx1; cx++) {
                int sx = cx * CELL_W + CELL_W / 2 - x, sy = cy * CELL_H + CELL_H / 2 - y;
                if (sx >= 0 && sy >= 0 && sx < w && sy < h) {
                    cell_bg(g, cx, cy, px[(size_t)sy * (size_t)pitch + (size_t)sx]);
                }
            }
        }
        return;
    }
    for (j = LT_MAX(0, g->clip.y - y); j < h && y + j < g->clip.y + g->clip.h; j++) {
        const lt_u32 *row = px + (size_t)j * (size_t)pitch;
        i = LT_MAX(0, g->clip.x - x);
        if (w > g->clip.x + g->clip.w - x) { ww = g->clip.x + g->clip.w - x; } else { ww = w; }
        while (i < ww) {
            lt_u32 v = row[i];
            if (LT_COLOR_A(v) == 0) { i++; continue; }
            put_span(g, x + i, y + j, 1, v, 255);
            i++;
        }
    }
    mark_clipped(g, x, y, w, h);
}

/* ------------------------------------------------------------------ */
/* coverage mask blit (anti-aliased text and icons)                    */
void lt_gfx_blit_mask(lt_gfx *g, int x, int y, const lt_u8 *a, int w, int h, int pitch,
                      lt_color col, const lt_u8 *lut)
{
    lt_canvas *c = g->canvas;
    int x0, y0, x1, y1, j, i, ca, sr, sg, sb;
    if (c == NULL || a == NULL || w <= 0 || h <= 0) { return; }
    x += g->ox; y += g->oy;
    x0 = LT_MAX(LT_MAX(g->clip.x, 0), x);
    y0 = LT_MAX(LT_MAX(g->clip.y, 0), y);
    x1 = LT_MIN(LT_MIN(g->clip.x + g->clip.w, c->w), x + w);
    y1 = LT_MIN(LT_MIN(g->clip.y + g->clip.h, c->h), y + h);
    if (x1 <= x0 || y1 <= y0) { return; }
    ca = LT_COLOR_A(col);
    if (ca <= 0) { return; }
    sr = LT_COLOR_R(col); sg = LT_COLOR_G(col); sb = LT_COLOR_B(col);
    for (j = y0; j < y1; j++) {
        const lt_u8 *src = a + (size_t)(j - y) * (size_t)pitch + (size_t)(x0 - x);
        lt_u32 *dst = c->px + (size_t)j * (size_t)c->pitch + x0;
        for (i = 0; i < x1 - x0; i++) {
            int cv = src[i], al, ia, r, gg, b;
            lt_u32 d;
            if (cv == 0) { continue; }
            if (lut != NULL) { cv = lut[cv]; }
            al = ca >= 255 ? cv : DIV255(ca * cv);
            if (al <= 0) { continue; }
            if (al >= 255) {
                dst[i] = LT_RGB(sr, sg, sb);
                continue;
            }
            ia = 255 - al;
            d = dst[i];
            r = sr * al + (int)((d >> 16) & 0xFF) * ia;
            gg = sg * al + (int)((d >> 8) & 0xFF) * ia;
            b = sb * al + (int)(d & 0xFF) * ia;
            dst[i] = LT_RGB(DIV255(r), DIV255(gg), DIV255(b));
        }
    }
    mark(g, x0, y0, x1 - x0, y1 - y0);
}

/* ------------------------------------------------------------------ */
/* bitmap text                                                         */
int lt_gfx_text_width(const char *s, int scale)
{
    int n = 0;
    if (scale < 1) { scale = 1; }
    while (lt_utf8_next(&s) != 0) { n++; }
    return n * LT_FONT_W * scale;
}

int lt_gfx_text_height(int scale)
{
    if (scale < 1) { scale = 1; }
    return LT_FONT_H * scale;
}

int lt_gfx_text(lt_gfx *g, int x, int y, const char *s, lt_color c, int scale)
{
    int total = lt_gfx_text_width(s, scale), cw, ch, x0;
    lt_u32 cp;
    if (scale < 1) { scale = 1; }
    if (g->canvas != NULL && g->cells == NULL && !g->bitmap_text &&
        lt_gfx_text_cellfont(g, x, y, s, c, scale)) {
        return total;
    }
    x += g->ox; y += g->oy;
    cw = LT_FONT_W * scale; ch = LT_FONT_H * scale;
    x0 = x;
    if (g->cells != NULL) {
        int cx = cdiv_floor(x, CELL_W), cy = cdiv_floor(y, CELL_H);
        while ((cp = lt_utf8_next(&s)) != 0) {
            {
                lt_cell *k = cell_vis(g, cx, cy);
                if (k != NULL && LT_COLOR_A(c) != 0) {
                    k->ch = cp;
                    k->fg = (c | OPAQUE_MASK) & LT_U32_MASK;
                    cell_dirty(g, cx, cy);
                }
            }
            cx++;
        }
        return total;
    }
    if (y + ch <= g->clip.y || y >= g->clip.y + g->clip.h ||
        x + total <= g->clip.x || x >= g->clip.x + g->clip.w) {
        return total;
    }
    while ((cp = lt_utf8_next(&s)) != 0) {
        const lt_u8 *gl;
        int row;
        if (x >= g->clip.x + g->clip.w) { break; }
        if (x + cw <= g->clip.x) { x += cw; continue; }
        gl = lt_font_glyph(cp);
        for (row = 0; row < LT_FONT_H; row++) {
            unsigned bits = gl[row];
            int bit = 0, sy;
            if (bits == 0) { continue; }
            while (bit < 8) {
                int run;
                if (!(bits & (0x80u >> bit))) { bit++; continue; }
                run = 1;
                while (bit + run < 8 && (bits & (0x80u >> (bit + run)))) { run++; }
                for (sy = 0; sy < scale; sy++) {
                    put_span(g, x + bit * scale, y + row * scale + sy, run * scale, c, 255);
                }
                bit += run;
            }
        }
        x += cw;
    }
    mark_clipped(g, x0, y, total, ch);
    return total;
}

/* ------------------------------------------------------------------ */
/* vector text (Hershey roman simplex)                                 */
#define VF_TOP (-12.0)
#define VF_BASE 9.0
#define VF_CAP (VF_BASE - VF_TOP)

static int vf_index(lt_u32 cp)
{
    if (cp >= 32 && cp <= 126) { return (int)cp - 32; }
    if (cp == 0xB0) { return 95; }
    return '?' - 32;
}

double lt_gfx_vtext_width(const char *s, double height)
{
    double w = 0.0, sc = height / VF_CAP;
    lt_u32 cp;
    if (lt_gfx_vtext_width_plex(s, height, &w)) { return w; }
    while ((cp = lt_utf8_next(&s)) != 0) {
        w += lt_vfont_adv[vf_index(cp)] * sc;
    }
    return w;
}

void lt_gfx_vtext(lt_gfx *g, double x, double y, const char *s, double height,
                  double rotation, int align, int valign, double stroke, lt_color c)
{
    double sc, tw, ca, sa, pen, vy;
    lt_u32 cp;
    if (height <= 0.0) { return; }
    if (g->canvas != NULL && g->cells == NULL && !g->bitmap_text &&
        lt_gfx_vtext_plex(g, x, y, s, height, rotation, align, valign, c)) {
        return;
    }
    sc = height / VF_CAP;
    tw = lt_gfx_vtext_width(s, height);
    if (g->cells != NULL) {
        int n = 0, cx, cy;
        const char *p = s;
        while (lt_utf8_next(&p) != 0) { n++; }
        cx = ifloor((x + g->ox) / CELL_W) - (align == 1 ? n / 2 : (align == 2 ? n : 0));
        cy = ifloor((y + g->oy) / CELL_H);
        while ((cp = lt_utf8_next(&s)) != 0) {
            cell_ch(g, cx, cy, cp, c);
            cx++;
        }
        return;
    }
    if (stroke <= 0.0) { stroke = height / 12.0; if (stroke < 1.0) { stroke = 1.0; } }
    ca = cos(rotation); sa = sin(rotation);
    pen = align == 1 ? -tw / 2.0 : (align == 2 ? -tw : 0.0);
    /* valign: 0 = y is the top of capitals, 1 = center, 2 = baseline */
    vy = valign == 0 ? VF_TOP : (valign == 1 ? (VF_TOP + VF_BASE) / 2.0 : VF_BASE);
    while ((cp = lt_utf8_next(&s)) != 0) {
        int gi = vf_index(cp), k, end;
        int have = 0;
        double px = 0.0, py = 0.0;
        k = lt_vfont_off[gi]; end = lt_vfont_off[gi + 1];
        for (; k < end; k++) {
            double u, v, rx, ry;
            if (lt_vfont_data[2 * k] == -128) { have = 0; continue; }
            u = pen + lt_vfont_data[2 * k] * sc;
            v = (lt_vfont_data[2 * k + 1] - vy) * sc;
            rx = x + u * ca - v * sa;
            ry = y + u * sa + v * ca;
            if (have) {
                /* lt_gfx_line adds the translation itself: undo it here */
                lt_gfx_line(g, px - g->ox, py - g->oy, rx - g->ox, ry - g->oy, stroke, c);
            } else if (k + 1 >= end || lt_vfont_data[2 * (k + 1)] == -128) {
                lt_gfx_line(g, rx - g->ox, ry - g->oy, rx - g->ox, ry - g->oy, stroke, c);
            }
            px = rx; py = ry; have = 1;
        }
        pen += lt_vfont_adv[gi] * sc;
    }
}
