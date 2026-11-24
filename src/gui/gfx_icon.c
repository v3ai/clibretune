/* gfx_icon.c - draw Lucide icons: stroked polylines with round caps and
 * joins, rendered into a coverage mask (distance-based anti-aliasing, max
 * over segments so overlaps never double-blend) and blitted with the
 * requested color. Data comes from icons_data.c (tools/gen_icons.py).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>
#include "gui.h"
#include "gfx_priv.h"

#define ICON_UNIT 32.0          /* stored units per icon unit */
#define ICON_GRID 24.0
#define ICON_MAX_PX 512
#define SMALL_MASK 4096

static int key_cmp(const char *k, const char *a)
{
    return strcmp(k, a);
}

int lt_icon_find(const char *name)
{
    char key[64];
    size_t n = 0;
    int lo, hi;
    if (name == NULL) { return LT_ICON_NONE; }
    for (; *name != '\0'; name++) {
        int ch = (unsigned char)*name;
        if (ch == '-' || ch == '_' || ch == ' ') { continue; }
        if (ch >= 'A' && ch <= 'Z') { ch = ch - 'A' + 'a'; }
        if (n + 1 >= sizeof(key)) { return LT_ICON_NONE; }
        key[n++] = (char)ch;
    }
    key[n] = '\0';
    lo = 0;
    hi = lt_icon_alias_count() - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2, id = 0, r;
        const char *a = lt_icon_alias_at(mid, &id);
        r = key_cmp(key, a);
        if (r == 0) { return id; }
        if (r < 0) { hi = mid - 1; } else { lo = mid + 1; }
    }
    return LT_ICON_NONE;
}

/* coverage of a stroke of radius r around segment (ax,ay)-(bx,by),
 * accumulated into mask (max) */
static void icon_segment(lt_u8 *mask, int mw, int mh, double ax, double ay, double bx,
                           double by, double r)
{
    double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy, reach = r + 1.0;
    int x0 = (int)floor((ax < bx ? ax : bx) - reach), x1 = (int)ceil((ax < bx ? bx : ax) + reach);
    int y0 = (int)floor((ay < by ? ay : by) - reach), y1 = (int)ceil((ay < by ? by : ay) + reach);
    int i, j;
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > mw) { x1 = mw; }
    if (y1 > mh) { y1 = mh; }
    for (j = y0; j < y1; j++) {
        lt_u8 *row = mask + (size_t)j * (size_t)mw;
        double py = (double)j + 0.5;
        for (i = x0; i < x1; i++) {
            double px = (double)i + 0.5, t = 0.0, ex, ey, cov;
            int v;
            if (len2 > 0.0) {
                t = ((px - ax) * dx + (py - ay) * dy) / len2;
                if (t < 0.0) { t = 0.0; }
                if (t > 1.0) { t = 1.0; }
            }
            ex = px - (ax + t * dx);
            ey = py - (ay + t * dy);
            cov = r + 0.5 - sqrt(ex * ex + ey * ey);
            if (cov <= 0.0) { continue; }
            if (cov > 1.0) { cov = 1.0; }
            v = (int)(cov * 255.0 + 0.5);
            if (v > (int)row[i]) { row[i] = (lt_u8)v; }
        }
    }
}

/* Rasterised icon masks are cached by icon, size, stroke and the sub-pixel position quantised
 * to 1/16 px: the stroke rasteriser is far costlier than a blit. Four ways per set, the least
 * recently used one replaced: a sidebar shows well over a hundred icons, and a direct-mapped
 * table re-rasterised the colliding ones on every redraw. */
#define ICON_SETS 128
#define ICON_WAYS 4
typedef struct icon_cached {
    int icon, fxq, fyq, mw;
    double size_px, stk;
    lt_u8 *mask;
    unsigned long used;
} icon_cached;
static icon_cached icon_cache[ICON_SETS * ICON_WAYS];
static unsigned long icon_clock;

void lt_icon_cache_release(void)
{
    int i;
    for (i = 0; i < ICON_SETS * ICON_WAYS; i++) {
        lt_free(icon_cache[i].mask);
        memset(&icon_cache[i], 0, sizeof(icon_cache[i]));
    }
}

void lt_gfx_icon(lt_gfx *g, int icon, double x, double y, double size_px, double stroke_px,
                 lt_color c)
{
    const lt_s16 *d = lt_icon_data(icon);
    double scale, r, fx, fy, stroke_in = stroke_px;
    int margin, ix, iy, mw, mh, fxq, fyq;
    size_t cells;
    icon_cached *e, *set;
    unsigned h;
    int w;
    if (d == NULL || g->canvas == NULL || g->cells != NULL) { return; }
    if (!(size_px >= 1.0) || size_px > (double)ICON_MAX_PX) { return; }
    if (!(fabs(x) < 1.0e6) || !(fabs(y) < 1.0e6)) { return; }
    scale = size_px / ICON_GRID;
    if (!(stroke_px > 0.0)) { stroke_px = 2.0 * scale; }
    r = stroke_px / 2.0;
    margin = (int)ceil(r) + 1;
    ix = (int)floor(x);
    iy = (int)floor(y);
    fxq = (int)((x - (double)ix) * 16.0 + 0.5);
    fyq = (int)((y - (double)iy) * 16.0 + 0.5);
    fx = (double)fxq / 16.0 + (double)margin;
    fy = (double)fyq / 16.0 + (double)margin;
    mw = (int)ceil(size_px) + 1 + 2 * margin;
    mh = mw;
    if (g->rec != NULL) {
        lt_dcmd *k = lt_rec_cmd(g, LT_OP_ICON, lt_rect_make(ix - margin + g->ox, iy - margin + g->oy, mw, mh));
        if (k != NULL) {
            k->c = c; k->i[0] = icon;
            k->d[0] = x; k->d[1] = y; k->d[2] = size_px; k->d[3] = stroke_in;
            lt_rec_done(g, k);
        }
        return;
    }
    cells = (size_t)mw * (size_t)mh;
    h = ((unsigned)icon * 73856093U) ^ ((unsigned)fxq * 19349663U) ^ ((unsigned)fyq * 83492791U) ^
        ((unsigned)(size_px * 8.0) * 2654435761U);
    set = &icon_cache[(h % ICON_SETS) * ICON_WAYS];
    e = NULL;
    for (w = 0; w < ICON_WAYS; w++) {
        icon_cached *q = &set[w];
        if (q->mask != NULL && q->icon == icon && q->fxq == fxq && q->fyq == fyq && q->mw == mw &&
            q->size_px == size_px && q->stk == stroke_px) {
            e = q;
            break;
        }
    }
    icon_clock++;
    if (e == NULL) {
        e = &set[0];
        for (w = 1; w < ICON_WAYS; w++) {
            if (set[w].mask == NULL || (e->mask != NULL && set[w].used < e->used)) { e = &set[w]; }
        }
        if (e->mask != NULL) { lt_free(e->mask); }
        e->mask = (lt_u8 *)lt_malloc(cells);
        e->icon = icon; e->fxq = fxq; e->fyq = fyq; e->mw = mw;
        e->size_px = size_px; e->stk = stroke_px;
        memset(e->mask, 0, cells);
        while (d[0] != 0) {
            int n = (int)(d[0] & 0x3FFF), closed = (d[0] & 0x4000) != 0, k;
            const lt_s16 *p = d + 1;
            double px0 = (double)p[0] / ICON_UNIT * scale + fx;
            double py0 = (double)p[1] / ICON_UNIT * scale + fy;
            double lx = px0, ly = py0;
            for (k = 1; k < n; k++) {
                double qx = (double)p[2 * k] / ICON_UNIT * scale + fx;
                double qy = (double)p[2 * k + 1] / ICON_UNIT * scale + fy;
                icon_segment(e->mask, mw, mh, lx, ly, qx, qy, r);
                lx = qx; ly = qy;
            }
            if (closed && n > 1) {
                icon_segment(e->mask, mw, mh, lx, ly, px0, py0, r);
            }
            d += 1 + 2 * n;
        }
    }
    e->used = icon_clock;
    lt_gfx_blit_mask(g, ix - margin, iy - margin, e->mask, mw, mh, mw, c, lt_gfx_contrast_lut(c));
}
