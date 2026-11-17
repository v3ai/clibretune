/* gfx_diff.c - frame differencing for backends that present sub-rectangles.
 * The previous presented frame is kept; the new one is compared per 64x64 tile (a memcmp of a
 * tile row is far cheaper than shipping the pixels) and only changed tiles are reported, merged
 * into a few rectangles.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "gui.h"

#define TILE 64

void lt_diff_free(lt_diff *d)
{
    if (d->prev != NULL) { lt_free(d->prev); }
    d->prev = NULL;
    d->w = d->h = 0;
}

void lt_diff_invalidate(lt_diff *d)
{
    d->w = d->h = 0;   /* next lt_diff_rects reports the whole frame */
}

/* a rect is "open" while the tile rows below it keep the same horizontal extent */
int lt_diff_rects(lt_diff *d, const lt_canvas *cur, lt_rect area, lt_rect *out, int max_out)
{
    int tw, th, tx, ty, n = 0, i;
    lt_rect full;
    if (cur == NULL || cur->px == NULL || cur->w <= 0 || cur->h <= 0 || max_out < 1) { return 0; }
    full = lt_rect_make(0, 0, cur->w, cur->h);
    if (d->prev == NULL || d->w != cur->w || d->h != cur->h) {
        lt_diff_free(d);
        d->prev = (lt_u32 *)lt_malloc((size_t)cur->w * (size_t)cur->h * sizeof(lt_u32));
        d->w = cur->w; d->h = cur->h;
        for (ty = 0; ty < cur->h; ty++) {
            memcpy(d->prev + (size_t)ty * (size_t)cur->w, cur->px + (size_t)ty * (size_t)cur->pitch,
                   (size_t)cur->w * sizeof(lt_u32));
        }
        out[0] = full;
        return 1;
    }
    area = lt_rect_intersect(area, full);
    if (area.w <= 0 || area.h <= 0) { return 0; }
    tw = (cur->w + TILE - 1) / TILE; th = (cur->h + TILE - 1) / TILE;
    for (ty = area.y / TILE; ty * TILE < area.y + area.h && ty < th; ty++) {
        int y0 = ty * TILE, y1 = LT_MIN(y0 + TILE, cur->h), run_start = -1;
        int row_rects_first = n;
        for (tx = area.x / TILE; tx <= tw; tx++) {
            int changed = 0;
            if (tx < tw && tx * TILE < area.x + area.w) {
                int x0 = tx * TILE, x1 = LT_MIN(x0 + TILE, cur->w), y;
                for (y = y0; y < y1; y++) {
                    if (memcmp(d->prev + (size_t)y * (size_t)cur->w + (size_t)x0,
                               cur->px + (size_t)y * (size_t)cur->pitch + (size_t)x0,
                               (size_t)(x1 - x0) * sizeof(lt_u32)) != 0) {
                        changed = 1;
                        break;
                    }
                }
                if (changed) {
                    for (y = y0; y < y1; y++) {
                        memcpy(d->prev + (size_t)y * (size_t)cur->w + (size_t)x0,
                               cur->px + (size_t)y * (size_t)cur->pitch + (size_t)x0,
                               (size_t)(x1 - x0) * sizeof(lt_u32));
                    }
                }
            }
            if (changed) {
                if (run_start < 0) { run_start = tx; }
            } else if (run_start >= 0) {
                int rx = run_start * TILE, rw = LT_MIN(tx * TILE, cur->w) - rx, merged = 0;
                for (i = 0; i < row_rects_first; i++) {
                    if (out[i].x == rx && out[i].w == rw && out[i].y + out[i].h == y0) {
                        out[i].h = y1 - out[i].y;
                        merged = 1;
                        break;
                    }
                }
                if (!merged) {
                    if (n >= max_out) {
                        /* too fragmented: collapse into the bounding box of everything so far */
                        lt_rect b = out[0];
                        int k;
                        for (k = 1; k < n; k++) {
                            int ex = LT_MAX(b.x + b.w, out[k].x + out[k].w);
                            int ey = LT_MAX(b.y + b.h, out[k].y + out[k].h);
                            b.x = LT_MIN(b.x, out[k].x); b.y = LT_MIN(b.y, out[k].y);
                            b.w = ex - b.x; b.h = ey - b.y;
                        }
                        {
                            int ex = LT_MAX(b.x + b.w, rx + rw), ey = LT_MAX(b.y + b.h, y1);
                            b.x = LT_MIN(b.x, rx); b.y = LT_MIN(b.y, y0);
                            b.w = ex - b.x; b.h = ey - b.y;
                        }
                        out[0] = b; n = 1; row_rects_first = 1;
                    } else {
                        out[n].x = rx; out[n].y = y0; out[n].w = rw; out[n].h = y1 - y0;
                        n++;
                    }
                }
                run_start = -1;
            }
        }
    }
    return n;
}
