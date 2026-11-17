/* ttf.h - TrueType font loader and anti-aliased glyph rasterizer.
 *
 * Written from scratch in Universal C (docs/UNIVERSAL_C.md); no third-party
 * code. Supports sfnt (TrueType outlines, 'glyf'): cmap formats 4 and 12,
 * head, hhea, hmtx, maxp, loca, simple and composite glyphs, legacy 'kern'
 * (format 0) and GPOS pair kerning (PairPos formats 1 and 2, extension
 * lookups). Glyph outlines are flattened (quadratic beziers) and rasterized
 * with exact signed-area coverage, so edges are anti-aliased with 256 levels
 * and no supersampling. Glyphs can be rasterized under any 2x2 matrix
 * (rotation, scale) at a fractional pen position.
 *
 * All big-endian table reads use explicit byte shifts and are bounds
 * checked: a malformed or truncated font never reads out of range, and
 * outline/bitmap sizes are capped, so lt_ttf_open() on arbitrary bytes is
 * safe (it returns NULL when the font is unusable).
 *
 * Not thread-safe: one lt_ttf (including its glyph cache) per thread.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_TTF_H
#define LT_TTF_H

#include "../lt.h"

LT_BEGIN_DECLS

typedef struct lt_ttf lt_ttf;

/* One piece of a font stored as several static arrays (see fonts_data.c). */
typedef struct lt_ttf_chunk {
    const lt_u8 *data;
    size_t len;
} lt_ttf_chunk;

/* Rendered glyph coverage. a[] is w*h bytes (row-major, pitch == w),
 * 0 = transparent, 255 = fully covered. (xoff, yoff) is the offset of the
 * upper-left pixel from the pen origin on the baseline, y growing downward
 * (so yoff is negative for glyph parts above the baseline). */
typedef struct lt_ttf_bitmap {
    const lt_u8 *a;
    int w, h;
    int xoff, yoff;
} lt_ttf_bitmap;

/* Horizontal sub-pixel positions used by the glyph cache. */
#define LT_TTF_SUBPOS 4

/* Open a font. data must stay valid until lt_ttf_close() unless copy is
 * true. Returns NULL (and sets lt_err) when the data is not a usable
 * TrueType font. */
lt_ttf *lt_ttf_open(const lt_u8 *data, size_t len, lt_bool copy);
/* Concatenate chunks into one heap buffer owned by the font. */
lt_ttf *lt_ttf_open_chunks(const lt_ttf_chunk *chunks, int count);
void lt_ttf_close(lt_ttf *f);

/* ---- metrics (font design units) ---- */
int lt_ttf_units_per_em(const lt_ttf *f);
int lt_ttf_num_glyphs(const lt_ttf *f);
int lt_ttf_ascent(const lt_ttf *f);     /* positive, above baseline */
int lt_ttf_descent(const lt_ttf *f);    /* negative, below baseline */
int lt_ttf_line_gap(const lt_ttf *f);
int lt_ttf_x_height(const lt_ttf *f);
int lt_ttf_cap_height(const lt_ttf *f);

/* ---- glyph lookup ---- */
int lt_ttf_glyph_index(const lt_ttf *f, lt_u32 cp);   /* 0 = missing */
int lt_ttf_advance(const lt_ttf *f, int gid);          /* design units */
/* kerning between two glyphs in design units (GPOS 'kern', else 'kern') */
int lt_ttf_kern(const lt_ttf *f, int left, int right);
/* glyph bounding box from the glyf header; LT_FALSE for empty glyphs */
lt_bool lt_ttf_glyph_box(const lt_ttf *f, int gid, int *x0, int *y0, int *x1, int *y1);
lt_bool lt_ttf_glyph_composite(const lt_ttf *f, int gid);

/* ---- scaling ---- */
/* Pixel scale for upright text at px (em height in pixels): *sx is exact
 * px/unitsPerEm; *sy is the same nudged so the x-height lands on a whole
 * pixel (light vertical grid fitting; crisp tops of lowercase letters when
 * the baseline is on an integer row). */
void lt_ttf_scale_for(const lt_ttf *f, double px, double *sx, double *sy);

/* ---- rasterization ---- */
/* Rasterize glyph gid under X = m[0]*u + m[2]*v, Y = m[1]*u + m[3]*v
 * (u,v in design units, v up; X,Y in pixels, Y down: an upright glyph at
 * scale s is m = {s, 0, 0, -s}) with the pen origin at (fx, fy). The result
 * lives in a scratch buffer owned by f and is valid until the next call that
 * rasterizes on f. Returns LT_FALSE (empty bitmap) for blank/invalid glyphs. */
lt_bool lt_ttf_raster(lt_ttf *f, int gid, const double m[4], double fx, double fy,
                      lt_ttf_bitmap *out);
/* Cached upright glyph at em size px and horizontal sub-pixel position
 * subpos (0..LT_TTF_SUBPOS-1, in 1/LT_TTF_SUBPOS pixel). The pointer is valid
 * until the next lt_ttf_glyph() call on f. NULL for blank glyphs (spaces). */
const lt_ttf_bitmap *lt_ttf_glyph(lt_ttf *f, int gid, double px, int subpos);

/* ---- cache control ---- */
void lt_ttf_cache_limit(lt_ttf *f, size_t max_bytes);   /* default 1 MiB */
void lt_ttf_cache_stats(const lt_ttf *f, int *entries, size_t *bytes,
                        long *hits, long *misses);
void lt_ttf_cache_clear(lt_ttf *f);

LT_END_DECLS

#endif /* LT_TTF_H */
