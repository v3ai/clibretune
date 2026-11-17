/* sim_logger.h - demo ECU trigger logger and console (internal to src/sim, used by sim_ecu.c).
 * The composite logger carries a 36-1 crank wheel with a cam sync pulse, coil and injector
 * traces at the engine speed; the console answers a few rusEFI-style commands.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_SIM_LOGGER_H
#define LT_SIM_LOGGER_H

#include "../lt.h"
#include "../ini/ini.h"

typedef struct lt_simlog {
    int    on;              /* logger started by the host */
    int    step;            /* 5-degree step within the 720 degree cycle (0..143) */
    double t_us;            /* running timestamp */
    int    synced;          /* cam pulse seen */
    int    last[6];         /* last emitted levels: pri, sec, sync, trigger, coil, inj */
    int    have_last;
    lt_u32 rng;
} lt_simlog;

#define LT_SIMLOG_RECORDS 100
/* 1 when cmd is one of the logger's start/stop/read commands (handled: reply payload in out) */
int  lt_simlog_command(lt_simlog *l, const lt_ini *ini, const lt_u8 *cmd, size_t n, double rpm,
                       lt_buf *out);
/* console line -> rusEFI style reply text (msg`..` pairs) appended to out */
void lt_sim_console(const char *text, double rpm, const char *sig, lt_buf *out);

#endif
