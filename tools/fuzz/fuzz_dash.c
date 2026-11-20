/* fuzz_dash.c - .dash / .gauge files: parse, validate, write back, build a view, update and
 * render it on a small canvas (what opening a dashboard does). */
#include "fuzz_common.h"
#include "dash/dash.h"
#include "gui/gui.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static lt_canvas cv;
    static lt_gfx g;
    static int ready;
    char err[160];
    char *s = fz_cstr(data, size);
    lt_dash_file *d;
    lt_gauge_file gf;
    lt_buf b;
    if (!ready) {
        lt_canvas_init(&cv, 160, 120);
        lt_gfx_init_canvas(&g, &cv);
        ready = 1;
    }
    d = lt_dash_parse(s, size, err, sizeof err);
    if (d) {
        lt_dash_report r;
        lt_dash_view *v;
        lt_rect area;
        lt_buf_init(&b);
        lt_dash_write(d, &b);
        lt_buf_free(&b);
        lt_dash_validate(d, NULL, 0, &r);
        lt_dash_report_free(&r);
        v = lt_dash_view_new(d);
        lt_dash_view_update(v, 0.0, NULL);
        lt_dash_view_update(v, 2500.0, NULL);
        area.x = 0; area.y = 0; area.w = 160; area.h = 120;
        lt_dash_view_render(v, &g, area, NULL);
        (void)lt_dash_view_hit(v, 30, 30);
        lt_dash_view_free(v);
        lt_dash_free(d);
    }
    memset(&gf, 0, sizeof gf);
    if (lt_gaugefile_parse(s, size, &gf, err, sizeof err) == 0) {
        lt_buf_init(&b);
        lt_gaugefile_write(&gf, &b);
        lt_buf_free(&b);
        lt_gaugefile_free(&gf);
    }
    free(s);
    return 0;
}
