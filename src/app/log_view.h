/* log_view.h - drawing of the log views used by the Data Logging / Datalog Viewer tabs
 * (dialogs_log.c): MegaLogViewer-style stacked graph panes (log_graph.c) and the scatter plot and
 * histogram / table generator (log_xy.c).  Logic lives in datalog/dl_view.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_LOG_VIEW_H
#define LT_LOG_VIEW_H

#include "app.h"
#include "../datalog/dl_view.h"

#define LT_LG_COLORS 8
extern const lt_color lt_lg_palette[LT_LG_COLORS];

/* ---- stacked panes ---- */
typedef struct lt_lg_view {
    double t0, t1;            /* visible time window (seconds of the log) */
    double cursor_t;          /* cursor time */
    int has_cursor;
    int sel_pane;             /* pane the setup editor works on */
    int drag;                 /* 0 none, 1 cursor, 2 pan */
    int drag_x;
    double drag_t0, drag_t1;
    int prev_down;
    int user_moved;           /* the user zoomed or panned since the flag was cleared */
} lt_lg_view;

/* Draw page pg of log L (NULL: empty panes) over v->t0..v->t1 inside r, with a time axis and the
 * cursor readout at the bottom.  Handles input inside r: click / drag = cursor (and selects the
 * pane), wheel = zoom around the mouse, right or middle drag (or Shift+drag) = pan, limited to
 * tmin..tmax.  Returns 1 when the user changed the view or the cursor. */
int  lt_lg_draw(lt_app *app, lt_rect r, const lt_log *L, const lt_graph_page *pg, lt_lg_view *v,
                double tmin, double tmax);
/* index of the last sample with t <= time (0 when before the start) */
long lt_lg_find(const lt_log *l, double t);

/* ---- scatter plot / histogram ---- */
typedef struct lt_lxy {
    int x, y, z;              /* channel indices (y/z: -1 = none) */
    char filter[160];
    char err[160];
    int window_only;          /* only the graph's visible window */
    /* histogram */
    int axes_src;             /* 0 auto bins, 1 from a tune table */
    int table_sel;            /* index into the INI tables */
    double nbx, nby;          /* auto bin counts */
    double min_hits;
    /* caches */
    unsigned char *mask; long mask_n; int mask_gen; char mask_filter[160]; long passed;
    lt_hist2d h; int h_ok; unsigned long h_sig;
    char herr[96];
    int log_gen;              /* log the caches belong to */
    int inited;
} lt_lxy;
void lt_lxy_free(lt_lxy *s);
/* mode 1 = scatter, 2 = histogram; log_gen changes whenever L is replaced */
void lt_lxy_draw(lt_app *app, lt_lxy *s, const lt_log *L, int log_gen, int mode, double win_t0,
                 double win_t1);

#endif
