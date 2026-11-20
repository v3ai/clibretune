/* app.h - application shell: state, tab/dialog registries, hooks for screens.
 *
 * The shell (app.c, app_shell.c, main.c) owns the window, menus, sidebar, tab
 * area, status bar, welcome screen, project/tune/ECU lifecycle and the
 * realtime service loop.  Screens plug in through two registries:
 *
 *   tab kinds    lt_app_tab_kind  { kind, draw, close }   (editors, dashboard...)
 *   dialog kinds lt_app_dialog_kind { kind, draw, close } (modal dialogs)
 *
 * Each future owner implements ONE registration function plus the open hooks
 * below; until then src/app/stubs_<owner>.c provides them (delete the stub
 * file when implementing for real):
 *   stubs_tables.c   lt_app_register_tables  + table/curve/autotune hooks
 *   stubs_dash.c     lt_app_register_dash    + dashboard hook
 *   stubs_dialogs.c  lt_app_register_dialogs + connect/settings/ini dialog/
 *                    basemap/restore/new project/datalog/special hooks
 *
 * EDITING CONTRACT: lt_app_pages(app) is the page source to edit (ECU shadow
 * memory when online/demo, the tune's pages otherwise).  After writing, call
 * lt_app_edited(app): it marks the tune modified and, when online, sends the
 * dirty ranges to the ECU RAM (burn persists them).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_APP_H
#define LT_APP_H

#include "../lt.h"
#include "../gui/ui.h"
#include "../ini/ini.h"
#include "../tableops/tableops.h"
#include "../proto/proto.h"
#include "../tune/tune.h"
#include "../project/project.h"
#include "../sim/sim.h"
#include "../datalog/datalog.h"
#include "prefs.h"
#include "i18n.h"

typedef struct lt_app lt_app;
typedef struct lt_app_tab lt_app_tab;
typedef struct lt_app_dialog lt_app_dialog;
typedef struct lt_app_ext lt_app_ext;     /* extension registry, see app_ext.h */

#define LT_APP_VERSION "0.1.0"

typedef enum lt_app_conn {
    LT_APP_OFFLINE = 0, LT_APP_CONNECTING, LT_APP_ONLINE, LT_APP_DEMO
} lt_app_conn;

/* ---- tabs ---- */
struct lt_app_tab {
    char kind[24];          /* registry key, e.g. "table" */
    char target[128];       /* table/curve/dialog name or special id */
    char title[96];
    void *data;             /* owner state (allocate in draw on first use; free in close) */
    int closable;
    int dirty;              /* show the dirty dot */
    int pinned;             /* protected: cannot be closed by the user (feat_tabs.c) */
};
typedef struct lt_app_tab_kind {
    const char *kind;       /* static string */
    /* Called every frame for the ACTIVE tab inside a region of exactly r
     * (layout functions work; r is also available for raw drawing). */
    void (*draw)(lt_app *app, lt_app_tab *tab, lt_rect r);
    void (*close)(lt_app *app, lt_app_tab *tab);   /* free tab->data; may be NULL */
} lt_app_tab_kind;

/* ---- modal dialogs (stack; only the top one gets input) ---- */
struct lt_app_dialog {
    char kind[24];
    char target[128];
    char title[96];
    void *data;
    int open;               /* draw clears it to close */
};
typedef struct lt_app_dialog_kind {
    const char *kind;
    void (*draw)(lt_app *app, lt_app_dialog *d);   /* every frame while open */
    void (*close)(lt_app *app, lt_app_dialog *d);  /* free d->data; may be NULL */
} lt_app_dialog_kind;

typedef struct lt_app_settings {
    char theme[24];
    int  metric;            /* 1 metric, 0 imperial */
    int  sidebar_w, sidebar_on;
    int  sidebar_cols;      /* terminal UI sidebar width in columns (0 = automatic) */
    int  show_ecu_menus;
    int  scale_pct;         /* 0 = auto (display hint), else 100..300 percent */
    int  win_w, win_h;      /* saved window size in LOGICAL px (0 = none) */
    int  win_x, win_y, win_pos;   /* saved position (win_pos = valid) */
    int  win_max;           /* was maximized */
    char backend[16];
    char last_project[512];
    char recent[8][512];    /* project directories, newest first */
    int  n_recent;
    char port[128];
    int  baud;
    int  auto_connect;
    char heatmap[16];
    int  temp_unit, press_unit, afr_unit;   /* 0 C/kPa/AFR, 1 F/psi/lambda, 2 K/bar/-, 3 -/inHg/- */
    int  poll_hz;           /* realtime poll rate 5..100 */
    int  reconnect_cmd;     /* reconnect after controller commands (default 1) */
    int  reconnect_fw;      /* reconnect after a firmware update (default 1) */
    int  auto_commit;       /* tune history on save: 0 never, 1 always, 2 ask (default 2) */
    char commit_fmt[96];    /* commit message format, {date} {time} {table} */
    lt_prefs prefs;         /* table display, behaviour, protocol tuning, session (prefs.h) */
} lt_app_settings;

struct lt_app {
    /* platform */
    lt_backend *backend;
    lt_canvas   canvas;
    lt_diff     diff;       /* last presented frame, for dirty-tile presenting */
    lt_cells    cells;
    lt_gfx      gfx;
    lt_ui      *ui;
    int         cell_mode;
    double      anim_ms;             /* >0: a live view wants a frame this often (set while drawing) */
    int         scale_pct;           /* effective UI scale in percent */
    int         quit, quit_asked, headless;
    lt_app_settings settings;
    char       *config_dir;

    /* project / definition / tune */
    lt_project  project;
    int         has_project;
    lt_ini     *ini;                 /* NULL on the welcome screen */
    char        ini_path[512];
    lt_tune    *tune;                /* project.tune when a project is open */
    int         modified;            /* tune changed since last save */
    int         keep_project_tune;   /* "Ignore" chosen on connect: disconnect must not copy the ECU into the project tune */
    lt_expr_cache expr;              /* per-frame menu/dialog conditions */

    /* ECU link */
    lt_app_conn conn;
    lt_ecusim  *sim;                 /* demo mode */
    lt_simlink *simlink;             /* demo over a modelled slow link (LT_DEMO_BAUD) */
    lt_proto   *proto;
    lt_stream  *link_stream;
    lt_ecu_mem *mem;                 /* ECU shadow (online/demo) */
    lt_ini_rt  *rt;                  /* latest realtime values (NULL w/o ini) */
    lt_varenv   varenv;              /* expression env: ini + pages + rt + pcvars */
    lt_buf      rtbuf;
    double      last_poll_ms, rate_t0_ms;
    int         rate_frames;
    double      data_rate;           /* realtime frames per second */
    unsigned long frames_ok, frames_bad;
    char        status[160];         /* last status message */
    char        signature[96];       /* ECU signature when connected */

    /* recorder */
    lt_recorder *rec;
    char       **rec_names;
    size_t       rec_n;
    double      *rec_vals;

    /* tabs, dialogs, registries */
    lt_app_tab  **tabs; int n_tabs, cap_tabs;
    int          active_tab;
    lt_app_dialog **dialogs; int n_dialogs, cap_dialogs;
    lt_app_tab_kind *tab_kinds; int n_tab_kinds, cap_tab_kinds;
    lt_app_dialog_kind *dlg_kinds; int n_dlg_kinds, cap_dlg_kinds;
    lt_app_ext  *ext;                /* feature registry (app_ext.c); features keep state there */

    /* shell UI state */
    char        search[64];
    lt_ui_filedlg fd;
    int         fd_purpose;          /* LT_APP_FD_* */
    int         burn_open, quit_open, close_proj_open;
    int         confirm_delete;      /* recent index awaiting a second click, -1 */
    int         theme_idx;
    int         settings_tab;        /* active tab of the Settings dialog */
    int         tree_expanded;       /* sidebar expand/collapse-all toggle */
    int         fd_ready;            /* purpose of a finished user file dialog, path in fd.path */
    char        title[160];
};

/* ---- lifecycle ---- */
lt_app *lt_app_new(void);
void    lt_app_free(lt_app *app);
/* open the window (backend NULL/"auto" = best); screenshot mode skips this */
/* UI scale in percent: env LT_SCALE ("150", "150%") beats the setting (0 =
 * auto), auto uses the backend hint (1.0 = 100%) rounded to 25%; clamped 100..300. */
int     lt_app_pick_scale(const char *env, int setting_pct, double hint);
/* build identifier (git commit) set by main(); "dev" when unknown. id must stay valid. */
void        lt_app_set_build_id(const char *id);
const char *lt_app_build_id(void);
/* scale auto-detection would choose right now (percent), whatever the setting says */
int     lt_app_detected_scale(lt_app *app);
/* Window plan: default 1280x800 logical, or the saved logical size, times pct;
 * falls back to the default when the saved size does not fit the screen
 * (scr_w/scr_h = 0 when unknown). Outputs physical size, position, LT_GEOM_*. */
void    lt_app_pick_window(const lt_app_settings *s, int pct, int scr_w, int scr_h,
                           int *w, int *h, int *x, int *y, int *flags);
/* Set the UI scale setting (0 = auto), recompute and apply the effective scale. */
void    lt_app_set_scale_setting(lt_app *app, int pct);
int     lt_app_open_window(lt_app *app, const char *backend, int w, int h);
/* main loop until quit */
int     lt_app_run(lt_app *app);
/* render `frames` frames of the current state offscreen and write a PPM.
 * Between frames the ECU service runs (sleeping ~60 ms) so live data appears. */
int     lt_app_screenshot(lt_app *app, const char *ppm_path, int w, int h, int frames);
/* one UI frame (menus, sidebar, tabs, status bar, dialogs) */
void    lt_app_frame(lt_app *app);
/* ECU service: realtime poll, recorder; returns 1 if new data arrived */
int     lt_app_service(lt_app *app);
void    lt_app_request_quit(lt_app *app);

/* ---- settings ---- */
void lt_app_settings_load(lt_app *app);
void lt_app_settings_save(lt_app *app);
void lt_app_apply_theme(lt_app *app, const char *name);
void lt_app_recent_add(lt_app *app, const char *dir);

/* ---- project / definition / tune ---- */
/* open INI without a project (offline); tune = defaults */
int  lt_app_open_ini(lt_app *app, const char *ini_path);
int  lt_app_open_project(lt_app *app, const char *dir);
int  lt_app_new_project(lt_app *app, const char *name, const char *ini_path);
int  lt_app_import_ts(lt_app *app, const char *ts_dir);
void lt_app_close_project(lt_app *app);        /* back to the welcome screen */
int  lt_app_save_tune(lt_app *app);            /* project tune or Save As when none */
int  lt_app_save_tune_as(lt_app *app, const char *path);
int  lt_app_load_tune(lt_app *app, const char *path);
/* same with a tune already in memory (ownership taken, also on error); label is shown in the toast */
int  lt_app_load_tune_mem(lt_app *app, lt_tune *t, const char *label);
int  lt_app_reset_defaults(lt_app *app);
/* demo mode: ini_path may be NULL = search the bundled demo.ini */
int  lt_app_start_demo(lt_app *app, const char *ini_path);
const char *lt_app_find_demo_ini(char *buf, size_t cap);

/* ---- pages / editing ---- */
lt_pagesrc lt_app_pages(lt_app *app);
void lt_app_edited(lt_app *app);               /* after any write through lt_app_pages */

/* ---- ECU link ---- */
int  lt_app_connect_demo(lt_app *app);
int  lt_app_connect_serial(lt_app *app, const char *port, long baud);
void lt_app_disconnect(lt_app *app);
int  lt_app_burn(lt_app *app);                 /* writes dirty ranges, burns, clears pending */
lt_bool lt_app_can_burn(const lt_app *app);
const char *lt_app_conn_label(const lt_app *app);
/* named realtime value; LT_FALSE if unknown */
lt_bool lt_app_channel(lt_app *app, const char *name, double *out);
/* Same, in the preferred display unit (G-90); label (optional) is the unit to show. */
lt_bool lt_app_channel_disp(lt_app *app, const char *name, double *out, const char **label);
void    lt_app_channel_conv(const lt_app *app, const char *name, lt_uconv *c);
/* data logging (project datalogs dir or ./datalogs) */
int  lt_app_log_toggle(lt_app *app);

/* ---- registries ---- */
void lt_app_register_tab_kind(lt_app *app, const lt_app_tab_kind *k);
void lt_app_register_dialog_kind(lt_app *app, const lt_app_dialog_kind *k);
/* open or focus the tab (dedup by kind+target); NULL if the kind is unknown */
lt_app_tab *lt_app_tab_open(lt_app *app, const char *kind, const char *target,
                            const char *title, int closable);
lt_app_tab *lt_app_tab_find(lt_app *app, const char *kind, const char *target);
void        lt_app_tab_close(lt_app *app, lt_app_tab *tab);
lt_app_tab *lt_app_tab_active(lt_app *app);
lt_app_dialog *lt_app_dialog_push(lt_app *app, const char *kind, const char *target,
                                  const char *title);
/* toast kinds: 0 info 1 success 2 warning 3 error */
void lt_app_toast(lt_app *app, int kind, const char *fmt, ...);
void lt_app_set_status(lt_app *app, const char *fmt, ...);
/* generic "not implemented yet" tab body for stubs */
void lt_app_placeholder_draw(lt_app *app, lt_app_tab *tab, lt_rect r);
/* resolve an INI menu/search target: table/curve/dialog/std/special */
int  lt_app_open_target(lt_app *app, const char *target, const char *label);
/* file dialog request: purpose decides what happens on OK (shell-handled) */
enum { LT_APP_FD_NONE = 0, LT_APP_FD_LOAD_TUNE, LT_APP_FD_SAVE_TUNE, LT_APP_FD_OPEN_INI_DEMO,
       LT_APP_FD_NEW_PROJECT_INI, LT_APP_FD_IMPORT_TS, LT_APP_FD_OPEN_PROJECT,
       LT_APP_FD_USER0 };
void lt_app_file_dialog(lt_app *app, int purpose, int mode, const char *dir,
                        const char *name, const char *filters);

/* path chosen by a finished LT_APP_FD_USER0+ / NEW_PROJECT_INI dialog of `purpose`
 * (valid until the next file dialog), else NULL. Clears the ready flag. */
const char *lt_app_file_result(lt_app *app, int purpose);

/* ---- hooks implemented by other owners (stubs provided) ---- */
/* tables owner (stubs_tables.c) */
void lt_app_register_tables(lt_app *app);
int  lt_app_table_editor_open(lt_app *app, const char *table_name);
int  lt_app_curve_editor_open(lt_app *app, const char *curve_name);
int  lt_app_autotune_open(lt_app *app);
void lt_app_tables_service(lt_app *app);   /* called by lt_app_service on each new realtime frame */
/* dashboard owner (stubs_dash.c) */
void lt_app_register_dash(lt_app *app);
int  lt_app_dashboard_tab_open(lt_app *app);
int  lt_dashx_kiosk_frame(lt_app *app);     /* full-screen dashboard: 1 = it drew the whole frame */
/* dialogs owner (stubs_dialogs.c) */
void lt_app_register_dialogs(lt_app *app);
int  lt_app_dialog_open(lt_app *app, const char *ini_dialog_name);
int  lt_app_datalog_open(lt_app *app, int viewer);       /* 0 logger, 1 viewer */
int  lt_app_special_open(lt_app *app, const char *id);   /* och-status, tooth-logger, composite-logger, console */
int  lt_app_connect_dialog(lt_app *app);
int  lt_app_settings_dialog(lt_app *app);
int  lt_app_basemap_wizard(lt_app *app);
int  lt_app_restore_points_dialog(lt_app *app);
int  lt_app_new_project_dialog(lt_app *app);

#endif /* LT_APP_H */
