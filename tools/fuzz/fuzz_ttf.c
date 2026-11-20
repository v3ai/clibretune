/* fuzz_ttf.c - TrueType fonts (user fonts and fonts embedded in dashboards): open, cmap,
 * kerning, metrics, boxes and rasterization at a few sizes. */
#include "fuzz_common.h"
#include "gui/ttf.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    lt_ttf *f = lt_ttf_open(data, size, LT_TRUE);
    lt_u32 cp;
    int g, ng, x0, y0, x1, y1;
    double m[4], sx, sy;
    lt_ttf_bitmap rb;
    if (!f) return 0;
    ng = lt_ttf_num_glyphs(f);
    (void)lt_ttf_units_per_em(f);
    (void)lt_ttf_ascent(f); (void)lt_ttf_descent(f); (void)lt_ttf_line_gap(f);
    (void)lt_ttf_x_height(f); (void)lt_ttf_cap_height(f);
    lt_ttf_scale_for(f, 13.0, &sx, &sy);
    for (cp = 0; cp < 0x180; cp++) (void)lt_ttf_glyph_index(f, cp);
    (void)lt_ttf_glyph_index(f, 0x1F600UL);
    (void)lt_ttf_glyph_index(f, 0xFFFFUL);
    for (g = -1; g < ng + 1 && g < 64; g++) {
        (void)lt_ttf_advance(f, g);
        (void)lt_ttf_kern(f, g, g + 1);
        (void)lt_ttf_glyph_box(f, g, &x0, &y0, &x1, &y1);
        (void)lt_ttf_glyph_composite(f, g);
        (void)lt_ttf_glyph(f, g, 12.0, g & 3);
        if ((g & 7) == 0) {
            (void)lt_ttf_glyph(f, g, 72.0, 0);
            m[0] = 0.02; m[1] = 0.01; m[2] = 0.01; m[3] = -0.02;
            (void)lt_ttf_raster(f, g, m, 0.25, 0.5, &rb);
        }
    }
    lt_ttf_close(f);
    return 0;
}
