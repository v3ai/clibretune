/* gauges_cell.c - gauge and indicator painters for the terminal (cell) UI:
 * framed panels with a bold value, an eighth-block bar with min/max labels
 * and a sparkline of the recent history.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "gauges_cell.h"

#define CG_HIST 96
#define CG_SLOTS 64
#define CG_SAMPLE_MS 200.0

typedef struct cg_hist {
    const void *key;
    double v[CG_HIST];
    int n;
    double last_ms;
} cg_hist;

static cg_hist *cg_tab = NULL;      /* allocated on first use (too big for a static) */
static int cg_next = 0;

static cg_hist *cg_get(const void *key)
{
    int i;
    cg_hist *h;
    if (cg_tab == NULL) { cg_tab = (cg_hist *)lt_calloc(CG_SLOTS, sizeof(cg_hist)); }
    for (i = 0; i < CG_SLOTS; i++) {
        if (cg_tab[i].key == key) { return &cg_tab[i]; }
    }
    h = &cg_tab[cg_next];
    cg_next = (cg_next + 1) % CG_SLOTS;
    memset(h, 0, sizeof(*h));
    h->key = key;
    h->last_ms = -1.0e9;
    return h;
}

static void cg_push(cg_hist *h, double v, double now)
{
    if (now - h->last_ms < CG_SAMPLE_MS) { return; }
    h->last_ms = now;
    if (h->n >= CG_HIST) {
        memmove(h->v, h->v + 1, sizeof(h->v[0]) * (CG_HIST - 1));
        h->n = CG_HIST - 1;
    }
    h->v[h->n++] = v;
}

/* trend scale: the history's own min/max, widened so a flat signal stays a flat line */
static void cg_scale(const cg_hist *h, double lo, double hi, double *a, double *b)
{
    int i;
    double mn, mx, pad = (hi - lo) * 0.02;
    if (h->n < 1) { *a = lo; *b = hi; return; }
    mn = mx = h->v[0];
    for (i = 1; i < h->n; i++) {
        if (h->v[i] < mn) { mn = h->v[i]; }
        if (h->v[i] > mx) { mx = h->v[i]; }
    }
    if (mx - mn < pad * 2.0) { double mid = (mx + mn) / 2.0; mn = mid - pad; mx = mid + pad; }
    *a = mn;
    *b = mx;
}

static lt_color cg_mix(lt_color a, lt_color b, int t)
{
    int r = (LT_COLOR_R(a) * (256 - t) + LT_COLOR_R(b) * t) / 256;
    int g = (LT_COLOR_G(a) * (256 - t) + LT_COLOR_G(b) * t) / 256;
    int bl = (LT_COLOR_B(a) * (256 - t) + LT_COLOR_B(b) * t) / 256;
    return LT_RGB(r, g, bl);
}

static int cg_len(const char *s)
{
    int n = 0;
    while (lt_utf8_next(&s) != 0) { n++; }
    return n;
}

/* snap a pixel rect to whole cells the way fills do (cell centers inside), at least 1x1 cell */
static lt_rect cg_align(lt_rect r)
{
    int x0 = (r.x + 4) / 8, x1 = (r.x + r.w + 4) / 8, y0 = (r.y + 8) / 16, y1 = (r.y + r.h + 8) / 16;
    if (x1 <= x0) { x1 = x0 + 1; }
    if (y1 <= y0) { y1 = y0 + 1; }
    return lt_rect_make(x0 * 8, y0 * 16, (x1 - x0) * 8, (y1 - y0) * 16);
}

/* draw s (UTF-8) clipped to maxcols columns; align 0 left, 1 center, 2 right */
static void cg_text(lt_gfx *g, int x, int y, int w, const char *s, lt_color fg, int attr, int align)
{
    char buf[160];
    int n = 0, cols = w / 8, len = cg_len(s);
    const char *p = s;
    size_t o = 0;
    if (cols <= 0) { return; }
    if (len > cols) {
        /* truncate with an ellipsis */
        while (n < cols - 1 && *p != '\0' && o + 8 < sizeof(buf)) {
            const char *q = p;
            (void)lt_utf8_next(&p);
            while (q < p && o + 5 < sizeof(buf)) { buf[o++] = *q++; }
            n++;
        }
        buf[o++] = '\xE2'; buf[o++] = '\x80'; buf[o++] = '\xA6';
        buf[o] = '\0';
        len = cols;
        s = buf;
    }
    if (align == 1) { x += (w - len * 8) / 2; }
    else if (align == 2) { x += w - len * 8; }
    lt_gfx_cell_text(g, x, y, s, fg, 0, attr);
}

void lt_gauge_cells_paint(lt_gfx *g, lt_rect r, const lt_gauge_cell *c)
{
    int cols, rows, x0, w, y;
    lt_color panel = c->back, line, track, tc, muted;
    lt_rect prev;
    char vu[160];
    cg_hist *h = cg_get(c->key);
    r = cg_align(r);
    cols = r.w / 8;
    rows = r.h / 16;
    if (cols < 3 || rows < 1) { return; }
    cg_push(h, c->value, c->now_ms);
    prev = lt_gfx_push_clip(g, r);
    if (LT_COLOR_R(panel) + LT_COLOR_G(panel) + LT_COLOR_B(panel) > 330) { panel = LT_RGB(18, 20, 24); }
    muted = cg_mix(c->title_fg, panel, 90);
    tc = c->title_fg;
    track = cg_mix(panel, LT_RGB(255, 255, 255), 26);
    line = c->zone >= 2 ? c->crit : (c->zone == 1 ? c->warn : cg_mix(panel, LT_RGB(150, 165, 190), 110));
    lt_gfx_fill_rect(g, r, panel);
    lt_snprintf(vu, sizeof(vu), "%s%s%s", c->value_text, c->units[0] != '\0' ? " " : "", c->units);
    if (rows >= 4 && cols >= 10) {
        int spark_rows;
        char cap[120];
        lt_gfx_rect(g, r, line);
        lt_snprintf(cap, sizeof(cap), " %s ", c->title);
        cg_text(g, r.x + 8, r.y, r.w - 16, cap, tc, LT_CELL_BOLD, 0);
        x0 = r.x + 16;
        w = r.w - 32;
        if (w < 8) { x0 = r.x + 8; w = r.w - 16; }
        y = r.y + 16;
        cg_text(g, x0, y, w, c->value_text, c->value_fg, LT_CELL_BOLD, 0);
        if (c->units[0] != '\0') {
            int vw = cg_len(c->value_text) * 8 + 8;
            if (vw < w) { cg_text(g, x0 + vw, y, w - vw, c->units, muted, 0, 0); }
        }
        y += 16;
        lt_gfx_cell_hbar(g, lt_rect_make(x0, y, w, 16), c->frac, c->bar, track);
        y += 16;
        if (rows >= 5) {
            if ((cg_len(c->min_text) + cg_len(c->max_text) + 1) * 8 <= w) {
                cg_text(g, x0, y, w, c->min_text, muted, 0, 0);
                cg_text(g, x0, y, w, c->max_text, muted, 0, 2);
            } else {
                cg_text(g, x0, y, w, c->max_text, muted, 0, 2);
            }
            y += 16;
        }
        spark_rows = (r.y + r.h - 16 - y) / 16;
        if (spark_rows > 6) { spark_rows = 6; }
        if (spark_rows >= 1) {
            double a, b;
            cg_scale(h, c->lo, c->hi, &a, &b);
            lt_gfx_cell_spark(g, lt_rect_make(x0, y, w, spark_rows * 16), h->v, h->n, a, b,
                              cg_mix(c->bar, panel, 80), panel);
        }
    } else if (rows >= 2) {
        x0 = r.x + 8;
        w = r.w - 16;
        cg_text(g, x0, r.y, w, c->title, tc, 0, 0);
        cg_text(g, x0, r.y, w, vu, c->value_fg, LT_CELL_BOLD, 2);
        lt_gfx_cell_hbar(g, lt_rect_make(x0, r.y + 16, w, 16), c->frac, c->bar, track);
        if (rows >= 3) {
            double a, b;
            cg_scale(h, c->lo, c->hi, &a, &b);
            lt_gfx_cell_spark(g, lt_rect_make(x0, r.y + 32, w, (rows - 2) * 16 > 32 ? 32 : (rows - 2) * 16),
                              h->v, h->n, a, b, cg_mix(c->bar, panel, 80), panel);
        }
    } else {
        char one[200];
        lt_snprintf(one, sizeof(one), "%s", c->title);
        cg_text(g, r.x + 8, r.y, r.w - 16, one, tc, 0, 0);
        cg_text(g, r.x + 8, r.y, r.w - 16, vu, c->value_fg, LT_CELL_BOLD, 2);
    }
    lt_gfx_pop_clip(g, prev);
}

void lt_indicator_cells_paint(lt_gfx *g, lt_rect r, const char *text, lt_color on_bg, lt_color on_fg,
                              lt_color off_bg, lt_color off_fg, lt_bool on)
{
    lt_rect prev;
    lt_color bg, fg;
    r = cg_align(r);
    prev = lt_gfx_push_clip(g, r);
    if (on) {
        bg = on_bg;
        fg = on_fg;
        if (LT_COLOR_R(bg) + LT_COLOR_G(bg) + LT_COLOR_B(bg) < 90) { bg = LT_RGB(34, 139, 80); fg = LT_RGB(255, 255, 255); }
    } else {
        LT_UNUSED(off_bg);
        LT_UNUSED(off_fg);
        bg = LT_RGB(26, 29, 35);
        fg = LT_RGB(104, 112, 128);
    }
    lt_gfx_fill_rect(g, r, bg);
    if (text != NULL) { cg_text(g, r.x, r.y, r.w, text, fg, on ? LT_CELL_BOLD : 0, 1); }
    lt_gfx_pop_clip(g, prev);
}
