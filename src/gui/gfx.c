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
    c->gen = lt_gfx_next_gen();
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
    lt_rec_canvas_changing(c);
    if (c->px != NULL) {
        lt_free(c->px);
    }
    c->px = NULL;
    c->w = c->h = c->pitch = 0;
    c->gen = lt_gfx_next_gen();
}

void lt_canvas_touch(lt_canvas *c)
{
    lt_rec_canvas_changing(c);
    c->gen = lt_gfx_next_gen();
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
    g->ui_pct = 0;
    g->rec = NULL;
}

int lt_gfx_px(const lt_gfx *g, int px)
{
    int pct = g->ui_pct;
    lt_s64 v;
    if (pct <= 0 || g->cells != NULL) { return px; }
    v = (lt_s64)px * pct;
    return (int)(v >= 0 ? (v + 50) / 100 : -((-v + 50) / 100));
}

double lt_gfx_fpx(const lt_gfx *g, double px)
{
    if (g->ui_pct <= 0 || g->cells != NULL) { return px; }
    return px * (double)g->ui_pct / 100.0;
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
    g->ui_pct = 0;
    g->rec = NULL;
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

void lt_rect_union_into(lt_rect *acc, lt_rect r)
{
    int x1, y1;
    if (r.w <= 0 || r.h <= 0) { return; }
    if (acc->w <= 0 || acc->h <= 0) { *acc = r; return; }
    x1 = LT_MAX(acc->x + acc->w, r.x + r.w);
    y1 = LT_MAX(acc->y + acc->h, r.y + r.h);
    acc->x = LT_MIN(acc->x, r.x);
    acc->y = LT_MIN(acc->y, r.y);
    acc->w = x1 - acc->x;
    acc->h = y1 - acc->y;
}

/* about to write g->canvas: a recorded frame referring to it keeps a copy; new generation */
static void prep(lt_gfx *g)
{
    if (g->canvas != NULL) {
        lt_rec_canvas_changing(g->canvas);
        g->canvas->gen = lt_gfx_next_gen();
    }
}

static lt_rect *g_measure;

void lt_gfx_measure_begin(lt_rect *acc) { g_measure = acc; }
void lt_gfx_measure_end(void) { g_measure = NULL; }
int lt_gfx_measuring(void) { return g_measure != NULL; }

/* record a call with only a colour and an absolute box; NULL if it touches nothing */
static lt_dcmd *rec_simple(lt_gfx *g, int op, lt_rect bb, lt_color c)
{
    lt_dcmd *k = lt_rec_cmd(g, op, bb);
    if (k != NULL) { k->c = c; }
    return k;
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
/* pixel blending. Red and blue are blended together in the two 16-bit halves of one word
 * (0x00RR00BB * alpha stays below 2^16 per half), with the same rounding as DIV255 per channel:
 * the results are bit-identical to blending each channel on its own. */
#define LANES 0x00FF00FFU

/* d blended with a colour whose premultiplied lanes are srb = (c & LANES) * a,
 * sg = G(c) * a, ia = 255 - a */
static LT_INLINE lt_u32 blend_lanes(lt_u32 d, lt_u32 srb, lt_u32 sg, lt_u32 ia)
{
    lt_u32 rb = (d & LANES) * ia + srb + 0x00800080U;
    lt_u32 gg = ((d >> 8) & 0xFFU) * ia + sg + 0x80U;
    rb = ((rb + ((rb >> 8) & LANES)) >> 8) & LANES;
    gg = ((gg + (gg >> 8)) >> 8) & 0xFFU;
    return 0xFF000000U | rb | (gg << 8);
}

/* one pixel at coverage cov (0..255) of colour col (alpha ca), like a 1-pixel put_span */
static LT_INLINE void put_px(lt_u32 *p, lt_color col, int ca, int cov)
{
    int a = cov < 255 ? DIV255(ca * cov) : ca;
    lt_u32 ua;
    if (a <= 0) { return; }
    if (a >= 255) { *p = (col | OPAQUE_MASK) & LT_U32_MASK; return; }
    ua = (lt_u32)a;
    *p = blend_lanes(*p, (col & LANES) * ua, ((col >> 8) & 0xFFU) * ua, 255U - ua);
}

/* the clip intersected with the canvas: every pixel inside may be written without checks */
static lt_rect vis_clip(const lt_gfx *g)
{
    return lt_rect_intersect(g->clip, lt_rect_make(0, 0, g->canvas->w, g->canvas->h));
}

/* put_px at (x, y) if inside vc (= vis_clip) */
static LT_INLINE void put_px_at(lt_gfx *g, const lt_rect *vc, int x, int y, lt_color col, int ca,
                                int cov)
{
    if (x < vc->x || y < vc->y || x >= vc->x + vc->w || y >= vc->y + vc->h) { return; }
    put_px(g->canvas->px + (size_t)y * (size_t)g->canvas->pitch + (size_t)x, col, ca, cov);
}

/* w pixels of value v, eight per step (compilers turn the step into wide stores) */
static void fill_px(lt_u32 *p, int w, lt_u32 v)
{
    int i;
    for (i = 0; i + 8 <= w; i += 8) {
        p[i] = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
        p[i + 4] = v; p[i + 5] = v; p[i + 6] = v; p[i + 7] = v;
    }
    for (; i < w; i++) { p[i] = v; }
}

/* pixel span primitive: horizontal run with constant coverage          */
static void put_span(lt_gfx *g, int x, int y, int w, lt_color col, int cov)
{
    lt_canvas *c = g->canvas;
    int x0, y0, x1, y1, a, i;
    lt_u32 *p, srb, sg, ia;

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
        fill_px(p, w, (col | OPAQUE_MASK) & LT_U32_MASK);
        return;
    }
    ia = 255U - (lt_u32)a;
    srb = (col & LANES) * (lt_u32)a;
    sg = ((col >> 8) & 0xFFU) * (lt_u32)a;
    for (i = 0; i < w; i++) { p[i] = blend_lanes(p[i], srb, sg, ia); }
}

/* Composite a premultiplied-ARGB layer (alpha in the top byte, colour already multiplied by it)
 * over the canvas at (x, y) in the current translation; honours the clip. */
void lt_gfx_blit_premul(lt_gfx *g, int x, int y, const lt_canvas *src)
{
    lt_canvas *c = g->canvas;
    lt_rect r;
    int yy, xx;
    if (c == NULL || src == NULL || src->px == NULL) { return; }
    if (g->rec != NULL) {
        lt_dcmd *k = lt_rec_cmd(g, LT_OP_PREMUL, lt_rect_make(x + g->ox, y + g->oy, src->w, src->h));
        if (k != NULL) {
            k->i[0] = x; k->i[1] = y;
            k->src = lt_rec_canvas(g, src);
            lt_rec_done(g, k);
        }
        return;
    }
    prep(g);
    x += g->ox; y += g->oy;
    r = lt_rect_intersect(lt_rect_make(x, y, src->w, src->h), g->clip);
    r = lt_rect_intersect(r, lt_rect_make(0, 0, c->w, c->h));
    if (r.w <= 0 || r.h <= 0) { return; }
    for (yy = r.y; yy < r.y + r.h; yy++) {
        const lt_u32 *sp = src->px + (size_t)(yy - y) * (size_t)src->pitch + (size_t)(r.x - x);
        lt_u32 *dp = c->px + (size_t)yy * (size_t)c->pitch + (size_t)r.x;
        /* layers are mostly opaque or empty runs: copy or skip a run at once */
        for (xx = 0; xx < r.w; xx++) {
            lt_u32 s = sp[xx];
            lt_u32 a = (s >> 24) & 0xFFU;
            if (a == 255U) {
                int e = xx + 1;
                while (e < r.w && ((sp[e] >> 24) & 0xFFU) == 255U) { e++; }
                memcpy(dp + xx, sp + xx, (size_t)(e - xx) * sizeof(lt_u32));
                xx = e - 1;
            } else if (a == 0U) {
                while (xx + 1 < r.w && ((sp[xx + 1] >> 24) & 0xFFU) == 0U) { xx++; }
            } else {
                /* source + destination * (255 - a) / 255 per channel, saturated at 255 */
                lt_u32 d = dp[xx], ia = 255U - a, rb, gg, ov;
                rb = (d & LANES) * ia + 0x00800080U;
                gg = ((d >> 8) & 0xFFU) * ia + 0x80U;
                rb = (((rb + ((rb >> 8) & LANES)) >> 8) & LANES) + (s & LANES);
                gg = (((gg + (gg >> 8)) >> 8) & 0xFFU) + ((s >> 8) & 0xFFU);
                ov = rb & 0x01000100U;
                rb = (rb | (ov - (ov >> 8))) & LANES;
                if (gg > 255U) { gg = 255U; }
                dp[xx] = 0xFF000000U | rb | (gg << 8);
            }
        }
    }
    mark_clipped(g, r.x, r.y, r.w, r.h);
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
        /* dark translucent fills (shadows, modal dimming) darken the glyph too */
        if (LT_COLOR_R(c) + LT_COLOR_G(c) + LT_COLOR_B(c) < 96) {
            k->fg = blend_color(k->fg, c, a);
        }
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
    if (g->rec != NULL) {
        lt_dcmd *k = rec_simple(g, LT_OP_CLEAR, g->clip, c);
        if (k != NULL) { lt_rec_done(g, k); }
        return;
    }
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
    prep(g);
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
    if (g->rec != NULL) {
        lt_dcmd *k = rec_simple(g, LT_OP_FILL, r, c);
        if (k != NULL) {
            k->i[0] = r.x; k->i[1] = r.y; k->i[2] = r.w; k->i[3] = r.h;
            k->ox = 0; k->oy = 0;
            lt_rec_done(g, k);
        }
        return;
    }
    r = lt_rect_intersect(r, g->clip);
    if (r.w <= 0 || r.h <= 0 || g->canvas == NULL) { return; }
    prep(g);
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
    {
        int ax0 = cdiv_floor(x0, CELL_W), ax1 = cdiv_floor(x1, CELL_W);
        cx0 = cclamp(ax0, 0, g->cells->cols);
        cx1 = cclamp(ax1, -1, g->cells->cols);
        for (cx = cx0; cx <= cx1; cx++) {
            lt_cell *k = cell_at(g, cx, cy);
            int bits = 3;
            if (k != NULL && (lt_cell_line_bits(k->ch) & 12) != 0) {
                /* ends abutting a vertical stroke become T junctions */
                if (cx == ax0 && cx != ax1) { bits = 2; }
                else if (cx == ax1 && cx != ax0) { bits = 1; }
            }
            cell_ch(g, cx, cy, lt_cell_line_merge(k != NULL ? k->ch : ' ', bits, 0), c);
        }
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
    {
        int ay0 = cdiv_floor(y0, CELL_H), ay1 = cdiv_floor(y1, CELL_H);
        cy0 = cclamp(ay0, 0, g->cells->rows);
        cy1 = cclamp(ay1, -1, g->cells->rows);
        for (cy = cy0; cy <= cy1; cy++) {
            lt_cell *k = cell_at(g, cx, cy);
            int bits = 12;
            if (k != NULL && (lt_cell_line_bits(k->ch) & 3) != 0) {
                if (cy == ay0 && cy != ay1) { bits = 8; }
                else if (cy == ay1 && cy != ay0) { bits = 4; }
            }
            cell_ch(g, cx, cy, lt_cell_line_merge(k != NULL ? k->ch : ' ', bits, 0), c);
        }
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
            int bits = 3, corner = 0;
            lt_cell *k;
            if (x0 == x1) { bits = 0; }
            else if (x == x0) { bits = 2; corner = 1; }
            else if (x == x1) { bits = 1; corner = 1; }
            k = cell_at(g, x, y0);
            cell_ch(g, x, y0, lt_cell_line_merge(k != NULL ? k->ch : ' ', (x0 == x1 ? 12 : bits) | (corner && y1 != y0 ? 8 : 0), 1), c);
            if (y1 != y0) {
                k = cell_at(g, x, y1);
                cell_ch(g, x, y1, lt_cell_line_merge(k != NULL ? k->ch : ' ', (x0 == x1 ? 12 : bits) | (corner ? 4 : 0), 1), c);
            }
        }
        for (y = ya; y <= yb; y++) {
            if (y != y0 && y != y1) {
                lt_cell *k = cell_at(g, x0, y);
                cell_ch(g, x0, y, lt_cell_line_merge(k != NULL ? k->ch : ' ', 12, 0), c);
                if (x1 != x0) {
                    k = cell_at(g, x1, y);
                    cell_ch(g, x1, y, lt_cell_line_merge(k != NULL ? k->ch : ' ', 12, 0), c);
                }
            }
        }
    }
}

void lt_gfx_fill_round_rect(lt_gfx *g, lt_rect r, int radius, lt_color c)
{
    int y, i, rad, x0, rx, ca;
    lt_rect vc;
    double rr;
    if (r.w <= 0 || r.h <= 0) { return; }
    rad = radius;
    if (rad > r.w / 2) { rad = r.w / 2; }
    if (rad > r.h / 2) { rad = r.h / 2; }
    if (rad <= 1 || g->cells != NULL) {
        lt_gfx_fill_rect(g, r, c);
        return;
    }
    if (g->rec != NULL) {
        lt_dcmd *k = rec_simple(g, LT_OP_ROUND, lt_rect_make(r.x + g->ox, r.y + g->oy, r.w, r.h), c);
        if (k != NULL) {
            k->i[0] = r.x; k->i[1] = r.y; k->i[2] = r.w; k->i[3] = r.h; k->d[0] = (double)radius;
            lt_rec_done(g, k);
        }
        return;
    }
    if (g->canvas == NULL) { return; }
    prep(g);
    r.x += g->ox; r.y += g->oy;
    rr = (double)rad;
    vc = vis_clip(g);
    ca = LT_COLOR_A(c);
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
        {
            /* coverage grows towards the middle: the corner pixels at full coverage join the
             * middle span, the anti-aliased ones are blended one by one */
            int f = rad, py = r.y + y, inrow = py >= vc.y && py < vc.y + vc.h;
            lt_u32 *row = inrow ? g->canvas->px + (size_t)py * (size_t)g->canvas->pitch : NULL;
            for (i = 0; i < rad; i++) {
                double dx = rr - (i + 0.5);
                double d = sqrt(dx * dx + dy * dy);
                int cv = cov255(clamp01(rr + 0.5 - d));
                if (cv >= 255) { f = i; break; }
                if (cv > 0 && row != NULL) {
                    int xa = x0 + i, xb = rx - 1 - i;
                    if (xa >= vc.x && xa < vc.x + vc.w) { put_px(row + xa, c, ca, cv); }
                    if (xb >= vc.x && xb < vc.x + vc.w) { put_px(row + xb, c, ca, cv); }
                }
            }
            put_span(g, x0 + f, py, r.w - 2 * f, c, 255);
        }
    }
    mark_clipped(g, r.x, r.y, r.w, r.h);
}

/* ------------------------------------------------------------------ */
/* soft shadow: `spread` nested translucent black round rects          */
#define SHADOW_MAX 256

typedef struct shadow_ring {
    lt_rect q;          /* absolute */
    int rad, plain, a;
    double rr;
} shadow_ring;

/* ring i of lt_gfx_shadow (i = spread..1): rectangle (relative), radius and alpha */
static lt_rect shadow_ring_rect(lt_rect r, int i, int n, int radius, int alpha, int *rad, int *a)
{
    int al = alpha * (n - i + 1) * (n - i + 1) / (n * n * 3 / 2 + 1);
    if (al < 1) { al = 1; }
    *a = al & 0xFF;
    *rad = radius + i;
    return lt_rect_make(r.x - i, r.y - i + n / 2, r.w + 2 * i, r.h + 2 * i);
}

/* coverage (0..255) of ring k at absolute pixel x in a row: lo..hi is the fully covered part,
 * dy the row's corner distance (< 0: no corner) */
static int ring_cov(const shadow_ring *k, int x, int lo, int hi, double dy)
{
    int j;
    double dx;
    if (x >= lo && x < hi) { return 255; }
    if (x < k->q.x || x >= k->q.x + k->q.w || dy < 0.0 || k->plain) { return 0; }
    j = x < k->q.x + k->rad ? x - k->q.x : k->q.x + k->q.w - 1 - x;
    dx = k->rr - (j + 0.5);
    return cov255(clamp01(k->rr + 0.5 - sqrt(dx * dx + dy * dy)));
}

static lt_u32 shadow_px(lt_u32 d, int a)
{
    int ia;
    if (a <= 0) { return d; }
    if (a >= 255) { return 0xFF000000UL; }
    ia = 255 - a;
    return (lt_u32)(0xFF000000UL | ((lt_u32)DIV255((int)((d >> 16) & 0xFF) * ia) << 16) |
                    ((lt_u32)DIV255((int)((d >> 8) & 0xFF) * ia) << 8) |
                    (lt_u32)DIV255((int)(d & 0xFF) * ia));
}

/* the combined effect of the first m rings on a fully covered pixel (luts + 256 * m) */
static LT_INLINE lt_u32 shadow_lut_px(const lt_u8 *lut, lt_u32 d)
{
    return (lt_u32)(0xFF000000UL | ((lt_u32)lut[(d >> 16) & 0xFF] << 16) |
                    ((lt_u32)lut[(d >> 8) & 0xFF] << 8) | (lt_u32)lut[d & 0xFF]);
}

/* ring tables of the last spread / alpha (they depend on nothing else); a dialog's shadow is
 * replayed once per tile row */
#define SHADOW_LUT_N 126
static lt_u8 g_shadow_luts[(SHADOW_LUT_N + 1) * 256];
static int g_shadow_lut_n = -1, g_shadow_lut_alpha = -1;

/* Exactly the pixels of: for i = spread..1, lt_gfx_fill_round_rect(ring i, radius + i,
 * black with the ring's alpha) (lt_ui_draw_shadow's rings). The rings are concentric, so in a row
 * the covered ones are a prefix of that order and, where all of them cover fully, their combined
 * effect is one per-channel table. In a row where no ring is at a corner, a pixel of the edge
 * band is fully covered by the outer rings up to its position (a table too); only the rows of
 * the anti-aliased corners are blended ring by ring. */
void lt_gfx_shadow(lt_gfx *g, lt_rect r, int radius, int spread, int alpha)
{
    shadow_ring *rg;
    lt_u8 *luts;
    int n = spread, i, k, y, ylo, yhi, cx0, cx1, rad, a, n_lut = 0;
    double *dys;
    int *lo, *hi;
    lt_canvas *c = g->canvas;
    if (n <= 0 || alpha <= 0) { return; }
    if (n > SHADOW_MAX || g->cells != NULL || c == NULL) {
        for (i = n; i >= 1; i--) {
            lt_rect q = shadow_ring_rect(r, i, n, radius, alpha, &rad, &a);
            lt_gfx_fill_round_rect(g, q, rad, LT_RGBA(0, 0, 0, a));
        }
        return;
    }
    if (g->rec != NULL) {
        lt_dcmd *kc = lt_rec_cmd(g, LT_OP_SHADOW, lt_rect_make(r.x - n + g->ox, r.y - n + n / 2 + g->oy,
                                                              r.w + 2 * n, r.h + 2 * n));
        if (kc != NULL) {
            kc->i[0] = r.x; kc->i[1] = r.y; kc->i[2] = r.w; kc->i[3] = r.h;
            kc->d[0] = (double)radius; kc->d[1] = (double)spread; kc->d[2] = (double)alpha;
            lt_rec_done(g, kc);
        }
        return;
    }
    prep(g);
    rg = (shadow_ring *)lt_malloc((size_t)n * sizeof(shadow_ring));
    dys = (double *)lt_malloc((size_t)n * sizeof(double));
    lo = (int *)lt_malloc((size_t)n * 2 * sizeof(int));
    hi = lo + n;
    for (k = 0; k < n; k++) {
        shadow_ring *s = &rg[k];
        s->q = shadow_ring_rect(r, n - k, n, radius, alpha, &s->rad, &s->a);
        s->q.x += g->ox; s->q.y += g->oy;
        if (s->rad > s->q.w / 2) { s->rad = s->q.w / 2; }
        if (s->rad > s->q.h / 2) { s->rad = s->q.h / 2; }
        s->plain = s->rad <= 1;
        s->rr = (double)s->rad;
        if (s->q.w <= 0 || s->q.h <= 0) { s->q.w = s->q.h = 0; }
    }
    /* luts + 256 * m: the first m rings' combined effect on a fully covered channel value */
    if (n <= SHADOW_LUT_N) {
        luts = g_shadow_luts;
        n_lut = (g_shadow_lut_n == n && g_shadow_lut_alpha == alpha) ? 0 : n;
        g_shadow_lut_n = n;
        g_shadow_lut_alpha = alpha;
    } else {
        luts = (lt_u8 *)lt_malloc(((size_t)n + 1) * 256U);
        n_lut = n;
    }
    if (n_lut > 0) {
        for (i = 0; i < 256; i++) { luts[i] = (lt_u8)i; }
    }
    for (k = 0; k < n_lut; k++) {
        const lt_u8 *pv = luts + (size_t)k * 256U;
        lt_u8 *nx = luts + (size_t)(k + 1) * 256U;
        for (i = 0; i < 256; i++) {
            nx[i] = (lt_u8)(rg[k].a >= 255 ? 0 : DIV255((int)pv[i] * (255 - rg[k].a)));
        }
    }
    cx0 = LT_MAX(g->clip.x, 0); cx1 = LT_MIN(g->clip.x + g->clip.w, c->w);
    ylo = LT_MAX(LT_MAX(g->clip.y, 0), rg[0].q.y);
    yhi = LT_MIN(LT_MIN(g->clip.y + g->clip.h, c->h), rg[0].q.y + rg[0].q.h);
    for (y = ylo; y < yhi && cx0 < cx1; y++) {
        int m = 0, ilo = -0x7FFFFFFF, ihi = 0x7FFFFFFF, ux0 = 0x7FFFFFFF, ux1 = -0x7FFFFFFF, x;
        int corner = 0;
        lt_u32 *p = c->px + (size_t)y * (size_t)c->pitch;
        const lt_u8 *lut;
        /* rings covering this row: a prefix of the drawing order */
        for (k = 0; k < n; k++) {
            const shadow_ring *s = &rg[k];
            int yr = y - s->q.y;
            double dy = -1.0;
            if (s->q.w <= 0 || yr < 0 || yr >= s->q.h) { break; }
            if (!s->plain) {
                if (yr < s->rad) { dy = s->rr - (yr + 0.5); }
                else if (yr >= s->q.h - s->rad) { dy = (yr + 0.5) - (s->q.h - s->rr); }
            }
            dys[k] = dy;
            if (!s->plain && dy >= 0.0) { corner = 1; }
            if (s->plain || dy < 0.0) { lo[k] = s->q.x; hi[k] = s->q.x + s->q.w; }
            else { lo[k] = s->q.x + s->rad; hi[k] = s->q.x + s->q.w - s->rad; }
            if (lo[k] > ilo) { ilo = lo[k]; }
            if (hi[k] < ihi) { ihi = hi[k]; }
            if (s->q.x < ux0) { ux0 = s->q.x; }
            if (s->q.x + s->q.w > ux1) { ux1 = s->q.x + s->q.w; }
            m++;
        }
        if (m == 0) { continue; }
        lut = luts + (size_t)m * 256U;
        if (ux0 < cx0) { ux0 = cx0; }
        if (ux1 > cx1) { ux1 = cx1; }
        if (!corner && ilo <= ihi) {
            /* left band: rings k with lo[k] <= x cover x (lo grows with k), right band: those
             * with hi[k] > x (hi shrinks with k); inside, all m */
            int j = 0, xe = LT_MIN(ilo, ux1);
            for (x = ux0; x < xe; x++) {
                while (j < m && lo[j] <= x) { j++; }
                p[x] = shadow_lut_px(luts + (size_t)j * 256U, p[x]);
            }
            xe = LT_MIN(ihi, ux1);
            for (x = LT_MAX(ilo, ux0); x < xe; x++) { p[x] = shadow_lut_px(lut, p[x]); }
            j = m;
            for (x = LT_MAX(ihi, ux0); x < ux1; x++) {
                while (j > 0 && hi[j - 1] <= x) { j--; }
                p[x] = shadow_lut_px(luts + (size_t)j * 256U, p[x]);
            }
            continue;
        }
        if (ihi < ilo) { ihi = ilo; }
        for (x = ux0; x < ux1; x++) {
            lt_u32 d = p[x];
            if (x >= ilo && x < ihi) {
                int xe = LT_MIN(ihi, ux1);
                for (; x < xe; x++) { p[x] = shadow_lut_px(lut, p[x]); }
                x--;
                continue;
            }
            for (k = 0; k < m; k++) {
                int cv = ring_cov(&rg[k], x, lo[k], hi[k], dys[k]);
                if (cv > 0) { d = shadow_px(d, cv < 255 ? DIV255(rg[k].a * cv) : rg[k].a); }
            }
            p[x] = d;
        }
    }
    if (luts != g_shadow_luts) { lt_free(luts); }
    lt_free(lo); lt_free(dys); lt_free(rg);
    mark_clipped(g, r.x - n + g->ox, r.y - n + n / 2 + g->oy, r.w + 2 * n, r.h + 2 * n);
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
    if (g->rec != NULL) {
        lt_dcmd *k = rec_simple(g, LT_OP_VGRAD, lt_rect_make(r.x + g->ox, r.y + g->oy, r.w, r.h), top);
        if (k != NULL) {
            k->c2 = bottom;
            k->i[0] = r.x; k->i[1] = r.y; k->i[2] = r.w; k->i[3] = r.h;
            lt_rec_done(g, k);
        }
        return;
    }
    if (g->canvas == NULL) { return; }
    prep(g);
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
    int py, ya, yb, ca;
    lt_rect vc;
    if (g->rec != NULL) {
        double ax = x0 + g->ox, bx = x1 + g->ox, ay = y0 + g->oy, by = y1 + g->oy;
        double r2 = (width < 0.0 ? 0.0 : width) / 2.0 + 0.5;
        int ix = ifloor((ax < bx ? ax : bx) - r2), iy = ifloor((ay < by ? ay : by) - r2);
        lt_dcmd *k = rec_simple(g, LT_OP_LINE, lt_rect_make(ix, iy, iceil((ax > bx ? ax : bx) + r2) - ix,
                                                          iceil((ay > by ? ay : by) + r2) - iy), c);
        if (k != NULL) {
            k->d[0] = x0; k->d[1] = y0; k->d[2] = x1; k->d[3] = y1; k->d[4] = width;
            lt_rec_done(g, k);
        }
        return;
    }
    x0 += g->ox; x1 += g->ox; y0 += g->oy; y1 += g->oy;
    if (width < 0.0) { width = 0.0; }
    dx = x1 - x0; dy = y1 - y0;
    len2 = dx * dx + dy * dy;
    len = sqrt(len2);
    if (g->cells != NULL) {
        char ch = line_char(dx, dy, width);
        int n, i;
        if (len > 1.0e6) { return; }
        if (width < 3.0 && dx != 0.0 && dy != 0.0) { lt_cell_braille_line(g, x0, y0, x1, y1, c); return; }
        n = (int)(len / 4.0) + 1;
        for (i = 0; i <= n; i++) {
            double t = (double)i / (double)n;
            cell_put_at(g, x0 + dx * t, y0 + dy * t, (lt_u32)(unsigned char)ch, c);
        }
        return;
    }
    rad = width / 2.0;
    rr = rad + 0.5;
    if (g->canvas == NULL) { return; }
    prep(g);
    vc = vis_clip(g);
    ca = LT_COLOR_A(c);
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
            if (cv > 0.0) { put_px_at(g, &vc, px, py, c, ca, cov255(cv)); }
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

/* Per-row scanline fill of an annular sector. Radial coverage comes from the pixel's distance to
 * the centre (sqrt only in the 1 px fringes; the fully covered band is found from per-row spans);
 * angular coverage is the signed perpendicular distance to the two edge lines (linear in x, so no
 * atan2/fmod): convex wedges (sweep <= 180 deg) intersect the two half-planes, reflex ones unite
 * them. Fully covered runs are filled as one span. */
static void arc_draw(lt_gfx *g, const arc_ctx *a, lt_color c)
{
    int py, ya, yb, convex, ca = LT_COLOR_A(c);
    double ro = a->rout + 1.0;
    lt_rect vc;
    double u0x = 0.0, u0y = 0.0, u1x = 0.0, u1y = 0.0;
    double rof = a->rout - 0.5, rif = a->rin + 0.5;
    if (!a->full) {
        u0x = cos(a->a0); u0y = sin(a->a0);
        u1x = cos(a->a0 + a->sweep); u1y = sin(a->a0 + a->sweep);
    }
    convex = a->sweep <= LT_PI;
    if (g->canvas == NULL) { return; }
    prep(g);
    vc = vis_clip(g);
    ya = ifloor(a->cy - ro); yb = iceil(a->cy + ro);
    if (ya < a->by0) { ya = a->by0; }
    if (yb > a->by1 + 1) { yb = a->by1 + 1; }
    if (ya < g->clip.y) { ya = g->clip.y; }
    if (yb > g->clip.y + g->clip.h) { yb = g->clip.y + g->clip.h; }
    for (py = ya; py < yb; py++) {
        double dy = py + 0.5 - a->cy, o2 = ro * ro - dy * dy, xo, xs = -1.0, xi = 0.0, xh = -1.0;
        double c0, c1, cb0 = 0.0, cb1 = 0.0;
        int xl, xr, px, sg, nseg, sx0[2], sx1[2];
        lt_u32 *row;
        if (o2 < 0.0 || py < vc.y || py >= vc.y + vc.h) { continue; }
        row = g->canvas->px + (size_t)py * (size_t)g->canvas->pitch;
        xo = sqrt(o2);
        xl = ifloor(a->cx - xo); xr = iceil(a->cx + xo);
        if (rof > 0.0 && rof * rof > dy * dy) { xs = sqrt(rof * rof - dy * dy); }
        if (rif > 0.0 && rif * rif > dy * dy) { xi = sqrt(rif * rif - dy * dy); }
        if (a->rin - 0.5 > 0.0 && (a->rin - 0.5) * (a->rin - 0.5) > dy * dy) {
            xh = sqrt((a->rin - 0.5) * (a->rin - 0.5) - dy * dy);
        }
        if (xl < g->clip.x) { xl = g->clip.x; }
        if (xr >= g->clip.x + g->clip.w) { xr = g->clip.x + g->clip.w - 1; }
        if (xl < a->bx0) { xl = a->bx0; }
        if (xr > a->bx1) { xr = a->bx1; }
        c0 = 0.0; c1 = 0.0;
        nseg = 1; sx0[0] = xl; sx1[0] = xr;
        if (!a->full) {
            double k0 = -u0y, k1 = u1y, lo = -1.0e30, hi = 1.0e30, t;
            cb0 = u0x * dy; cb1 = -dy * u1x;
            /* columns (relative to cx) where c0 > -0.5 / c1 > -0.5; reflex wedges: the gap is
             * where both are <= -0.5 */
            if (convex) {
                if (k0 > 1.0e-12) { t = (-0.5 - cb0) / k0; if (t > lo) { lo = t; } }
                else if (k0 < -1.0e-12) { t = (-0.5 - cb0) / k0; if (t < hi) { hi = t; } }
                else if (cb0 <= -0.5) { hi = -1.0e30; }
                if (k1 > 1.0e-12) { t = (-0.5 - cb1) / k1; if (t > lo) { lo = t; } }
                else if (k1 < -1.0e-12) { t = (-0.5 - cb1) / k1; if (t < hi) { hi = t; } }
                else if (cb1 <= -0.5) { hi = -1.0e30; }
                if (lo > hi) { continue; }
                { int pl = iceil(a->cx + lo - 0.5) - 1, pr = ifloor(a->cx + hi - 0.5) + 1;
                  if (pl > sx0[0]) { sx0[0] = pl; }
                  if (pr < sx1[0]) { sx1[0] = pr; } }
            } else {
                /* gap = { c0 <= -0.5 } and { c1 <= -0.5 }: an interval in dx */
                if (k0 > 1.0e-12) { t = (-0.5 - cb0) / k0; if (t < hi) { hi = t; } }
                else if (k0 < -1.0e-12) { t = (-0.5 - cb0) / k0; if (t > lo) { lo = t; } }
                else if (cb0 > -0.5) { lo = 1.0e30; }
                if (k1 > 1.0e-12) { t = (-0.5 - cb1) / k1; if (t < hi) { hi = t; } }
                else if (k1 < -1.0e-12) { t = (-0.5 - cb1) / k1; if (t > lo) { lo = t; } }
                else if (cb1 > -0.5) { lo = 1.0e30; }
                if (lo <= hi) {
                    int gl = iceil(a->cx + lo - 0.5) + 1, gr = ifloor(a->cx + hi - 0.5) - 1;
                    if (gl <= gr) {
                        sx0[1] = gr + 1; sx1[1] = xr; sx1[0] = gl - 1; nseg = 2;
                    }
                }
            }
        }
        for (sg = 0; sg < nseg; sg++) {
        int run = -1;
        /* pixels outside the visible clip would be dropped by put_span: skip them */
        if (sx0[sg] < vc.x) { sx0[sg] = vc.x; }
        if (sx1[sg] > vc.x + vc.w - 1) { sx1[sg] = vc.x + vc.w - 1; }
        for (px = sx0[sg]; px <= sx1[sg]; px++) {
            double dx = px + 0.5 - a->cx, adx = fabs(dx), cv, ac;
            if (adx < xh) {                 /* inside the hole: nothing to draw */
                int nx = iceil(a->cx + xh - 0.5);
                if (run >= 0) { put_span(g, run, py, px - run, c, 255); run = -1; }
                if (nx > px) { px = nx - 1; continue; }
            }
            if (adx <= xs && adx >= xi) {
                cv = 1.0;
            } else {
                double d = sqrt(dx * dx + dy * dy);
                cv = clamp01(a->rout + 0.5 - d);
                if (a->rin > 0.0) {
                    cv = cv + clamp01(d - a->rin + 0.5) - 1.0;
                    if (cv < 0.0) { cv = 0.0; }
                }
            }
            if (cv > 0.0 && !a->full) {
                double m;
                c0 = cb0 - u0y * dx; c1 = dx * u1y + cb1;
                m = convex ? (c0 < c1 ? c0 : c1) : (c0 > c1 ? c0 : c1);
                ac = clamp01(m + 0.5);
                cv *= ac;
            }
            if (cv >= 1.0) {
                if (run < 0) { run = px; }
                continue;
            }
            if (run >= 0) { put_span(g, run, py, px - run, c, 255); run = -1; }
            if (cv > 0.0) { put_px(row + px, c, ca, cov255(cv)); }
        }
        if (run >= 0) { put_span(g, run, py, sx1[sg] + 1 - run, c, 255); }
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

/* record an annulus/sector call: the box is the sector's (translated) bounding box */
static void rec_arc(lt_gfx *g, int op, double rin, double rout, double a0, double a1,
                    const double *par, lt_color c)
{
    arc_ctx a;
    lt_dcmd *k;
    int i;
    arc_setup(&a, par[0] + g->ox, par[1] + g->oy, rin, rout, a0, a1);
    k = rec_simple(g, op, lt_rect_make(a.bx0, a.by0, a.bx1 - a.bx0 + 1, a.by1 - a.by0 + 1), c);
    if (k == NULL) { return; }
    for (i = 0; i < 6; i++) { k->d[i] = par[i]; }
    lt_rec_done(g, k);
}

void lt_gfx_fill_circle(lt_gfx *g, double cx, double cy, double r, lt_color c)
{
    arc_ctx a;
    if (g->rec != NULL) {
        double par[6];
        par[0] = cx; par[1] = cy; par[2] = r; par[3] = par[4] = par[5] = 0.0;
        rec_arc(g, LT_OP_FCIRCLE, 0.0, r, 0.0, TWO_PI, par, c);
        return;
    }
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
    if (g->rec != NULL) {
        double par[6];
        par[0] = cx; par[1] = cy; par[2] = r; par[3] = width; par[4] = par[5] = 0.0;
        rec_arc(g, LT_OP_CIRCLE, r - width / 2.0, r + width / 2.0, 0.0, TWO_PI, par, c);
        return;
    }
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
    if (g->rec != NULL) {
        double par[6];
        par[0] = cx; par[1] = cy; par[2] = r; par[3] = a0; par[4] = a1; par[5] = width;
        rec_arc(g, LT_OP_ARC, r - width / 2.0, r + width / 2.0, a0, a1, par, c);
        return;
    }
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
    if (g->rec != NULL) {
        double par[6];
        par[0] = cx; par[1] = cy; par[2] = r_in; par[3] = r_out; par[4] = a0; par[5] = a1;
        rec_arc(g, LT_OP_FARC, r_in, r_out, a0, a1, par, c);
        return;
    }
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
    int i, ne = 0, py, ya, yb, cx0, cx1, width, ca;
    lt_rect vc;
    double miny = 1e30, maxy = -1e30, minx = 1e30, maxx = -1e30;
    edge *edges;
    double *xs, *acc;
    if (n < 3) { return; }
    if (g->rec != NULL) {
        lt_dcmd *k;
        for (i = 0; i < n; i++) {
            double x = xy[2 * i] + g->ox, y = xy[2 * i + 1] + g->oy;
            if (x < minx) { minx = x; }
            if (x > maxx) { maxx = x; }
            if (y < miny) { miny = y; }
            if (y > maxy) { maxy = y; }
        }
        if (!(maxx >= minx) || !(maxy >= miny)) { return; }
        cx0 = ifloor(minx); ya = ifloor(miny);
        k = rec_simple(g, LT_OP_POLY, lt_rect_make(cx0, ya, iceil(maxx) + 1 - cx0,
                                                   iceil(maxy) + 1 - ya), c);
        if (k != NULL) {
            k->i[0] = n;
            k->off = lt_rec_doubles(g, xy, 2L * (long)n);
            k->len = 2L * (long)n;
            lt_rec_done(g, k);
        }
        return;
    }
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
    if (g->canvas == NULL) { return; }
    prep(g);
    vc = vis_clip(g);
    ca = LT_COLOR_A(c);
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
            if (cv > 0) { put_px_at(g, &vc, cx0 + x, py, c, ca, cv); }
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
    if (w <= 0 || h <= 0) { return; }
    if (g->rec != NULL) {
        lt_dcmd *k = rec_simple(g, LT_OP_BLIT, lt_rect_make(x + g->ox, y + g->oy, w, h), 0);
        if (k != NULL) {
            k->i[0] = x; k->i[1] = y; k->i[2] = w; k->i[3] = h;
            k->off = lt_rec_pixels(g, px, w, h, pitch);
            k->len = (long)w * (long)h;
            lt_rec_done(g, k);
        }
        return;
    }
    x += g->ox; y += g->oy;
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
    if (g->canvas == NULL) { return; }
    prep(g);
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
    int x0, y0, x1, y1, j, i, ca;
    if (c == NULL || a == NULL || w <= 0 || h <= 0) { return; }
    if (g_measure != NULL) {
        lt_rect_union_into(g_measure, lt_rect_make(x + g->ox, y + g->oy, w, h));
        return;
    }
    if (g->rec != NULL) {
        lt_dcmd *k = rec_simple(g, LT_OP_MASK, lt_rect_make(x + g->ox, y + g->oy, w, h), col);
        if (k != NULL) {
            k->i[0] = x; k->i[1] = y; k->i[2] = w; k->i[3] = h;
            k->off = lt_rec_bytes(g, a, 0);
            for (j = 0; j < h; j++) { (void)lt_rec_bytes(g, a + (size_t)j * (size_t)pitch, (long)w); }
            k->len = (long)w * (long)h;
            if (lut != NULL) { k->off2 = lt_rec_bytes(g, lut, 256); }
            lt_rec_done(g, k);
        }
        return;
    }
    prep(g);
    x += g->ox; y += g->oy;
    x0 = LT_MAX(LT_MAX(g->clip.x, 0), x);
    y0 = LT_MAX(LT_MAX(g->clip.y, 0), y);
    x1 = LT_MIN(LT_MIN(g->clip.x + g->clip.w, c->w), x + w);
    y1 = LT_MIN(LT_MIN(g->clip.y + g->clip.h, c->h), y + h);
    if (x1 <= x0 || y1 <= y0) { return; }
    ca = LT_COLOR_A(col);
    if (ca <= 0) { return; }
    {
        lt_u32 crb = col & LANES, cg = (col >> 8) & 0xFFU, opaque = (col | OPAQUE_MASK) & LT_U32_MASK;
        for (j = y0; j < y1; j++) {
            const lt_u8 *src = a + (size_t)(j - y) * (size_t)pitch + (size_t)(x0 - x);
            lt_u32 *dst = c->px + (size_t)j * (size_t)c->pitch + x0;
            for (i = 0; i < x1 - x0; i++) {
                int cv = src[i];
                lt_u32 al;
                if (cv == 0) { continue; }
                if (lut != NULL) { cv = lut[cv]; }
                al = (lt_u32)(ca >= 255 ? cv : DIV255(ca * cv));
                if (al == 0U) { continue; }
                if (al >= 255U) { dst[i] = opaque; continue; }
                dst[i] = blend_lanes(dst[i], crb * al, cg * al, 255U - al);
            }
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
    if (g->rec != NULL) {
        lt_rect bb = lt_rect_make(x + g->ox, y + g->oy, total, LT_FONT_H * scale);
        lt_dcmd *k;
        if (!g->bitmap_text) {
            lt_rect acc = lt_rect_make(0, 0, 0, 0);
            struct lt_rec *saved = g->rec;
            int ok;
            g->rec = NULL;
            lt_gfx_measure_begin(&acc);
            ok = lt_gfx_text_cellfont(g, x, y, s, c, scale);
            lt_gfx_measure_end();
            g->rec = saved;
            if (ok) { bb = acc; }
        }
        k = rec_simple(g, LT_OP_TEXT, bb, c);
        if (k != NULL) {
            k->i[0] = x; k->i[1] = y; k->i[2] = scale;
            k->len = (long)strlen(s) + 1;
            k->off = lt_rec_bytes(g, s, k->len);
            lt_rec_done(g, k);
        }
        return total;
    }
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
    prep(g);
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

/* ------------------------------------------------------------------ */
/* replay of a recorded call (gfx_rec.c)                               */
void lt_gfx_replay(lt_gfx *g, const struct lt_rec *r, const lt_dcmd *k, lt_rect lim)
{
    lt_rect sclip = g->clip, ri = lt_rect_make(k->i[0], k->i[1], k->i[2], k->i[3]);
    int sox = g->ox, soy = g->oy, sbt = g->bitmap_text;
    const char *b = k->off >= 0 ? lt_rec_bytes_at(r, k->off) : NULL;
    g->clip = lt_rect_intersect(k->clip, lim);
    g->ox = k->ox; g->oy = k->oy; g->bitmap_text = k->bitmap_text;
    switch (k->op) {
    case LT_OP_CLEAR: lt_gfx_clear(g, k->c); break;
    case LT_OP_FILL: fill_rect_abs(g, ri, k->c); break;
    case LT_OP_ROUND: lt_gfx_fill_round_rect(g, ri, (int)k->d[0], k->c); break;
    case LT_OP_VGRAD: lt_gfx_fill_rect_vgrad(g, ri, k->c, k->c2); break;
    case LT_OP_LINE: lt_gfx_line(g, k->d[0], k->d[1], k->d[2], k->d[3], k->d[4], k->c); break;
    case LT_OP_FCIRCLE: lt_gfx_fill_circle(g, k->d[0], k->d[1], k->d[2], k->c); break;
    case LT_OP_CIRCLE: lt_gfx_circle(g, k->d[0], k->d[1], k->d[2], k->d[3], k->c); break;
    case LT_OP_ARC:
        lt_gfx_arc(g, k->d[0], k->d[1], k->d[2], k->d[3], k->d[4], k->d[5], k->c);
        break;
    case LT_OP_FARC:
        lt_gfx_fill_arc(g, k->d[0], k->d[1], k->d[2], k->d[3], k->d[4], k->d[5], k->c);
        break;
    case LT_OP_POLY: lt_gfx_fill_poly(g, lt_rec_doubles_at(r, k->off), k->i[0], k->c); break;
    case LT_OP_BLIT:
        lt_gfx_blit(g, k->i[0], k->i[1], lt_rec_pixels_at(r, k->off), k->i[2], k->i[3], k->i[2]);
        break;
    case LT_OP_PREMUL: lt_gfx_blit_premul(g, k->i[0], k->i[1], lt_rec_canvas_at(r, k->src)); break;
    case LT_OP_MASK:
        lt_gfx_blit_mask(g, k->i[0], k->i[1], (const lt_u8 *)b, k->i[2], k->i[3], k->i[2], k->c,
                         k->off2 >= 0 ? (const lt_u8 *)lt_rec_bytes_at(r, k->off2) : NULL);
        break;
    case LT_OP_TEXT: (void)lt_gfx_text(g, k->i[0], k->i[1], b, k->c, k->i[2]); break;
    case LT_OP_TEXTF: (void)lt_gfx_text_run(g, k->f, k->d[0], k->d[1], k->i[0], b, k->c); break;
    case LT_OP_TEXTROT:
        lt_gfx_text_rot(g, k->f, k->d[0], k->d[1], k->d[2], b, k->d[3], k->i[0], k->i[1], k->c);
        break;
    case LT_OP_ICON: lt_gfx_icon(g, k->i[0], k->d[0], k->d[1], k->d[2], k->d[3], k->c); break;
    case LT_OP_SHADOW: lt_gfx_shadow(g, ri, (int)k->d[0], (int)k->d[1], (int)k->d[2]); break;
    default: break;
    }
    g->clip = sclip;
    g->ox = sox; g->oy = soy; g->bitmap_text = sbt;
}
