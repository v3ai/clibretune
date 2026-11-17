/* annot.c - change annotations (see annot.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "annot.h"
#include "proj_priv.h"
#include "../plat/plat.h"

static const char *const tag_names[LT_ANNOT_TAG_COUNT] = {"Info", "Warning", "Critical", "Success", "Todo"};

void lt_annot_init(lt_annot_set *s)
{
    s->v = NULL;
    s->n = 0;
}

void lt_annot_free(lt_annot_set *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) {
        lt_free(s->v[i].key);
        lt_free(s->v[i].text);
        lt_free(s->v[i].author);
    }
    lt_free(s->v);
    lt_annot_init(s);
}

const char *lt_annot_tag_name(int tag)
{
    if (tag < 0 || tag >= LT_ANNOT_TAG_COUNT) return "";
    return tag_names[tag];
}

int lt_annot_tag_parse(const char *name)
{
    int i;
    if (name == NULL) return LT_ANNOT_NONE;
    for (i = 0; i < LT_ANNOT_TAG_COUNT; i++)
        if (lt_stricmp(name, tag_names[i]) == 0) return i;
    return LT_ANNOT_NONE;
}

static lt_annot *find(const lt_annot_set *s, const char *key)
{
    size_t i;
    for (i = 0; i < s->n; i++)
        if (strcmp(s->v[i].key, key) == 0) return &s->v[i];
    return NULL;
}

const lt_annot *lt_annot_get(const lt_annot_set *s, const char *key)
{
    return key != NULL ? find(s, key) : NULL;
}

int lt_annot_put(lt_annot_set *s, const char *key, const char *text, int tag, const char *author)
{
    char *k, *t;
    lt_annot *a;
    if (key == NULL || text == NULL) return LT_EINVAL;
    k = lt_strdup(key);
    t = lt_strdup(text);
    lt_str_trim(k);
    lt_str_trim(t);
    if (k[0] == '\0' || t[0] == '\0') {
        lt_free(k);
        lt_free(t);
        lt_err_set("an annotation needs a key and a text");
        return LT_EINVAL;
    }
    if (tag < LT_ANNOT_NONE || tag >= LT_ANNOT_TAG_COUNT) tag = LT_ANNOT_NONE;
    a = find(s, k);
    if (a != NULL) {
        lt_free(a->text);
        a->text = t;
        a->tag = tag;
        lt_proj_utc_now(a->modified);
        lt_free(k);
        return LT_OK;
    }
    s->v = (lt_annot *)lt_realloc(s->v, (s->n + 1) * sizeof *s->v);
    a = &s->v[s->n++];
    memset(a, 0, sizeof *a);
    a->key = k;
    a->text = t;
    a->author = lt_strdup(author != NULL ? author : "");
    a->tag = tag;
    lt_proj_utc_now(a->created);
    return LT_OK;
}

lt_bool lt_annot_delete(lt_annot_set *s, const char *key)
{
    size_t i;
    if (key == NULL) return LT_FALSE;
    for (i = 0; i < s->n; i++) {
        if (strcmp(s->v[i].key, key) == 0) {
            lt_free(s->v[i].key);
            lt_free(s->v[i].text);
            lt_free(s->v[i].author);
            memmove(&s->v[i], &s->v[i + 1], (s->n - i - 1) * sizeof *s->v);
            s->n--;
            return LT_TRUE;
        }
    }
    return LT_FALSE;
}

static lt_bool in_table(const char *key, const char *table)
{
    size_t n = strlen(table);
    return strncmp(key, table, n) == 0 && (key[n] == '\0' || key[n] == ':');
}

size_t lt_annot_count_table(const lt_annot_set *s, const char *table)
{
    size_t i, c = 0;
    for (i = 0; i < s->n; i++)
        if (in_table(s->v[i].key, table)) c++;
    return c;
}

void lt_annot_cell_key(char *out, size_t cap, const char *table, int row, int col)
{
    lt_snprintf(out, cap, "%s:%d:%d", table, row, col);
}

static const lt_annot_set *g_sort_set;

static int tag_rank(int tag)
{
    /* Critical, Warning, Todo, Info, Success, none */
    switch (tag) {
    case LT_ANNOT_CRITICAL: return 0;
    case LT_ANNOT_WARNING: return 1;
    case LT_ANNOT_TODO: return 2;
    case LT_ANNOT_INFO: return 3;
    case LT_ANNOT_SUCCESS: return 4;
    default: return 5;
    }
}

static int cmp_idx(const void *a, const void *b)
{
    const lt_annot *x = &g_sort_set->v[*(const size_t *)a], *y = &g_sort_set->v[*(const size_t *)b];
    int rx = tag_rank(x->tag), ry = tag_rank(y->tag);
    if (rx != ry) return rx < ry ? -1 : 1;
    return strcmp(x->key, y->key);
}

size_t lt_annot_order(const lt_annot_set *s, const char *table, size_t *idx)
{
    size_t i, n = 0;
    for (i = 0; i < s->n; i++)
        if (table == NULL || in_table(s->v[i].key, table)) idx[n++] = i;
    if (n > 1) {
        g_sort_set = s;
        qsort(idx, n, sizeof(size_t), cmp_idx);
    }
    return n;
}

char *lt_annot_path(const lt_project *p)
{
    return lt_project_file(p, "projectCfg/annotations.json");
}

int lt_annot_load(lt_annot_set *s, const char *path)
{
    char err[160];
    lt_json *j;
    const lt_json *arr;
    size_t i;
    lt_annot_free(s);
    if (!lt_file_exists(path)) return LT_OK;
    j = lt_json_load(path, err, sizeof err);
    if (j == NULL) {
        lt_err_set("annotations: %s", err);
        return LT_EDATA;
    }
    arr = lt_json_get(j, "annotations");
    for (i = 0; arr != NULL && lt_json_at(arr, i) != NULL; i++) {
        const lt_json *o = lt_json_at(arr, i);
        const char *key = lt_json_get_str(o, "key", ""), *text = lt_json_get_str(o, "text", "");
        lt_annot *a;
        if (lt_annot_put(s, key, text, lt_annot_tag_parse(lt_json_get_str(o, "tag", "")),
                         lt_json_get_str(o, "author", "")) != LT_OK)
            continue;
        a = find(s, key);
        if (a != NULL) {
            lt_strlcpy(a->created, lt_json_get_str(o, "created", ""), sizeof a->created);
            lt_strlcpy(a->modified, lt_json_get_str(o, "modified", ""), sizeof a->modified);
        }
    }
    lt_json_free(j);
    return LT_OK;
}

int lt_annot_save(const lt_annot_set *s, const char *path)
{
    lt_json *root = lt_json_obj(), *arr = lt_json_arr();
    size_t i;
    int rc;
    char *dir, *sl;
    for (i = 0; i < s->n; i++) {
        const lt_annot *a = &s->v[i];
        lt_json *o = lt_json_obj();
        lt_json_obj_set(o, "key", lt_json_str(a->key));
        lt_json_obj_set(o, "text", lt_json_str(a->text));
        if (a->tag != LT_ANNOT_NONE) lt_json_obj_set(o, "tag", lt_json_str(lt_annot_tag_name(a->tag)));
        if (a->author[0] != '\0') lt_json_obj_set(o, "author", lt_json_str(a->author));
        lt_json_obj_set(o, "created", lt_json_str(a->created));
        if (a->modified[0] != '\0') lt_json_obj_set(o, "modified", lt_json_str(a->modified));
        lt_json_arr_push(arr, o);
    }
    lt_json_obj_set(root, "annotations", arr);
    dir = lt_strdup(path);
    sl = strrchr(dir, '/');
    if (sl != NULL) {
        *sl = '\0';
        (void)lt_plat_mkdirs(dir);
    }
    lt_free(dir);
    rc = lt_json_save(root, path, 1);
    lt_json_free(root);
    return rc == 0 ? LT_OK : LT_EIO;
}
