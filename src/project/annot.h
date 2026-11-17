/* annot.h - change annotations: notes attached to a constant, a table or a single table cell
 * (key "const", "table" or "table:row:col"), kept with the project in
 * projectCfg/annotations.json (the original keeps them inside the .msq).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_PROJECT_ANNOT_H
#define LT_PROJECT_ANNOT_H

#include "project.h"

LT_BEGIN_DECLS

enum { LT_ANNOT_NONE = -1, LT_ANNOT_INFO = 0, LT_ANNOT_WARNING, LT_ANNOT_CRITICAL,
       LT_ANNOT_SUCCESS, LT_ANNOT_TODO, LT_ANNOT_TAG_COUNT };

typedef struct lt_annot {
    char *key, *text, *author;      /* author "" when unknown */
    char created[21], modified[21]; /* RFC 3339 UTC; modified "" until edited */
    int  tag;                       /* LT_ANNOT_* */
} lt_annot;

typedef struct lt_annot_set { lt_annot *v; size_t n; } lt_annot_set;

void lt_annot_init(lt_annot_set *s);
void lt_annot_free(lt_annot_set *s);
/* "Info" ... "Todo"; "" for none. Parse is case-insensitive, unknown/NULL gives LT_ANNOT_NONE. */
const char *lt_annot_tag_name(int tag);
int         lt_annot_tag_parse(const char *name);

/* Add or replace the note of `key` (a replacement keeps `created` and stamps `modified`).
 * Returns LT_EINVAL for an empty key or text. */
int  lt_annot_put(lt_annot_set *s, const char *key, const char *text, int tag, const char *author);
const lt_annot *lt_annot_get(const lt_annot_set *s, const char *key);
lt_bool lt_annot_delete(lt_annot_set *s, const char *key);
/* notes of one table: the key equals `table` or starts with "table:" */
size_t lt_annot_count_table(const lt_annot_set *s, const char *table);
/* "table:row:col" */
void lt_annot_cell_key(char *out, size_t cap, const char *table, int row, int col);
/* indexes ordered Critical > Warning > Todo > Info > Success > untagged, then by key; when
 * `table` is not NULL only that table's notes. idx needs room for s->n entries; returns the count. */
size_t lt_annot_order(const lt_annot_set *s, const char *table, size_t *idx);

/* projectCfg/annotations.json of a project (malloc'd) */
char *lt_annot_path(const lt_project *p);
/* a missing file gives an empty set (0); a damaged one LT_EDATA (set left empty) */
int  lt_annot_load(lt_annot_set *s, const char *path);
int  lt_annot_save(const lt_annot_set *s, const char *path);

LT_END_DECLS

#endif
