/* action.c - action scripting: JSON model, recorder, validation, replay.
 * Spec: docs/spec/09-project.md section 8.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include "proj_priv.h"
#include "action.h"
#include "../plat/plat.h"
#include "../tableops/tableops.h"

static const char *const type_names[] = {"TableEdit", "ConstantChange", "BulkOperation",
                                         "ExecuteLuaScript", "Pause", "SendCommand"};

const char *lt_action_type_name(lt_act_type t)
{
    return ((int)t >= 0 && (int)t < 6) ? type_names[t] : "Unknown";
}

/* ---------------------------------------------------------------- */
/* actions                                                           */
/* ---------------------------------------------------------------- */
void lt_action_init(lt_action *a, lt_act_type type)
{
    memset(a, 0, sizeof *a);
    a->type = type;
}

void lt_action_clear(lt_action *a)
{
    size_t i;
    lt_free(a->table_name);
    lt_free(a->constant_name);
    lt_free(a->operation);
    lt_free(a->script);
    lt_free(a->description);
    lt_free(a->command);
    lt_free(a->cells);
    for (i = 0; i < a->nparams; i++) lt_free(a->param_keys[i]);
    lt_free(a->param_keys);
    lt_free(a->param_vals);
    lt_free(a->old_values);
    memset(a, 0, sizeof *a);
}

void lt_action_copy(lt_action *dst, const lt_action *src)
{
    size_t i;
    *dst = *src;
    dst->table_name = lt_proj_dup_or_null(src->table_name);
    dst->constant_name = lt_proj_dup_or_null(src->constant_name);
    dst->operation = lt_proj_dup_or_null(src->operation);
    dst->script = lt_proj_dup_or_null(src->script);
    dst->description = lt_proj_dup_or_null(src->description);
    dst->command = lt_proj_dup_or_null(src->command);
    dst->cells = NULL;
    dst->param_keys = NULL;
    dst->param_vals = NULL;
    dst->old_values = NULL;
    if (src->ncells) {
        dst->cells = (lt_act_cell *)lt_malloc(src->ncells * sizeof(lt_act_cell));
        memcpy(dst->cells, src->cells, src->ncells * sizeof(lt_act_cell));
    }
    if (src->nparams) {
        dst->param_keys = (char **)lt_malloc(src->nparams * sizeof(char *));
        dst->param_vals = (double *)lt_malloc(src->nparams * sizeof(double));
        for (i = 0; i < src->nparams; i++) {
            dst->param_keys[i] = lt_strdup(src->param_keys[i]);
            dst->param_vals[i] = src->param_vals[i];
        }
    }
    if (src->nold) {
        dst->old_values = (double *)lt_malloc(src->nold * sizeof(double));
        memcpy(dst->old_values, src->old_values, src->nold * sizeof(double));
    }
}

void lt_action_table_edit(lt_action *a, const char *table, unsigned x, unsigned y,
                          double new_value, int has_old, double old_value)
{
    lt_action_init(a, LT_ACT_TABLE_EDIT);
    a->table_name = lt_strdup(table);
    a->x_index = (unsigned short)x;
    a->y_index = (unsigned short)y;
    a->new_value = new_value;
    a->has_old = has_old;
    a->old_value = old_value;
}

void lt_action_constant_change(lt_action *a, const char *name, double new_value, int has_old,
                               double old_value)
{
    lt_action_init(a, LT_ACT_CONSTANT_CHANGE);
    a->constant_name = lt_strdup(name);
    a->new_value = new_value;
    a->has_old = has_old;
    a->old_value = old_value;
}

void lt_action_bulk(lt_action *a, const char *operation, const char *table)
{
    lt_action_init(a, LT_ACT_BULK);
    a->operation = lt_strdup(operation);
    a->table_name = lt_strdup(table);
}

void lt_action_bulk_add_cell(lt_action *a, unsigned x, unsigned y)
{
    a->cells = (lt_act_cell *)lt_realloc(a->cells, (a->ncells + 1) * sizeof(lt_act_cell));
    a->cells[a->ncells].x = (unsigned short)x;
    a->cells[a->ncells].y = (unsigned short)y;
    a->ncells++;
}

void lt_action_bulk_set_param(lt_action *a, const char *key, double value)
{
    size_t i;
    for (i = 0; i < a->nparams; i++) {
        if (strcmp(a->param_keys[i], key) == 0) {
            a->param_vals[i] = value;
            return;
        }
    }
    a->param_keys = (char **)lt_realloc(a->param_keys, (a->nparams + 1) * sizeof(char *));
    a->param_vals = (double *)lt_realloc(a->param_vals, (a->nparams + 1) * sizeof(double));
    a->param_keys[a->nparams] = lt_strdup(key);
    a->param_vals[a->nparams] = value;
    a->nparams++;
}

void lt_action_pause(lt_action *a, unsigned long ms)
{
    lt_action_init(a, LT_ACT_PAUSE);
    a->duration_ms = ms;
}

void lt_action_send_command(lt_action *a, const char *cmd)
{
    lt_action_init(a, LT_ACT_SEND_CMD);
    a->command = lt_strdup(cmd);
}

void lt_action_lua(lt_action *a, const char *script, const char *description)
{
    lt_action_init(a, LT_ACT_LUA);
    a->script = lt_strdup(script);
    a->description = lt_strdup(description ? description : "");
}

/* ---------------------------------------------------------------- */
/* sets                                                              */
/* ---------------------------------------------------------------- */
void lt_action_set_init(lt_action_set *s)
{
    memset(s, 0, sizeof *s);
}

void lt_action_set_free(lt_action_set *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) lt_action_clear(&s->actions[i]);
    lt_free(s->actions);
    lt_free(s->id);
    lt_free(s->name);
    lt_free(s->description);
    lt_free(s->version);
    lt_free(s->meta.created_by);
    lt_free(s->meta.created_at);
    lt_free(s->meta.modified_at);
    lt_proj_strv_free(s->meta.tags, s->meta.ntags);
    lt_proj_strv_free(s->meta.compatible_ecus, s->meta.necus);
    memset(s, 0, sizeof *s);
}

static lt_json *act_strs_to_json(char *const *v, size_t n)
{
    lt_json *a = lt_json_arr();
    size_t i;
    for (i = 0; i < n; i++) lt_json_arr_push(a, lt_json_str(v[i]));
    return a;
}

static void set_err(char *err, size_t errlen, const char *prefix, const char *msg)
{
    if (err != NULL && errlen > 0) lt_snprintf(err, errlen, "%s: %s", prefix, msg);
    lt_err_set("%s: %s", prefix, msg);
}

static lt_json *action_to_json(const lt_action *a, const char **bad)
{
    lt_json *o = lt_json_obj(), *d = lt_json_obj();
    size_t i;
    switch (a->type) {
    case LT_ACT_TABLE_EDIT:
        if (!lt_isfinite(a->new_value) || (a->has_old && !lt_isfinite(a->old_value)))
            *bad = "non-finite number";
        lt_json_obj_set(d, "table_name", lt_json_str(a->table_name ? a->table_name : ""));
        lt_json_obj_set(d, "x_index", lt_json_num(a->x_index));
        lt_json_obj_set(d, "y_index", lt_json_num(a->y_index));
        lt_json_obj_set(d, "new_value", lt_json_num(a->new_value));
        lt_json_obj_set(d, "old_value", a->has_old ? lt_json_num(a->old_value) : lt_json_null());
        break;
    case LT_ACT_CONSTANT_CHANGE:
        if (!lt_isfinite(a->new_value) || (a->has_old && !lt_isfinite(a->old_value)))
            *bad = "non-finite number";
        lt_json_obj_set(d, "constant_name", lt_json_str(a->constant_name ? a->constant_name : ""));
        lt_json_obj_set(d, "new_value", lt_json_num(a->new_value));
        lt_json_obj_set(d, "old_value", a->has_old ? lt_json_num(a->old_value) : lt_json_null());
        break;
    case LT_ACT_BULK: {
        lt_json *cells = lt_json_arr(), *params = lt_json_obj();
        lt_json_obj_set(d, "operation", lt_json_str(a->operation ? a->operation : ""));
        lt_json_obj_set(d, "table_name", lt_json_str(a->table_name ? a->table_name : ""));
        for (i = 0; i < a->ncells; i++) {
            lt_json *c = lt_json_arr();
            lt_json_arr_push(c, lt_json_num(a->cells[i].x));
            lt_json_arr_push(c, lt_json_num(a->cells[i].y));
            lt_json_arr_push(cells, c);
        }
        lt_json_obj_set(d, "cells", cells);
        for (i = 0; i < a->nparams; i++) {
            if (!lt_isfinite(a->param_vals[i])) *bad = "non-finite number";
            lt_json_obj_set(params, a->param_keys[i], lt_json_num(a->param_vals[i]));
        }
        lt_json_obj_set(d, "parameters", params);
        if (a->has_old_values) {
            lt_json *ov = lt_json_arr();
            for (i = 0; i < a->nold; i++) {
                if (!lt_isfinite(a->old_values[i])) *bad = "non-finite number";
                lt_json_arr_push(ov, lt_json_num(a->old_values[i]));
            }
            lt_json_obj_set(d, "old_values", ov);
        } else {
            lt_json_obj_set(d, "old_values", lt_json_null());
        }
        break;
    }
    case LT_ACT_LUA:
        lt_json_obj_set(d, "script", lt_json_str(a->script ? a->script : ""));
        lt_json_obj_set(d, "description", lt_json_str(a->description ? a->description : ""));
        break;
    case LT_ACT_PAUSE:
        lt_json_obj_set(d, "duration_ms", lt_json_num((double)a->duration_ms));
        break;
    case LT_ACT_SEND_CMD:
        lt_json_obj_set(d, "command", lt_json_str(a->command ? a->command : ""));
        break;
    }
    lt_json_obj_set(o, "type", lt_json_str(lt_action_type_name(a->type)));
    lt_json_obj_set(o, "data", d);
    return o;
}

char *lt_action_set_serialize(const lt_action_set *s, char *err, size_t errlen)
{
    lt_json *o = lt_json_obj(), *acts = lt_json_arr(), *meta = lt_json_obj();
    const char *bad = NULL;
    char *r;
    size_t i;

    lt_json_obj_set(o, "id", lt_json_str(s->id ? s->id : ""));
    lt_json_obj_set(o, "name", lt_json_str(s->name ? s->name : ""));
    lt_json_obj_set(o, "description", lt_json_str(s->description ? s->description : ""));
    lt_json_obj_set(o, "version", lt_json_str(s->version ? s->version : "1.0"));
    for (i = 0; i < s->n; i++) lt_json_arr_push(acts, action_to_json(&s->actions[i], &bad));
    lt_json_obj_set(o, "actions", acts);
    lt_json_obj_set(meta, "created_by", lt_json_str(s->meta.created_by ? s->meta.created_by : ""));
    lt_json_obj_set(meta, "created_at", lt_json_str(s->meta.created_at ? s->meta.created_at : ""));
    lt_json_obj_set(meta, "modified_at",
                    lt_json_str(s->meta.modified_at ? s->meta.modified_at : ""));
    lt_json_obj_set(meta, "tags", act_strs_to_json(s->meta.tags, s->meta.ntags));
    lt_json_obj_set(meta, "compatible_ecus",
                    act_strs_to_json(s->meta.compatible_ecus, s->meta.necus));
    lt_json_obj_set(o, "metadata", meta);
    if (bad != NULL) {
        lt_json_free(o);
        set_err(err, errlen, "Failed to serialize action set", bad);
        return NULL;
    }
    r = lt_json_to_string(o, 1);
    lt_json_free(o);
    return r;
}

/* ---- deserialize ---- */
typedef struct dctx {
    char msg[200];
    int failed;
} dctx;

static void dfail(dctx *d, const char *fmt, const char *a, size_t idx)
{
    if (d->failed) return;
    d->failed = 1;
    lt_snprintf(d->msg, sizeof d->msg, fmt, a, (unsigned long)idx);
}

static char *d_str(dctx *d, const lt_json *o, const char *key)
{
    const lt_json *v = lt_json_get(o, key);
    if (v == NULL || v->type != LT_JSON_STR) {
        dfail(d, "missing or invalid field `%s`", key, 0);
        return lt_strdup("");
    }
    return lt_strdup(v->str);
}

static double d_num(dctx *d, const lt_json *o, const char *key)
{
    const lt_json *v = lt_json_get(o, key);
    if (v == NULL || v->type != LT_JSON_NUM) {
        dfail(d, "missing or invalid field `%s`", key, 0);
        return 0;
    }
    return v->num;
}

static double d_uint(dctx *d, const lt_json *o, const char *key, double max)
{
    double v = d_num(d, o, key);
    if (v < 0 || v > max || v != floor(v)) {
        dfail(d, "field `%s` out of range", key, 0);
        return 0;
    }
    return v;
}

static void d_strs(dctx *d, const lt_json *o, const char *key, char ***out, size_t *n)
{
    const lt_json *v = lt_json_get(o, key);
    size_t i;
    *out = NULL;
    *n = 0;
    if (v == NULL || v->type != LT_JSON_ARR) {
        dfail(d, "missing or invalid field `%s`", key, 0);
        return;
    }
    for (i = 0; i < v->n; i++) {
        if (v->items[i]->type != LT_JSON_STR) {
            dfail(d, "field `%s` must hold strings", key, 0);
            return;
        }
        *out = (char **)lt_realloc(*out, (*n + 1) * sizeof(char *));
        (*out)[(*n)++] = lt_strdup(v->items[i]->str);
    }
}

static void d_action(dctx *d, const lt_json *o, lt_action *a)
{
    const char *type = lt_json_get_str(o, "type", NULL);
    const lt_json *data = lt_json_get(o, "data"), *v;
    size_t i;
    int t = -1;

    lt_action_init(a, LT_ACT_TABLE_EDIT);
    if (o->type != LT_JSON_OBJ || type == NULL || data == NULL || data->type != LT_JSON_OBJ) {
        dfail(d, "malformed action %lu", "", 0);
        return;
    }
    for (i = 0; i < 6; i++)
        if (strcmp(type, type_names[i]) == 0) t = (int)i;
    if (t < 0) {
        dfail(d, "unknown variant `%s`", type, 0);
        return;
    }
    a->type = (lt_act_type)t;
    switch (a->type) {
    case LT_ACT_TABLE_EDIT:
        a->table_name = d_str(d, data, "table_name");
        a->x_index = (unsigned short)d_uint(d, data, "x_index", 65535.0);
        a->y_index = (unsigned short)d_uint(d, data, "y_index", 65535.0);
        a->new_value = d_num(d, data, "new_value");
        v = lt_json_get(data, "old_value");
        if (v != NULL && v->type == LT_JSON_NUM) {
            a->has_old = 1;
            a->old_value = v->num;
        } else if (v != NULL && v->type != LT_JSON_NULL) {
            dfail(d, "invalid field `%s`", "old_value", 0);
        }
        break;
    case LT_ACT_CONSTANT_CHANGE:
        a->constant_name = d_str(d, data, "constant_name");
        a->new_value = d_num(d, data, "new_value");
        v = lt_json_get(data, "old_value");
        if (v != NULL && v->type == LT_JSON_NUM) {
            a->has_old = 1;
            a->old_value = v->num;
        } else if (v != NULL && v->type != LT_JSON_NULL) {
            dfail(d, "invalid field `%s`", "old_value", 0);
        }
        break;
    case LT_ACT_BULK:
        a->operation = d_str(d, data, "operation");
        a->table_name = d_str(d, data, "table_name");
        v = lt_json_get(data, "cells");
        if (v == NULL || v->type != LT_JSON_ARR) {
            dfail(d, "missing or invalid field `%s`", "cells", 0);
        } else {
            for (i = 0; i < v->n; i++) {
                const lt_json *c = v->items[i];
                if (c->type != LT_JSON_ARR || c->n != 2 || c->items[0]->type != LT_JSON_NUM ||
                    c->items[1]->type != LT_JSON_NUM || c->items[0]->num < 0 ||
                    c->items[0]->num > 65535 || c->items[1]->num < 0 || c->items[1]->num > 65535) {
                    dfail(d, "invalid cell in `%s`", "cells", 0);
                    break;
                }
                lt_action_bulk_add_cell(a, (unsigned)c->items[0]->num, (unsigned)c->items[1]->num);
            }
        }
        v = lt_json_get(data, "parameters");
        if (v == NULL || v->type != LT_JSON_OBJ) {
            dfail(d, "missing or invalid field `%s`", "parameters", 0);
        } else {
            for (i = 0; i < v->n; i++) {
                if (v->items[i]->type != LT_JSON_NUM) {
                    dfail(d, "invalid value in `%s`", "parameters", 0);
                    break;
                }
                lt_action_bulk_set_param(a, v->keys[i], v->items[i]->num);
            }
        }
        v = lt_json_get(data, "old_values");
        if (v != NULL && v->type == LT_JSON_ARR) {
            a->has_old_values = 1;
            for (i = 0; i < v->n; i++) {
                if (v->items[i]->type != LT_JSON_NUM) {
                    dfail(d, "invalid value in `%s`", "old_values", 0);
                    break;
                }
                a->old_values = (double *)lt_realloc(a->old_values, (a->nold + 1) * sizeof(double));
                a->old_values[a->nold++] = v->items[i]->num;
            }
        } else if (v != NULL && v->type != LT_JSON_NULL) {
            dfail(d, "invalid field `%s`", "old_values", 0);
        }
        break;
    case LT_ACT_LUA:
        a->script = d_str(d, data, "script");
        a->description = d_str(d, data, "description");
        break;
    case LT_ACT_PAUSE:
        a->duration_ms = (unsigned long)d_uint(d, data, "duration_ms", 4294967295.0);
        break;
    case LT_ACT_SEND_CMD:
        a->command = d_str(d, data, "command");
        break;
    }
}

int lt_action_set_deserialize(const char *json, lt_action_set *out, char *err, size_t errlen)
{
    char perr[160];
    lt_json *j = lt_json_parse(json, strlen(json), perr, sizeof perr);
    dctx d;
    const lt_json *acts, *meta;
    lt_action_set s;
    size_t i;

    memset(&d, 0, sizeof d);
    lt_action_set_init(&s);
    if (j == NULL) {
        set_err(err, errlen, "Failed to deserialize action set", perr);
        return LT_EDATA;
    }
    if (j->type != LT_JSON_OBJ) dfail(&d, "expected an object%s", "", 0);
    s.id = d_str(&d, j, "id");
    s.name = d_str(&d, j, "name");
    s.description = d_str(&d, j, "description");
    s.version = d_str(&d, j, "version");
    acts = lt_json_get(j, "actions");
    meta = lt_json_get(j, "metadata");
    if (acts == NULL || acts->type != LT_JSON_ARR) dfail(&d, "missing or invalid field `%s`", "actions", 0);
    if (meta == NULL || meta->type != LT_JSON_OBJ) {
        dfail(&d, "missing or invalid field `%s`", "metadata", 0);
    } else {
        s.meta.created_by = d_str(&d, meta, "created_by");
        s.meta.created_at = d_str(&d, meta, "created_at");
        s.meta.modified_at = d_str(&d, meta, "modified_at");
        d_strs(&d, meta, "tags", &s.meta.tags, &s.meta.ntags);
        d_strs(&d, meta, "compatible_ecus", &s.meta.compatible_ecus, &s.meta.necus);
    }
    if (!d.failed && acts != NULL) {
        s.actions = (lt_action *)lt_calloc(acts->n ? acts->n : 1, sizeof(lt_action));
        for (i = 0; i < acts->n; i++) {
            d_action(&d, acts->items[i], &s.actions[s.n]);
            s.n++;
            if (d.failed) break;
        }
    }
    lt_json_free(j);
    if (d.failed) {
        lt_action_set_free(&s);
        set_err(err, errlen, "Failed to deserialize action set", d.msg);
        return LT_EDATA;
    }
    *out = s;
    return LT_OK;
}

int lt_action_set_save(const lt_action_set *s, const char *path)
{
    char err[200], *txt = lt_action_set_serialize(s, err, sizeof err);
    int rc;
    if (txt == NULL) return LT_EDATA;
    rc = lt_proj_write_atomic(path, txt, strlen(txt));
    lt_free(txt);
    return rc;
}

int lt_action_set_load(const char *path, lt_action_set *out)
{
    size_t n = 0;
    char err[200], *txt = lt_file_read(path, &n);
    int rc;
    if (txt == NULL) return LT_ENOENT;
    rc = lt_action_set_deserialize(txt, out, err, sizeof err);
    lt_free(txt);
    return rc;
}

/* ---------------------------------------------------------------- */
/* recorder                                                          */
/* ---------------------------------------------------------------- */
static void make_uuid(char out[37])
{
    unsigned char b[16];
    static unsigned long counter = 0;
    /* universal C has no entropy source: mix clocks, a counter and a stack
       address so ids differ between calls and between processes */
    lt_u32 h = lt_crc32(0, &counter, sizeof counter);
    unsigned long t = lt_proj_secs_now();
    double ms = lt_plat_now_ms();
    unsigned char addr[sizeof(void *)];
    const void *sp = (const void *)b;
    size_t i;
    memcpy(addr, &sp, sizeof addr);
    for (i = 0; i < 16; i++) {
        h = lt_crc32(h, &t, sizeof t);
        h = lt_crc32(h, &ms, sizeof ms);
        h = lt_crc32(h, addr, sizeof addr);
        h = lt_crc32(h, &i, sizeof i);
        b[i] = (unsigned char)(h >> 8);
    }
    counter++;
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    lt_snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
                b[13], b[14], b[15]);
}

void lt_arec_init(lt_arec *r)
{
    r->cur = NULL;
}

void lt_arec_free(lt_arec *r)
{
    if (r->cur != NULL) {
        lt_action_set_free(r->cur);
        lt_free(r->cur);
        r->cur = NULL;
    }
}

char *lt_arec_start(lt_arec *r, const char *name, const char *desc, const char *created_by)
{
    lt_action_set *s;
    char id[37], now[21];
    lt_arec_free(r);
    s = (lt_action_set *)lt_calloc(1, sizeof *s);
    make_uuid(id);
    lt_proj_utc_now(now);
    s->id = lt_strdup(id);
    s->name = lt_strdup(name ? name : "");
    s->description = lt_strdup(desc ? desc : "");
    s->version = lt_strdup("1.0");
    s->meta.created_by = lt_strdup(created_by ? created_by : "");
    s->meta.created_at = lt_strdup(now);
    s->meta.modified_at = lt_strdup(now);
    r->cur = s;
    return lt_strdup(id);
}

lt_action_set *lt_arec_stop(lt_arec *r)
{
    lt_action_set *s = r->cur;
    r->cur = NULL;
    return s;
}

int lt_arec_record(lt_arec *r, const lt_action *a)
{
    lt_action_set *s = r->cur;
    if (s == NULL) {
        lt_err_set("No recording in progress");
        return LT_EINVAL;
    }
    s->actions = (lt_action *)lt_realloc(s->actions, (s->n + 1) * sizeof(lt_action));
    lt_action_copy(&s->actions[s->n], a);
    s->n++;
    return LT_OK;
}

const lt_action_set *lt_arec_current(const lt_arec *r)
{
    return r->cur;
}

lt_bool lt_arec_is_recording(const lt_arec *r)
{
    return r->cur != NULL;
}

int lt_arec_add_tag(lt_arec *r, const char *tag)
{
    lt_action_meta *m;
    if (r->cur == NULL) {
        lt_err_set("No recording in progress");
        return LT_EINVAL;
    }
    m = &r->cur->meta;
    m->tags = (char **)lt_realloc(m->tags, (m->ntags + 1) * sizeof(char *));
    m->tags[m->ntags++] = lt_strdup(tag);
    return LT_OK;
}

int lt_arec_set_compatible_ecus(lt_arec *r, char **ecus, size_t n)
{
    lt_action_meta *m;
    size_t i;
    if (r->cur == NULL) {
        lt_err_set("No recording in progress");
        return LT_EINVAL;
    }
    m = &r->cur->meta;
    lt_proj_strv_free(m->compatible_ecus, m->necus);
    m->compatible_ecus = n ? (char **)lt_malloc(n * sizeof(char *)) : NULL;
    for (i = 0; i < n; i++) m->compatible_ecus[i] = lt_strdup(ecus[i]);
    m->necus = n;
    return LT_OK;
}

/* ---------------------------------------------------------------- */
/* validation                                                        */
/* ---------------------------------------------------------------- */
void lt_act_strv_free(char **v, size_t n)
{
    lt_proj_strv_free(v, n);
}

static void fmt_num(char *buf, size_t cap, double v)
{
    if (lt_isnan(v)) lt_strlcpy(buf, "NaN", cap);
    else if (!lt_isfinite(v)) lt_strlcpy(buf, v < 0 ? "-inf" : "inf", cap);
    else lt_fmt_f64_shortest(buf, cap, v);
}

static void push_msg(char ***v, size_t *n, const char *s)
{
    *v = (char **)lt_realloc(*v, (*n + 1) * sizeof(char *));
    (*v)[(*n)++] = lt_strdup(s);
}

static lt_bool has_param(const lt_action *a, const char *key)
{
    size_t i;
    for (i = 0; i < a->nparams; i++)
        if (strcmp(a->param_keys[i], key) == 0) return LT_TRUE;
    return LT_FALSE;
}

static double get_param(const lt_action *a, const char *key, double dflt)
{
    size_t i;
    for (i = 0; i < a->nparams; i++)
        if (strcmp(a->param_keys[i], key) == 0) return a->param_vals[i];
    return dflt;
}

static void validate_constant(const lt_ini_const *c, const lt_action *a, size_t idx, char ***errs,
                              size_t *ne)
{
    char msg[600], vs[40], ms[40];
    double v = a->new_value, lo = 0.0, hi = 0.0;
    const char *units = c->units ? c->units : "";
    fmt_num(vs, sizeof vs, v);
    if (c->range_resolved && v < c->min) {
        fmt_num(ms, sizeof ms, c->min);
        lt_snprintf(msg, sizeof msg, "Action %lu: Value %s for constant '%s' below minimum %s (%s)",
                    (unsigned long)idx, vs, c->name, ms, units);
        push_msg(errs, ne, msg);
    }
    if (c->range_resolved && v > c->max) {
        fmt_num(ms, sizeof ms, c->max);
        lt_snprintf(msg, sizeof msg, "Action %lu: Value %s for constant '%s' above maximum %s (%s)",
                    (unsigned long)idx, vs, c->name, ms, units);
        push_msg(errs, ne, msg);
    }
    {
        lt_bool have = LT_FALSE;
        if (c->type == LT_DT_BITS) {
            lo = 0;
            hi = 255;
            have = LT_TRUE;
        } else if (c->type != LT_DT_STRING) {
            have = lt_dtype_range(c->type, &lo, &hi);
        }
        if (have && fabs(c->scale) > DBL_MIN) {
            double raw = (v - c->translate) / c->scale;
            if (raw < lo || raw > hi) {
                lt_snprintf(msg, sizeof msg,
                            "Action %lu: Value %s (%s) for constant '%s' outside storable range "
                            "[raw %.0f..%.0f] for %s",
                            (unsigned long)idx, vs, units, c->name, lo, hi, lt_dtype_name(c->type));
                push_msg(errs, ne, msg);
            }
        }
    }
    if (c->n_bit_options > 0) {
        lt_bool ok = LT_FALSE;
        size_t k;
        for (k = 0; k < c->n_bit_options; k++)
            if (v == (double)k) ok = LT_TRUE;
        if (!ok) {
            lt_buf b;
            lt_buf_init(&b);
            for (k = 0; k < c->n_bit_options; k++) {
                if (k) lt_buf_puts(&b, ", ");
                lt_buf_puts(&b, c->bit_options[k] ? c->bit_options[k] : "");
            }
            lt_snprintf(msg, sizeof msg,
                        "Action %lu: Value %s for bits constant '%s' not one of %lu (%.200s)",
                        (unsigned long)idx, vs, c->name, (unsigned long)c->n_bit_options,
                        b.data ? b.data : "");
            lt_buf_free(&b);
            push_msg(errs, ne, msg);
        }
    }
}

int lt_action_validate(const lt_action_set *s, const lt_ini *ini, char ***warnings, size_t *nw,
                       char ***errors, size_t *ne)
{
    char **w = NULL, **e = NULL;
    size_t cw = 0, ce = 0, i, k;
    char msg[700], vs[40];

    for (i = 0; i < s->n; i++) {
        const lt_action *a = &s->actions[i];
        unsigned long ix = (unsigned long)i;
        const lt_ini_table *tb = NULL;
        switch (a->type) {
        case LT_ACT_TABLE_EDIT:
            if (!a->table_name || !a->table_name[0]) {
                lt_snprintf(msg, sizeof msg, "Action %lu: Empty table name", ix);
                push_msg(&e, &ce, msg);
            }
            if (!lt_isfinite(a->new_value)) {
                fmt_num(vs, sizeof vs, a->new_value);
                lt_snprintf(msg, sizeof msg, "Action %lu: Invalid value %s", ix, vs);
                push_msg(&e, &ce, msg);
            }
            if (ini != NULL && a->table_name && a->table_name[0]) {
                tb = lt_ini_find_table(ini, a->table_name);
                if (tb == NULL) {
                    lt_snprintf(msg, sizeof msg,
                                "Action %lu: Table '%s' not found in ECU definition", ix,
                                a->table_name);
                    push_msg(&e, &ce, msg);
                } else {
                    if (a->x_index >= tb->x_size) {
                        lt_snprintf(msg, sizeof msg,
                                    "Action %lu: x_index %u out of bounds for table '%s' (width %lu)",
                                    ix, (unsigned)a->x_index, a->table_name,
                                    (unsigned long)tb->x_size);
                        push_msg(&e, &ce, msg);
                    }
                    if (a->y_index >= tb->y_size) {
                        lt_snprintf(msg, sizeof msg,
                                    "Action %lu: y_index %u out of bounds for table '%s' (height "
                                    "%lu)", ix, (unsigned)a->y_index, a->table_name,
                                    (unsigned long)tb->y_size);
                        push_msg(&e, &ce, msg);
                    }
                }
            }
            break;
        case LT_ACT_CONSTANT_CHANGE:
            if (!a->constant_name || !a->constant_name[0]) {
                lt_snprintf(msg, sizeof msg, "Action %lu: Empty constant name", ix);
                push_msg(&e, &ce, msg);
            }
            if (!lt_isfinite(a->new_value)) {
                fmt_num(vs, sizeof vs, a->new_value);
                lt_snprintf(msg, sizeof msg, "Action %lu: Invalid value %s", ix, vs);
                push_msg(&e, &ce, msg);
            }
            if (ini != NULL && a->constant_name && a->constant_name[0]) {
                const lt_ini_const *c = lt_ini_find_const(ini, a->constant_name);
                if (c == NULL) {
                    lt_snprintf(msg, sizeof msg,
                                "Action %lu: Constant '%s' not found in ECU definition", ix,
                                a->constant_name);
                    push_msg(&e, &ce, msg);
                } else {
                    validate_constant(c, a, i, &e, &ce);
                }
            }
            break;
        case LT_ACT_BULK:
            if (!a->table_name || !a->table_name[0]) {
                lt_snprintf(msg, sizeof msg, "Action %lu: Empty table name", ix);
                push_msg(&e, &ce, msg);
            }
            if (a->ncells == 0) {
                lt_snprintf(msg, sizeof msg, "Action %lu: No cells selected for %s", ix,
                            a->operation ? a->operation : "");
                push_msg(&w, &cw, msg);
            }
            if (ini != NULL && a->table_name && a->table_name[0]) {
                tb = lt_ini_find_table(ini, a->table_name);
                if (tb == NULL) {
                    lt_snprintf(msg, sizeof msg,
                                "Action %lu: Table '%s' not found in ECU definition", ix,
                                a->table_name);
                    push_msg(&e, &ce, msg);
                } else {
                    for (k = 0; k < a->ncells; k++) {
                        if (a->cells[k].x >= tb->x_size) {
                            lt_snprintf(msg, sizeof msg,
                                        "Action %lu: cell x=%u out of bounds for table '%s' (width "
                                        "%lu)", ix, (unsigned)a->cells[k].x, a->table_name,
                                        (unsigned long)tb->x_size);
                            push_msg(&e, &ce, msg);
                        }
                        if (a->cells[k].y >= tb->y_size) {
                            lt_snprintf(msg, sizeof msg,
                                        "Action %lu: cell y=%u out of bounds for table '%s' "
                                        "(height %lu)", ix, (unsigned)a->cells[k].y, a->table_name,
                                        (unsigned long)tb->y_size);
                            push_msg(&e, &ce, msg);
                        }
                    }
                }
            }
            if (a->operation && strcmp(a->operation, "scale") == 0 && !has_param(a, "factor")) {
                lt_snprintf(msg, sizeof msg, "Action %lu: scale missing 'factor' parameter", ix);
                push_msg(&e, &ce, msg);
            }
            if (a->operation && strcmp(a->operation, "rebin") == 0 &&
                (!has_param(a, "x_bins") || !has_param(a, "y_bins"))) {
                lt_snprintf(msg, sizeof msg, "Action %lu: rebin missing bin parameters", ix);
                push_msg(&e, &ce, msg);
            }
            break;
        case LT_ACT_LUA:
            if (!a->script || !a->script[0]) {
                lt_snprintf(msg, sizeof msg, "Action %lu: Empty Lua script", ix);
                push_msg(&e, &ce, msg);
            }
            break;
        case LT_ACT_PAUSE:
            if (a->duration_ms == 0) {
                lt_snprintf(msg, sizeof msg, "Action %lu: Zero-duration pause", ix);
                push_msg(&w, &cw, msg);
            }
            break;
        case LT_ACT_SEND_CMD:
            if (!a->command || !a->command[0]) {
                lt_snprintf(msg, sizeof msg, "Action %lu: Empty command", ix);
                push_msg(&e, &ce, msg);
            } else if (ini != NULL && lt_ini_find_command(ini, a->command) == NULL) {
                lt_snprintf(msg, sizeof msg,
                            "Action %lu: Command '%s' not supported by ECU definition", ix,
                            a->command);
                push_msg(&e, &ce, msg);
            }
            break;
        }
    }
    if (ce > 0) {
        /* warnings are dropped when there are errors (spec) */
        lt_proj_strv_free(w, cw);
        w = NULL;
        cw = 0;
    }
    if (warnings) *warnings = w;
    else lt_proj_strv_free(w, cw);
    if (nw) *nw = cw;
    if (errors) *errors = e;
    else lt_proj_strv_free(e, ce);
    if (ne) *ne = ce;
    return ce > 0 ? LT_EINVAL : LT_OK;
}

char *lt_action_summarize(const lt_action_set *s)
{
    lt_buf b;
    size_t counts[6] = {0, 0, 0, 0, 0, 0}, i;
    lt_buf_init(&b);
    lt_buf_printf(&b, "Action Set: %s\nActions: %lu\n", s->name ? s->name : "",
                  (unsigned long)s->n);
    for (i = 0; i < s->n; i++) counts[s->actions[i].type]++;
    for (i = 0; i < 6; i++)
        if (counts[i] > 0) lt_buf_printf(&b, "  - %s: %lu\n", type_names[i], (unsigned long)counts[i]);
    return lt_buf_take(&b);
}

/* ---------------------------------------------------------------- */
/* replay                                                            */
/* ---------------------------------------------------------------- */
void lt_replay_result_free(lt_replay_result *r)
{
    lt_proj_strv_free(r->warnings, r->n_warnings);
    memset(r, 0, sizeof *r);
}

static void rwarn(lt_replay_result *r, const char *msg)
{
    r->warnings = (char **)lt_realloc(r->warnings, (r->n_warnings + 1) * sizeof(char *));
    r->warnings[r->n_warnings++] = lt_strdup(msg);
}

/* make sure the page holding `c` exists in the tune image */
static int ensure_const_page(lt_replay_env *env, const lt_ini_const *c)
{
    if (c->is_pc_var) return LT_OK;
    if (env->tune->pages[c->page] == NULL) lt_tune_apply_to_pages(env->tune, env->ini);
    if (env->tune->pages[c->page] == NULL) {
        lt_err_set("page %d is not available in the tune", (int)c->page);
        return LT_ENOENT;
    }
    return LT_OK;
}

static int load_view(lt_replay_env *env, const lt_ini_table *t, lt_tblview *v)
{
    const lt_ini_const *z = lt_ini_find_const(env->ini, t->z_const);
    lt_pagesrc src;
    int rc;
    if (z == NULL) {
        lt_err_set("table '%s' has no cell constant", t->name);
        return LT_ENOENT;
    }
    rc = ensure_const_page(env, z);
    if (rc != LT_OK) return rc;
    src = lt_tune_pagesrc(env->tune);
    lt_tblview_init(v);
    rc = lt_tblview_load_table(v, env->ini, t, &src);
    if (rc != 0) {
        lt_tblview_free(v);
        return rc < 0 ? rc : LT_EDATA;
    }
    return LT_OK;
}

static int exec_table_edit(lt_replay_env *env, const lt_action *a)
{
    const lt_ini_table *t = lt_ini_find_table(env->ini, a->table_name);
    lt_tblview v;
    lt_pagesrc dst;
    int rc;
    if (t == NULL) return LT_ENOENT;
    rc = load_view(env, t, &v);
    if (rc != LT_OK) return rc;
    if (a->y_index >= v.rows || a->x_index >= v.cols) {
        lt_tblview_free(&v);
        lt_err_set("cell (%u,%u) is outside the active table size", (unsigned)a->x_index,
                   (unsigned)a->y_index);
        return LT_ERANGE;
    }
    lt_tblview_set(&v, a->y_index, a->x_index, a->new_value);
    dst = lt_tune_pagesrc(env->tune);
    rc = lt_tblview_commit(&v, env->ini, &dst);
    lt_tblview_free(&v);
    return rc < 0 ? rc : LT_OK;
}

static int exec_bulk(lt_replay_env *env, const lt_action *a, lt_replay_result *res, int *applied)
{
    const lt_ini_table *t = lt_ini_find_table(env->ini, a->table_name);
    lt_tblview v;
    lt_pagesrc dst;
    lt_tcell *sel;
    size_t i;
    int rc;
    const char *op = a->operation ? a->operation : "";

    *applied = 1;
    if (t == NULL) return LT_ENOENT;
    if (strcmp(op, "rebin") == 0) {
        rwarn(res, "rebin replay is not supported; action skipped");
        *applied = 0;
        return LT_OK;
    }
    if (strcmp(op, "scale") != 0 && strcmp(op, "smooth") != 0 && strcmp(op, "interpolate") != 0 &&
        strcmp(op, "set_equal") != 0 && strcmp(op, "offset") != 0) {
        lt_err_set("unknown bulk operation '%s'", op);
        return LT_EINVAL;
    }
    rc = load_view(env, t, &v);
    if (rc != LT_OK) return rc;
    sel = (lt_tcell *)lt_malloc((a->ncells ? a->ncells : 1) * sizeof(lt_tcell));
    for (i = 0; i < a->ncells; i++) {
        if (a->cells[i].y >= v.rows || a->cells[i].x >= v.cols) {
            lt_err_set("cell (%u,%u) is outside the active table size", (unsigned)a->cells[i].x,
                       (unsigned)a->cells[i].y);
            lt_free(sel);
            lt_tblview_free(&v);
            return LT_ERANGE;
        }
        sel[i].y = a->cells[i].y;
        sel[i].x = a->cells[i].x;
    }
    if (strcmp(op, "scale") == 0) {
        lt_tblview_scale(&v, sel, a->ncells, get_param(a, "factor", 1.0));
    } else if (strcmp(op, "offset") == 0) {
        lt_tblview_offset(&v, sel, a->ncells, get_param(a, "offset", 0.0));
    } else if (strcmp(op, "set_equal") == 0) {
        lt_tblview_set_equal(&v, sel, a->ncells, get_param(a, "value", 0.0));
    } else if (strcmp(op, "smooth") == 0) {
        lt_tblview_smooth(&v, sel, a->ncells, get_param(a, "factor", 1.0));
    } else if (has_param(a, "axis")) {
        lt_tblview_interp_linear(&v, sel, a->ncells, get_param(a, "axis", 0.0) != 0.0 ? 1 : 0);
    } else {
        lt_tblview_interp2d(&v, sel, a->ncells);
    }
    lt_free(sel);
    dst = lt_tune_pagesrc(env->tune);
    rc = lt_tblview_commit(&v, env->ini, &dst);
    lt_tblview_free(&v);
    return rc < 0 ? rc : LT_OK;
}

static int exec_constant(lt_replay_env *env, const lt_action *a)
{
    const lt_ini_const *c = lt_ini_find_const(env->ini, a->constant_name);
    lt_pagesrc src;
    int rc;
    if (c == NULL) return LT_ENOENT;
    if (c->is_pc_var) {
        lt_tune_value tv;
        lt_tune_value_init(&tv);
        tv.kind = LT_TV_SCALAR;
        tv.scalar = a->new_value;
        lt_tune_set_pc_variable(env->tune, c->name, &tv);
        lt_tune_value_clear(&tv);
        return LT_OK;
    }
    rc = ensure_const_page(env, c);
    if (rc != LT_OK) return rc;
    src = lt_tune_pagesrc(env->tune);
    rc = lt_ini_set(env->ini, &src, a->constant_name, a->new_value);
    return rc < 0 ? rc : LT_OK;
}

int lt_action_replay(const lt_action_set *s, lt_replay_env *env, lt_replay_result *res)
{
    lt_replay_result local;
    char **errs = NULL, **warns = NULL;
    size_t ne = 0, nw = 0, i;
    int rc = LT_OK;

    if (res == NULL) res = &local;
    memset(res, 0, sizeof *res);
    if (env == NULL || env->tune == NULL) {
        lt_err_set("replay needs a tune");
        return LT_EINVAL;
    }
    if (lt_action_validate(s, env->ini, &warns, &nw, &errs, &ne) != LT_OK) {
        lt_err_set("%s", errs[0]);
        lt_proj_strv_free(errs, ne);
        lt_proj_strv_free(warns, nw);
        return LT_EINVAL;
    }
    for (i = 0; i < nw; i++) rwarn(res, warns[i]);
    lt_proj_strv_free(warns, nw);

    for (i = 0; i < s->n && rc == LT_OK; i++) {
        const lt_action *a = &s->actions[i];
        int applied = 1;
        char msg[300];
        switch (a->type) {
        case LT_ACT_TABLE_EDIT:
            if (env->ini == NULL) {
                lt_err_set("table edits need an ECU definition");
                rc = LT_EINVAL;
            } else {
                rc = exec_table_edit(env, a);
            }
            break;
        case LT_ACT_CONSTANT_CHANGE:
            if (env->ini == NULL) {
                lt_err_set("constant changes need an ECU definition");
                rc = LT_EINVAL;
            } else {
                rc = exec_constant(env, a);
            }
            break;
        case LT_ACT_BULK:
            if (env->ini == NULL) {
                lt_err_set("bulk operations need an ECU definition");
                rc = LT_EINVAL;
            } else {
                rc = exec_bulk(env, a, res, &applied);
            }
            break;
        case LT_ACT_LUA:
            lt_snprintf(msg, sizeof msg, "Action %lu: Lua scripts are not executed", (unsigned long)i);
            rwarn(res, msg);
            applied = 0;
            break;
        case LT_ACT_PAUSE:
            if (env->sleep_ms) env->sleep_ms(env->ud, a->duration_ms);
            else lt_plat_sleep_ms((int)a->duration_ms);
            break;
        case LT_ACT_SEND_CMD:
            if (env->send_command) {
                rc = env->send_command(env->ud, a->command);
            } else {
                lt_snprintf(msg, sizeof msg, "Action %lu: no command channel, '%.100s' skipped",
                            (unsigned long)i, a->command);
                rwarn(res, msg);
                applied = 0;
            }
            break;
        }
        if (rc != LT_OK) {
            lt_snprintf(msg, sizeof msg, "Action %lu failed: %.200s", (unsigned long)i,
                        lt_err_get());
            lt_err_set("%s", msg);
        } else if (applied) {
            res->applied++;
        } else {
            res->skipped++;
        }
    }
    if (env->ini != NULL && res->applied > 0) lt_tune_capture_from_pages(env->tune, env->ini);
    if (res == &local) lt_replay_result_free(&local);
    return rc;
}
