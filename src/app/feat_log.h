/* feat_log.h - shared state of the data logging features (feat_logging.c, feat_math.c) used by
 * the logger / viewer tabs (dialogs_log.c): smart recording, sample rate, recording channel
 * selection, alert rules, project math channels.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_FEAT_LOG_H
#define LT_FEAT_LOG_H

#include "app_ext.h"
#include "../datalog/dl_ana.h"

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

void lt_feat_logging_register(lt_app_ext *reg);
void lt_feat_math_register(lt_app_ext *reg);

#endif
