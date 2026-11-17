/* tables_priv.h - helpers shared by tables_*.c (table, curve and AutoTune screens).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_TABLES_PRIV_H
#define LT_TABLES_PRIV_H

#include "app.h"
#include "../tableops/tableops.h"
#include "../autotune/autotune.h"

/* fixed-point format with `digits` decimals (clamped 0..6) */
void lt_tabs_fmt(char *buf, size_t cap, double v, int digits);
/* heat colour of v within [lo,hi] using the configured scheme, blended with the table bg */
lt_color lt_tabs_heat(const lt_app *app, const lt_theme *t, double lo, double hi, double v);
/* readable ink (near-black or white) for text on a heat-map cell colour */
lt_color lt_tabs_heat_text(lt_color bg);
/* number of decimals of an INI constant (default 0) and its value limits */
int  lt_tabs_const_digits(const lt_ini *ini, const char *name);
/* round to `digits` decimals */
double lt_tabs_round(double v, int digits);
/* key helper: ch pressed with or without shift, no other modifiers */
int  lt_tabs_key(lt_ui *ui, int ch);
/* parse "1, 2.5 3" into out (cap entries); returns count or -1 on a bad token */
int  lt_tabs_parse_list(const char *s, double *out, int cap);
/* nearest-index of the live channel value in bins; LT_FALSE when the channel is unavailable */
lt_bool lt_tabs_live_index(lt_app *app, const char *channel, const double *bins, size_t n,
                           double *value, size_t *idx);

/* autotune service (feeds samples each realtime frame while a session runs) */
void lt_tabs_autotune_service(lt_app *app);

/* tab kinds */
extern const lt_app_tab_kind lt_tabs_kind_table, lt_tabs_kind_curve, lt_tabs_kind_autotune;

#endif
