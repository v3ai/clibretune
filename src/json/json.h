/* json.h - tiny JSON DOM (parse + write). Not in the original module list but
 * shared by project.json, ini repository index.json, math channels, props
 * tools: one implementation instead of three.
 *
 * Numbers are doubles. Object keys keep insertion order. Parse accepts
 * standard JSON (RFC 8259) and ignores unknown keys at the caller's level.
 * Strings: \" \\ \/ \b \f \n \r \t \uXXXX (surrogate pairs combined) -> UTF-8.
 *
 * MEMORY: lt_json_free(root) frees the whole tree. lt_json_obj_set/arr_push
 * take ownership of the child value.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_JSON_H
#define LT_JSON_H

#include "../lt.h"
#include "../lt_codes.h"

typedef enum lt_json_type {
    LT_JSON_NULL = 0, LT_JSON_BOOL, LT_JSON_NUM, LT_JSON_STR, LT_JSON_ARR, LT_JSON_OBJ
} lt_json_type;

typedef struct lt_json {
    lt_json_type type;
    double  num;
    int     boolean;
    char   *str;                  /* STR */
    struct lt_json **items;       /* ARR elements / OBJ values */
    char  **keys;                 /* OBJ keys (parallel to items) */
    size_t  n;
    size_t  cap;
} lt_json;

/* parse: NULL + err message on syntax error (trailing garbage is an error) */
lt_json *lt_json_parse(const char *text, size_t len, char *err, size_t errsz);
void     lt_json_free(lt_json *j);

/* constructors */
lt_json *lt_json_null(void);
lt_json *lt_json_bool(int b);
lt_json *lt_json_num(double v);
lt_json *lt_json_str(const char *s);      /* copies; NULL -> JSON null */
lt_json *lt_json_arr(void);
lt_json *lt_json_obj(void);
void     lt_json_obj_set(lt_json *obj, const char *key, lt_json *val);  /* replaces existing key */
void     lt_json_arr_push(lt_json *arr, lt_json *val);

/* accessors (tolerant: wrong type or missing -> default) */
const lt_json *lt_json_get(const lt_json *obj, const char *key);        /* NULL if absent */
const char *lt_json_get_str(const lt_json *obj, const char *key, const char *dflt);
double      lt_json_get_num(const lt_json *obj, const char *key, double dflt);
int         lt_json_get_bool(const lt_json *obj, const char *key, int dflt);
const lt_json *lt_json_at(const lt_json *arr, size_t i);                 /* NULL if out of range */

/* serialise. pretty: 2-space indent, "key": value (serde_json::to_string_pretty
 * layout; empty arrays/objects as [] / {}). Numbers: integers without ".0",
 * else shortest round-trip form. */
void  lt_json_write(const lt_json *j, int pretty, lt_buf *out);
char *lt_json_to_string(const lt_json *j, int pretty);                  /* malloc'd */
/* append a JSON string literal (quotes + escapes) for s */
void  lt_json_put_string(lt_buf *out, const char *s);
/* read/write a whole JSON file */
lt_json *lt_json_load(const char *path, char *err, size_t errsz);
int      lt_json_save(const lt_json *j, const char *path, int pretty);

#endif /* LT_JSON_H */
