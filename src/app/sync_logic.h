/* sync_logic.h - connect/tune-sync logic without UI: project tune vs ECU tune comparison,
 * signature classification and INI matching, reconnect scheduling.  Used by feat_sync.c
 * and feat_reconnect.c; unit-tested in tests/test_sync.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_SYNC_LOGIC_H
#define LT_SYNC_LOGIC_H

#include "app.h"

/* ---- signature matching ---- */
enum { LT_SIG_NONE = 0, LT_SIG_EXACT = 1, LT_SIG_PARTIAL = 2 };
/* How well `ini_sig` fits an ECU reporting `ecu_sig`: EXACT (case-insensitive equal),
 * PARTIAL (same platform: common leading word/prefix, e.g. "speeduino 202305" vs
 * "speeduino 202310"), else NONE. Empty signatures give NONE. */
int lt_sync_sig_match(const char *ini_sig, const char *ecu_sig);

typedef struct lt_sync_ini_match {
    char id[96], name[96], signature[96];
    int kind;                       /* LT_SIG_EXACT / LT_SIG_PARTIAL */
} lt_sync_ini_match;
/* Repository INIs that fit the ECU signature, exact matches first. Returns the count
 * (<= cap); an unusable repository gives 0. */
int lt_sync_find_inis(const lt_ini_repo *repo, const char *ecu_sig, lt_sync_ini_match *out, int cap);

/* ---- project tune vs ECU tune ---- */
typedef struct lt_sync_cmp {
    lt_tune *proj, *ecu;            /* both normalised through the INI (pages + named values) */
    lt_tune_diff *diff;             /* named constants that differ, project = a, ECU = b */
    size_t pages_diff, bytes_diff;  /* raw page comparison */
} lt_sync_cmp;
/* Compare the project tune with the ECU image in `mem`. NULL on bad arguments. */
lt_sync_cmp *lt_sync_compare(const lt_ini *ini, const lt_tune *project, const lt_ecu_mem *mem);
void         lt_sync_cmp_free(lt_sync_cmp *c);
lt_bool      lt_sync_cmp_identical(const lt_sync_cmp *c);
/* One-line summary, e.g. "12 settings differ (3 pages, 214 bytes)". */
void lt_sync_cmp_summary(const lt_sync_cmp *c, char *buf, size_t cap);
/* Display text of difference i ("name: project  /  ECU"). */
void lt_sync_diff_line(const lt_sync_cmp *c, size_t i, char *buf, size_t cap);
/* Project and ECU value text of difference i. */
void lt_sync_diff_values(const lt_sync_cmp *c, size_t i, char *a, size_t cap_a, char *b, size_t cap_b);
/* Make the ECU image the project tune (project tune <- mem). */
int  lt_sync_apply_ecu(lt_tune *project, const lt_ini *ini, const lt_ecu_mem *mem);
/* Put the project tune into `mem` as dirty ranges (caller sends them with
 * lt_proto_write_dirty). Returns the number of bytes changed or <0. */
long lt_sync_apply_project(const lt_sync_cmp *c, lt_ecu_mem *mem);

/* ---- reconnect scheduling ---- */
typedef struct lt_reconnect {
    int armed, tries, max_tries;
    double next_ms;                 /* earliest time of the next attempt */
    char port[128];
    char source[24];
} lt_reconnect;
/* source: "link-lost", "controller-command", "firmware" (anything containing "firmware"); delay
 * and retry counts negative = defaults (2000/4 command, 8000/10 firmware, 1500/5 link lost).
 * Returns 0 when armed, -1 when the matching setting disables it (settings may be NULL). */
int  lt_reconnect_arm(lt_reconnect *rc, const lt_app_settings *s, const char *source,
                      const char *port, int delay_ms, int retries, double now_ms);
/* Decide the next action: 0 nothing yet, 1 attempt now, 2 give up (disarms). When 1 is
 * returned the attempt is counted and the next one scheduled 400 ms later. */
int  lt_reconnect_step(lt_reconnect *rc, double now_ms, int online);
/* Public entry for other features (controller commands, firmware update): arms the app's
 * reconnect with the last port. Returns 0 or -1 when disabled in the settings. */
int  lt_app_reconnect_request(lt_app *app, const char *source, int delay_ms, int retries);

#endif
