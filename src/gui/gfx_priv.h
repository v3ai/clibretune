/* gfx_priv.h - private hooks between gfx.c (bitmap/Hershey text) and
 * gfx_text.c (TrueType text). Not part of the public gui.h contract.
 * Each function returns nonzero when it handled the request, zero when the
 * caller should use its own bitmap/Hershey implementation.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_GFX_PRIV_H
#define LT_GFX_PRIV_H

#include "gui.h"

LT_BEGIN_DECLS

/* legacy 8x16 bitmap-font call mapped onto Plex Mono at the equivalent size */
int lt_gfx_text_cellfont(lt_gfx *g, int x, int y, const char *s, lt_color c, int scale);
/* legacy Hershey vector text mapped onto Plex Sans Medium */
int lt_gfx_vtext_plex(lt_gfx *g, double x, double y, const char *s, double height,
                      double rotation, int align, int valign, lt_color c);
int lt_gfx_vtext_width_plex(const char *s, double height, double *w);

/* box-line merging for cell mode: bits 1 left, 2 right, 4 up, 8 down; add the bits to the
 * light box glyph already in a cell (rounded corners when round and the cell was empty) */
lt_u32 lt_cell_line_merge(lt_u32 old, int add, int round);
int lt_cell_line_bits(lt_u32 ch);
/* thin line as braille dots (2x4 per cell) between two absolute pixel points; dots merge into
 * braille already in the cells, blank and box-drawing cells are taken over, text is kept */
void lt_cell_braille_line(lt_gfx *g, double x0, double y0, double x1, double y1, lt_color c);

/* coverage remapping table (256 entries) for text/icons of color c: boosts
 * light-on-dark and tames dark-on-light so stroke weight looks the same */
const lt_u8 *lt_gfx_contrast_lut(lt_color c);

/* generated icon tables (icons_data.c): polylines per tools/gen_icons.py */
const lt_s16 *lt_icon_data(int id);
int lt_icon_alias_count(void);
const char *lt_icon_alias_at(int i, int *id);

/* ---- retained rendering (gfx_rec.c): the display list ----
 * Each recorded call keeps the parameters of the public function (replayed by calling it again)
 * plus the drawing state. bb is the absolute region the call can touch: a conservative box
 * already intersected with the clip and the surface. */
enum {
    LT_OP_CLEAR = 1, LT_OP_FILL, LT_OP_ROUND, LT_OP_VGRAD, LT_OP_LINE, LT_OP_FCIRCLE,
    LT_OP_CIRCLE, LT_OP_ARC, LT_OP_FARC, LT_OP_POLY, LT_OP_BLIT, LT_OP_PREMUL, LT_OP_MASK,
    LT_OP_TEXT, LT_OP_TEXTF, LT_OP_TEXTROT, LT_OP_VTEXT, LT_OP_ICON, LT_OP_SHADOW
};

typedef struct lt_dcmd {
    int op;
    lt_rect clip, bb;
    int ox, oy, bitmap_text;
    lt_color c, c2;
    int i[4];
    double d[7];
    long off, len;         /* data: bytes (text, masks) / doubles (polygons) / pixels (blits) */
    long off2;             /* second byte block (mask lut) or -1 */
    lt_font *f;            /* font of text calls */
    int src;               /* LT_OP_PREMUL: index of the source canvas */
    lt_u32 h1, h2;         /* hash of everything that determines the pixels */
} lt_dcmd;

/* start a recorded call: NULL when it cannot touch any pixel (nothing to record); bb is the
 * absolute box, intersected here with the clip and the surface */
lt_dcmd *lt_rec_cmd(lt_gfx *g, int op, lt_rect bb);
long lt_rec_bytes(lt_gfx *g, const void *data, long n);     /* copy; returns the offset */
long lt_rec_doubles(lt_gfx *g, const double *v, long n);
long lt_rec_pixels(lt_gfx *g, const lt_u32 *px, int w, int h, int pitch);
int  lt_rec_canvas(lt_gfx *g, const lt_canvas *c);          /* source index (premul blits) */
void lt_rec_done(lt_gfx *g, lt_dcmd *cmd);                   /* hashes the finished call */
/* data access during replay */
const char *lt_rec_bytes_at(const struct lt_rec *r, long off);
const double *lt_rec_doubles_at(const struct lt_rec *r, long off);
const lt_u32 *lt_rec_pixels_at(const struct lt_rec *r, long off);
const lt_canvas *lt_rec_canvas_at(const struct lt_rec *r, int src);
/* a canvas is about to change or go away: a recorded frame that blits it keeps a copy */
void lt_rec_canvas_changing(const lt_canvas *c);
/* new content generation number */
lt_u32 lt_gfx_next_gen(void);

/* replay one call into g (g->rec must be NULL; clip, translation and bitmap_text are set from
 * the call, the clip intersected with lim) */
void lt_gfx_replay(lt_gfx *g, const struct lt_rec *r, const lt_dcmd *c, lt_rect lim);
/* measuring (bounding boxes of text): while active, lt_gfx_blit_mask only adds its unclipped
 * destination rectangle to *acc */
void lt_gfx_measure_begin(lt_rect *acc);
void lt_gfx_measure_end(void);
int  lt_gfx_measuring(void);
void lt_rect_union_into(lt_rect *acc, lt_rect r);
/* the TrueType glyph run behind lt_gfx_text_ex/_bl and upright lt_gfx_text_rot; returns the
 * unrounded advance */
double lt_gfx_text_run(lt_gfx *g, lt_font *f, double px, double x, int baseline, const char *s,
                       lt_color c);

LT_END_DECLS

#endif /* LT_GFX_PRIV_H */
