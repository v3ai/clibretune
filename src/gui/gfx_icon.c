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

void lt_gfx_icon(lt_gfx *g, int icon, double x, double y, double size_px, double stroke_px,
                 lt_color c)
{
    const lt_s16 *d = lt_icon_data(icon);
    lt_u8 stack_mask[SMALL_MASK];
    lt_u8 *mask = stack_mask;
    double scale, r, fx, fy;
    int margin, ix, iy, mw, mh;
    size_t cells;
    if (d == NULL || g->canvas == NULL || g->cells != NULL) { return; }
    if (!(size_px >= 1.0) || size_px > (double)ICON_MAX_PX) { return; }
    if (!(fabs(x) < 1.0e6) || !(fabs(y) < 1.0e6)) { return; }
    scale = size_px / ICON_GRID;
    if (!(stroke_px > 0.0)) { stroke_px = 2.0 * scale; }
    r = stroke_px / 2.0;
    margin = (int)ceil(r) + 1;
    ix = (int)floor(x);
    iy = (int)floor(y);
    fx = x - (double)ix + (double)margin;
    fy = y - (double)iy + (double)margin;
    mw = (int)ceil(size_px) + 1 + 2 * margin;
    mh = mw;
    cells = (size_t)mw * (size_t)mh;
    if (cells > sizeof(stack_mask)) {
        mask = (lt_u8 *)lt_malloc(cells);
    }
    memset(mask, 0, cells);
    while (d[0] != 0) {
        int n = (int)(d[0] & 0x3FFF), closed = (d[0] & 0x4000) != 0, k;
        const lt_s16 *p = d + 1;
        double px0 = (double)p[0] / ICON_UNIT * scale + fx;
        double py0 = (double)p[1] / ICON_UNIT * scale + fy;
        double lx = px0, ly = py0;
        for (k = 1; k < n; k++) {
            double qx = (double)p[2 * k] / ICON_UNIT * scale + fx;
            double qy = (double)p[2 * k + 1] / ICON_UNIT * scale + fy;
            icon_segment(mask, mw, mh, lx, ly, qx, qy, r);
            lx = qx; ly = qy;
        }
        if (closed && n > 1) {
            icon_segment(mask, mw, mh, lx, ly, px0, py0, r);
        }
        d += 1 + 2 * n;
    }
    lt_gfx_blit_mask(g, ix - margin, iy - margin, mask, mw, mh, mw, c, lt_gfx_contrast_lut(c));
    if (mask != stack_mask) { lt_free(mask); }
}
