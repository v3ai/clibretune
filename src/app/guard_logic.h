/* guard_logic.h - never lose work, without UI: the question asked before quit / close project /
 * disconnect when changes would be lost (text, buttons and what each button does), and the
 * autosave recovery file of a project (<project>/recovery/autosave.msq). Unit-tested in
 * tests/test_guard.c; feat_guard.c draws and acts.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_GUARD_LOGIC_H
#define LT_GUARD_LOGIC_H

#include "../lt.h"
#include "../tune/tune.h"

#define LT_GUARD_QUIT       0
#define LT_GUARD_CLOSE      1
#define LT_GUARD_DISCONNECT 2

/* what a button does */
#define LT_GA_CANCEL  0      /* stay */
#define LT_GA_BURN    1      /* burn to flash (which also saves the project tune), then go on */
#define LT_GA_SAVE    2      /* save the project tune, then go on */
#define LT_GA_PROCEED 3      /* go on; changes are dropped (or were kept elsewhere) */

/* unburned: count of settings only in ECU RAM, 0 none, LT_GUARD_SOME when the count is not
 * known */
#define LT_GUARD_SOME (-1)

typedef struct lt_guard_q {
    char title[64];
    char msg[480];
    char buttons[128];       /* "A|B|C" for lt_ui_msgbox */
    int  act[4];             /* LT_GA_* per button, 1-based button k -> act[k-1] */
    int  n;                  /* buttons */
} lt_guard_q;

/* 1 and *q filled when `what` would lose something: unburned settings (online ECU only) or,
 * for quit/close, unsaved project changes. 0: nothing to ask, just do it. */
int lt_guard_question(int what, int unburned, int unsaved, lt_guard_q *q);
/* "3 unburned changes", "1 unburned change", "Unburned changes" */
void lt_guard_unburned_text(int unburned, char *out, size_t cap);

/* recovery file of the project in `dir` (malloc'd) */
char *lt_guard_recovery_path(const char *dir);
/* write tune as the recovery file (atomic; creates recovery/). 0 ok */
int  lt_guard_recovery_write(const char *dir, const lt_tune *t);
/* remove it (0 ok or absent) */
int  lt_guard_recovery_remove(const char *dir);
/* After the project was opened: 1 when a recovery file exists that differs from `current`
 * (the project's tune; NULL = none) - a crash or kill left work behind; a recovery file
 * with no differences is removed and 0 returned. *when gets its time stamp ("" unknown). */
int  lt_guard_recovery_pending(const char *dir, const lt_tune *current, char *when, size_t wcap,
                               int *n_diff);

#endif
