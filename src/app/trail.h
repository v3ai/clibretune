/* trail.h - the live-cell trail of the table editor: the cells the ECU operating point visited,
 * newest first, each with the time it was last occupied.  With a fade time (Settings > Table
 * display, "Trail fade") a cell disappears that many seconds after the operating point left it
 * and its opacity falls linearly with age; with fade 0 the trail never fades and keeps the last
 * LT_TRAIL_KEEP cells, dimmer by position.  Pure data, no UI (tables_table.c draws it).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_TRAIL_H
#define LT_TRAIL_H

#include "../lt.h"

#define LT_TRAIL_MAX 48        /* cells kept while fading by age */
#define LT_TRAIL_KEEP 20       /* cells kept when the trail never fades */

typedef struct lt_trail {
    int r[LT_TRAIL_MAX], c[LT_TRAIL_MAX];
    double t[LT_TRAIL_MAX];    /* ms of the last visit */
    int n;
} lt_trail;

void lt_trail_clear(lt_trail *tr);
/* the operating point is at (r, c) now: a new head entry, or the head's time refreshed when it
 * did not move; the oldest entry drops out when the trail is full */
void lt_trail_push(lt_trail *tr, int r, int c, double now_ms);
/* drop what has faded out (age >= fade_sec) or, with fade_sec 0, everything past LT_TRAIL_KEEP */
void lt_trail_prune(lt_trail *tr, double now_ms, int fade_sec);
/* opacity 0..1 of entry k (0 = newest): 1 - age / fade, or 1 - 0.85 k / LT_TRAIL_KEEP when the
 * trail never fades; 0 for entries that are out of range or expired */
double lt_trail_alpha(const lt_trail *tr, int k, double now_ms, int fade_sec);

#endif
