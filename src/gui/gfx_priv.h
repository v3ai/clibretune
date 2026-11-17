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

/* coverage remapping table (256 entries) for text/icons of color c: boosts
 * light-on-dark and tames dark-on-light so stroke weight looks the same */
const lt_u8 *lt_gfx_contrast_lut(lt_color c);

/* generated icon tables (icons_data.c): polylines per tools/gen_icons.py */
const lt_s16 *lt_icon_data(int id);
int lt_icon_alias_count(void);
const char *lt_icon_alias_at(int i, int *id);

LT_END_DECLS

#endif /* LT_GFX_PRIV_H */
