/* trail.c - live-cell trail with age-based fading (see trail.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "trail.h"

void lt_trail_clear(lt_trail *tr)
{
    tr->n = 0;
}

void lt_trail_push(lt_trail *tr, int r, int c, double now_ms)
{
    int k;
    if (tr->n > 0 && tr->r[0] == r && tr->c[0] == c) {
        tr->t[0] = now_ms;
        return;
    }
    k = tr->n < LT_TRAIL_MAX ? tr->n : LT_TRAIL_MAX - 1;
    for (; k > 0; k--) {
        tr->r[k] = tr->r[k - 1];
        tr->c[k] = tr->c[k - 1];
        tr->t[k] = tr->t[k - 1];
    }
    tr->r[0] = r;
    tr->c[0] = c;
    tr->t[0] = now_ms;
    if (tr->n < LT_TRAIL_MAX) { tr->n++; }
}

void lt_trail_prune(lt_trail *tr, double now_ms, int fade_sec)
{
    int k;
    if (fade_sec <= 0) {
        if (tr->n > LT_TRAIL_KEEP) { tr->n = LT_TRAIL_KEEP; }
        return;
    }
    /* entries are newest first, so the first expired one ends the trail */
    for (k = 0; k < tr->n; k++) {
        if (now_ms - tr->t[k] >= (double)fade_sec * 1000.0) {
            tr->n = k;
            return;
        }
    }
}

double lt_trail_alpha(const lt_trail *tr, int k, double now_ms, int fade_sec)
{
    double a, age;
    if (k < 0 || k >= tr->n) { return 0.0; }
    if (fade_sec <= 0) {
        if (k >= LT_TRAIL_KEEP) { return 0.0; }
        return 1.0 - 0.85 * (double)k / (double)LT_TRAIL_KEEP;
    }
    age = now_ms - tr->t[k];
    if (age < 0.0) { age = 0.0; }
    a = 1.0 - age / ((double)fade_sec * 1000.0);
    return a > 0.0 ? a : 0.0;
}
