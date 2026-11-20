/* at_ui.h - helpers shared by the AutoTune family of tabs (VE AutoTune in tables_autotune.c,
 * WUE Analyze in feat_wue.c, Trim Table Analyze in feat_trim.c).  Implemented in
 * tables_autotune.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_AT_UI_H
#define LT_AT_UI_H

#include "app.h"
#include "prefs.h"

/* colour of heat-map context ctx (LT_HEAT_VALUE / _CHANGE / _COVERAGE) at t in 0..1: the
 * context's custom colour stops, else its stock scheme (Settings > Table display) */
lt_color lt_at_ui_heat(const lt_app *app, int ctx, double t);
/* change-context colour of a signed relative change d (> 0 = richer: more fuel), full colour
 * at |d| >= full; no change is the middle of the scheme */
lt_color lt_at_ui_change(const lt_app *app, double d, double full);
/* draw the change legend ("Richer" ... "Leaner") into r */
void     lt_at_ui_change_legend(lt_app *app, lt_rect r);
/* pending VE AutoTune correction at (rpm, load): recommended / current VE of that cell (1 when
 * the cell has no recommendation).  LT_FALSE when no AutoTune session has recommendations. */
lt_bool  lt_at_ui_ve_factor(lt_app *app, double rpm, double load, double *factor);
/* the same as a table on the VE axes (malloc'd x[nx], y[ny], z[ny*nx]; free with lt_free);
 * LT_FALSE when there are no recommendations */
lt_bool  lt_at_ui_ve_factor_table(lt_app *app, double **x, size_t *nx, double **y, size_t *ny,
                                  double **z);

#endif
