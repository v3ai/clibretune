/* gauges_cell.c - gauge and indicator painters for the terminal (cell) UI:
 * framed panels with a large block-digit value, a zone colored bar with min/max labels,
 * observed hi/avg/lo and a braille trend line that fills the rest of the panel.
 * Indicators are fixed width pills.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "gauges_cell.h"

#define CG_HIST 256
#define CG_SLOTS 64
#define CG_SAMPLE_MS 100.0

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

/* observed min / max / mean of the history */
static void cg_stats(const cg_hist *h, double *mn, double *mx, double *avg)
{
    int i;
    double sum = 0.0;
    if (h->n < 1) { *mn = *mx = *avg = 0.0; return; }
    *mn = *mx = h->v[0];
    for (i = 0; i < h->n; i++) {
        if (h->v[i] < *mn) { *mn = h->v[i]; }
        if (h->v[i] > *mx) { *mx = h->v[i]; }
        sum += h->v[i];
    }
    *avg = sum / (double)h->n;
}

/* trend scale: the history's own min/max, widened so a flat signal stays a flat line */
static void cg_scale(const cg_hist *h, double lo, double hi, double *a, double *b)
{
    double mn, mx, avg, pad = (hi - lo) * 0.03;
    if (h->n < 1) { *a = lo; *b = hi; return; }
    cg_stats(h, &mn, &mx, &avg);
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

static int cg_lum(lt_color c)
{
    return (LT_COLOR_R(c) * 3 + LT_COLOR_G(c) * 6 + LT_COLOR_B(c)) / 10;
}

static int cg_len(const char *s)
{
    int n = 0;
    while (lt_utf8_next(&s) != 0) { n++; }
    return n;
}

/* ---- label shortening ------------------------------------------------- */
static int cg_is_vowel(char c)
{
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

static int cg_ascii(const char *s)
{
    for (; *s != '\0'; s++) { if ((unsigned char)*s >= 0x80) { return 0; } }
    return 1;
}

/* copy the first cols - 1 characters of s and an ellipsis */
static void cg_cut(const char *s, int cols, char *out, size_t cap)
{
    size_t o = 0;
    int n = 0;
    const char *p = s;
    if (cols < 1 || cap < 5) { if (cap > 0) { out[0] = '\0'; } return; }
    while (n < cols - 1 && *p != '\0' && o + 5 < cap) {
        const char *q = p;
        (void)lt_utf8_next(&p);
        while (q < p && o + 5 < cap) { out[o++] = *q++; }
        n++;
    }
    out[o++] = '\xE2'; out[o++] = '\x80'; out[o++] = '\xA6';
    out[o] = '\0';
}

void lt_cell_abbrev(const char *s, int cols, char *out, size_t cap)
{
    char w[160];
    size_t len, i;
    int k, pass;
    if (cap == 0) { return; }
    if (cg_len(s) <= cols) { lt_strlcpy(out, s, cap); return; }
    if (!cg_ascii(s) || strlen(s) >= sizeof(w)) { cg_cut(s, cols, out, cap); return; }
    lt_strlcpy(w, s, sizeof(w));
    len = strlen(w);
    /* drop inner vowels, last word first, one word per step */
    for (pass = 0; pass < 2 && (int)len > cols; pass++) {
        k = (int)len - 1;
        while (k > 0 && (int)len > cols) {
            /* find the word [a, b) ending at k */
            int b = k + 1, a = k;
            while (a > 0 && w[a - 1] != ' ' && w[a - 1] != '_' && w[a - 1] != '-') { a--; }
            for (i = (size_t)b; i > (size_t)a + 1 && (int)len > cols;) {
                i--;
                if (cg_is_vowel(w[i]) && (pass == 1 || (i + 1 < (size_t)b))) {
                    memmove(w + i, w + i + 1, len - i);
                    len--;
                }
            }
            k = a - 2;
        }
    }
    /* then the spaces */
    for (i = 0; i < len && (int)len > cols;) {
        if (w[i] == ' ' && i > 0) { memmove(w + i, w + i + 1, len - i); len--; } else { i++; }
    }
    if ((int)len <= cols) { lt_strlcpy(out, w, cap); return; }
    cg_cut(w, cols, out, cap);
}

/* snap a pixel rect to whole cells the way fills do (cell centers inside), at least 1x1 cell */
static lt_rect cg_align(lt_rect r)
{
    int x0 = (r.x + 4) / 8, x1 = (r.x + r.w + 4) / 8, y0 = (r.y + 8) / 16, y1 = (r.y + r.h + 8) / 16;
    if (x1 <= x0) { x1 = x0 + 1; }
    if (y1 <= y0) { y1 = y0 + 1; }
    return lt_rect_make(x0 * 8, y0 * 16, (x1 - x0) * 8, (y1 - y0) * 16);
}

/* draw s (UTF-8) clipped to w px; align 0 left, 1 center, 2 right */
static void cg_text(lt_gfx *g, int x, int y, int w, const char *s, lt_color fg, int attr, int align)
{
    char buf[160];
    int cols = w / 8, len = cg_len(s);
    if (cols <= 0) { return; }
    if (len > cols) {
        cg_cut(s, cols, buf, sizeof(buf));
        len = cols;
        s = buf;
    }
    if (align == 1) { x += (w - len * 8) / 2; }
    else if (align == 2) { x += w - len * 8; }
    lt_gfx_cell_text(g, x, y, s, fg, 0, attr);
}

/* ---- block digits: 3x5 pixels packed two per cell row with half blocks ---- */
static const lt_u8 cg_digit[13][5] = {
    { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 },
    { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 2, 2, 2 },
    { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 }, { 0, 0, 0, 0, 1 }, { 0, 0, 7, 0, 0 },
    { 0, 0, 0, 0, 0 }
};

static int cg_glyph(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c == '.') { return 10; }
    if (c == '-') { return 11; }
    if (c == ' ') { return 12; }
    return -1;
}

/* columns needed for the block digits of s, 0 when s has other characters */
static int cg_big_cols(const char *s)
{
    int n = 0, gl;
    for (; *s != '\0'; s++) {
        gl = cg_glyph(*s);
        if (gl < 0) { return 0; }
        n += (gl == 10 ? 1 : 3) + 1;
    }
    return n > 0 ? n - 1 : 0;
}

static void cg_big_paint(lt_gfx *g, int x, int y, const char *s, lt_color fg)
{
    static const char *const part[4] = { " ", "\xE2\x96\x80", "\xE2\x96\x84", "\xE2\x96\x88" };
    int row, col, gl, gw;
    for (; *s != '\0'; s++) {
        gl = cg_glyph(*s);
        gw = gl == 10 ? 1 : 3;
        for (row = 0; row < 3; row++) {
            for (col = 0; col < gw; col++) {
                int shift = gw == 1 ? 0 : 2 - col;
                int top = (cg_digit[gl][row * 2] >> shift) & 1;
                int bot = row * 2 + 1 < 5 ? (cg_digit[gl][row * 2 + 1] >> shift) & 1 : 0;
                int pi = top + 2 * bot;
                if (pi != 0) { lt_gfx_cell_text(g, x + col * 8, y + row * 16, part[pi], fg, 0, 0); }
            }
        }
        x += (gw + 1) * 8;
    }
}

/* zone of a position 0..1 of the range: 0 normal, 1 warning, 2 critical */
static int cg_zone_of(const lt_gauge_cell *c, double f)
{
    if (c->zone_at[2] >= 0.0 && f <= c->zone_at[2]) { return 2; }
    if (c->zone_at[3] >= 0.0 && f >= c->zone_at[3]) { return 2; }
    if (c->zone_at[0] >= 0.0 && f <= c->zone_at[0]) { return 1; }
    if (c->zone_at[1] >= 0.0 && f >= c->zone_at[1]) { return 1; }
    return 0;
}

/* eighth-block bar: filled cells take the color of the zone they sit in, the empty track
 * is tinted where the warning / critical zones are */
static void cg_zbar(lt_gfx *g, int x, int y, int w, const lt_gauge_cell *c, lt_color panel,
                    lt_color track)
{
    static const char *const part[8] = { " ", "\xE2\x96\x8F", "\xE2\x96\x8E", "\xE2\x96\x8D",
                                          "\xE2\x96\x8C", "\xE2\x96\x8B", "\xE2\x96\x8A", "\xE2\x96\x89" };
    int cols = w / 8, i, eighths;
    double f = c->frac;
    LT_UNUSED(panel);
    if (cols < 1) { return; }
    if (!(f > 0.0)) { f = 0.0; }
    if (f > 1.0) { f = 1.0; }
    eighths = (int)(f * (double)(cols * 8) + 0.5);
    for (i = 0; i < cols; i++) {
        double mid = ((double)i + 0.5) / (double)cols;
        int z = cg_zone_of(c, mid);
        lt_color zc = z >= 2 ? c->crit : (z == 1 ? c->warn : c->base);
        lt_color tb = z >= 2 ? cg_mix(track, c->crit, 52) : (z == 1 ? cg_mix(track, c->warn, 46) : track);
        int e = eighths - i * 8;
        if (e >= 8) {
            lt_gfx_cell_text(g, x + i * 8, y, "\xE2\x96\x88", zc, tb, 0);
        } else if (e > 0) {
            lt_gfx_cell_text(g, x + i * 8, y, part[e], zc, tb, 0);
        } else {
            lt_gfx_cell_text(g, x + i * 8, y, " ", zc, tb, 0);
        }
    }
}

void lt_gauge_cells_paint(lt_gfx *g, lt_rect r, const lt_gauge_cell *c)
{
    int cols, rows, x0, w, y, inner;
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
    track = cg_mix(panel, LT_RGB(255, 255, 255), 24);
    line = c->zone >= 2 ? c->crit : (c->zone == 1 ? c->warn : cg_mix(panel, LT_RGB(150, 165, 190), 110));
    lt_gfx_fill_rect(g, r, panel);
    lt_snprintf(vu, sizeof(vu), "%s%s%s", c->value_text, c->units[0] != '\0' ? " " : "", c->units);
    inner = rows - 2;
    if (rows >= 4 && cols >= 10) {
        char cap[120], hi[48], lo[48], av[48], tmp[64];
        double mn, mx, avg, a, b;
        int units_in_title, big, vh, spark_rows, big_w, used;
        lt_gfx_rect(g, r, line);
        units_in_title = c->units[0] != '\0' && cg_len(c->title) + cg_len(c->units) + 8 <= cols;
        lt_snprintf(cap, sizeof(cap), " %s ", c->title);
        cg_text(g, r.x + 8, r.y, r.w - 16 - (units_in_title ? (cg_len(c->units) + 2) * 8 : 0), cap, tc,
                LT_CELL_BOLD, 0);
        if (units_in_title) {
            lt_snprintf(tmp, sizeof(tmp), " %s ", c->units);
            cg_text(g, r.x + r.w - 16 - cg_len(tmp) * 8 + 8, r.y, cg_len(tmp) * 8, tmp, muted, 0, 0);
        }
        x0 = r.x + 16;
        w = r.w - 32;
        if (w < 8) { x0 = r.x + 8; w = r.w - 16; }
        y = r.y + 16;
        cg_stats(h, &mn, &mx, &avg);
        lt_snprintf(hi, sizeof(hi), "%.*f", c->digits, mx);
        lt_snprintf(lo, sizeof(lo), "%.*f", c->digits, mn);
        lt_snprintf(av, sizeof(av), "%.*f", c->digits, avg);
        big_w = cg_big_cols(c->value_text);
        big = big_w > 0 && big_w * 8 <= w && inner >= 7;
        vh = big ? 3 : 1;
        if (big) {
            cg_big_paint(g, x0, y, c->value_text, c->value_fg);
            if (!units_in_title && c->units[0] != '\0' && (big_w + 1 + cg_len(c->units)) * 8 <= w) {
                cg_text(g, x0 + (big_w + 1) * 8, y + 32, w - (big_w + 1) * 8, c->units, muted, 0, 0);
            }
            /* observed hi / avg / lo beside the digits */
            if (w - big_w * 8 >= 9 * 8 && h->n > 1) {
                int sx = x0 + big_w * 8 + 8, sw = w - big_w * 8 - 8;
                lt_snprintf(tmp, sizeof(tmp), "hi %s", hi);
                cg_text(g, sx, y, sw, tmp, muted, 0, 2);
                lt_snprintf(tmp, sizeof(tmp), "avg %s", av);
                cg_text(g, sx, y + 16, sw, tmp, muted, 0, 2);
                lt_snprintf(tmp, sizeof(tmp), "lo %s", lo);
                cg_text(g, sx, y + 32, sw, tmp, muted, 0, 2);
            }
        } else {
            cg_text(g, x0, y, w, c->value_text, c->value_fg, LT_CELL_BOLD, 0);
            if (!units_in_title && c->units[0] != '\0') {
                int vw = cg_len(c->value_text) * 8 + 8;
                if (vw < w) { cg_text(g, x0 + vw, y, w - vw, c->units, muted, 0, 0); }
            }
            lt_snprintf(tmp, sizeof(tmp), "hi %s  lo %s", hi, lo);
            used = cg_len(c->value_text) + (units_in_title || c->units[0] == '\0' ? 0 : 1 + cg_len(c->units));
            if (h->n > 1 && (used + 2 + cg_len(tmp)) * 8 <= w) {
                cg_text(g, x0, y, w, tmp, muted, 0, 2);
            }
        }
        y += vh * 16;
        cg_zbar(g, x0, y, w, c, panel, track);
        y += 16;
        if (inner >= vh + 2) {
            if ((cg_len(c->min_text) + cg_len(c->max_text) + 1) * 8 <= w) {
                cg_text(g, x0, y, w, c->min_text, muted, 0, 0);
                cg_text(g, x0, y, w, c->max_text, muted, 0, 2);
            } else {
                cg_text(g, x0, y, w, c->max_text, muted, 0, 2);
            }
            y += 16;
        }
        spark_rows = (r.y + r.h - 16 - y) / 16;
        if (spark_rows >= 1) {
            lt_color pc = c->zone >= 2 ? c->crit : (c->zone == 1 ? c->warn : c->base);
            lt_rect pr = lt_rect_make(x0, y, w, spark_rows * 16);
            cg_scale(h, c->lo, c->hi, &a, &b);
            lt_gfx_cell_plot(g, pr, h->v, h->n, a, b, cg_mix(pc, panel, 150), panel, LT_PLOT_CLEAR | LT_PLOT_FILL);
            lt_gfx_cell_plot(g, pr, h->v, h->n, a, b, pc, panel, 0);
        }
    } else if (rows >= 2) {
        x0 = r.x + 8;
        w = r.w - 16;
        cg_text(g, x0, r.y, w, c->title, tc, 0, 0);
        cg_text(g, x0, r.y, w, vu, c->value_fg, LT_CELL_BOLD, 2);
        cg_zbar(g, x0, r.y + 16, w, c, panel, track);
        if (rows >= 3) {
            double a, b;
            lt_rect pr = lt_rect_make(x0, r.y + 32, w, (rows - 2) * 16);
            cg_scale(h, c->lo, c->hi, &a, &b);
            lt_gfx_cell_plot(g, pr, h->v, h->n, a, b, cg_mix(c->base, panel, 150), panel, LT_PLOT_CLEAR | LT_PLOT_FILL);
            lt_gfx_cell_plot(g, pr, h->v, h->n, a, b, c->base, panel, 0);
        }
    } else {
        cg_text(g, r.x + 8, r.y, r.w - 16, c->title, tc, 0, 0);
        cg_text(g, r.x + 8, r.y, r.w - 16, vu, c->value_fg, LT_CELL_BOLD, 2);
    }
    lt_gfx_pop_clip(g, prev);
}

/* a pill: marker, then the label shortened to what is left; lit ones in their own colors */
void lt_indicator_cells_paint(lt_gfx *g, lt_rect r, const char *text, lt_color on_bg, lt_color on_fg,
                              lt_color off_bg, lt_color off_fg, lt_bool on)
{
    lt_rect prev;
    lt_color bg, fg, mk;
    char lab[160];
    int cols;
    r = cg_align(r);
    cols = r.w / 8;
    prev = lt_gfx_push_clip(g, r);
    if (on) {
        bg = on_bg;
        if (cg_lum(bg) < 36) { bg = LT_RGB(34, 139, 80); }
        fg = cg_lum(bg) > 150 ? LT_RGB(16, 18, 22) : LT_RGB(255, 255, 255);
        mk = fg;
        LT_UNUSED(on_fg);
    } else {
        LT_UNUSED(off_bg);
        LT_UNUSED(off_fg);
        bg = LT_RGB(26, 29, 35);
        fg = LT_RGB(120, 128, 144);
        mk = LT_RGB(70, 76, 90);
    }
    lt_gfx_fill_rect(g, r, bg);
    if (text != NULL && cols >= 3) {
        lt_cell_abbrev(text, cols - 3, lab, sizeof(lab));
        lt_gfx_cell_text(g, r.x + 8, r.y, on ? "\xE2\x97\x8F" : "\xE2\x97\x8B", mk, 0, 0);
        cg_text(g, r.x + 24, r.y, r.w - 24, lab, fg, on ? LT_CELL_BOLD : 0, 0);
    } else if (text != NULL) {
        cg_text(g, r.x, r.y, r.w, text, fg, on ? LT_CELL_BOLD : 0, 1);
    }
    lt_gfx_pop_clip(g, prev);
}
