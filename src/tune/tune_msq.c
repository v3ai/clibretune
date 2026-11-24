/* tune_msq.c - lt_tune lifecycle, .msq reader and writer (spec 04 section 1).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tune_priv.h"
#include "../plat/plat.h"

/* ---------------------------------------------------------------------- */
/* lifecycle                                                               */
/* ---------------------------------------------------------------------- */
static void free_str(char **p)
{
    if (*p) lt_free(*p);
    *p = NULL;
}

static void map_free_entries(lt_map *m)
{
    size_t i;
    for (i = 0; i < m->cap; i++)
        if (m->slots[i].used == 1) lt_tune_entry_free((lt_tune_entry *)m->slots[i].val);
    lt_map_free(m);
}

void lt_tune_invalidate(lt_tune *t)
{
    size_t i;
    for (i = 0; i < t->n_sorted; i++) lt_free(t->sorted_names[i]);
    if (t->sorted_names) lt_free(t->sorted_names);
    t->sorted_names = NULL;
    t->n_sorted = 0;
}

lt_tune *lt_tune_new(const char *signature)
{
    lt_tune *t = (lt_tune *)lt_calloc(1, sizeof *t);
    char *now = lt_tune_now_rfc3339();
    t->version = lt_strdup("1.1");
    t->signature = lt_strdup(signature ? signature : "");
    t->created = now;
    t->modified = lt_strdup(now);
    lt_map_init(&t->constants, LT_FALSE);
    lt_map_init(&t->pc_variables, LT_FALSE);
    return t;
}

static void manifest_free(lt_tune *t)
{
    size_t i;
    for (i = 0; i < t->n_manifest; i++) {
        lt_free(t->manifest[i].name);
        lt_free(t->manifest[i].type);
    }
    if (t->manifest) lt_free(t->manifest);
    t->manifest = NULL;
    t->n_manifest = 0;
    t->has_manifest = LT_FALSE;
}

static void metadata_free(lt_tune *t)
{
    if (t->ini_metadata) {
        lt_free(t->ini_metadata->signature);
        lt_free(t->ini_metadata->name);
        lt_free(t->ini_metadata->hash);
        lt_free(t->ini_metadata->spec_version);
        lt_free(t->ini_metadata->saved_at);
        lt_free(t->ini_metadata);
        t->ini_metadata = NULL;
    }
}

void lt_tune_free(lt_tune *t)
{
    size_t i;
    if (!t) return;
    free_str(&t->version);
    free_str(&t->signature);
    free_str(&t->author);
    free_str(&t->description);
    free_str(&t->created);
    free_str(&t->modified);
    free_str(&t->firmware_info);
    for (i = 0; i < 256; i++) {
        if (t->pages[i]) lt_free(t->pages[i]);
        if (t->page_sizes[i]) lt_free(t->page_sizes[i]);
    }
    map_free_entries(&t->constants);
    map_free_entries(&t->pc_variables);
    for (i = 0; i < t->n_ecu_settings; i++) lt_free(t->ecu_settings[i]);
    if (t->ecu_settings) lt_free(t->ecu_settings);
    metadata_free(t);
    manifest_free(t);
    lt_tune_invalidate(t);
    lt_free(t);
}

static char *dup_opt(const char *s)
{
    return s ? lt_strdup(s) : NULL;
}

static void entry_copy_into(lt_tune_entry *dst, const lt_tune_entry *src)
{
    lt_tune_value_clear(&dst->value);
    lt_tune_value_copy(&dst->value, &src->value);
    dst->page = src->page;
    dst->has_attrs = src->has_attrs;
    free_str(&dst->attrs.digits);
    free_str(&dst->attrs.units);
    free_str(&dst->attrs.rows);
    free_str(&dst->attrs.cols);
    dst->attrs.digits = dup_opt(src->attrs.digits);
    dst->attrs.units = dup_opt(src->attrs.units);
    dst->attrs.rows = dup_opt(src->attrs.rows);
    dst->attrs.cols = dup_opt(src->attrs.cols);
}

static void map_clone(lt_map *dst, const lt_map *src)
{
    size_t i;
    for (i = 0; i < src->cap; i++) {
        if (src->slots[i].used == 1) {
            lt_tune_entry *e = lt_tune_entry_new();
            entry_copy_into(e, (const lt_tune_entry *)src->slots[i].val);
            lt_map_put(dst, src->slots[i].key, e);
        }
    }
}

lt_tune *lt_tune_clone(const lt_tune *s)
{
    lt_tune *t = lt_tune_new("");
    size_t i;
    free_str(&t->version);
    free_str(&t->signature);
    free_str(&t->created);
    free_str(&t->modified);
    t->version = dup_opt(s->version);
    t->signature = dup_opt(s->signature);
    t->author = dup_opt(s->author);
    t->description = dup_opt(s->description);
    t->created = dup_opt(s->created);
    t->modified = dup_opt(s->modified);
    t->firmware_info = dup_opt(s->firmware_info);
    for (i = 0; i < 256; i++) {
        if (s->pages[i]) {
            t->pages[i] = (lt_u8 *)lt_malloc(s->page_len[i] ? s->page_len[i] : 1);
            memcpy(t->pages[i], s->pages[i], s->page_len[i]);
            t->page_len[i] = s->page_len[i];
        }
        t->page_sizes[i] = dup_opt(s->page_sizes[i]);
    }
    map_clone(&t->constants, &s->constants);
    map_clone(&t->pc_variables, &s->pc_variables);
    if (s->n_ecu_settings) {
        t->ecu_settings = (char **)lt_malloc(s->n_ecu_settings * sizeof(char *));
        for (i = 0; i < s->n_ecu_settings; i++) t->ecu_settings[i] = lt_strdup(s->ecu_settings[i]);
        t->n_ecu_settings = s->n_ecu_settings;
    }
    if (s->ini_metadata) {
        t->ini_metadata = (lt_ini_metadata *)lt_calloc(1, sizeof *t->ini_metadata);
        t->ini_metadata->signature = lt_strdup(s->ini_metadata->signature);
        t->ini_metadata->name = lt_strdup(s->ini_metadata->name);
        t->ini_metadata->hash = lt_strdup(s->ini_metadata->hash);
        t->ini_metadata->spec_version = lt_strdup(s->ini_metadata->spec_version);
        t->ini_metadata->saved_at = lt_strdup(s->ini_metadata->saved_at);
    }
    t->has_manifest = s->has_manifest;
    if (s->n_manifest) {
        t->manifest = (lt_manifest_entry *)lt_malloc(s->n_manifest * sizeof(lt_manifest_entry));
        for (i = 0; i < s->n_manifest; i++) {
            t->manifest[i] = s->manifest[i];
            t->manifest[i].name = lt_strdup(s->manifest[i].name);
            t->manifest[i].type = lt_strdup(s->manifest[i].type);
        }
        t->n_manifest = s->n_manifest;
    }
    return t;
}

void lt_tune_touch(lt_tune *t)
{
    free_str(&t->modified);
    t->modified = lt_tune_now_rfc3339();
}

void lt_tune_set_signature(lt_tune *t, const char *sig)
{
    free_str(&t->signature);
    t->signature = lt_strdup(sig ? sig : "");
}

/* ---------------------------------------------------------------------- */
/* pages                                                                   */
/* ---------------------------------------------------------------------- */
void lt_tune_set_page(lt_tune *t, int page, const lt_u8 *data, size_t len)
{
    lt_u8 *copy;
    if (page < 0 || page > 255) return;
    copy = (lt_u8 *)lt_malloc(len ? len : 1);
    if (len) memcpy(copy, data, len);
    if (t->pages[page]) lt_free(t->pages[page]);
    t->pages[page] = copy;
    t->page_len[page] = len;
}

const lt_u8 *lt_tune_get_page(const lt_tune *t, int page, size_t *len)
{
    if (page < 0 || page > 255 || !t->pages[page]) return NULL;
    if (len) *len = t->page_len[page];
    return t->pages[page];
}

int lt_tune_patch_page(lt_tune *t, int page, size_t off, const lt_u8 *data, size_t n)
{
    if (page < 0 || page > 255 || !t->pages[page]) return LT_ENOENT;
    if (off > t->page_len[page] || n > t->page_len[page] - off) return LT_ERANGE;
    if (n) memcpy(t->pages[page] + off, data, n);
    return 0;
}

static const lt_u8 *ps_page(void *ud, int page, size_t *len)
{
    return lt_tune_get_page((const lt_tune *)ud, page, len);
}

static int ps_write(void *ud, int page, size_t off, const lt_u8 *data, size_t n)
{
    return lt_tune_patch_page((lt_tune *)ud, page, off, data, n);
}

lt_pagesrc lt_tune_pagesrc(lt_tune *t)
{
    lt_pagesrc s;
    s.ud = t;
    s.page = ps_page;
    s.write = ps_write;
    return s;
}

/* ---------------------------------------------------------------------- */
/* named values                                                            */
/* ---------------------------------------------------------------------- */
static lt_tune_entry *entry_slot(lt_map *m, const char *name)
{
    lt_tune_entry *e = (lt_tune_entry *)lt_map_get(m, name);
    if (!e) {
        e = lt_tune_entry_new();
        lt_map_put(m, name, e);
    }
    return e;
}

void lt_tune_set_constant(lt_tune *t, const char *name, const lt_tune_value *v, int page)
{
    lt_bool existed = lt_map_has(&t->constants, name);
    lt_tune_entry *e = entry_slot(&t->constants, name);
    lt_tune_value_clear(&e->value);
    lt_tune_value_copy(&e->value, v);
    e->page = (lt_u8)(page < 0 || page > 255 ? 0 : page);
    if (!existed) lt_tune_invalidate(t);
}

const lt_tune_entry *lt_tune_get_constant(const lt_tune *t, const char *name)
{
    return (const lt_tune_entry *)lt_map_get(&t->constants, name);
}

void lt_tune_set_pc_variable(lt_tune *t, const char *name, const lt_tune_value *v)
{
    lt_tune_entry *e = entry_slot(&t->pc_variables, name);
    lt_tune_value_clear(&e->value);
    lt_tune_value_copy(&e->value, v);
}

const lt_tune_entry *lt_tune_get_pc_variable(const lt_tune *t, const char *name)
{
    return (const lt_tune_entry *)lt_map_get(&t->pc_variables, name);
}

const lt_tune_entry *lt_tune_get_value(const lt_tune *t, const char *name)
{
    const lt_tune_entry *e = lt_tune_get_constant(t, name);
    return e ? e : lt_tune_get_pc_variable(t, name);
}

size_t lt_tune_n_constants(const lt_tune *t)
{
    return t->constants.len;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* sorted array of (borrowed) key pointers of a map */
static const char **sorted_keys(const lt_map *m, size_t *n_out)
{
    const char **k = (const char **)lt_malloc((m->len + 1) * sizeof(char *));
    size_t i, n = 0;
    for (i = 0; i < m->cap; i++)
        if (m->slots[i].used == 1) k[n++] = m->slots[i].key;
    qsort(k, n, sizeof(char *), cmp_str);
    *n_out = n;
    return k;
}

const char *lt_tune_constant_name(const lt_tune *ct, size_t i)
{
    lt_tune *t = (lt_tune *)ct;   /* cache is logically const */
    if (!t->sorted_names) {
        size_t n, j;
        const char **k = sorted_keys(&t->constants, &n);
        t->sorted_names = (char **)lt_malloc((n + 1) * sizeof(char *));
        for (j = 0; j < n; j++) t->sorted_names[j] = lt_strdup(k[j]);
        t->n_sorted = n;
        lt_free((void *)k);
    }
    return i < t->n_sorted ? t->sorted_names[i] : NULL;
}

/* ---------------------------------------------------------------------- */
/* sanitize                                                                */
/* ---------------------------------------------------------------------- */
static int sanitize_map(lt_map *m)
{
    size_t i;
    int fixed = 0;
    for (i = 0; i < m->cap; i++) {
        lt_tune_entry *e;
        if (m->slots[i].used != 1) continue;
        e = (lt_tune_entry *)m->slots[i].val;
        if (e->value.kind == LT_TV_STRING && e->value.str && e->value.str[0] == '<' &&
            (strstr(e->value.str, "constant") || strstr(e->value.str, "pcVariable"))) {
            lt_free(e->value.str);
            e->value.str = lt_strdup("");
            fixed++;
        }
    }
    return fixed;
}

int lt_tune_sanitize(lt_tune *t)
{
    return sanitize_map(&t->constants) + sanitize_map(&t->pc_variables);
}

/* ---------------------------------------------------------------------- */
/* reading                                                                 */
/* ---------------------------------------------------------------------- */
static lt_bool valid_utf8(const lt_u8 *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        lt_u8 c = s[i];
        size_t extra;
        lt_u32 cp;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return LT_FALSE;
        }
        if (i + extra >= n) return LT_FALSE;
        {
            size_t k;
            for (k = 1; k <= extra; k++) {
                if ((s[i + k] & 0xC0) != 0x80) return LT_FALSE;
                cp = (cp << 6) | (s[i + k] & 0x3F);
            }
        }
        if ((extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
            return LT_FALSE;
        if (cp >= 0xD800 && cp <= 0xDFFF) return LT_FALSE;
        i += extra + 1;
    }
    return LT_TRUE;
}

/* returns malloc'd UTF-8 text (NUL terminated) */
static char *to_utf8(const char *data, size_t len, size_t *out_len)
{
    const lt_u8 *s = (const lt_u8 *)data;
    char *r;
    size_t i, o = 0;
    if (valid_utf8(s, len)) {
        r = (char *)lt_malloc(len + 1);
        memcpy(r, data, len);
        r[len] = '\0';
        *out_len = len;
        return r;
    }
    r = (char *)lt_malloc(len * 2 + 1);
    for (i = 0; i < len; i++) {
        if (s[i] < 0x80) {
            r[o++] = (char)s[i];
        } else {
            r[o++] = (char)(0xC0 | (s[i] >> 6));
            r[o++] = (char)(0x80 | (s[i] & 0x3F));
        }
    }
    r[o] = '\0';
    *out_len = o;
    return r;
}

static void set_opt_attr(char **dst, const char *v)
{
    free_str(dst);
    if (v) *dst = lt_strdup(v);
}

static void add_entry(lt_tune *t, lt_map *m, const lt_xml_node *n, int page, int is_pc)
{
    const char *name = lt_xml_attr_get(n, "name");
    const char *a_dig = lt_xml_attr_get(n, "digits");
    const char *a_un = lt_xml_attr_get(n, "units");
    const char *a_rows = lt_xml_attr_get(n, "rows");
    const char *a_cols = lt_xml_attr_get(n, "cols");
    lt_tune_entry *e;
    lt_tune_value v;

    if (!name) return;
    if (n->self_closing) {
        lt_tune_value_init(&v);
        v.kind = LT_TV_STRING;
        v.str = lt_strdup("");
    } else {
        lt_tune_value_parse(&v, n->raw_text ? n->raw_text : (n->text ? n->text : ""));
    }
    if (!lt_map_has(m, name) && !is_pc) lt_tune_invalidate(t);
    e = entry_slot(m, name);
    lt_tune_value_clear(&e->value);
    e->value = v;
    if (!is_pc) e->page = (lt_u8)page;
    if (a_dig || a_un || a_rows || a_cols) {
        set_opt_attr(&e->attrs.digits, a_dig);
        set_opt_attr(&e->attrs.units, a_un);
        set_opt_attr(&e->attrs.rows, a_rows);
        set_opt_attr(&e->attrs.cols, a_cols);
        e->has_attrs = LT_TRUE;
    }
}

static const lt_xml_node *find_sig_attr(const lt_xml_node *n)
{
    size_t i;
    const lt_xml_node *r;
    if (lt_xml_attr_get(n, "signature")) return n;
    for (i = 0; i < n->n_kids; i++) {
        r = find_sig_attr(n->kids[i]);
        if (r) return r;
    }
    return NULL;
}

static lt_u8 hexval(char c)
{
    if (c >= '0' && c <= '9') return (lt_u8)(c - '0');
    if (c >= 'a' && c <= 'f') return (lt_u8)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (lt_u8)(c - 'A' + 10);
    return 255;
}

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static void read_page_data(lt_tune *t, const lt_xml_node *n)
{
    const char *pg = lt_xml_attr_get(n, "page");
    const char *s = n->text ? n->text : "";
    int page;
    char *compact;
    size_t i, o = 0, cl, nb = 0;
    lt_u8 *bytes;

    if (!pg || !lt_tune_parse_u8(pg, &page)) return;
    compact = (char *)lt_malloc(strlen(s) + 1);
    for (i = 0; s[i]; i++)
        if (!is_ws(s[i])) compact[o++] = s[i];
    compact[o] = '\0';
    cl = o;
    bytes = (lt_u8 *)lt_malloc(cl / 2 + 1);
    for (i = 0; i + 1 < cl; i += 2) {
        lt_u8 hi = hexval(compact[i]), lo = hexval(compact[i + 1]);
        if (hi == 255 || lo == 255) continue;
        bytes[nb++] = (lt_u8)((hi << 4) | lo);
    }
    if (nb > 0) lt_tune_set_page(t, page, bytes, nb);
    lt_free(bytes);
    lt_free(compact);
}

static double parse_or(const char *s, double dflt)
{
    double d;
    if (s && lt_tune_parse_num(s, strlen(s), &d)) return d;
    return dflt;
}

static void read_manifest(lt_tune *t, const lt_xml_node *root)
{
    lt_vec es;
    size_t i;
    lt_vec_init(&es, sizeof(const lt_xml_node *));
    lt_xml_find_all(root, "entry", &es);
    for (i = 0; i < es.len; i++) {
        const lt_xml_node *n = LT_VEC_AT(&es, const lt_xml_node *, i);
        const char *name = lt_xml_attr_get(n, "name");
        const char *ty = lt_xml_attr_get(n, "type");
        const char *pg = lt_xml_attr_get(n, "page");
        const char *off = lt_xml_attr_get(n, "offset");
        lt_manifest_entry *m;
        int pgv = 0;
        long offv = 0;
        if (!n->parent || strcmp(n->parent->name, "constantManifest") != 0) continue;
        if (!name || !*name) continue;
        t->manifest = (lt_manifest_entry *)lt_realloc(
            t->manifest, (t->n_manifest + 1) * sizeof(lt_manifest_entry));
        m = &t->manifest[t->n_manifest++];
        m->name = lt_strdup(name);
        m->type = lt_strdup(ty ? ty : "");
        if (!(pg && lt_tune_parse_u8(pg, &pgv))) pgv = 0;
        m->page = (lt_u8)pgv;
        if (!(off && lt_parse_long(off, &offv) && offv >= 0)) offv = 0;
        m->offset = (lt_u32)offv;
        m->scale = parse_or(lt_xml_attr_get(n, "scale"), 1.0);
        m->translate = parse_or(lt_xml_attr_get(n, "translate"), 0.0);
        t->has_manifest = LT_TRUE;
    }
    lt_vec_free(&es);
}

static const char *attr_or_empty(const lt_xml_node *n, const char *name)
{
    const char *v = lt_xml_attr_get(n, name);
    return v ? v : "";
}

lt_tune *lt_tune_parse_msq(const char *data, size_t len)
{
    size_t ulen;
    char *text = to_utf8(data, len, &ulen);
    char err[128];
    lt_xml_node *root;
    lt_tune *t;
    const lt_xml_node *n;
    lt_vec v;
    size_t i, j;
    int cur = 0;

    root = lt_xml_parse(text, ulen, LT_XML_LENIENT, err, sizeof err);
    lt_free(text);
    if (!root) {
        lt_err_set("Not a valid MSQ file");
        return NULL;
    }
    t = lt_tune_new("");
    /* Parsed tunes start from Default: no timestamps */
    free_str(&t->created);
    free_str(&t->modified);

    n = lt_xml_find(root, "versionInfo");
    if (n) {
        lt_tune_set_signature(t, attr_or_empty(n, "signature"));
    } else {
        const lt_xml_node *s = find_sig_attr(root);
        if (s) lt_tune_set_signature(t, lt_xml_attr_get(s, "signature"));
    }
    n = lt_xml_find(root, "bibliography");
    if (n) {
        const char *a = lt_xml_attr_get(n, "author");
        const char *w = lt_xml_attr_get(n, "writeDate");
        const char *c = lt_xml_attr_get(n, "created");
        if (a) t->author = lt_strdup(a);
        if (w) t->modified = lt_strdup(w);
        if (c) t->created = lt_strdup(c);
    } else {
        const char *ts = lt_xml_attr_get(root, "timestamp");
        if (ts) t->modified = lt_strdup(ts);
    }
    n = lt_xml_find(root, "iniMetadata");
    if (n) {
        t->ini_metadata = (lt_ini_metadata *)lt_calloc(1, sizeof *t->ini_metadata);
        t->ini_metadata->signature = lt_strdup(attr_or_empty(n, "signature"));
        t->ini_metadata->name = lt_strdup(attr_or_empty(n, "name"));
        t->ini_metadata->hash = lt_strdup(attr_or_empty(n, "hash"));
        t->ini_metadata->spec_version = lt_strdup(attr_or_empty(n, "specVersion"));
        t->ini_metadata->saved_at = lt_strdup(attr_or_empty(n, "savedAt"));
    }
    read_manifest(t, root);

    lt_vec_init(&v, sizeof(const lt_xml_node *));
    lt_xml_find_all(root, "pageData", &v);
    for (i = 0; i < v.len; i++) read_page_data(t, LT_VEC_AT(&v, const lt_xml_node *, i));
    lt_vec_clear(&v);

    lt_xml_find_all(root, "setting", &v);
    for (i = 0; i < v.len; i++) {
        const lt_xml_node *s = LT_VEC_AT(&v, const lt_xml_node *, i);
        const char *nm = lt_xml_attr_get(s, "name");
        if (!nm || !s->parent || strcmp(s->parent->name, "settings") != 0) continue;
        t->ecu_settings = (char **)lt_realloc(t->ecu_settings, (t->n_ecu_settings + 1) * sizeof(char *));
        t->ecu_settings[t->n_ecu_settings++] = lt_strdup(nm);
    }
    lt_vec_clear(&v);

    lt_xml_find_all(root, "page", &v);
    if (v.len > 0) {
        for (i = 0; i < v.len; i++) {
            const lt_xml_node *pn = LT_VEC_AT(&v, const lt_xml_node *, i);
            const char *num = lt_xml_attr_get(pn, "number");
            const char *sz = lt_xml_attr_get(pn, "size");
            int pv;
            if (num && lt_tune_parse_u8(num, &pv)) {
                cur = pv;
                if (sz) {
                    free_str(&t->page_sizes[cur]);
                    t->page_sizes[cur] = lt_strdup(sz);
                }
            }
            for (j = 0; j < pn->n_kids; j++)
                if (strcmp(pn->kids[j]->name, "constant") == 0)
                    add_entry(t, &t->constants, pn->kids[j], cur, 0);
            for (j = 0; j < pn->n_kids; j++)
                if (strcmp(pn->kids[j]->name, "pcVariable") == 0)
                    add_entry(t, &t->pc_variables, pn->kids[j], cur, 1);
        }
    } else {
        lt_vec_clear(&v);
        lt_xml_find_all(root, "constant", &v);
        for (i = 0; i < v.len; i++) add_entry(t, &t->constants, LT_VEC_AT(&v, const lt_xml_node *, i), 0, 0);
        if (t->constants.len == 0) {
            lt_vec_clear(&v);
            lt_xml_find_all(root, "pcVariable", &v);
            for (i = 0; i < v.len; i++)
                add_entry(t, &t->pc_variables, LT_VEC_AT(&v, const lt_xml_node *, i), 0, 1);
        }
    }
    lt_vec_free(&v);
    lt_xml_free(root);

    if (t->constants.len == 0 && t->signature[0] == '\0') {
        lt_tune_free(t);
        lt_err_set("Not a valid MSQ file");
        return NULL;
    }
    lt_tune_sanitize(t);
    return t;
}

/* the file ends with the closing root tag. The XML reader is lenient (it keeps what it parsed
 * of a cut-off document), so without this a truncated tune - an interrupted copy, a crash of an
 * older version mid-save - would load with constants silently missing. */
static lt_bool msq_complete(const char *d, size_t n)
{
    while (n > 0 && (d[n - 1] == ' ' || d[n - 1] == '\t' || d[n - 1] == '\r' ||
                     d[n - 1] == '\n' || d[n - 1] == '\0')) n--;
    return n >= 6 && lt_strnicmp(d + n - 6, "</msq>", 6) == 0 ? LT_TRUE : LT_FALSE;
}

lt_tune *lt_tune_load(const char *path)
{
    size_t len = 0;
    char *data = lt_file_read(path, &len);
    lt_tune *t;
    if (!data) return NULL;
    if (!msq_complete(data, len)) {
        lt_free(data);
        lt_err_set("%s is incomplete: the file ends before </msq> (cut off?)", lt_path_base(path));
        return NULL;
    }
    t = lt_tune_parse_msq(data, len);
    lt_free(data);
    return t;
}

/* ---------------------------------------------------------------------- */
/* writing                                                                 */
/* ---------------------------------------------------------------------- */
static void put_esc(lt_buf *b, const char *s)
{
    char *e = lt_xml_escape(s ? s : "");
    lt_buf_puts(b, e);
    lt_free(e);
}

static void put_attr(lt_buf *b, const char *name, const char *val)
{
    lt_buf_putc(b, ' ');
    lt_buf_puts(b, name);
    lt_buf_puts(b, "=\"");
    put_esc(b, val);
    lt_buf_putc(b, '"');
}

static void put_num(lt_buf *b, double v, int rust)
{
    char n[420];
    lt_tune_fmt_num(n, sizeof n, v, rust);
    lt_buf_puts(b, n);
}

static void put_value(lt_buf *b, const lt_tune_value *v)
{
    size_t i;
    switch (v->kind) {
    case LT_TV_SCALAR:
        put_num(b, v->scalar, 0);
        break;
    case LT_TV_ARRAY:
        for (i = 0; i < v->n; i++) {
            if (i) lt_buf_putc(b, ' ');
            put_num(b, v->arr[i], 0);
        }
        break;
    case LT_TV_STRING:
        lt_buf_putc(b, '"');
        put_esc(b, v->str);
        lt_buf_putc(b, '"');
        break;
    case LT_TV_BOOL:
        lt_buf_puts(b, v->boolean ? "true" : "false");
        break;
    }
}

static void put_element(lt_buf *b, const char *tag, const char *name, const lt_tune_entry *e)
{
    lt_buf_puts(b, "    <");
    lt_buf_puts(b, tag);
    if (e->attrs.cols) put_attr(b, "cols", e->attrs.cols);
    if (e->attrs.digits) put_attr(b, "digits", e->attrs.digits);
    put_attr(b, "name", name);
    if (e->attrs.rows) put_attr(b, "rows", e->attrs.rows);
    if (e->attrs.units) put_attr(b, "units", e->attrs.units);
    lt_buf_putc(b, '>');
    put_value(b, &e->value);
    lt_buf_puts(b, "</");
    lt_buf_puts(b, tag);
    lt_buf_puts(b, ">\n");
}

typedef struct sent { const char *name; const lt_tune_entry *e; } sent;

static int cmp_sent(const void *a, const void *b)
{
    const sent *x = (const sent *)a, *y = (const sent *)b;
    if (x->e->page != y->e->page) return x->e->page < y->e->page ? -1 : 1;
    return strcmp(x->name, y->name);
}

static int cmp_sent_name(const void *a, const void *b)
{
    return strcmp(((const sent *)a)->name, ((const sent *)b)->name);
}

static sent *collect(const lt_map *m, size_t *n_out, int by_page)
{
    sent *a = (sent *)lt_malloc((m->len + 1) * sizeof(sent));
    size_t i, n = 0;
    for (i = 0; i < m->cap; i++) {
        if (m->slots[i].used == 1) {
            a[n].name = m->slots[i].key;
            a[n].e = (const lt_tune_entry *)m->slots[i].val;
            n++;
        }
    }
    qsort(a, n, sizeof(sent), by_page ? cmp_sent : cmp_sent_name);
    *n_out = n;
    return a;
}

void lt_tune_to_msq(const lt_tune *t, lt_buf *b)
{
    sent *cs, *ps;
    size_t ncs, nps, i, j, npages = 0;
    int maxpage = -1, p;

    lt_buf_puts(b, "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n");
    if (t->description) {
        char *c = lt_xml_comment_escape(t->description);
        lt_buf_puts(b, "<!-- ");
        lt_buf_puts(b, c);
        lt_buf_puts(b, " -->\n");
        lt_free(c);
    }
    lt_buf_puts(b, "<msq xmlns=\"http://www.msefi.com/:msq\">\n");

    cs = collect(&t->constants, &ncs, 1);
    for (i = 0; i < ncs; i++)
        if ((int)cs[i].e->page > maxpage) maxpage = cs[i].e->page;
    for (p = 0; p < 256; p++)
        if (t->pages[p]) npages++;
    {
        size_t np = npages;
        if ((size_t)(maxpage + 1) > np) np = (size_t)(maxpage + 1);
        lt_buf_puts(b, "  <versionInfo");
        put_attr(b, "signature", t->signature);
        put_attr(b, "firmwareInfo", t->firmware_info ? t->firmware_info : t->signature);
        lt_buf_printf(b, " nPages=\"%lu\"/>\n", (unsigned long)np);
    }
    if (t->author || t->created || t->modified) {
        lt_buf_puts(b, "  <bibliography");
        if (t->author) put_attr(b, "author", t->author);
        if (t->created) put_attr(b, "created", t->created);
        if (t->modified) put_attr(b, "writeDate", t->modified);
        lt_buf_puts(b, "/>\n");
    }
    if (t->ini_metadata) {
        lt_buf_puts(b, "  <iniMetadata");
        put_attr(b, "signature", t->ini_metadata->signature);
        put_attr(b, "name", t->ini_metadata->name);
        put_attr(b, "hash", t->ini_metadata->hash);
        put_attr(b, "specVersion", t->ini_metadata->spec_version);
        put_attr(b, "savedAt", t->ini_metadata->saved_at);
        lt_buf_puts(b, "/>\n");
    }
    if (t->has_manifest) {
        lt_buf_puts(b, "  <constantManifest>\n");
        for (i = 0; i < t->n_manifest; i++) {
            const lt_manifest_entry *m = &t->manifest[i];
            lt_buf_puts(b, "    <entry");
            put_attr(b, "name", m->name);
            put_attr(b, "type", m->type);
            lt_buf_printf(b, " page=\"%u\" offset=\"%lu\" scale=\"", (unsigned)m->page,
                          (unsigned long)m->offset);
            put_num(b, m->scale, 1);
            lt_buf_puts(b, "\" translate=\"");
            put_num(b, m->translate, 1);
            lt_buf_puts(b, "\"/>\n");
        }
        lt_buf_puts(b, "  </constantManifest>\n");
    }
    if (t->n_ecu_settings > 0) {
        lt_buf_puts(b, "  <settings Comment=\"These setting are only used if this msq is "
                       "opened without a project.\">\n");
        for (i = 0; i < t->n_ecu_settings; i++) {
            lt_buf_puts(b, "    <setting");
            put_attr(b, "name", t->ecu_settings[i]);
            lt_buf_puts(b, "/>\n");
        }
        lt_buf_puts(b, "  </settings>\n");
    }
    for (i = 0; i < ncs;) {
        int pg = cs[i].e->page;
        lt_buf_printf(b, "  <page number=\"%d\"", pg);
        if (t->page_sizes[pg]) put_attr(b, "size", t->page_sizes[pg]);
        lt_buf_puts(b, ">\n");
        for (j = i; j < ncs && cs[j].e->page == pg; j++) put_element(b, "constant", cs[j].name, cs[j].e);
        lt_buf_puts(b, "  </page>\n");
        i = j;
    }
    for (p = 0; p < 256; p++) {
        static const char hx[] = "0123456789abcdef";
        size_t k;
        if (!t->pages[p]) continue;
        lt_buf_reserve(b, t->page_len[p] * 2 + 64);
        lt_buf_printf(b, "  <pageData page=\"%d\">", p);
        for (k = 0; k < t->page_len[p]; k++) {
            lt_buf_putc(b, hx[t->pages[p][k] >> 4]);
            lt_buf_putc(b, hx[t->pages[p][k] & 15]);
        }
        lt_buf_puts(b, "</pageData>\n");
    }
    ps = collect(&t->pc_variables, &nps, 0);
    if (nps > 0) {
        lt_buf_puts(b, "  <page number=\"-1\">\n");
        for (i = 0; i < nps; i++) put_element(b, "pcVariable", ps[i].name, ps[i].e);
        lt_buf_puts(b, "  </page>\n");
    }
    lt_buf_puts(b, "</msq>\n");
    lt_free(cs);
    lt_free(ps);
}

int lt_tune_save(const lt_tune *t, const char *path)
{
    lt_buf b;
    int rc;
    const char *ext = lt_path_ext(path);
    if (lt_stricmp(ext, ".json") == 0) {
        lt_err_set("Unknown format: json");
        return LT_EUNSUPPORTED;
    }
    lt_buf_init(&b);
    lt_tune_to_msq(t, &b);
    rc = lt_plat_write_atomic(path, b.data, b.len);   /* never leaves a truncated tune */
    lt_buf_free(&b);
    return rc;
}

/* ---------------------------------------------------------------------- */
/* pcVariableValues.msq                                                    */
/* ---------------------------------------------------------------------- */
int lt_tune_save_pc_variables(const lt_tune *t, const char *path, const char *signature)
{
    lt_buf b;
    sent *ps;
    size_t n, i;
    char date[64];
    time_t now = time(NULL);
    struct tm *g = gmtime(&now);
    int rc;

    if (!g || strftime(date, sizeof date, "%a %b %d %H:%M:%S %Y", g) == 0)
        lt_strlcpy(date, "Thu Jan 01 00:00:00 1970", sizeof date);
    lt_buf_init(&b);
    lt_buf_puts(&b, "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n");
    lt_buf_puts(&b, "<msq xmlns=\"http://www.msefi.com/:msq\">\n");
    lt_buf_printf(&b, "<bibliography author=\"clibretune\" writeDate=\"%s\"/>\n", date);
    lt_buf_puts(&b, "<versionInfo fileFormat=\"4.0\"");
    put_attr(&b, "signature", signature ? signature : "");
    lt_buf_puts(&b, "/>\n");
    ps = collect(&t->pc_variables, &n, 0);
    if (n > 0) {
        lt_buf_puts(&b, "<page number=\"-1\">\n");
        for (i = 0; i < n; i++) {
            lt_buf_puts(&b, "<constant");
            put_attr(&b, "name", ps[i].name);
            lt_buf_putc(&b, '>');
            put_value(&b, &ps[i].e->value);
            lt_buf_puts(&b, "</constant>\n");
        }
        lt_buf_puts(&b, "</page>\n");
    }
    lt_buf_puts(&b, "</msq>\n");
    lt_free(ps);
    rc = lt_plat_write_atomic(path, b.data, b.len);
    lt_buf_free(&b);
    return rc;
}

static void merge_into_pc(lt_tune *t, const lt_map *src)
{
    size_t i;
    for (i = 0; i < src->cap; i++) {
        if (src->slots[i].used == 1) {
            const lt_tune_entry *se = (const lt_tune_entry *)src->slots[i].val;
            lt_tune_entry *e = entry_slot(&t->pc_variables, src->slots[i].key);
            lt_tune_value_clear(&e->value);
            lt_tune_value_copy(&e->value, &se->value);
        }
    }
}

int lt_tune_load_pc_variables(lt_tune *t, const char *path)
{
    lt_tune *l = lt_tune_load(path);
    if (!l) return LT_EDATA;
    merge_into_pc(t, &l->constants);
    merge_into_pc(t, &l->pc_variables);
    lt_tune_free(l);
    return 0;
}
