/* ini_parse.c - INI driver: lines, preprocessor, includes, identity/protocol keys,
 * [Constants], [PcVariables], [OutputChannels].
 * Spec: docs/spec/01-ini-core.md sections 1-6, 7.1.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "ini_priv.h"

/* ------------------------------------------------------------------ */
/* generic helpers                                                     */
/* ------------------------------------------------------------------ */

void *ini_push(void **arr, size_t *n, size_t elem)
{
    char *p = (char *)lt_realloc(*arr, (*n + 1) * elem);
    *arr = p;
    memset(p + *n * elem, 0, elem);
    (*n)++;
    return p + (*n - 1) * elem;
}

/* Arrays built here only ever grow through this function (they start NULL), so the
 * capacity is implied: the next power of two >= n. Growing only when n is 0 or a power
 * of two keeps appends amortized O(1); a realloc per element made long $define option
 * lists quadratic (seconds per INI under allocators that always copy on realloc). */
void ini_str_push(char ***arr, size_t *n, char *s)
{
    char **a = *arr;
    if (*n == 0 || (*n & (*n - 1)) == 0) {
        a = (char **)lt_realloc(*arr, (*n == 0 ? 1 : *n * 2) * sizeof(char *));
    }
    a[*n] = s;
    *arr = a;
    (*n)++;
}

void ini_set_str(char **dst, char *val)
{
    if (*dst) lt_free(*dst);
    *dst = val;
}

void *ini_put(void ***arr, size_t *n, lt_map *idx, const char *name, void *elem,
              void (*freefn)(void *))
{
    void *old = lt_map_get(idx, name);
    size_t i;

    if (old) {
        for (i = 0; i < *n; i++) {
            if ((*arr)[i] == old) {
                (*arr)[i] = elem;
                if (freefn) freefn(old);
                lt_map_put(idx, name, elem);
                return elem;
            }
        }
    }
    *arr = (void **)lt_realloc(*arr, (*n + 1) * sizeof(void *));
    (*arr)[(*n)++] = elem;
    lt_map_put(idx, name, elem);
    return elem;
}

static void lt_slist_free(ini_strlist *l)
{
    if (!l) return;
    ini_strv_free(l->v, l->n);
    lt_free(l);
}

/* ------------------------------------------------------------------ */
/* path helpers                                                        */
/* ------------------------------------------------------------------ */

static lt_bool path_is_abs(const char *p)
{
    if (p[0] == '/' || p[0] == '\\') return LT_TRUE;
    if (((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')) && p[1] == ':')
        return LT_TRUE;
    return LT_FALSE;
}

/* directory part of path (malloc'd, "" if none) */
static char *path_dir(const char *path)
{
    const char *b = path + strlen(path);
    while (b > path && b[-1] != '/' && b[-1] != '\\') b--;
    if (b == path) return lt_strdup("");
    if (b - path == 1) return lt_strndup(path, 1);
    return lt_strndup(path, (size_t)(b - path - 1));
}

/* collapse "." / ".." / duplicate separators; '\\' -> '/' (malloc'd) */
static char *path_norm(const char *path)
{
    lt_buf out;
    char **seg = NULL;
    size_t nseg = 0, i;
    const char *p = path;
    int abs_ = (path[0] == '/' || path[0] == '\\');

    lt_buf_init(&out);
    while (*p) {
        const char *e = p;
        char *s;
        while (*e && *e != '/' && *e != '\\') e++;
        s = lt_strndup(p, (size_t)(e - p));
        if (s[0] == '\0' || strcmp(s, ".") == 0) {
            lt_free(s);
        } else if (strcmp(s, "..") == 0 && nseg > 0 && strcmp(seg[nseg - 1], "..") != 0) {
            lt_free(seg[--nseg]);
            lt_free(s);
        } else {
            ini_str_push(&seg, &nseg, s);
        }
        p = *e ? e + 1 : e;
    }
    if (abs_) lt_buf_putc(&out, '/');
    for (i = 0; i < nseg; i++) {
        if (i) lt_buf_putc(&out, '/');
        lt_buf_puts(&out, seg[i]);
    }
    ini_strv_free(seg, nseg);
    if (!out.data) lt_buf_puts(&out, "");
    return lt_buf_take(&out);
}

/* ------------------------------------------------------------------ */
/* lt_ini lifecycle                                                    */
/* ------------------------------------------------------------------ */

static lt_ini *ini_new(void)
{
    lt_ini *ini = (lt_ini *)lt_calloc(1, sizeof(lt_ini));
    ini_ctx *cx = (ini_ctx *)lt_calloc(1, sizeof(ini_ctx));
    lt_ini_protocol *p = &ini->proto;

    ini->signature = lt_strdup("");
    ini->version_info = lt_strdup("");
    ini->ini_spec_version = lt_strdup("3.64");
    ini->endian = LT_ENDIAN_BIG;
    lt_map_init(&ini->constant_idx, LT_FALSE);
    lt_map_init(&ini->channel_idx, LT_FALSE);
    lt_map_init(&ini->table_idx, LT_FALSE);
    lt_map_init(&ini->curve_idx, LT_FALSE);
    lt_map_init(&ini->gauge_idx, LT_FALSE);
    lt_map_init(&ini->dialog_idx, LT_FALSE);
    lt_map_init(&ini->help_idx, LT_FALSE);
    lt_map_init(&ini->command_idx, LT_FALSE);
    lt_map_init(&ini->pc_variables, LT_FALSE);
    lt_map_init(&ini->default_values, LT_FALSE);
    lt_map_init(&ini->maximum_elements, LT_FALSE);
    lt_map_init(&ini->active_symbols, LT_FALSE);
    lt_map_init(&ini->tested_symbols, LT_FALSE);

    p->query_command = ini_blob_from_str("Q");
    p->burst_get_command = ini_blob_from_str("A");
    p->och_get_command = ini_blob_new("", 0);
    p->page_activation_delay = 500;
    p->blocking_factor = 256;
    p->block_read_timeout = 1000;
    p->write_blocks = 1;
    p->enable_can_id = 1;
    p->default_baud_rate = 115200;
    p->default_ip_port = 29001;
    p->table_blocking_factor = -1;
    p->default_runtime_record_per_sec = -1;

    lt_map_init(&cx->defines, LT_FALSE);
    lt_buf_init(&cx->logger_raw);
    lt_map_init(&cx->included, LT_FALSE);
    lt_vec_init(&cx->std_panels, sizeof(lt_ini_dialog *));
    lt_map_init(&cx->managed, LT_FALSE);
    ini->priv = cx;
    return ini;
}

void ini_pstate_free(ini_pstate *st)
{
    if (st->section) lt_free(st->section);
    if (st->category) lt_free(st->category);
    lt_vec_free(&st->limits);
    st->section = NULL;
    st->category = NULL;
}

void lt_ini_options_default(lt_ini_options *o)
{
    o->symbols = NULL;
    o->n_symbols = 0;
    o->managed = NULL;
    o->n_managed = 0;
}

/* ------------------------------------------------------------------ */
/* constants                                                           */
/* ------------------------------------------------------------------ */

static lt_ini_const *const_new(const char *name)
{
    lt_ini_const *c = (lt_ini_const *)lt_calloc(1, sizeof(lt_ini_const));
    c->name = lt_strdup(name);
    c->endian_override = -1;
    c->shape.kind = LT_SH_SCALAR;
    c->bit_pos = c->bit_size = c->bit_disp = -1;
    c->units = lt_strdup("");
    c->scale = 1.0;
    c->translate = 0.0;
    c->min = 0.0;
    c->max = 255.0;
    c->range_resolved = LT_TRUE;
    return c;
}

/* units/scale/translate/min/max/digits starting at parts[units_idx] */
static void apply_units_scale(lt_ini_const *c, char **parts, size_t n, size_t units_idx)
{
    size_t si = units_idx + 1;
    double v;
    unsigned long u;

    if (n > units_idx) ini_set_str(&c->units, ini_dequote(parts[units_idx]));
    if (n > si) {
        if (ini_parse_f64(parts[si], &v)) c->scale = v;
        else {
            c->scale = 1.0;
            c->scale_expr = ini_deferred_expr(parts[si]);
        }
    }
    if (n > si + 1) {
        if (ini_parse_f64(parts[si + 1], &v)) c->translate = v;
        else {
            c->translate = 0.0;
            c->translate_expr = ini_deferred_expr(parts[si + 1]);
        }
    }
    if (n > si + 2) {
        if (ini_parse_f64(parts[si + 2], &v)) c->min = v;
        else {
            c->min = 0.0;
            c->min_expr = ini_deferred_expr(parts[si + 2]);
        }
    }
    if (n > si + 3) {
        if (ini_parse_f64(parts[si + 3], &v)) c->max = v;
        else {
            c->max = 255.0;
            c->max_expr = ini_deferred_expr(parts[si + 3]);
        }
    }
    if (n > si + 4 && ini_parse_uint(parts[si + 4], &u)) c->digits = (int)u;
    c->range_resolved = (c->min_expr == NULL && c->max_expr == NULL) ? LT_TRUE : LT_FALSE;
}

/* set bit option at index (grows with NULL holes) */
static void opt_set(char ***v, size_t *n, size_t idx, char *label)
{
    while (*n <= idx) ini_str_push(v, n, NULL);
    if ((*v)[idx]) lt_free((*v)[idx]);
    (*v)[idx] = label;
}

static void build_bit_options(lt_ini_const *c, char **parts, size_t n, size_t start,
                              const lt_map *defines)
{
    size_t i;
    char **v = NULL;
    size_t nv = 0;

    for (i = start; i < n; i++) {
        const char *p = parts[i];
        char *label;
        size_t k = 0;

        if (p[0] == '{') continue;
        while (p[k] >= '0' && p[k] <= '9') k++;
        if (k > 0 && p[k] == '=') {
            unsigned long idx = 0;
            char *num = lt_strndup(p, k);
            if (ini_parse_uint(num, &idx) && idx < 100000UL) {
                label = ini_dequote(p + k + 1);
                opt_set(&v, &nv, (size_t)idx, label);
            }
            lt_free(num);
            continue;
        }
        label = ini_dequote(p);
        if (label[0] == '$' && defines) {
            ini_strlist *l = (ini_strlist *)lt_map_get(defines, label + 1);
            if (l) {
                size_t j;
                for (j = 0; j < l->n; j++) ini_str_push(&v, &nv, lt_strdup(l->v[j]));
                lt_free(label);
                continue;
            }
        }
        if (!label[0]) {
            lt_free(label);
            ini_str_push(&v, &nv, NULL);
        } else {
            ini_str_push(&v, &nv, label);
        }
    }
    while (nv > 0 && v[nv - 1] == NULL) nv--;
    if (nv == 0) {
        if (v) lt_free(v);
        v = NULL;
    }
    c->bit_options = v;
    c->n_bit_options = nv;
}

lt_ini_const *ini_parse_constant_line(const char *name, const char *value, int page,
                                      unsigned long last_off, const char *help,
                                      const lt_map *defines)
{
    char **parts;
    size_t n = ini_split(value, &parts);
    lt_ini_const *c = NULL;
    char *cls = NULL;
    lt_dtype t;
    int ov = -1;
    unsigned long off;
    size_t units_idx = 3;

    if (n < 3) goto out;
    cls = lt_strdup(parts[0]);
    {
        size_t k;
        for (k = 0; cls[k]; k++)
            if (cls[k] >= 'A' && cls[k] <= 'Z') cls[k] = (char)(cls[k] - 'A' + 'a');
    }
    if (lt_dtype_parse(parts[1], &t, &ov) != 0) goto out;
    if (lt_stricmp(parts[2], "lastOffset") == 0) off = last_off;
    else if (!ini_parse_uint(parts[2], &off)) goto out;

    c = const_new(name);
    c->page = (lt_u8)page;
    c->offset = (lt_u32)off;
    c->type = t;
    c->endian_override = ov;
    if (help) c->help = lt_strdup(help);

    if (strcmp(cls, "bits") == 0) {
        c->type = LT_DT_BITS;
        if (n > 3) {
            int pos, size, disp;
            if (ini_parse_bit_range(parts[3], &pos, &size, &disp)) {
                c->bit_pos = pos;
                c->bit_size = size;
                c->bit_disp = disp;
            }
        }
        build_bit_options(c, parts, n, 4, defines);
        c->size_bytes = 0;
        goto out;
    }
    if (strcmp(cls, "array") == 0) {
        if (n > 3) ini_parse_shape(parts[3], &c->shape, &c->dyn);
        units_idx = 4;
    } else if (strcmp(cls, "string") == 0) {
        unsigned long len;
        c->type = LT_DT_STRING;
        if (n > 3 && ini_parse_uint(parts[3], &len)) {
            c->shape.kind = LT_SH_1D;
            c->shape.n = (size_t)len;
        }
        units_idx = 4;
    }
    apply_units_scale(c, parts, n, units_idx);
    if (c->type == LT_DT_STRING) c->size_bytes = lt_shape_count(&c->shape);
    else c->size_bytes = lt_dtype_size(c->type) * lt_shape_count(&c->shape);
out:
    if (cls) lt_free(cls);
    ini_strv_free(parts, n);
    return c;
}

lt_ini_const *ini_parse_pcvar_line(const char *name, const char *value, const char *help,
                                   const lt_map *defines)
{
    char **parts;
    size_t n = ini_split(value, &parts);
    lt_ini_const *c = NULL;
    char *cls;
    lt_dtype t;
    size_t k, units_idx = 2;

    if (n < 2) goto out;
    cls = lt_strdup(parts[0]);
    for (k = 0; cls[k]; k++)
        if (cls[k] >= 'A' && cls[k] <= 'Z') cls[k] = (char)(cls[k] - 'A' + 'a');
    if (lt_dtype_parse(parts[1], &t, NULL) != 0) {
        lt_free(cls);
        goto out;
    }
    c = const_new(name);
    c->page = 255;
    c->offset = 0;
    c->type = t;
    c->is_pc_var = LT_TRUE;
    if (help) c->help = lt_strdup(help);
    if (strcmp(cls, "bits") == 0) {
        int pos, size, disp;
        c->type = LT_DT_BITS;
        if (n > 2 && ini_parse_bit_range(parts[2], &pos, &size, &disp)) {
            c->bit_pos = pos;
            c->bit_size = size;
            c->bit_disp = disp;
        }
        build_bit_options(c, parts, n, 3, defines);
        lt_free(cls);
        goto out;
    }
    if (strcmp(cls, "array") == 0) {
        if (n > 2) ini_parse_shape(parts[2], &c->shape, &c->dyn);
        units_idx = 3;
    }
    lt_free(cls);
    apply_units_scale(c, parts, n, units_idx);
    if (c->type == LT_DT_STRING) c->size_bytes = lt_shape_count(&c->shape);
    else c->size_bytes = lt_dtype_size(c->type) * lt_shape_count(&c->shape);
out:
    ini_strv_free(parts, n);
    return c;
}

/* "name;+help text" -> name (malloc'd) and help (malloc'd or NULL) */
static char *extract_help(const char *key, char **help_out)
{
    const char *semi = strchr(key, ';');
    char *name, *rest, *h;
    char *semi2;
    char *dq;

    *help_out = NULL;
    if (!semi) {
        name = lt_strdup(key);
        return lt_str_trim(name);
    }
    name = lt_strndup(key, (size_t)(semi - key));
    lt_str_trim(name);
    rest = lt_strdup(semi + 1);
    lt_str_trim(rest);
    if (rest[0] == '+') {
        h = lt_strdup(rest + 1);
        lt_str_trim(h);
        semi2 = strchr(h, ';');
        if (semi2) {
            *semi2 = '\0';
            lt_str_trim(h);
        }
        dq = ini_dequote(h);
        lt_free(h);
        if (dq[0]) *help_out = dq;
        else lt_free(dq);
    }
    lt_free(rest);
    return name;
}

static int resolve_pcvar_byte(const lt_ini *ini, const char *name)
{
    double *d = (double *)lt_map_get(&ini->default_values, name);
    double v = 0;
    const lt_ini_const *c;
    if (d) v = *d;
    else {
        c = (const lt_ini_const *)lt_map_get(&ini->constant_idx, name);
        if (c) v = c->min;
    }
    if (!(v > 0)) v = 0;
    if (v > 255) v = 255;
    return (int)v;
}

int lt_ini_pcvar_byte(const lt_ini *ini, const char *name)
{
    void *v = lt_map_get(&ini->pc_variables, name);
    if (!v) return -1;
    return (int)((size_t)v - 1);
}

static void add_constant(lt_ini *ini, lt_ini_const *c)
{
    lt_ini_const *old = (lt_ini_const *)lt_map_get(&ini->constant_idx, c->name);
    ini_put((void ***)&ini->constants, &ini->n_constants, &ini->constant_idx, c->name, c,
            ini_freev_const);
    (void)old;
}

static void handle_constants(lt_ini *ini, ini_ctx *cx, ini_pstate *st, const char *rawkey,
                             const char *value)
{
    char *help = NULL;
    char *name = extract_help(rawkey, &help);
    lt_ini_const *c;

    if (lt_stricmp(name, "page") == 0) {
        unsigned long u;
        st->page = 0;
        if (ini_parse_uint(value, &u)) st->page = u > 0 ? (int)(u - 1) : 0;
        else st->page = 0;
        st->last_offset = 0;
        goto done;
    }
    if (ini_proto_key(ini, name, value)) goto done;
    c = ini_parse_constant_line(name, value, st->page, st->last_offset, help, &cx->defines);
    if (c) {
        st->last_offset = c->offset;
        add_constant(ini, c);
    }
done:
    lt_free(name);
    if (help) lt_free(help);
}

static void handle_pcvar(lt_ini *ini, ini_ctx *cx, const char *rawkey, const char *value)
{
    char *help = NULL;
    char *name = extract_help(rawkey, &help);
    lt_ini_const *c = ini_parse_pcvar_line(name, value, help, &cx->defines);

    if (c) {
        char **parts;
        size_t n = ini_split(value, &parts);
        char *up = lt_strdup(parts[0]);
        size_t k;
        for (k = 0; up[k]; k++)
            if (up[k] >= 'a' && up[k] <= 'z') up[k] = (char)(up[k] - 'a' + 'A');
        add_constant(ini, c);
        if (strcmp(up, "BITS") == 0 || strcmp(up, "SCALAR") == 0 || strcmp(up, "U08") == 0 ||
            strcmp(up, "S08") == 0 || strcmp(up, "U16") == 0 || strcmp(up, "S16") == 0) {
            lt_map_put(&ini->pc_variables, name,
                       (void *)(size_t)(resolve_pcvar_byte(ini, name) + 1));
        }
        lt_free(up);
        ini_strv_free(parts, n);
    }
    lt_free(name);
    if (help) lt_free(help);
}

/* post pass: re-resolve all pc variable bytes (defaults now known) */
void ini_resolve_pcvars(lt_ini *ini)
{
    size_t i;
    for (i = 0; i < ini->pc_variables.cap; i++) {
        lt_map_entry *e = &ini->pc_variables.slots[i];
        if (e->used == 1) e->val = (void *)(size_t)(resolve_pcvar_byte(ini, e->key) + 1);
    }
}

/* ------------------------------------------------------------------ */
/* identity and protocol keys                                          */
/* ------------------------------------------------------------------ */

static void cmdlist_set(lt_ini_cmdlist *l, const char *value)
{
    char **parts;
    size_t n = ini_split(value, &parts), i;

    ini_free_cmdlist(l);
    l->v = (lt_ini_blob *)lt_calloc(n, sizeof(lt_ini_blob));
    l->n = n;
    for (i = 0; i < n; i++) {
        char *q = ini_dequote(parts[i]);
        l->v[i] = ini_blob_from_str(q);
        lt_free(q);
    }
    ini_strv_free(parts, n);
}

static void set_ulong(unsigned long *dst, const char *value, unsigned long dflt)
{
    char *c = ini_clean(value);
    unsigned long u;
    *dst = ini_parse_uint(c, &u) ? u : dflt;
    lt_free(c);
}

static int is(const char *a, const char *b)
{
    return lt_stricmp(a, b) == 0;
}

/* returns 1 if the key was a protocol/identity-neutral key and was handled */
int ini_proto_key(lt_ini *ini, const char *key, const char *value)
{
    lt_ini_protocol *p = &ini->proto;
    char *q;

    if (is(key, "messageEnvelopeFormat")) {
        ini_set_str(&p->message_envelope_format, ini_dequote(value));
        p->uses_modern = strstr(p->message_envelope_format, "msEnvelope") != NULL;
    } else if (is(key, "maxUnusedRuntimeRange")) {
        set_ulong(&p->max_unused_runtime_range, value, 0);
    } else if (is(key, "endianness")) {
        char *l = lt_strdup(value);
        size_t k;
        for (k = 0; l[k]; k++)
            if (l[k] >= 'A' && l[k] <= 'Z') l[k] = (char)(l[k] - 'A' + 'a');
        ini->endian = strstr(l, "little") ? LT_ENDIAN_LITTLE : LT_ENDIAN_BIG;
        lt_free(l);
    } else if (is(key, "nPages") || is(key, "numPages")) {
        unsigned long u;
        char *c = ini_clean(value);
        ini->n_pages = ini_parse_uint(c, &u) && u < 256 ? (size_t)u : 0;
        lt_free(c);
    } else if (is(key, "pageSize") || is(key, "pageSizes")) {
        char **parts;
        size_t n = ini_split_naive(value, &parts), i, cnt = 0;
        unsigned long *v = (unsigned long *)lt_calloc(n, sizeof(unsigned long));
        for (i = 0; i < n; i++) {
            unsigned long u;
            char *d = ini_dequote(parts[i]);
            if (ini_parse_uint(d, &u)) v[cnt++] = u;
            lt_free(d);
        }
        ini_strv_free(parts, n);
        if (p->page_sizes) lt_free(p->page_sizes);
        p->page_sizes = v;
        p->n_page_sizes = cnt;
        ini->n_pages = cnt;
    } else if (is(key, "pageIdentifier")) {
        char **parts;
        size_t n = ini_split(value, &parts), i;
        ini_free_str_array(p->page_identifiers_raw, p->n_page_identifiers_raw);
        p->page_identifiers_raw = (char **)lt_calloc(n, sizeof(char *));
        p->n_page_identifiers_raw = n;
        for (i = 0; i < n; i++) p->page_identifiers_raw[i] = ini_dequote(parts[i]);
        ini_strv_free(parts, n);
    } else if (is(key, "pageReadCommand")) {
        cmdlist_set(&p->page_read_commands, value);
    } else if (is(key, "pageValueWrite") || is(key, "pageChunkWrite")) {
        cmdlist_set(&p->page_chunk_write_commands, value);
    } else if (is(key, "burnCommand")) {
        cmdlist_set(&p->burn_commands, value);
    } else if (is(key, "crc32CheckCommand")) {
        cmdlist_set(&p->crc32_check_commands, value);
    } else if (is(key, "pageActivate")) {
        cmdlist_set(&p->page_activate_commands, value);
    } else if (is(key, "retrieveConfigError")) {
        ini_set_str(&p->retrieve_config_error, ini_dequote(value));
    } else if (is(key, "pageActivationDelay") || is(key, "pageActivationDelayMs")) {
        set_ulong(&p->page_activation_delay, value, 500);
    } else if (is(key, "blockingFactor")) {
        set_ulong(&p->blocking_factor, value, 256);
    } else if (is(key, "interWriteDelay")) {
        set_ulong(&p->inter_write_delay, value, 0);
    } else if (is(key, "blockReadTimeout")) {
        set_ulong(&p->block_read_timeout, value, 1000);
    } else if (is(key, "delayAfterPortOpen")) {
        set_ulong(&p->delay_after_port_open, value, 0);
    } else if (is(key, "writeBlocks") || is(key, "tsWriteBlocks")) {
        char *c = ini_clean(value);
        p->write_blocks = (lt_stricmp(c, "on") == 0 || lt_stricmp(c, "true") == 0 ||
                           strcmp(c, "1") == 0) ? 1 : 0;
        lt_free(c);
    } else if (is(key, "enable2ndByteCanID")) {
        char *c = ini_clean(value);
        p->enable_can_id = (lt_stricmp(c, "false") == 0 || strcmp(c, "0") == 0) ? 0 : 1;
        lt_free(c);
    } else if (is(key, "tableWriteCommand")) {
        ini_set_str(&p->table_write_command, ini_dequote(value));
    } else if (is(key, "tableCrcCommand")) {
        ini_set_str(&p->table_crc_command, ini_dequote(value));
    } else if (is(key, "tableBlockingFactor")) {
        unsigned long u;
        char *c = ini_clean(value);
        if (ini_parse_uint(c, &u)) p->table_blocking_factor = (int)u;
        lt_free(c);
    } else if (is(key, "noCommReadDelay")) {
        set_ulong(&p->no_comm_read_delay, value, 0);
    } else if (is(key, "defaultRuntimeRecordPerSec")) {
        unsigned long u;
        char *c = ini_clean(value);
        if (ini_parse_uint(c, &u)) p->default_runtime_record_per_sec = (int)u;
        lt_free(c);
    } else if (is(key, "refreshLocalStoreOnActivity")) {
        p->refresh_local_store_on_activity = ini_parse_bool(value);
    } else if (is(key, "restrictSquirtRelationship")) {
        p->restrict_squirt_relationship = ini_parse_bool(value);
    } else if (is(key, "forceBigEndianProtocol")) {
        p->force_big_endian_protocol = ini_parse_bool(value);
    } else if (is(key, "useLegacyFTempUnits")) {
        p->use_legacy_f_temp_units = ini_parse_bool(value);
    } else if (is(key, "surpressConfigErrorVerbiage") || is(key, "suppressConfigErrorVerbiage")) {
        p->suppress_config_error_verbiage = ini_parse_bool(value);
    } else if (is(key, "validateArrayBounds")) {
        p->validate_array_bounds = ini_parse_bool(value);
    } else if (is(key, "ignoreMissingBitOptions")) {
        p->ignore_missing_bit_options = ini_parse_bool(value);
    } else if (is(key, "filterEchoBytes")) {
        p->filter_echo_bytes = ini_parse_bool(value);
    } else if (is(key, "envelopedScanCommands")) {
        p->enveloped_scan_commands = ini_parse_bool(value);
    } else if (is(key, "defaultIpAddress")) {
        ini_set_str(&p->default_ip_address, ini_dequote(value));
    } else if (is(key, "defaultIpPort")) {
        unsigned long u;
        char *c = ini_clean(value);
        if (ini_parse_uint(c, &u) && u < 65536UL) p->default_ip_port = (int)u;
        lt_free(c);
    } else if (is(key, "replayConfigTable") || is(key, "replayReadCommand") ||
               is(key, "replayRecordCountParam")) {
        /* parsed by the reference but unused (replay is out of scope) */
    } else if (is(key, "ochGetCommand")) {
        q = ini_clean(value);
        ini_blob_free(&p->och_get_command);
        {
            char *d = ini_dequote(q);
            p->och_get_command = ini_blob_from_str(d);
            lt_free(d);
        }
        lt_free(q);
    } else if (is(key, "ochBlockSize")) {
        set_ulong(&p->och_block_size, value, 0);
    } else {
        return 0;
    }
    return 1;
}

static void handle_identity(lt_ini *ini, const char *key, const char *value)
{
    lt_ini_protocol *p = &ini->proto;

    if (is(key, "signature")) {
        ini_set_str(&ini->signature, ini_dequote(value));
    } else if (is(key, "signaturePrefix")) {
        ini_set_str(&ini->signature_prefix, ini_dequote(value));
    } else if (is(key, "queryCommand")) {
        char *d = ini_dequote(value);
        ini_blob_free(&p->query_command);
        p->query_command = ini_blob_from_str(d);
        lt_free(d);
    } else if (is(key, "versionInfo")) {
        ini_set_str(&ini->version_info, ini_dequote(value));
    } else if (is(key, "iniSpecVersion")) {
        ini_set_str(&ini->ini_spec_version, ini_dequote(value));
    } else {
        ini_proto_key(ini, key, value);
    }
}

/* ------------------------------------------------------------------ */
/* [OutputChannels]                                                    */
/* ------------------------------------------------------------------ */

lt_ini_channel *ini_parse_channel_line(const char *name, const char *value)
{
    char *v = lt_strdup(value);
    lt_ini_channel *ch = NULL;

    lt_str_trim(v);
    if (v[0] == '{') {
        char *close = strchr(v, '}');
        char *rest;
        char **parts;
        size_t n;
        if (!close) goto out;
        ch = (lt_ini_channel *)lt_calloc(1, sizeof(lt_ini_channel));
        ch->name = lt_strdup(name);
        ch->type = LT_DT_F32;
        ch->bit_pos = ch->bit_count = -1;
        ch->bit_storage = LT_DT_U08;
        ch->scale = 1.0;
        ch->expression = lt_strndup(v + 1, (size_t)(close - v - 1));
        lt_str_trim(ch->expression);
        rest = lt_strdup(close + 1);
        lt_str_trim(rest);
        if (rest[0] == ',') {
            char *r2 = lt_strdup(rest + 1);
            lt_free(rest);
            rest = lt_str_trim(r2);
        }
        n = ini_split_naive(rest, &parts);
        ch->units = ini_dequote(parts[0]);
        ini_strv_free(parts, n);
        lt_free(rest);
        goto out;
    } else {
        char **parts;
        size_t n = ini_split_naive(v, &parts), sh = 0;
        int is_bits = 0;
        lt_dtype t;
        unsigned long off;

        if (n >= 2) {
            if (lt_stricmp(parts[0], "bits") == 0) {
                is_bits = 1;
                sh = 1;
            } else if (lt_stricmp(parts[0], "scalar") == 0) {
                sh = 1;
            }
        }
        if (n - sh >= 2 && lt_dtype_parse(parts[sh], &t, NULL) == 0 &&
            ini_parse_uint(parts[sh + 1], &off)) {
            char **p = parts + sh;
            size_t pn = n - sh;
            double d;
            ch = (lt_ini_channel *)lt_calloc(1, sizeof(lt_ini_channel));
            ch->name = lt_strdup(name);
            ch->offset = (lt_u32)off;
            ch->bit_pos = ch->bit_count = -1;
            ch->bit_storage = LT_DT_U08;
            ch->scale = 1.0;
            ch->units = lt_strdup("");
            if (is_bits) {
                ch->bit_storage = t;
                ch->type = LT_DT_BITS;
                if (pn > 2) {
                    if (p[2][0] == '[') {
                        char *t2 = lt_strdup(p[2]);
                        size_t a = 0, e = strlen(t2);
                        char *colon;
                        unsigned long st, en;
                        while (a < e && t2[a] == '[') a++;
                        while (e > a && t2[e - 1] == ']') e--;
                        t2[e] = '\0';
                        colon = strchr(t2 + a, ':');
                        if (colon) *colon = '\0';
                        lt_str_trim(t2 + a);
                        if (ini_parse_uint(t2 + a, &st) && st < 256) {
                            ch->bit_pos = (int)st;
                            if (!colon) {
                                ch->bit_count = 1;
                            } else {
                                char *et = colon + 1;
                                lt_str_trim(et);
                                if (ini_parse_uint(et, &en) && en >= st)
                                    ch->bit_count = (int)(en - st + 1);
                            }
                        }
                        lt_free(t2);
                    } else {
                        ini_set_str(&ch->units, ini_dequote(p[2]));
                    }
                }
            } else {
                ch->type = t;
                if (pn > 2) ini_set_str(&ch->units, ini_dequote(p[2]));
                if (pn > 3 && ini_parse_f64(p[3], &d)) ch->scale = d;
                if (pn > 4 && ini_parse_f64(p[4], &d)) ch->translate = d;
            }
        }
        ini_strv_free(parts, n);
    }
out:
    lt_free(v);
    return ch;
}

static void add_channel(lt_ini *ini, lt_ini_channel *ch)
{
    if (ch->expression) {
        char err[96];
        ch->ast = lt_expr_compile(ch->expression, err, sizeof err);
    }
    ini_put((void ***)&ini->channels, &ini->n_channels, &ini->channel_idx, ch->name, ch,
            ini_freev_channel);
}

static void handle_outch(lt_ini *ini, const char *key, const char *value)
{
    lt_ini_channel *ch;
    if (is(key, "ochBlockSize") || is(key, "ochGetCommand")) {
        ini_proto_key(ini, key, value);
        return;
    }
    if (lt_str_istarts(key, "scattered")) return;
    ch = ini_parse_channel_line(key, value);
    if (ch) add_channel(ini, ch);
}

/* ------------------------------------------------------------------ */
/* line driver                                                         */
/* ------------------------------------------------------------------ */

static int cond_live(const lt_vec *stack)
{
    size_t i;
    for (i = 0; i < stack->len; i++)
        if (!LT_VEC_AT(stack, int, i)) return 0;
    return 1;
}

static void do_define(ini_ctx *cx, const char *rest)
{
    char *t = lt_strdup(rest);
    char *eq, *name, *vals;
    char **parts;
    size_t n, i;
    ini_strlist *l;
    const char *s = t;

    lt_str_trim(t);
    if (*s == '=') {
        s++;
        while (*s == ' ' || *s == '\t') s++;
    }
    eq = (char *)strchr(s, '=');
    if (!eq) {
        lt_free(t);
        return;
    }
    name = lt_strndup(s, (size_t)(eq - s));
    lt_str_trim(name);
    vals = lt_strdup(eq + 1);
    n = ini_split(vals, &parts);
    l = (ini_strlist *)lt_calloc(1, sizeof(ini_strlist));
    for (i = 0; i < n; i++) {
        char *d = ini_dequote(parts[i]);
        if (d[0] == '$') {
            ini_strlist *ref = (ini_strlist *)lt_map_get(&cx->defines, d + 1);
            if (ref) {
                size_t j;
                for (j = 0; j < ref->n; j++) ini_str_push(&l->v, &l->n, lt_strdup(ref->v[j]));
                lt_free(d);
                continue;
            }
        }
        if (!d[0]) lt_free(d);
        else ini_str_push(&l->v, &l->n, d);
    }
    ini_strv_free(parts, n);
    if (name[0] && l->n > 0) {
        ini_strlist *old = (ini_strlist *)lt_map_get(&cx->defines, name);
        if (old) lt_slist_free(old);
        lt_map_put(&cx->defines, name, l);
    } else {
        lt_slist_free(l);
    }
    lt_free(name);
    lt_free(vals);
    lt_free(t);
}

static int parse_text(lt_ini *ini, ini_ctx *cx, const char *text, size_t len,
                      const char *base_dir, int depth);

static int do_include(lt_ini *ini, ini_ctx *cx, const char *rest, const char *base_dir,
                      int depth)
{
    char *path = ini_dequote(rest);
    char *full, *norm, *dir, *data, *text;
    size_t dlen = 0, tlen = 0;
    int rc;

    if (depth >= INI_MAX_INCLUDE_DEPTH) {
        lt_err_set("Include error: maximum depth (%d) exceeded", INI_MAX_INCLUDE_DEPTH);
        lt_free(path);
        return LT_EDATA;
    }
    if (path_is_abs(path) || !base_dir || !base_dir[0]) full = lt_strdup(path);
    else full = lt_path_join(base_dir, path);
    norm = path_norm(full);
    if (lt_map_has(&cx->included, norm)) {
        lt_err_set("Include error: circular reference detected for '%s'", path);
        lt_free(path);
        lt_free(full);
        lt_free(norm);
        return LT_EDATA;
    }
    lt_map_put(&cx->included, norm, (void *)cx);
    if (!lt_file_exists(full) || !(data = lt_file_read(full, &dlen))) {
        lt_err_set("Include error: file not found '%s'", path);
        lt_free(path);
        lt_free(full);
        lt_free(norm);
        return LT_ENOENT;
    }
    text = lt_ini_decode((const lt_u8 *)data, dlen, &tlen);
    lt_free(data);
    dir = path_dir(full);
    rc = parse_text(ini, cx, text, tlen, dir, depth + 1);
    lt_free(dir);
    lt_free(text);
    lt_free(path);
    lt_free(full);
    lt_free(norm);
    return rc;
}

static void set_section(lt_ini *ini, ini_pstate *st, const char *inner)
{
    char *t = lt_strdup(inner);
    char *amp;
    int ok = 1;
    size_t k;

    lt_str_trim(t);
    amp = strchr(t, '&');
    if (amp) {
        char *conds = lt_strdup(amp + 1);
        char *tok;
        char *save;
        *amp = '\0';
        lt_str_trim(t);
        /* split conditions on '&' */
        tok = conds;
        for (;;) {
            char *next = strchr(tok, '&');
            int neg = 0;
            char *sym;
            if (next) *next = '\0';
            sym = lt_strdup(tok);
            lt_str_trim(sym);
            if (sym[0] == '!') {
                neg = 1;
                save = sym + 1;
                memmove(sym, save, strlen(save) + 1);
                lt_str_trim(sym);
            }
            if (sym[0]) {
                int has = lt_map_has(&ini->active_symbols, sym) ? 1 : 0;
                if (neg ? has : !has) ok = 0;
            }
            lt_free(sym);
            if (!next) break;
            tok = next + 1;
        }
        lt_free(conds);
    }
    for (k = 0; t[k]; k++)
        if (t[k] >= 'A' && t[k] <= 'Z') t[k] = (char)(t[k] - 'A' + 'a');
    if (st->section) lt_free(st->section);
    if (!ok) st->section = lt_strdup("");
    else if (strcmp(t, "uidialogs") == 0) st->section = lt_strdup("userdefined");
    else st->section = lt_strdup(t);
    lt_free(t);
}

static void dispatch(lt_ini *ini, ini_ctx *cx, ini_pstate *st, const char *line,
                     const char *key, const char *value)
{
    const char *s = st->section;

    if (!s[0]) return;
    if (strcmp(s, "megatune") == 0) {
        handle_identity(ini, key, value);
    } else if (strcmp(s, "tunerstudio") == 0) {
        handle_identity(ini, key, value);
    } else if (strcmp(s, "constants") == 0) {
        handle_constants(ini, cx, st, key, value);
    } else if (strcmp(s, "pcvariables") == 0) {
        handle_pcvar(ini, cx, key, value);
    } else if (strcmp(s, "outputchannels") == 0) {
        handle_outch(ini, key, value);
    } else if (strcmp(s, "burstmode") == 0) {
        if (is(key, "getCommand")) {
            ini_blob_free(&ini->proto.burst_get_command);
            {
                char *d = ini_dequote(value);
                ini->proto.burst_get_command = ini_blob_from_str(d);
                lt_free(d);
            }
        } else if (is(key, "ochGetCommand") || is(key, "ochBlockSize")) {
            ini_proto_key(ini, key, value);
        }
    } else if (strcmp(s, "loggerdefinition") == 0) {
        lt_buf_puts(&cx->logger_raw, line);
        lt_buf_putc(&cx->logger_raw, '\n');
    } else {
        ini_section_line(ini, cx, st, key, value);
    }
}

static int parse_text(lt_ini *ini, ini_ctx *cx, const char *text, size_t len,
                      const char *base_dir, int depth)
{
    ini_pstate st;
    lt_vec cond;
    size_t pos = 0;
    int rc = 0, first;
    lt_buf cur;

    memset(&st, 0, sizeof st);
    st.section = lt_strdup("");
    lt_vec_init(&st.limits, sizeof(ini_limit));
    lt_vec_init(&cond, sizeof(int));
    lt_buf_init(&cur);

    while (pos < len && rc == 0) {
        size_t e = pos, n;
        char *line, *stripped;
        const char *p;
        size_t cut;

        /* ---- one physical line (+ continuations) ---- */
        lt_buf_clear(&cur);
        first = 1;
        for (;;) {
            e = pos;
            while (e < len && text[e] != '\n') e++;
            n = e - pos;
            if (n > 0 && text[pos + n - 1] == '\r') n--;
            if (first) {
                lt_buf_append(&cur, text + pos, n);
                first = 0;
            } else {
                char *seg = lt_strndup(text + pos, n);
                lt_str_trim(seg);
                lt_buf_puts(&cur, seg);
                lt_free(seg);
            }
            pos = e < len ? e + 1 : e;
            /* continuation: ends with backslash + optional whitespace */
            {
                size_t k = cur.len;
                while (k > 0 && (cur.data[k - 1] == ' ' || cur.data[k - 1] == '\t' ||
                                 cur.data[k - 1] == '\r'))
                    k--;
                if (k > 0 && cur.data[k - 1] == '\\' && pos < len) {
                    cur.len = k - 1;
                    cur.data[cur.len] = '\0';
                    if (cur.len == 0) cur.data[0] = '\0';
                    continue;
                }
            }
            break;
        }
        line = cur.data ? cur.data : (char *)"";
        cut = ini_comment_cut(line);
        stripped = lt_strndup(line, cut);
        lt_str_trim(stripped);
        if (!stripped[0]) {
            lt_free(stripped);
            continue;
        }
        p = stripped;

        if (p[0] == '#') {
            int live = cond_live(&cond);
            if (lt_str_starts(p, "#set ")) {
                if (live) {
                    char *sym = lt_strdup(p + 5);
                    lt_str_trim(sym);
                    if (!lt_map_has(&cx->managed, sym))
                        lt_map_put(&ini->active_symbols, sym, (void *)cx);
                    lt_free(sym);
                }
            } else if (lt_str_starts(p, "#unset ")) {
                if (live) {
                    char *sym = lt_strdup(p + 7);
                    lt_str_trim(sym);
                    /* lt_map has no delete: rebuild without the symbol */
                    if (lt_map_has(&ini->active_symbols, sym) && !lt_map_has(&cx->managed, sym)) {
                        lt_map nm;
                        size_t i;
                        lt_map_init(&nm, LT_FALSE);
                        for (i = 0; i < ini->active_symbols.cap; i++) {
                            lt_map_entry *en = &ini->active_symbols.slots[i];
                            if (en->used == 1 && strcmp(en->key, sym) != 0)
                                lt_map_put(&nm, en->key, en->val);
                        }
                        lt_map_free(&ini->active_symbols);
                        ini->active_symbols = nm;
                    }
                    lt_free(sym);
                }
            } else if (lt_str_starts(p, "#if ")) {
                char *sym = lt_strdup(p + 4);
                int v;
                lt_str_trim(sym);
                lt_map_put(&ini->tested_symbols, sym, (void *)cx);
                v = lt_map_has(&ini->active_symbols, sym) ? 1 : 0;
                *(int *)lt_vec_push(&cond) = v;
                lt_free(sym);
            } else if (strcmp(p, "#else") == 0) {
                if (cond.len > 0) LT_VEC_AT(&cond, int, cond.len - 1) = !LT_VEC_AT(&cond, int, cond.len - 1);
            } else if (strcmp(p, "#endif") == 0) {
                if (cond.len > 0) cond.len--;
            } else if (lt_str_starts(p, "#define")) {
                if (live) do_define(cx, p + 7);
            } else if (lt_str_starts(p, "#include")) {
                if (live) rc = do_include(ini, cx, p + 8, base_dir, depth);
            }
            lt_free(stripped);
            continue;
        }
        if (!cond_live(&cond)) {
            lt_free(stripped);
            continue;
        }
        if (p[0] == '[' && p[strlen(p) - 1] == ']') {
            char *inner = lt_strndup(p + 1, strlen(p) - 2);
            ini_flush_limits(ini, &st);
            set_section(ini, &st, inner);
            lt_free(inner);
            lt_free(stripped);
            continue;
        }
        {
            char *eq = (char *)strchr(p, '=');
            if (eq) {
                char *key = lt_strndup(p, (size_t)(eq - p));
                char *val = lt_strdup(eq + 1);
                lt_str_trim(key);
                lt_str_trim(val);
                dispatch(ini, cx, &st, p, key, val);
                lt_free(key);
                lt_free(val);
            } else if (strcmp(st.section, "menu") == 0 && is(p, "std_separator")) {
                dispatch(ini, cx, &st, p, p, "");
            }
        }
        lt_free(stripped);
    }
    ini_flush_limits(ini, &st);
    lt_buf_free(&cur);
    lt_vec_free(&cond);
    ini_pstate_free(&st);
    return rc;
}

/* ------------------------------------------------------------------ */
/* public loaders                                                      */
/* ------------------------------------------------------------------ */

static lt_ini *load_common(const char *text, size_t len, const char *base_dir,
                           const lt_ini_options *opts, const char *source_path)
{
    lt_ini *ini = ini_new();
    ini_ctx *cx = (ini_ctx *)ini->priv;
    size_t i;
    int rc;

    if (opts && opts->symbols)
        for (i = 0; i < opts->n_symbols; i++) {
            lt_map_put(&ini->active_symbols, opts->symbols[i], (void *)cx);
            lt_map_put(&cx->managed, opts->symbols[i], (void *)cx);
        }
    if (opts && opts->managed)
        for (i = 0; i < opts->n_managed; i++)
            lt_map_put(&cx->managed, opts->managed[i], (void *)cx);
    if (source_path) {
        char *n = path_norm(source_path);
        lt_map_put(&cx->included, n, (void *)cx);
        lt_free(n);
        ini->source_path = lt_strdup(source_path);
    }
    rc = parse_text(ini, cx, text, len, base_dir, 0);
    if (rc != 0) {
        lt_ini_free(ini);
        return NULL;
    }
    ini_post_process(ini, source_path ? lt_path_base(source_path) : NULL);
    return ini;
}

lt_ini *lt_ini_load_string(const char *text, size_t len, const char *base_dir,
                           const lt_ini_options *opts)
{
    size_t tlen = 0;
    char *dec;
    lt_ini *ini;

    if (!text) text = "";
    dec = lt_ini_decode((const lt_u8 *)text, len, &tlen);
    ini = load_common(dec, tlen, base_dir, opts, NULL);
    lt_free(dec);
    return ini;
}

lt_ini *lt_ini_load_file(const char *path, const lt_ini_options *opts)
{
    size_t dlen = 0, tlen = 0;
    char *data, *dec, *dir;
    lt_ini *ini;

    if (!lt_file_exists(path) || !(data = lt_file_read(path, &dlen))) {
        lt_err_set("Include error: file not found '%s'", path);
        return NULL;
    }
    dec = lt_ini_decode((const lt_u8 *)data, dlen, &tlen);
    lt_free(data);
    dir = path_dir(path);
    ini = load_common(dec, tlen, dir, opts, path);
    lt_free(dir);
    lt_free(dec);
    return ini;
}
