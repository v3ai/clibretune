/* sync_logic.c - see sync_logic.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "sync_logic.h"

/* ---- signature matching ---- */
static size_t sig_word_len(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0' && s[n] != ' ' && s[n] != '\t') { n++; }
    return n;
}

int lt_sync_sig_match(const char *ini_sig, const char *ecu_sig)
{
    size_t a, b;
    if (ini_sig == NULL || ecu_sig == NULL || ini_sig[0] == '\0' || ecu_sig[0] == '\0') {
        return LT_SIG_NONE;
    }
    if (lt_stricmp(ini_sig, ecu_sig) == 0) { return LT_SIG_EXACT; }
    if (lt_str_istarts(ini_sig, ecu_sig) || lt_str_istarts(ecu_sig, ini_sig)) { return LT_SIG_PARTIAL; }
    a = sig_word_len(ini_sig);
    b = sig_word_len(ecu_sig);
    if (a >= 3 && a == b && lt_strnicmp(ini_sig, ecu_sig, a) == 0) { return LT_SIG_PARTIAL; }
    return LT_SIG_NONE;
}

int lt_sync_find_inis(const lt_ini_repo *repo, const char *ecu_sig, lt_sync_ini_match *out, int cap)
{
    int n = 0, pass;
    size_t i;
    if (repo == NULL || out == NULL || cap <= 0) { return 0; }
    for (pass = LT_SIG_EXACT; pass <= LT_SIG_PARTIAL; pass++) {
        for (i = 0; i < repo->n && n < cap; i++) {
            const lt_ini_entry *e = &repo->e[i];
            if (e->signature == NULL || lt_sync_sig_match(e->signature, ecu_sig) != pass) { continue; }
            lt_strlcpy(out[n].id, e->id != NULL ? e->id : "", sizeof(out[n].id));
            lt_strlcpy(out[n].name, e->name != NULL ? e->name : out[n].id, sizeof(out[n].name));
            lt_strlcpy(out[n].signature, e->signature, sizeof(out[n].signature));
            out[n].kind = pass;
            n++;
        }
    }
    return n;
}

/* ---- tune comparison ---- */
lt_sync_cmp *lt_sync_compare(const lt_ini *ini, const lt_tune *project, const lt_ecu_mem *mem)
{
    lt_sync_cmp *c;
    size_t np, i;
    if (ini == NULL || project == NULL || mem == NULL) { return NULL; }
    c = (lt_sync_cmp *)lt_calloc(1, sizeof(*c));
    np = lt_ecu_mem_page_count(mem);
    c->proj = lt_tune_clone(project);
    /* the page images are authoritative; only a tune without them is built from named values
     * (re-encoding would otherwise create differences the user never made) */
    for (i = 0; i < np; i++) {
        size_t pl = 0;
        if (lt_tune_get_page(c->proj, (int)i, &pl) == NULL) { lt_tune_apply_to_pages(c->proj, ini); break; }
    }
    lt_tune_capture_from_pages(c->proj, ini);
    c->ecu = lt_tune_clone(project);
    lt_tune_from_mem(c->ecu, mem);
    lt_tune_capture_from_pages(c->ecu, ini);
    for (i = 0; i < np; i++) {
        size_t el = 0, pl = 0, k, bad = 0;
        const lt_u8 *e = lt_ecu_mem_page(mem, (int)i, &el);
        const lt_u8 *p = lt_tune_get_page(c->proj, (int)i, &pl);
        if (e == NULL) { continue; }
        for (k = 0; k < el; k++) {
            if (p == NULL || k >= pl || p[k] != e[k]) { bad++; }
        }
        if (bad > 0) { c->pages_diff++; c->bytes_diff += bad; }
    }
    c->diff = lt_tune_diff_compare(c->proj, c->ecu);
    return c;
}

void lt_sync_cmp_free(lt_sync_cmp *c)
{
    if (c == NULL) { return; }
    if (c->diff != NULL) { lt_tune_diff_free(c->diff); }
    if (c->proj != NULL) { lt_tune_free(c->proj); }
    if (c->ecu != NULL) { lt_tune_free(c->ecu); }
    lt_free(c);
}

lt_bool lt_sync_cmp_identical(const lt_sync_cmp *c)
{
    return (c == NULL || c->bytes_diff == 0) ? LT_TRUE : LT_FALSE;
}

void lt_sync_cmp_summary(const lt_sync_cmp *c, char *buf, size_t cap)
{
    unsigned long n = (c != NULL && c->diff != NULL) ? (unsigned long)c->diff->n : 0UL;
    if (c == NULL || c->bytes_diff == 0) { lt_strlcpy(buf, "The project tune and the ECU match.", cap); return; }
    lt_snprintf(buf, cap, "%lu setting%s differ (%lu page%s, %lu byte%s)", n, n == 1UL ? "" : "s",
                (unsigned long)c->pages_diff, c->pages_diff == 1 ? "" : "s",
                (unsigned long)c->bytes_diff, c->bytes_diff == 1 ? "" : "s");
}

static void val_text(const lt_tune_value *v, char *buf, size_t cap)
{
    char num[40];
    if (v == NULL) { lt_strlcpy(buf, "-", cap); return; }
    switch (v->kind) {
    case LT_TV_STRING: lt_strlcpy(buf, v->str != NULL ? v->str : "", cap); break;
    case LT_TV_BOOL: lt_strlcpy(buf, v->boolean ? "true" : "false", cap); break;
    case LT_TV_ARRAY: lt_snprintf(buf, cap, "[%lu values]", (unsigned long)v->n); break;
    default: lt_fmt_f64_shortest(num, sizeof(num), v->scalar); lt_strlcpy(buf, num, cap); break;
    }
}

void lt_sync_diff_values(const lt_sync_cmp *c, size_t i, char *a, size_t cap_a, char *b, size_t cap_b)
{
    if (c == NULL || c->diff == NULL || i >= c->diff->n) { a[0] = '\0'; b[0] = '\0'; return; }
    val_text(c->diff->diffs[i].value_a, a, cap_a);
    val_text(c->diff->diffs[i].value_b, b, cap_b);
}

void lt_sync_diff_line(const lt_sync_cmp *c, size_t i, char *buf, size_t cap)
{
    char a[48], b[48];
    if (c == NULL || c->diff == NULL || i >= c->diff->n) { buf[0] = '\0'; return; }
    lt_sync_diff_values(c, i, a, sizeof(a), b, sizeof(b));
    lt_snprintf(buf, cap, "%s: %s  /  %s", c->diff->diffs[i].name, a, b);
}

int lt_sync_apply_ecu(lt_tune *project, const lt_ini *ini, const lt_ecu_mem *mem)
{
    if (project == NULL || ini == NULL || mem == NULL) { return -1; }
    lt_tune_from_mem(project, mem);
    lt_tune_capture_from_pages(project, ini);
    return 0;
}

long lt_sync_apply_project(const lt_sync_cmp *c, lt_ecu_mem *mem)
{
    size_t np, i;
    long total = 0;
    if (c == NULL || mem == NULL) { return -1; }
    np = lt_ecu_mem_page_count(mem);
    for (i = 0; i < np; i++) {
        size_t el = 0, pl = 0, k, start;
        const lt_u8 *e = lt_ecu_mem_page(mem, (int)i, &el);
        const lt_u8 *p = lt_tune_get_page(c->proj, (int)i, &pl);
        if (e == NULL || p == NULL) { continue; }
        if (pl > el) { pl = el; }
        k = 0;
        while (k < pl) {
            if (p[k] == e[k]) { k++; continue; }
            start = k;
            while (k < pl && p[k] != e[k]) { k++; }
            if (lt_ecu_mem_write(mem, (int)i, start, p + start, k - start) < 0) { return -1; }
            total += (long)(k - start);
        }
    }
    return total;
}

/* ---- reconnect ---- */
int lt_reconnect_arm(lt_reconnect *rc, const lt_app_settings *s, const char *source,
                     const char *port, int delay_ms, int retries, double now_ms)
{
    int fw = strstr(source, "firmware") != NULL;
    int cmd = strstr(source, "controller-command") != NULL;
    if (s != NULL && ((cmd && !s->reconnect_cmd) || (fw && !s->reconnect_fw))) { return -1; }
    if (delay_ms < 0) { delay_ms = fw ? 8000 : (cmd ? 2000 : 1500); }
    if (retries < 0) { retries = fw ? 10 : (cmd ? 4 : 5); }
    memset(rc, 0, sizeof(*rc));
    rc->armed = 1;
    rc->max_tries = retries;
    rc->next_ms = now_ms + (double)delay_ms;
    lt_strlcpy(rc->port, port != NULL ? port : "", sizeof(rc->port));
    lt_strlcpy(rc->source, source, sizeof(rc->source));
    return 0;
}

int lt_reconnect_step(lt_reconnect *rc, double now_ms, int online)
{
    if (!rc->armed) { return 0; }
    if (online) { rc->armed = 0; return 0; }
    if (now_ms < rc->next_ms) { return 0; }
    if (rc->tries >= rc->max_tries) { rc->armed = 0; return 2; }
    rc->tries++;
    rc->next_ms = now_ms + 400.0;
    return 1;
}
