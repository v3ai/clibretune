/* i18n.c - lt_tr lookup, language choice and its persistence (see i18n.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "i18n.h"

static int cur_lang = 0;

int lt_i18n_count(void) { return LT_I18N_NLANG; }

const char *lt_i18n_code(int idx)
{
    return (idx >= 0 && idx < LT_I18N_NLANG) ? lt_i18n_langs[idx].code : "en";
}

const char *lt_i18n_name(int idx)
{
    return (idx >= 0 && idx < LT_I18N_NLANG) ? lt_i18n_langs[idx].name : "English";
}

int lt_i18n_find(const char *code)
{
    int i;
    if (code == NULL) { return -1; }
    for (i = 0; i < LT_I18N_NLANG; i++) {
        if (strcmp(lt_i18n_langs[i].code, code) == 0) { return i; }
    }
    return -1;
}

void lt_i18n_set(int idx) { cur_lang = (idx >= 0 && idx < LT_I18N_NLANG) ? idx : 0; }
int  lt_i18n_get(void) { return cur_lang; }

const char *lt_i18n_lookup(int idx, const char *en)
{
    const lt_i18n_lang *L;
    int lo = 0, hi;
    if (en == NULL) { return ""; }
    if (idx <= 0 || idx >= LT_I18N_NLANG) { return en; }
    L = &lt_i18n_langs[idx];
    hi = L->n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = strcmp(en, L->tab[mid].en);
        if (c == 0) { return L->tab[mid].tr; }
        if (c < 0) { hi = mid - 1; } else { lo = mid + 1; }
    }
    return en;
}

const char *lt_tr(const char *en) { return lt_i18n_lookup(cur_lang, en); }

const char *lt_tr_list(const char *list, char *out, size_t cap)
{
    const char *p = list;
    char piece[96];
    if (cap == 0) { return list; }
    out[0] = '\0';
    if (cur_lang == 0) { return list; }
    while (*p != '\0') {
        size_t k = 0;
        while (p[k] != '\0' && p[k] != '|') { k++; }
        if (k >= sizeof(piece)) { k = sizeof(piece) - 1; }
        memcpy(piece, p, k);
        piece[k] = '\0';
        if (out[0] != '\0') { (void)lt_strlcat(out, "|", cap); }
        (void)lt_strlcat(out, lt_tr(piece), cap);
        p += k;
        while (*p != '\0' && *p != '|') { p++; }     /* piece was cut */
        if (*p == '|') { p++; }
    }
    return out;
}

int lt_i18n_load(const char *dir)
{
    char path[600];
    char *txt;
    char *p;
    int idx;
    size_t len = 0;
    lt_i18n_set(0);
    if (dir == NULL) { return -1; }
    lt_snprintf(path, sizeof(path), "%s/language.conf", dir);
    txt = lt_file_read(path, &len);
    if (txt == NULL) { return -1; }
    p = strstr(txt, "language=");
    if (p != NULL) {
        char code[16];
        size_t k = 0;
        p += 9;
        while (p[k] != '\0' && p[k] != '\n' && p[k] != '\r' && k + 1 < sizeof(code)) {
            code[k] = p[k];
            k++;
        }
        code[k] = '\0';
        idx = lt_i18n_find(code);
        if (idx >= 0) { lt_i18n_set(idx); }
    }
    lt_free(txt);
    return 0;
}

int lt_i18n_save(const char *dir)
{
    char path[600], line[48];
    if (dir == NULL) { return -1; }
    lt_snprintf(path, sizeof(path), "%s/language.conf", dir);
    lt_snprintf(line, sizeof(line), "language=%s\n", lt_i18n_code(cur_lang));
    return lt_file_write(path, line, strlen(line));
}
