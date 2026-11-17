/* inilib.c - signature matching against the INI repository; change a project's INI.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "inilib.h"
#include "proj_priv.h"
#include "../plat/plat.h"
#include "../ini/ini.h"

static int is_alnum_c(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

size_t lt_sig_normalize(const char *sig, char *out, size_t cap)
{
    size_t n = 0;
    int pending = 0;
    if (cap == 0) { return 0; }
    if (sig != NULL) {
        for (; *sig != '\0'; sig++) {
            char c = *sig;
            if (is_alnum_c(c)) {
                if (pending && n > 0 && n + 1 < cap) { out[n++] = ' '; }
                pending = 0;
                if (c >= 'A' && c <= 'Z') { c = (char)(c - 'A' + 'a'); }
                if (n + 1 < cap) { out[n++] = c; }
            } else {
                pending = 1;
            }
        }
    }
    out[n] = '\0';
    return n;
}

static const char *last_token(const char *s)
{
    const char *sp = strrchr(s, ' ');
    return sp != NULL ? sp + 1 : s;
}

static size_t first_token_len(const char *s)
{
    const char *sp = strchr(s, ' ');
    return sp != NULL ? (size_t)(sp - s) : strlen(s);
}

static const char *const sig_keywords[] = { "uaefi", "speeduino", "rusefi", "epicefi", "megasquirt" };

int lt_sig_compare(const char *ecu_sig, const char *ini_sig, const char *ini_prefix)
{
    char a[256], b[256], p[256];
    const char *ta, *tb;
    size_t i, la, lb;
    (void)lt_sig_normalize(ecu_sig, a, sizeof(a));
    (void)lt_sig_normalize(ini_sig, b, sizeof(b));
    if (strcmp(a, b) == 0) { return LT_SIG_EXACT; }
    if (ini_prefix != NULL) {
        size_t lp = lt_sig_normalize(ini_prefix, p, sizeof(p));
        if (lp > 0 && strncmp(a, p, lp) == 0) { return LT_SIG_EXACT; }
    }
    /* same long trailing token (build hash) */
    ta = last_token(a);
    tb = last_token(b);
    if (strlen(ta) > 6 && strcmp(ta, tb) == 0) { return LT_SIG_EXACT; }
    if (a[0] == '\0' || b[0] == '\0') { return LT_SIG_MISMATCH; }
    if (strstr(a, b) != NULL || strstr(b, a) != NULL) { return LT_SIG_PARTIAL; }
    la = first_token_len(a);
    lb = first_token_len(b);
    if (la == lb && strncmp(a, b, la) == 0) { return LT_SIG_PARTIAL; }
    for (i = 0; i < LT_COUNTOF(sig_keywords); i++) {
        if (strstr(a, sig_keywords[i]) != NULL && strstr(b, sig_keywords[i]) != NULL) {
            return LT_SIG_PARTIAL;
        }
    }
    return LT_SIG_MISMATCH;
}

static const char *entry_name(const lt_ini_entry *e)
{
    return e->name != NULL ? e->name : (e->id != NULL ? e->id : "");
}

size_t lt_inilib_find(const lt_ini_repo *r, const char *ecu_sig, lt_inilib_match *out, size_t cap)
{
    lt_inilib_match *all;
    size_t i, n = 0, j, k;
    if (r == NULL || r->n == 0 || ecu_sig == NULL) { return 0; }
    all = (lt_inilib_match *)lt_calloc(r->n, sizeof(*all));
    for (i = 0; i < r->n; i++) {
        int m = lt_sig_compare(ecu_sig, r->e[i].signature != NULL ? r->e[i].signature : "", NULL);
        if (m == LT_SIG_MISMATCH) { continue; }
        /* insertion sort: exact first, then by name */
        for (j = n; j > 0; j--) {
            const lt_inilib_match *q = &all[j - 1];
            if (q->match > m || (q->match == m &&
                lt_stricmp(entry_name(&r->e[q->index]), entry_name(&r->e[i])) <= 0)) { break; }
        }
        for (k = n; k > j; k--) { all[k] = all[k - 1]; }
        all[j].index = i;
        all[j].match = m;
        n++;
    }
    for (i = 0; i < n && i < cap && out != NULL; i++) { out[i] = all[i]; }
    lt_free(all);
    return n;
}

int lt_project_set_ini(lt_project *p, const char *ini_src)
{
    lt_ini *ini;
    char *dst, *tmp;
    int rc;
    if (p == NULL || p->path == NULL || ini_src == NULL) {
        lt_err_set("No project or INI given");
        return LT_EINVAL;
    }
    ini = lt_ini_load_file(ini_src, NULL);
    if (ini == NULL) { return LT_EDATA; }
    dst = lt_project_ini_path(p);
    tmp = lt_proj_join3(p->path, "projectCfg", "definition.ini.new");
    rc = lt_proj_copy_file(ini_src, tmp);
    if (rc == 0) { rc = lt_plat_rename(tmp, dst); }
    lt_free(tmp);
    lt_free(dst);
    if (rc < 0) {
        lt_ini_free(ini);
        return rc;
    }
    lt_proj_set_str(&p->cfg.signature, ini->signature != NULL ? ini->signature : "");
    lt_ini_free(ini);
    return lt_project_save_config(p);
}
