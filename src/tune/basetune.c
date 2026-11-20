/* basetune.c - Base Tune library catalog: lookup, filtering, decoding of the embedded files.
 * The embedded data itself is basetune_data.c (CC BY-NC 4.0, generated).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "basetune.h"

size_t lt_basetune_count(void)
{
    return lt_basetune_tab_n;
}

const lt_basetune *lt_basetune_at(size_t i)
{
    return i < lt_basetune_tab_n ? &lt_basetune_tab[i] : NULL;
}

const lt_basetune *lt_basetune_find(const char *key)
{
    size_t i;
    if (key == NULL) { return NULL; }
    for (i = 0; i < lt_basetune_tab_n; i++) {
        if (strcmp(lt_basetune_tab[i].key, key) == 0) { return &lt_basetune_tab[i]; }
    }
    return NULL;
}

char *lt_basetune_text(const lt_basetune *b, size_t *len)
{
    lt_buf buf;
    size_t i;
    lt_buf_init(&buf);
    if (b != NULL) {
        for (i = 0; i < b->n_chunks; i++) { lt_buf_puts(&buf, b->chunks[i]); }
    }
    if (len != NULL) { *len = buf.len; }
    return lt_buf_take(&buf);
}

lt_tune *lt_basetune_load(const lt_basetune *b)
{
    size_t len = 0;
    char *txt;
    lt_tune *t;
    if (b == NULL || b->n_chunks == 0) {
        lt_err_set("No such base tune");
        return NULL;
    }
    txt = lt_basetune_text(b, &len);
    t = lt_tune_parse_msq(txt, len);
    lt_free(txt);
    return t;
}

/* ---- search ---- */
static int lower_ascii(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* case-insensitive (ASCII) search of needle[0..n) in hay */
static lt_bool ci_find(const char *hay, const char *needle, size_t n)
{
    size_t i, j, hl;
    if (n == 0) { return LT_TRUE; }
    if (hay == NULL) { return LT_FALSE; }
    hl = strlen(hay);
    for (i = 0; i + n <= hl; i++) {
        for (j = 0; j < n; j++) {
            if (lower_ascii((unsigned char)hay[i + j]) != lower_ascii((unsigned char)needle[j])) { break; }
        }
        if (j == n) { return LT_TRUE; }
    }
    return LT_FALSE;
}

static lt_bool word_in(const lt_basetune *b, const char *w, size_t n)
{
    return ci_find(b->name, w, n) || ci_find(b->make, w, n) || ci_find(b->board, w, n) ||
           ci_find(b->provider, w, n) || ci_find(b->description, w, n) ||
           ci_find(b->signature, w, n) || ci_find(b->kind, w, n);
}

lt_bool lt_basetune_match(const lt_basetune *b, const char *make, const char *query)
{
    const char *p;
    if (b == NULL) { return LT_FALSE; }
    if (make != NULL && make[0] != '\0' && lt_stricmp(make, b->make) != 0) { return LT_FALSE; }
    p = query != NULL ? query : "";
    for (;;) {
        size_t n = 0;
        while (*p == ' ' || *p == '\t') { p++; }
        if (*p == '\0') { break; }
        while (p[n] != '\0' && p[n] != ' ' && p[n] != '\t') { n++; }
        if (!word_in(b, p, n)) { return LT_FALSE; }
        p += n;
    }
    return LT_TRUE;
}

size_t lt_basetune_filter(const char *make, const char *query, size_t *out, size_t cap)
{
    size_t i, n = 0;
    for (i = 0; i < lt_basetune_tab_n; i++) {
        if (!lt_basetune_match(&lt_basetune_tab[i], make, query)) { continue; }
        if (out != NULL && n < cap) { out[n] = i; }
        n++;
    }
    return n;
}

/* distinct values of one field, in catalog order */
static size_t distinct(int field, const char **out, size_t cap)
{
    size_t i, j, n = 0;
    for (i = 0; i < lt_basetune_tab_n; i++) {
        const char *v = field == 0 ? lt_basetune_tab[i].make : lt_basetune_tab[i].provider;
        lt_bool seen = LT_FALSE;
        for (j = 0; j < i; j++) {
            const char *u = field == 0 ? lt_basetune_tab[j].make : lt_basetune_tab[j].provider;
            if (strcmp(u, v) == 0) { seen = LT_TRUE; break; }
        }
        if (seen) { continue; }
        if (out != NULL && n < cap) { out[n] = v; }
        n++;
    }
    return n;
}

size_t lt_basetune_makes(const char **out, size_t cap)
{
    return distinct(0, out, cap);
}

size_t lt_basetune_providers(const char **out, size_t cap)
{
    return distinct(1, out, cap);
}

int lt_basetune_family(const char *signature)
{
    static const char *const rus[] = { "rusefi", "fome", "epicefi", "uaefi", "miata", "hellen" };
    size_t i;
    if (signature == NULL || signature[0] == '\0') { return LT_BASETUNE_FAM_UNKNOWN; }
    if (ci_find(signature, "speeduino", 9)) { return LT_BASETUNE_FAM_SPEEDUINO; }
    for (i = 0; i < LT_COUNTOF(rus); i++) {
        if (ci_find(signature, rus[i], strlen(rus[i]))) { return LT_BASETUNE_FAM_RUSEFI; }
    }
    return LT_BASETUNE_FAM_UNKNOWN;
}

lt_bool lt_basetune_named(const char *provider)
{
    return provider != NULL && provider[0] != '\0' && lt_stricmp(provider, "N/A") != 0 &&
           lt_stricmp(provider, "unknown") != 0;
}

void lt_basetune_credit(const lt_basetune *b, char *out, size_t cap)
{
    if (b == NULL) {
        lt_snprintf(out, cap, "Speeduino base tunes: %s, %s, %s (non-commercial use only)",
                    LT_BASETUNE_AUTHORS, lt_basetune_source, LT_BASETUNE_LICENSE);
        return;
    }
    lt_snprintf(out, cap, "Tune by %s - Speeduino Tunes, %s - %s (non-commercial use only)",
                lt_basetune_named(b->provider) ? b->provider : "a contributor not named upstream",
                LT_BASETUNE_AUTHORS, LT_BASETUNE_LICENSE);
}

void lt_basetune_origin(const lt_basetune *b, char *out, size_t cap)
{
    if (b == NULL) { lt_strlcpy(out, "", cap); return; }
    lt_snprintf(out, cap, "Made from the Speeduino base tune \"%s\" (%s) by %s, %s, %s, licensed under %s "
                "%s; values converted to this project's ECU definition.",
                b->name, b->key, lt_basetune_named(b->provider) ? b->provider : "a contributor not named upstream",
                LT_BASETUNE_AUTHORS, lt_basetune_source, LT_BASETUNE_LICENSE, LT_BASETUNE_LICENSE_URL);
}
