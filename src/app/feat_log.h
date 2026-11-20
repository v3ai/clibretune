/* feat_log.h - shared state of the data logging features (feat_logging.c, feat_math.c) used by
 * the logger / viewer tabs (dialogs_log.c): smart recording, sample rate, recording channel
 * selection, alert rules, project math channels.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_FEAT_LOG_H
#define LT_FEAT_LOG_H

#include "app_ext.h"
#include "../datalog/dl_ana.h"
#include "../datalog/dl_view.h"

#define LT_LOGX_RULES 4

typedef struct lt_logx {
    int auto_rec;                 /* smart recording on */
    int key_rpm;                  /* key-on threshold 50..500 */
    int key_timeout;              /* key-off timeout 1..10 s */
    int rate_hz;                  /* recorder sample rate 1..200 */
    int alert_on;                 /* large table change warning */
    double alert_abs, alert_pct;
    lt_alert_rule rules[LT_LOGX_RULES];
    char **off; size_t n_off;     /* channels excluded from recording */
    /* runtime */
    lt_keytrack key;
    int auto_started;             /* current recording was started by smart recording */
    int was_rec;
    int sig_applied;
    int cfg_loaded;
    lt_math_set math;             /* project math channels */
    char math_path[512];
    int math_loaded;
    int rec_fmt;                  /* recording file format: 0 MLG, 1 MSL */
    /* live math channels (virtual realtime channels) */
    lt_app *app;
    lt_math_live live;
    int math_gen, virt_gen;       /* math set generation / generation published as virtual */
    unsigned long live_stamp;     /* app->frames_ok at the last evaluation */
    double live_ms;
    int virt_rec_gen;             /* virtual channel generation the recorder was built with */
    /* the loaded datalog: opened in the Datalog Viewer, also used by the curve X-Y overlay */
    lt_log shared; int have_shared; char shared_path[512];
    int shared_math;              /* math channels appended to it */
    int shared_gen;               /* bumped on every load */
    int shot_checked;             /* LT_SHOT_LOG handled */
} lt_logx;

lt_logx *lt_logx_get(lt_app *app);
void     lt_logx_save(lt_app *app);                 /* write logging.conf */
/* excluded-channel helpers */
int      lt_logx_excluded(const lt_logx *x, const char *name);
void     lt_logx_set_excluded(lt_logx *x, const char *name, int excluded);
/* project math channels (reloaded when the project changes) */
lt_math_set *lt_logx_math(lt_app *app);
int      lt_logx_math_save(lt_app *app);
/* warn (toast) when a table edit changes a value too much; returns 1 when it warned */
int      lt_logx_check_change(lt_app *app, double old_v, double new_v);
/* live value of a math channel (evaluated against the latest realtime frame); LT_FALSE when the
 * name is not a math channel or it cannot be evaluated */
lt_bool  lt_logx_live_math(lt_app *app, const char *name, double *out);
/* the loaded datalog (NULL when none): what the Datalog Viewer shows and the curve overlay uses */
lt_log  *lt_logx_shared_log(lt_app *app);
/* read a log file into it (math channels appended); 0, or negative with err filled */
int      lt_logx_load_log(lt_app *app, const char *path, char *err, size_t errlen);
/* graph setup file (project graph_setup.json, else the config dir), malloc'd */
char    *lt_logx_graph_path(lt_app *app);

void lt_feat_logging_register(lt_app_ext *reg);
void lt_feat_math_register(lt_app_ext *reg);

#endif
