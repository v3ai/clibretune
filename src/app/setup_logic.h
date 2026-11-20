/* setup_logic.h - the new-engine setup checklist without UI: the steps (engine, injectors,
 * trigger, MAP, TPS, temperature sensors, wideband, ignition), finding the page of the loaded
 * ECU definition's menus that handles each one (rusEFI / FOME / epicEFI, Speeduino, MegaSquirt
 * labels and dialog names), and the per-project ticks in <project>/setup.conf.
 * Unit-tested in tests/test_guard.c; feat_setup.c draws it.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_SETUP_LOGIC_H
#define LT_SETUP_LOGIC_H

#include "../lt.h"
#include "../ini/ini.h"

#define LT_SETUP_STEPS 8

typedef struct lt_setup_step {
    const char *id;            /* "engine", "injectors", ... (stable, stored) */
    const char *title;         /* "Engine: displacement and cylinders" */
    const char *why;           /* one line on what to enter */
} lt_setup_step;

typedef struct lt_setup_link {
    char target[128];          /* dialog / table / curve / std_* name, "" when not found */
    char where[160];           /* "Sensors > Throttle Position Sensor" */
} lt_setup_link;

typedef struct lt_setup_state {
    int done[LT_SETUP_STEPS];
    int dismissed;             /* the user hid the checklist */
    int shown;                 /* it was opened once for this project (auto-open only once) */
} lt_setup_state;

const lt_setup_step *lt_setup_step_at(int i);
/* find the menu entry for every step; returns how many were found */
int  lt_setup_match(const lt_ini *ini, lt_setup_link links[LT_SETUP_STEPS]);
int  lt_setup_count_done(const lt_setup_state *s);
/* <dir>/setup.conf; a missing file gives a fresh state and returns 1, else 0 (-1 error) */
int  lt_setup_load(lt_setup_state *s, const char *dir);
int  lt_setup_save(const lt_setup_state *s, const char *dir);

#endif
