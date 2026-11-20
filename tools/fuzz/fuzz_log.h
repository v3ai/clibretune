/* fuzz_log.h - what the log viewer does with a parsed log (shared by the MLG/MSL/CSV harnesses) */
#ifndef LT_FUZZ_LOG_H
#define LT_FUZZ_LOG_H

#include "fuzz_common.h"
#include "datalog/datalog.h"
#include "datalog/dl_ana.h"

static void fz_log_use(lt_log *l)
{
    lt_buf b;
    lt_player p;
    lt_stats st;
    int c, k;
    (void)lt_log_duration(l);
    for (c = 0; c < l->n_ch && c < 64; c++) {
        (void)lt_stats_log_channel(l, c, 0, l->n, &st);
        if (l->names && l->names[c]) (void)lt_log_channel_index(l, l->names[c]);
    }
    lt_player_init(&p, l);
    lt_player_set_loop(&p, LT_TRUE);
    for (k = 0; k < 8; k++) (void)lt_player_advance(&p, 0.37);
    lt_player_seek_time(&p, 1.0);
    lt_buf_init(&b);
    lt_msl_format(&b, l);
    lt_buf_free(&b);
}

#endif
