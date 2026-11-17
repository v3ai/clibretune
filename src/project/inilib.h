/* inilib.h - INI library lookup by ECU signature, and "change a project's INI".
 *
 * Small, UI-free API over the local INI repository (lt_ini_repo in project.h):
 *   lt_sig_compare      how well an ECU signature fits an INI signature (port of the original's
 *                       normalised comparison: exact / partial / mismatch)
 *   lt_inilib_find      the repository INIs matching an ECU signature, best first; used by the
 *                       "ECU signature differs" dialog to offer "matching INIs from the INI library"
 *   lt_project_set_ini  replace a project's projectCfg/definition.ini and update its signature
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_INILIB_H
#define LT_INILIB_H

#include "project.h"

LT_BEGIN_DECLS

#define LT_SIG_MISMATCH 0
#define LT_SIG_PARTIAL  1
#define LT_SIG_EXACT    2

/* lowercase alphanumerics, every other run of characters collapsed to one space, trimmed.
 * Writes at most cap bytes (NUL-terminated); returns the length written. */
size_t lt_sig_normalize(const char *sig, char *out, size_t cap);
/* LT_SIG_EXACT / LT_SIG_PARTIAL / LT_SIG_MISMATCH. ini_prefix (the INI's signaturePrefix) may be NULL. */
int lt_sig_compare(const char *ecu_sig, const char *ini_sig, const char *ini_prefix);

typedef struct lt_inilib_match {
    size_t index;        /* index into repo->e[] (valid until the repository is closed/changed) */
    int    match;        /* LT_SIG_EXACT or LT_SIG_PARTIAL */
} lt_inilib_match;

/* Repository entries that match ecu_sig, exact matches first, then by name. Writes up to cap
 * results into out (may be NULL when cap == 0) and returns the total number of matches.
 * Typical use:
 *     lt_ini_repo repo; lt_inilib_match m[8]; size_t n;
 *     if (lt_ini_repo_open(&repo, NULL) == 0) {
 *         n = lt_inilib_find(&repo, ecu_sig, m, 8);
 *         ... repo.e[m[0].index].name / .signature, lt_ini_repo_path(&repo, repo.e[..].id) ...
 *         lt_ini_repo_close(&repo);
 *     } */
size_t lt_inilib_find(const lt_ini_repo *r, const char *ecu_sig, lt_inilib_match *out, size_t cap);

/* Replace <project>/projectCfg/definition.ini by the file ini_src (which must parse) and store
 * its signature in the project config. Does not touch the tune. 0 or negative (lt_err_get). */
int lt_project_set_ini(lt_project *p, const char *ini_src);

LT_END_DECLS

#endif
