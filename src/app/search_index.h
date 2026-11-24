/* search_index.h - the settings search of the sidebar (Ctrl+K): every field inside every dialog
 * of the loaded definition, found by its caption or its constant name. Built once per INI (a
 * few hundred KB at most for the largest rusEFI definitions: entries point into the INI), then
 * ranked per keystroke: a whole word beats a word start beats a substring. No UI; unit-tested
 * in tests/test_search.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_SEARCH_INDEX_H
#define LT_SEARCH_INDEX_H

#include "../lt.h"
#include "../ini/ini.h"

typedef struct lt_sidx lt_sidx;

typedef struct lt_sidx_hit {
    const char *caption;            /* the field's caption (its constant name when it has none) */
    const char *title;              /* the dialog it opens in, as the sidebar names it */
    const char *target;             /* that dialog's name: what the tab opens */
    const lt_ini_comp *comp;        /* the field inside it (or inside one of its panels) */
    size_t entry;                   /* for lt_sidx_visible */
    int score;
    int in_caption;                 /* words of the query found in the caption itself */
} lt_sidx_hit;

/* index every field (FIELD, DISPLAYONLY, CHECKBOX, SLIDER, SETTINGSEL) of the dialogs the menus
 * open, through their nested panels; dialogs no menu reaches are indexed under their own title */
lt_sidx *lt_sidx_build(const lt_ini *ini);
void     lt_sidx_free(lt_sidx *x);
size_t   lt_sidx_count(const lt_sidx *x);

/* The best `max` hits for q, best first (ties: more words found in the caption rather than only
 * in the constant name, then the shorter caption, then INI order); the array
 * stays valid until the next query with another text or lt_sidx_free. *total (may be NULL) is
 * the number of fields that match. Every word of q must match the caption or the name. */
size_t lt_sidx_query(lt_sidx *x, const char *q, size_t max, const lt_sidx_hit **out, size_t *total);

/* how well q matches text: 0 not at all, else for each word of q 3 (a whole word of text), 2
 * (the start of a word) or 1 (inside a word), summed; camelCase and snake_case split words.
 * The tree uses it too so that its entries and the fields rank on the same scale. */
int      lt_sidx_score(const char *text, const char *q);

/* whether the field would be drawn: its own condition, those of the panels around it and that
 * of the menu entry, each tested by fn (1 = true) */
typedef int (*lt_sidx_cond_fn)(void *ud, const char *cond);
int      lt_sidx_visible(const lt_sidx *x, size_t entry, lt_sidx_cond_fn fn, void *ud);

#endif
