/* Minimal stand-ins for gfx symbols, used by run_x11_mock.sh only when the
 * real gfx module is absent. */
#include "../src/gui/gui.h"
#include <stdlib.h>
lt_rect lt_rect_make(int x, int y, int w, int h) { lt_rect r; r.x = x; r.y = y; r.w = w; r.h = h; return r; }
lt_rect lt_rect_intersect(lt_rect a, lt_rect b) {
    int x0 = LT_MAX(a.x, b.x), y0 = LT_MAX(a.y, b.y);
    int x1 = LT_MIN(a.x + a.w, b.x + b.w), y1 = LT_MIN(a.y + a.h, b.y + b.h);
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    return lt_rect_make(x0, y0, x1 - x0, y1 - y0);
}
void lt_canvas_init(lt_canvas *c, int w, int h) {
    c->w = w; c->h = h; c->stride = w;
    c->px = (lt_u32 *)calloc((size_t)w * (size_t)h, sizeof(lt_u32));
}
void lt_canvas_free(lt_canvas *c) { free(c->px); c->px = NULL; }
