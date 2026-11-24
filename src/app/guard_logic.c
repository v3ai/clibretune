/* guard_logic.c - never lose work, without UI (see guard_logic.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "guard_logic.h"
#include "../plat/plat.h"

static void add_btn(lt_guard_q *q, const char *label, int act)
{
    if (q->n > 0) { lt_strlcat(q->buttons, "|", sizeof(q->buttons)); }
    lt_strlcat(q->buttons, label, sizeof(q->buttons));
    q->act[q->n++] = act;
}

void lt_guard_unburned_text(int unburned, char *out, size_t cap)
{
    if (unburned == 1) { lt_strlcpy(out, "1 unburned change", cap); }
    else if (unburned > 1) { lt_snprintf(out, cap, "%d unburned changes", unburned); }
    else { lt_strlcpy(out, "Unburned changes", cap); }
}

int lt_guard_question(int what, int unburned, int unsaved, lt_guard_q *q)
{
    static const char *const verb[3] = { "quit", "close", "disconnect" };
    char b[64], ram[240];
    const char *v = verb[what >= 0 && what <= 2 ? what : 0];
    memset(q, 0, sizeof(*q));
    if (what == LT_GUARD_DISCONNECT) { unsaved = 0; }      /* the tune keeps the ECU's values */
    if (unburned == 0 && !unsaved) { return 0; }
    if (unburned > 1) {
        lt_snprintf(ram, sizeof(ram), "%d changed settings are only in the ECU's working memory. The ECU "
                    "forgets them when it is switched off unless you burn them.", unburned);
    } else if (unburned == 1) {
        lt_strlcpy(ram, "1 changed setting is only in the ECU's working memory. The ECU forgets it when "
                   "it is switched off unless you burn it.", sizeof(ram));
    } else {
        lt_strlcpy(ram, "Some changes are only in the ECU's working memory. The ECU forgets them when it "
                   "is switched off unless you burn them.", sizeof(ram));
    }
    if (unburned != 0 && unsaved) {
        lt_strlcpy(q->title, "Burn and save your changes?", sizeof(q->title));
        lt_snprintf(q->msg, sizeof(q->msg), "%s\n\nThe project's tune file is not saved either.", ram);
    } else if (unburned != 0) {
        lt_strlcpy(q->title, "Burn your changes?", sizeof(q->title));
        lt_snprintf(q->msg, sizeof(q->msg), "%s%s", ram,
                    what == LT_GUARD_DISCONNECT ? "\n\nclibretune keeps them in your tune either way." : "");
    } else {
        lt_strlcpy(q->title, "Save your changes?", sizeof(q->title));
        lt_strlcpy(q->msg, "The tune has changes that are not saved in the project.", sizeof(q->msg));
    }
    if (unburned != 0) {
        lt_snprintf(b, sizeof(b), "Burn and %s", v);
        add_btn(q, b, LT_GA_BURN);
    }
    if (unsaved) {
        lt_snprintf(b, sizeof(b), "Save and %s", v);
        add_btn(q, b, LT_GA_SAVE);
        lt_snprintf(b, sizeof(b), "%c%s without saving", v[0] - 'a' + 'A', v + 1);
        add_btn(q, b, LT_GA_PROCEED);
    } else {
        lt_snprintf(b, sizeof(b), "%c%s without burning", v[0] - 'a' + 'A', v + 1);
        add_btn(q, b, LT_GA_PROCEED);
    }
    add_btn(q, "Cancel", LT_GA_CANCEL);
    return 1;
}

/* ------------------------------------------------------------------ */
char *lt_guard_recovery_path(const char *dir)
{
    char *rd = lt_path_join(dir, "recovery"), *p;
    if (rd == NULL) { return NULL; }
    p = lt_path_join(rd, "autosave.msq");
    lt_free(rd);
    return p;
}

static char *stamp_path(const char *dir)
{
    char *rd = lt_path_join(dir, "recovery"), *p;
    if (rd == NULL) { return NULL; }
    p = lt_path_join(rd, "autosave.txt");
    lt_free(rd);
    return p;
}

int lt_guard_recovery_write(const char *dir, const lt_tune *t)
{
    char *rd = lt_path_join(dir, "recovery"), *p, *sp;
    char stamp[24];
    int rc = -1;
    if (rd == NULL) { return -1; }
    if (lt_plat_mkdirs(rd) == 0) {
        p = lt_guard_recovery_path(dir);
        sp = stamp_path(dir);
        rc = p != NULL ? lt_tune_save(t, p) : -1;
        if (rc == 0 && sp != NULL) {
            lt_plat_timestamp(stamp, sizeof(stamp));
            (void)lt_plat_write_atomic(sp, stamp, strlen(stamp));
        }
        lt_free(p);
        lt_free(sp);
    }
    lt_free(rd);
    return rc < 0 ? -1 : 0;
}

int lt_guard_recovery_remove(const char *dir)
{
    char *p = lt_guard_recovery_path(dir), *sp = stamp_path(dir), *rd = lt_path_join(dir, "recovery");
    int rc = 0;
    if (p != NULL && lt_file_exists(p) && lt_plat_remove(p) != 0) { rc = -1; }
    if (sp != NULL && lt_file_exists(sp)) { (void)lt_plat_remove(sp); }
    if (rd != NULL && lt_plat_is_dir(rd)) { (void)lt_plat_remove(rd); }       /* only when empty */
    lt_free(p);
    lt_free(sp);
    lt_free(rd);
    return rc;
}

int lt_guard_recovery_pending(const char *dir, const lt_tune *current, char *when, size_t wcap,
                              int *n_diff)
{
    char *p = lt_guard_recovery_path(dir), *sp = stamp_path(dir), *s;
    lt_tune *t;
    int pending = 0;
    if (wcap > 0) { when[0] = '\0'; }
    if (n_diff != NULL) { *n_diff = 0; }
    if (p == NULL || !lt_file_exists(p)) { lt_free(p); lt_free(sp); return 0; }
    t = lt_tune_load(p);
    if (t != NULL) {
        if (current == NULL) {
            pending = 1;
        } else {
            lt_tune_diff *d = lt_tune_diff_compare(current, t);
            if (d != NULL) {
                pending = !lt_tune_diff_identical(d);
                if (n_diff != NULL) { *n_diff = (int)d->n; }
                /* page images differ but no named value does: still worth offering */
                if (!pending) {
                    int pg;
                    for (pg = 0; pg < 256 && !pending; pg++) {
                        if (t->page_len[pg] != current->page_len[pg] ||
                            (t->page_len[pg] > 0 &&
                             memcmp(t->pages[pg], current->pages[pg], t->page_len[pg]) != 0)) {
                            pending = 1;
                        }
                    }
                }
                lt_tune_diff_free(d);
            }
        }
        lt_tune_free(t);
    }
    if (!pending) {
        (void)lt_guard_recovery_remove(dir);
    } else if (sp != NULL && (s = lt_file_read(sp, NULL)) != NULL) {
        size_t n = strlen(s);
        while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) { s[--n] = '\0'; }
        lt_strlcpy(when, s, wcap);
        lt_free(s);
    }
    lt_free(p);
    lt_free(sp);
    return pending;
}
