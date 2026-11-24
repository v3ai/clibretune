/* dash_tab.h - state shared by the dashboard tab (dash_tab.c), its panels (dash_panels.c) and the
 * dashboard feature (feat_dashboard.c: settings page, full screen, kiosk).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_DASH_TAB_H
#define LT_DASH_TAB_H

#include "app.h"
#include "app_ext.h"
#include "dash_gauges.h"
#include "../dash/dash_edit.h"
#include "../dash/dash_lib.h"

#define DT_N_PAINTERS 20

/* ---- persistent dashboard settings (<config dir>/dashboard.conf) ---- */
typedef struct dash_cfg {
    int loaded;
    int refresh_hz;          /* 10, 15, 20, 25, 30, 60 */
    int right_align;         /* right-align numeric values */
    int ind_cols;            /* indicator columns, 0 = auto */
    int ind_fill;            /* stretch the last indicator row */
    int ind_wrap;            /* 0 scale text to fit, 1 wrap it */
    int snap;                /* snap to grid while dragging */
    int free_move;           /* ignore snapping */
    int lock;                /* lock gauge positions */
    int auto_sync;           /* adopt INI gauge ranges on load */
    /* runtime */
    int kiosk;               /* dashboard only, window full screen */
    double kiosk_t0;
    int demo;                /* gauge demo sweep */
} dash_cfg;
dash_cfg *lt_dashx_cfg(lt_app *app);          /* loads on first use */
void      lt_dashx_cfg_save(lt_app *app);
void      lt_dashx_apply(lt_app *app);        /* push display options into the painters */
/* enter / leave full screen kiosk; returns 0, or -1 when there is no dashboard to show */
int       lt_dashx_set_kiosk(lt_app *app, int on);
/* the shell calls this at the start of every frame; returns 1 when it drew the whole frame */
int       lt_dashx_kiosk_frame(lt_app *app);

/* ---- tab state ---- */
enum { DS_FRONT = 0, DS_BASIC, DS_TUNING, DS_TELEM, DS_FILE, DS_N };
enum { DM_NONE = 0, DM_MOVE, DM_SIZE, DM_BAND };
enum { MG_NONE = 0, MG_NEW, MG_RENAME, MG_DUP, MG_DELETE, MG_RESET, MG_IMPORT };

typedef struct dash_state {
    lt_dash_file *file;
    lt_dash_view *view;
    lt_expr_cache cache;
    const lt_expr_env *env;
    const lt_ini *ini;
    int src;
    char path[512];                 /* file of the open dashboard, "" for a built-in source */
    int designer, sel, mode, ctx_idx, ctx_x, ctx_y;
    int prev_down, prev_rdown, drag_x, drag_y, moved;
    double *orig_all;               /* rects of all components at drag start (4 each) */
    size_t n_orig;
    unsigned char *msel;            /* multi-selection flags, n_msel entries */
    size_t n_msel;
    lt_rect cluster, area;
    int dirty, rebuild;
    int props_open, props_idx;
    lt_rect props_rect;
    char filter[64];
    int chan_sel, hooked;
    /* history */
    lt_dash_hist hist;
    int hist_ready, changed;
    double change_ms;
    char *clip;                     /* copied components as .dash text */
    /* validation / compatibility */
    lt_dash_report rep;
    int rep_valid, valid_open, compat_hidden;
    lt_rect valid_rect;
    lt_dash_compat compat;
    int compat_valid;
    /* panels */
    int layers_open, palette_open, assets_open, zoom_open, zoom_idx;
    lt_rect layers_rect, palette_rect, assets_rect, zoom_rect;
    int pal_cat, pal_sel, pal_drag, pal_chan;
    char pal_filter[64];
    int asset_sel;
    /* library / management */
    int selector_open, mgmt, mgmt_open;
    char mgmt_name[96], mgmt_target[512];
    int sel_row;
    lt_dashlib lib;
    int lib_valid;
    char imp_path[512];
    int imp_open, imp_ts;
    lt_dashlib imp_lib;
    int imp_sel;
    lt_dash_compat imp_rep;
    int imp_have;
    char pal_name[96];
    int pal_prev;
    /* interaction extras */
    double last_click_ms;
    int last_click_x, last_click_y, last_click_idx;
    double down_ms;
    int hover_idx;
    double hover_ms;
    int long_fired, press_idx;
    /* refresh cap cache */
    lt_u32 *cache_px;
    int cache_w, cache_h;
    double cache_ms;
    int refresh_now;
} dash_state;

dash_state *lt_dtab_state(lt_app *app);       /* NULL when no dashboard tab was drawn yet */

/* operations used by the panels */
void lt_dtab_touch(dash_state *st);                          /* a change was made */
void lt_dtab_flush(dash_state *st);                          /* record a pending change in the history */
void lt_dtab_clear_sel(dash_state *st);
void lt_dtab_select(dash_state *st, int idx, int add);       /* add: toggle in the multi-selection */
int  lt_dtab_is_sel(const dash_state *st, int idx);
size_t lt_dtab_sel_list(const dash_state *st, size_t *out, size_t cap);   /* selected indices, ascending */
void lt_dtab_remove_sel(dash_state *st);
void lt_dtab_add_gauge(lt_app *app, dash_state *st, double x, double y, lt_gauge_painter pt, const char *chan);
void lt_dtab_add_indicator(lt_app *app, dash_state *st, double x, double y, const char *chan);
void lt_dtab_undo(lt_app *app, dash_state *st);
void lt_dtab_redo(lt_app *app, dash_state *st);
void lt_dtab_copy(lt_app *app, dash_state *st);
void lt_dtab_paste(lt_app *app, dash_state *st);
void lt_dtab_select_all(dash_state *st);
/* the properties panel for component idx (-1 closes it): toolbar Properties, double-click in the
 * designer, the context menu; while it is open it follows the designer's selection */
void lt_dtab_props_open(dash_state *st, int idx);
/* load a dashboard file / a built-in source; remember it in the project */
int  lt_dtab_load_file(lt_app *app, dash_state *st, const char *path);
void lt_dtab_load_builtin(lt_app *app, dash_state *st, int src);
void lt_dtab_revalidate(lt_app *app, dash_state *st);
char *lt_dtab_dir(lt_app *app);                              /* project dashboards folder (malloc'd) or NULL */
void lt_dtab_lib_refresh(lt_app *app, dash_state *st);
const char *lt_dtab_current_name(const dash_state *st, char *buf, size_t cap);
int  lt_dtab_save(lt_app *app, dash_state *st);              /* to st->path; 0 ok */
void lt_dtab_reset_defaults(lt_app *app, dash_state *st);
void lt_dtab_draw_body(lt_app *app, lt_app_tab *tab, lt_rect r);   /* the tab body, kiosk frame */
void lt_dtab_cfg_changed(lt_app *app);                      /* settings changed */

/* panels (dash_panels.c) */
void lt_dpanel_selector(lt_app *app, dash_state *st);        /* selector, management, import */
void lt_dpanel_validation(lt_app *app, dash_state *st);
void lt_dpanel_compat_bar(lt_app *app, dash_state *st);
void lt_dpanel_layers(lt_app *app, dash_state *st);
void lt_dpanel_palette(lt_app *app, dash_state *st);
void lt_dpanel_assets(lt_app *app, dash_state *st);
void lt_dpanel_zoom(lt_app *app, dash_state *st);
void lt_dpanel_align_bar(lt_app *app, dash_state *st);
/* open the import dialog for a file (also used by the screenshot hook) */
void lt_dpanel_import_path(lt_app *app, dash_state *st, const char *path);
int  lt_dpanel_file_results(lt_app *app, dash_state *st);    /* file dialog results of the panels */
void lt_dpanel_request(lt_app *app, dash_state *st, int what);
enum { DP_IMPORT = 1, DP_EXPORT, DP_ADD_ASSET, DP_IMPORT_TS };

#endif
