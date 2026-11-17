/* gauges_cell.h - terminal (cell mode) gauge and indicator painters.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_GAUGES_CELL_H
#define LT_GAUGES_CELL_H

#include "../gui/gui.h"

LT_BEGIN_DECLS

typedef struct lt_gauge_cell {
    const void *key;           /* identity for the history (the gauge config pointer) */
    const char *title, *units, *value_text, *min_text, *max_text;
    double value, frac, lo, hi, now_ms;
    double zone_at[4];         /* low warn, high warn, low crit, high crit as 0..1 of the range; < 0 none */
    int zone;                  /* 0 normal, 1 warning, 2 critical */
    int digits;                /* decimals for the observed min / max / average */
    lt_color back, title_fg, value_fg, bar, base, warn, crit;
} lt_gauge_cell;

/* paint into r (pixel units, cell aligned); keeps a short history per key for the trend line */
void lt_gauge_cells_paint(lt_gfx *g, lt_rect r, const lt_gauge_cell *c);
void lt_indicator_cells_paint(lt_gfx *g, lt_rect r, const char *text, lt_color on_bg, lt_color on_fg,
                              lt_color off_bg, lt_color off_fg, lt_bool on);

/* shorten a label to at most cols columns: drops inner vowels word by word, then the spaces,
 * then cuts with an ellipsis (never a mid-word "Config...Settin..." pair) */
void lt_cell_abbrev(const char *s, int cols, char *out, size_t cap);
LT_END_DECLS

#endif
