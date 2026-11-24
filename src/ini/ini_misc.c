/* ini_misc.c - gauge zones, diagnostic loggers, ECU detection, .inc lookup tables.
 * Spec: docs/spec/01-ini-core.md 4.2, 7.3, 7.13, 11; docs/spec/02-ini-expr-ui.md 5.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include "ini_priv.h"

/* ------------------------------------------------------------------ */
/* gauge zones                                                         */
/* ------------------------------------------------------------------ */

lt_bool lt_ini_gauge_is_danger(const lt_ini_gauge *g, double v)
{
    return (v <= g->low_danger || v >= g->high_danger) ? LT_TRUE : LT_FALSE;
}

lt_bool lt_ini_gauge_is_warning(const lt_ini_gauge *g, double v)
{
    return ((v <= g->low_warning && v > g->low_danger) ||
            (v >= g->high_warning && v < g->high_danger)) ? LT_TRUE : LT_FALSE;
}

lt_bool lt_ini_gauge_is_normal(const lt_ini_gauge *g, double v)
{
    return (v > g->low_warning && v < g->high_warning) ? LT_TRUE : LT_FALSE;
}

size_t lt_ini_ref_wire_bytes(const lt_ini_ref_table *t)
{
    return t->adc_count * t->bytes_per_adc;
}

/* ------------------------------------------------------------------ */
/* ECU detection                                                       */
/* ------------------------------------------------------------------ */

static char *lower(const char *s)
{
    char *r = lt_strdup(s ? s : "");
    size_t i;
    for (i = 0; r[i]; i++)
        if (r[i] >= 'A' && r[i] <= 'Z') r[i] = (char)(r[i] - 'A' + 'a');
    return r;
}

lt_ecu_type lt_ecu_detect(const char *signature, const char *filename_hint)
{
    char *sig = lower(signature), *hint = lower(filename_hint);
    lt_ecu_type t = LT_ECU_UNKNOWN;

    if (strstr(sig, "fome") || strstr(hint, "fome")) t = LT_ECU_FOME;
    else if (strstr(sig, "epicefi") || strstr(sig, "epicecu") || strstr(hint, "epicefi") ||
             strstr(hint, "epicecu"))
        t = LT_ECU_EPICEFI;
    else if (strstr(sig, "rusefi")) t = LT_ECU_RUSEFI;
    else if (strstr(sig, "speeduino")) t = LT_ECU_SPEEDUINO;
    else if (strncmp(sig, "ms3", 3) == 0) t = LT_ECU_MS3;
    else if (strncmp(sig, "ms2", 3) == 0 || strstr(sig, "ms2extra")) t = LT_ECU_MS2;
    lt_free(sig);
    lt_free(hint);
    return t;
}

const char *lt_ecu_type_name(lt_ecu_type t)
{
    switch (t) {
    case LT_ECU_SPEEDUINO: return "Speeduino";
    case LT_ECU_RUSEFI: return "rusEFI";
    case LT_ECU_FOME: return "FOME";
    case LT_ECU_EPICEFI: return "epicEFI";
    case LT_ECU_MS2: return "MegaSquirt 2";
    case LT_ECU_MS3: return "MegaSquirt 3";
    default: return "Unknown";
    }
}

lt_bool lt_ecu_supports_console(lt_ecu_type t)
{
    return (t == LT_ECU_RUSEFI || t == LT_ECU_FOME || t == LT_ECU_EPICEFI) ? LT_TRUE : LT_FALSE;
}

lt_bool lt_ini_supports_console(const lt_ini *ini)
{
    return (lt_ecu_supports_console(ini->ecu_type) && ini->n_commands > 0) ? LT_TRUE : LT_FALSE;
}

/* ------------------------------------------------------------------ */
/* completeness                                                        */
/* ------------------------------------------------------------------ */

lt_bool lt_ini_looks_incomplete(const lt_ini *ini, char *why, size_t cap)
{
    size_t i, items = 0;
    if (why != NULL && cap > 0) why[0] = '\0';
    if (ini == NULL) return LT_TRUE;
    for (i = 0; i < ini->n_menus; i++) items += ini->menus[i].n_items;
    if (items == 0 && ini->n_dialogs == 0) {
        if (why != NULL) {
            lt_snprintf(why, cap, "It has no menus and no dialogs, so there are no settings to open "
                        "(%lu constants, %lu output channels).", (unsigned long)ini->n_constants,
                        (unsigned long)ini->n_channels);
        }
        return LT_TRUE;
    }
    /* pageSize per page: every constant must fit (pages without a size are not judged) */
    for (i = 0; i < ini->n_constants && ini->proto.n_page_sizes > 0; i++) {
        const lt_ini_const *c = ini->constants[i];
        unsigned long need, size;
        if (c->is_pc_var) continue;
        if (c->type == LT_DT_BITS) {
            int top = (c->bit_pos > 0 ? c->bit_pos : 0) + (c->bit_size > 0 ? c->bit_size : 1);
            need = (unsigned long)c->offset + (unsigned long)((top + 7) / 8);
        } else {
            need = (unsigned long)c->offset + (unsigned long)c->size_bytes;
        }
        if ((size_t)c->page >= ini->proto.n_page_sizes) {
            if (why != NULL) {
                lt_snprintf(why, cap, "The setting '%s' is on page %d, but the definition only "
                            "describes %lu page(s).", c->name, (int)c->page + 1,
                            (unsigned long)ini->proto.n_page_sizes);
            }
            return LT_TRUE;
        }
        size = ini->proto.page_sizes[c->page];
        if (size > 0 && need > size) {
            if (why != NULL) {
                lt_snprintf(why, cap, "The setting '%s' needs bytes %lu-%lu of page %d, but that "
                            "page is only %lu bytes long.", c->name, (unsigned long)c->offset,
                            need - 1, (int)c->page + 1, size);
            }
            return LT_TRUE;
        }
    }
    return LT_FALSE;
}

/* ------------------------------------------------------------------ */
/* diagnostic loggers                                                  */
/* ------------------------------------------------------------------ */

static lt_ini_blob logger_cmd(const char *value, const lt_map *pcv)
{
    char *q = ini_dequote(value);
    lt_ini_blob s = ini_substitute(q, strlen(q), pcv);
    lt_free(q);
    return s;
}

void ini_parse_loggers(lt_ini *ini, const char *raw)
{
    const char *p = raw;

    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char *line = lt_strndup(p, n);
        char *semi, *eq, *key, *val;
        lt_ini_logger *lg = ini->n_loggers ? &ini->loggers[ini->n_loggers - 1] : NULL;

        p += n + (e ? 1 : 0);
        semi = strchr(line, ';');
        if (semi) *semi = '\0';
        lt_str_trim(line);
        eq = strchr(line, '=');
        if (!line[0] || !eq) {
            lt_free(line);
            continue;
        }
        key = lt_strndup(line, (size_t)(eq - line));
        val = lt_strdup(eq + 1);
        lt_str_trim(key);
        lt_str_trim(val);

        if (lt_stricmp(key, "loggerDef") == 0) {
            char **parts;
            size_t np = ini_split_naive(val, &parts);
            lg = (lt_ini_logger *)ini_push((void **)&ini->loggers, &ini->n_loggers,
                                           sizeof(lt_ini_logger));
            lg->name = lt_strdup(parts[0]);
            lg->label = np > 1 ? ini_dequote(parts[1]) : lt_strdup("");
            lg->kind = np > 2 ? lt_strdup(parts[2]) : lt_strdup("");
            lg->data_read_timeout_ms = 5000;
            lg->start_command = ini_blob_new("", 0);
            lg->stop_command = ini_blob_new("", 0);
            lg->data_read_command = ini_blob_new("", 0);
            ini_strv_free(parts, np);
        } else if (lg) {
            if (lt_stricmp(key, "startCommand") == 0) {
                ini_blob_free(&lg->start_command);
                lg->start_command = logger_cmd(val, &ini->pc_variables);
            } else if (lt_stricmp(key, "stopCommand") == 0) {
                ini_blob_free(&lg->stop_command);
                lg->stop_command = logger_cmd(val, &ini->pc_variables);
            } else if (lt_stricmp(key, "dataReadCommand") == 0) {
                ini_blob_free(&lg->data_read_command);
                lg->data_read_command = logger_cmd(val, &ini->pc_variables);
            } else if (lt_stricmp(key, "dataReadTimeout") == 0) {
                unsigned long u;
                char *q = ini_dequote(val);
                if (ini_parse_uint(q, &u)) lg->data_read_timeout_ms = u;
                lt_free(q);
            } else if (lt_stricmp(key, "continuousRead") == 0) {
                char *q = ini_dequote(val);
                lg->continuous_read = lt_stricmp(q, "true") == 0 ? LT_TRUE : LT_FALSE;
                lt_free(q);
            } else if (lt_stricmp(key, "dataReadyCondition") == 0) {
                char *q = ini_dequote(val);
                lt_buf b;
                size_t i;
                lt_buf_init(&b);
                for (i = 0; q[i]; i++)
                    if (q[i] != '{' && q[i] != '}') lt_buf_putc(&b, (unsigned char)q[i]);
                lt_free(q);
                if (lg->data_ready_condition) lt_free(lg->data_ready_condition);
                lg->data_ready_condition = b.data ? lt_buf_take(&b) : lt_strdup("");
                lt_str_trim(lg->data_ready_condition);
            } else if (lt_stricmp(key, "recordDef") == 0) {
                char **parts;
                size_t np = ini_split_naive(val, &parts), i, cnt = 0;
                unsigned long v[3] = {0, 0, 0};
                for (i = 0; i < np; i++) {
                    unsigned long u;
                    if (ini_parse_uint(parts[i], &u)) {
                        if (cnt < 3) v[cnt] = u;
                        cnt++;
                    }
                }
                if (cnt >= 3) {
                    lg->header_len = v[0];
                    lg->footer_len = v[1];
                    lg->record_len = v[2];
                }
                ini_strv_free(parts, np);
            } else if (lt_stricmp(key, "recordField") == 0) {
                char **parts;
                size_t np = ini_split_naive(val, &parts);
                if (np >= 5) {
                    lt_ini_logger_field *f = (lt_ini_logger_field *)ini_push(
                        (void **)&lg->fields, &lg->n_fields, sizeof(lt_ini_logger_field));
                    unsigned long u;
                    double d;
                    f->name = lt_strdup(parts[0]);
                    f->label = ini_dequote(parts[1]);
                    f->start_bit = ini_parse_uint(parts[2], &u) ? (unsigned)u : 0;
                    f->bit_count = ini_parse_uint(parts[3], &u) ? (unsigned)u : 0;
                    f->scale = ini_parse_f64(parts[4], &d) ? d : 1.0;
                    f->units = np > 5 ? ini_dequote(parts[5]) : lt_strdup("");
                }
                ini_strv_free(parts, np);
            }
        }
        lt_free(key);
        lt_free(val);
        lt_free(line);
    }
}

/* bit i of the record: bit 0 is the LSB of the LAST byte */
static lt_bool read_bits(const lt_u8 *rec, size_t len, unsigned start, unsigned count,
                         double *out)
{
    unsigned i;
    double v = 0.0, w = 1.0;

    if (count == 0 || count > 64) return LT_FALSE;
    if ((size_t)start + count > len * 8) return LT_FALSE;
    for (i = 0; i < count; i++) {
        unsigned bit = start + i;
        lt_u8 byte = rec[len - 1 - bit / 8];
        if ((byte >> (bit % 8)) & 1) v += w;
        w *= 2.0;
    }
    *out = v;
    return LT_TRUE;
}

size_t lt_ini_logger_decode(const lt_ini_logger *lg, const lt_u8 *rec, size_t len,
                            double *vals, int *present)
{
    size_t i, cnt = 0;
    for (i = 0; i < lg->n_fields; i++) {
        double raw;
        const lt_ini_logger_field *f = &lg->fields[i];
        if (read_bits(rec, len, f->start_bit, f->bit_count, &raw)) {
            if (vals) vals[i] = raw * f->scale;
            if (present) present[i] = 1;
            cnt++;
        } else {
            if (vals) vals[i] = 0.0;
            if (present) present[i] = 0;
        }
    }
    return cnt;
}

lt_bool lt_ini_logger_is_empty(const lt_ini_logger *lg, const lt_u8 *rec, size_t len)
{
    size_t i;
    for (i = 0; i < lg->n_fields; i++) {
        double raw;
        const lt_ini_logger_field *f = &lg->fields[i];
        if (read_bits(rec, len, f->start_bit, f->bit_count, &raw) && raw * f->scale != 0.0)
            return LT_FALSE;
    }
    return LT_TRUE;
}

size_t lt_ini_logger_record_count(const lt_ini_logger *lg, size_t payload_len)
{
    size_t skip = lg->header_len + lg->footer_len;
    if (lg->record_len == 0) return 0;
    if (payload_len <= skip) return 0;
    return (payload_len - skip) / lg->record_len;
}

/* ------------------------------------------------------------------ */
/* .inc lookup tables                                                  */
/* ------------------------------------------------------------------ */

typedef struct kv { double k, v; size_t idx; } kv;

static int kv_cmp(const void *a, const void *b)
{
    const kv *x = (const kv *)a, *y = (const kv *)b;
    if (x->k < y->k) return -1;
    if (x->k > y->k) return 1;
    return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    return s;
}

lt_inc_table *lt_inc_parse(const char *text, const char *name)
{
    lt_inc_table *t = (lt_inc_table *)lt_calloc(1, sizeof(lt_inc_table));
    lt_vec pairs, direct;
    const char *p = text;
    size_t i;
    int is_direct = 0;

    lt_vec_init(&pairs, sizeof(kv));
    lt_vec_init(&direct, sizeof(double));
    t->name = lt_strdup(name ? name : "");
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char *line = lt_strndup(p, n);
        char *semi;
        char c0;

        p += n + (e ? 1 : 0);
        lt_str_trim(line);
        c0 = line[0];
        if (!c0 || c0 == ';' || c0 == '#' || c0 == '\'' || c0 == ':') {
            lt_free(line);
            continue;
        }
        semi = strchr(line, ';');
        if (semi) {
            *semi = '\0';
            lt_str_trim(line);
        }
        if (!line[0]) {
            lt_free(line);
            continue;
        }
        if ((line[0] == 'D' || line[0] == 'd') &&
            (line[1] == 'B' || line[1] == 'b' || line[1] == 'W' || line[1] == 'w')) {
            char *vp = lt_strdup(line + 2);
            size_t l;
            double d;
            is_direct = 1;
            lt_str_trim(vp);
            l = strlen(vp);
            while (l > 0 && (vp[l - 1] == 'T' || vp[l - 1] == 't')) vp[--l] = '\0';
            lt_str_trim(vp);
            if (ini_parse_f64(vp, &d)) *(double *)lt_vec_push(&direct) = d;
            lt_free(vp);
        } else {
            char *a = NULL, *b = NULL;
            double kd, vd;
            if (strchr(line, '\t')) {
                char *tab = strchr(line, '\t');
                char *tab2;
                a = lt_strndup(line, (size_t)(tab - line));
                tab2 = strchr(tab + 1, '\t');
                b = tab2 ? lt_strndup(tab + 1, (size_t)(tab2 - tab - 1)) : lt_strdup(tab + 1);
            } else {
                const char *s = skip_ws(line), *e1;
                e1 = s;
                while (*e1 && *e1 != ' ' && *e1 != '\t') e1++;
                if (*e1) {
                    const char *s2 = skip_ws(e1), *e2 = s2;
                    while (*e2 && *e2 != ' ' && *e2 != '\t') e2++;
                    a = lt_strndup(s, (size_t)(e1 - s));
                    b = lt_strndup(s2, (size_t)(e2 - s2));
                }
            }
            if (a && b) {
                lt_str_trim(a);
                lt_str_trim(b);
                if (ini_parse_f64(a, &kd) && ini_parse_f64(b, &vd)) {
                    kv *k = (kv *)lt_vec_push(&pairs);
                    k->k = kd;
                    k->v = vd;
                    k->idx = pairs.len;
                }
            }
            if (a) lt_free(a);
            if (b) lt_free(b);
        }
        lt_free(line);
    }
    if (is_direct) {
        t->direct = LT_TRUE;
        t->n = direct.len;
        t->vals = (double *)direct.data;
        if (t->n == 0) {
            lt_vec_free(&direct);
            t->vals = NULL;
        }
        lt_vec_free(&pairs);
    } else {
        if (pairs.len > 1) qsort(pairs.data, pairs.len, sizeof(kv), kv_cmp);
        t->n = pairs.len;
        t->keys = (double *)lt_calloc(t->n ? t->n : 1, sizeof(double));
        t->vals = (double *)lt_calloc(t->n ? t->n : 1, sizeof(double));
        for (i = 0; i < t->n; i++) {
            t->keys[i] = LT_VEC_AT(&pairs, kv, i).k;
            t->vals[i] = LT_VEC_AT(&pairs, kv, i).v;
        }
        lt_vec_free(&pairs);
        lt_vec_free(&direct);
    }
    return t;
}

void lt_inc_free(lt_inc_table *t)
{
    if (!t) return;
    if (t->name) lt_free(t->name);
    if (t->keys) lt_free(t->keys);
    if (t->vals) lt_free(t->vals);
    lt_free(t);
}

lt_bool lt_inc_lookup(const lt_inc_table *t, double key, double *out)
{
    size_t lo, hi;

    if (t->direct) {
        size_t idx;
        double r;
        if (t->n == 0) return LT_FALSE;
        r = lt_round(key);
        if (!(r > 0)) idx = 0;
        else if (r >= 4294967295.0) return LT_FALSE;
        else idx = (size_t)r;
        if (idx >= t->n) return LT_FALSE;
        *out = t->vals[idx];
        return LT_TRUE;
    }
    if (t->n == 0) return LT_FALSE;
    if (key <= t->keys[0]) {
        *out = t->vals[0];
        return LT_TRUE;
    }
    if (key >= t->keys[t->n - 1]) {
        *out = t->vals[t->n - 1];
        return LT_TRUE;
    }
    lo = 0;
    hi = t->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (t->keys[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    /* lo = first index with key >= requested; lo >= 1 here */
    {
        double x0 = t->keys[lo - 1], x1 = t->keys[lo];
        double y0 = t->vals[lo - 1], y1 = t->vals[lo];
        if (fabs(x1 - x0) < DBL_EPSILON) *out = y0;
        else *out = y0 + (key - x0) / (x1 - x0) * (y1 - y0);
    }
    return LT_TRUE;
}

void lt_inc_cache_init(lt_inc_cache *c)
{
    c->search_paths = NULL;
    c->n_paths = 0;
    lt_map_init(&c->tables, LT_FALSE);
}

void lt_inc_cache_free(lt_inc_cache *c)
{
    size_t i;
    for (i = 0; i < c->tables.cap; i++)
        if (c->tables.slots[i].used == 1) lt_inc_free((lt_inc_table *)c->tables.slots[i].val);
    lt_map_free(&c->tables);
    ini_free_str_array(c->search_paths, c->n_paths);
    c->search_paths = NULL;
    c->n_paths = 0;
}

void lt_inc_cache_add_path(lt_inc_cache *c, const char *dir)
{
    size_t i;
    for (i = 0; i < c->n_paths; i++)
        if (strcmp(c->search_paths[i], dir) == 0) return;
    ini_str_push(&c->search_paths, &c->n_paths, lt_strdup(dir));
}

static lt_inc_table *load_inc(const char *path, const char *filename)
{
    size_t n = 0, tl = 0;
    char *data, *text, *stem, *dot;
    const char *base;
    lt_inc_table *t;

    if (!lt_file_exists(path)) return NULL;
    data = lt_file_read(path, &n);
    if (!data) return NULL;
    text = lt_ini_decode((const lt_u8 *)data, n, &tl);
    lt_free(data);
    base = lt_path_base(filename);
    stem = lt_strdup(base);
    dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
    t = lt_inc_parse(text, stem);
    lt_free(stem);
    lt_free(text);
    return t;
}

const lt_inc_table *lt_inc_cache_get(lt_inc_cache *c, const char *filename)
{
    size_t i;
    lt_inc_table *t;
    lt_bool has_inc = lt_str_ends(filename, ".inc") || lt_str_ends(filename, ".INC") ||
                      lt_str_ends(filename, ".Inc");

    t = (lt_inc_table *)lt_map_get(&c->tables, filename);
    if (t) return t;
    for (i = 0; i < c->n_paths; i++) {
        char *path = lt_path_join(c->search_paths[i], filename);
        t = load_inc(path, filename);
        lt_free(path);
        if (!t && !has_inc) {
            char *fn2 = (char *)lt_malloc(strlen(filename) + 5);
            strcpy(fn2, filename);
            strcat(fn2, ".inc");
            path = lt_path_join(c->search_paths[i], fn2);
            t = load_inc(path, fn2);
            lt_free(path);
            lt_free(fn2);
        }
        if (t) {
            lt_map_put(&c->tables, filename, t);
            return t;
        }
    }
    return NULL;
}
