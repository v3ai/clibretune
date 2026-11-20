/* ini_priv.h - internal helpers shared by the src/ini/ .c files (not public API).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_INI_PRIV_H
#define LT_INI_PRIV_H

#include "ini.h"

/* ---- text helpers (ini_text.c) ---- */
/* Number of leading bytes of `line` kept after comment stripping (spec 01 2.2). */
size_t ini_comment_cut(const char *line);
/* split_ini_line (quote + brace aware) / naive ',' split. Parts are trimmed,
 * malloc'd; always >= 1 part. Free with ini_strv_free. */
size_t ini_split(const char *v, char ***parts);
size_t ini_split_naive(const char *v, char ***parts);
void   ini_strv_free(char **p, size_t n);
/* trim, then strip ALL leading/trailing '"' (malloc'd) */
char  *ini_dequote(const char *s);
/* strip all leading/trailing '{' '}' chars; trim afterwards if do_trim (malloc'd) */
char  *ini_unbrace(const char *s, int do_trim);
lt_bool ini_is_braced(const char *s);
/* menu/panel condition: unbrace+trim, NULL if empty or "0" or "1" */
char  *ini_cond(const char *s);
/* "{ expr }" -> trimmed inner or NULL when not a non-empty braced text */
char  *ini_deferred_expr(const char *s);

lt_bool ini_parse_uint(const char *s, unsigned long *out);   /* decimal, whole string */
lt_bool ini_parse_f64(const char *s, double *out);
int     ini_parse_bool(const char *s);                       /* cut at ';', true/1/on/yes */
/* value up to (excluding) the first ';', trimmed (malloc'd) */
char  *ini_clean(const char *s);

lt_ini_blob ini_blob_new(const char *p, size_t n);
lt_ini_blob ini_blob_from_str(const char *s);
void        ini_blob_free(lt_ini_blob *b);
/* \xNN, \0, \n, \r, \\ decoding (spec 01 5.3); result NUL-terminated blob */
lt_ini_blob ini_parse_escape(const char *s, size_t n);
/* $name / \$name -> one byte from pc variables map (name -> (size_t)(byte+1)) */
lt_ini_blob ini_substitute(const char *s, size_t n, const lt_map *pcvars);

int ini_parse_shape(const char *s, lt_shape *sh, lt_dynsize **dyn_out);
lt_bool ini_parse_bit_range(const char *s, int *pos, int *size, int *disp);

/* ---- parser output helpers (ini_parse.c, ini_post.c) ---- */
void ini_free_contents(lt_ini *ini);
void ini_post_process(lt_ini *ini, const char *filename_hint);
void ini_free_const(lt_ini_const *c);
void ini_free_channel(lt_ini_channel *c);
void ini_free_table(lt_ini_table *t);
void ini_free_curve(lt_ini_curve *c);
void ini_free_dialog(lt_ini_dialog *d);
void ini_free_menu_item(lt_ini_menu_item *m);
void ini_free_cmd(lt_ini_cmd *c);
void ini_free_help(void *h);
void ini_free_ref_table(lt_ini_ref_table *t);
void ini_free_analyze(lt_ini_analyze *a);
void ini_free_logger(lt_ini_logger *l);
void ini_free_gauge(lt_ini_gauge *g);
/* void * adapters: callbacks must have the exact pointer type (no UB casts) */
void ini_freev_const(void *p);
void ini_freev_channel(void *p);
void ini_freev_table(void *p);
void ini_freev_curve(void *p);
void ini_freev_dialog(void *p);
void ini_freev_cmd(void *p);
void ini_freev_gauge(void *p);
void ini_free_cmdlist(lt_ini_cmdlist *l);
void ini_free_blob_array(lt_ini_blob *v, size_t n);
void ini_free_str_array(char **v, size_t n);

/* parse the raw [LoggerDefinition] lines (block form) */
void ini_parse_loggers(lt_ini *ini, const char *raw_lines);

/* gauge line parser (ini_misc.c) */
lt_ini_gauge *ini_parse_gauge_line(const char *name, const char *value);
lt_ini_channel *ini_parse_channel_line(const char *name, const char *value);
lt_ini_const *ini_parse_constant_line(const char *name, const char *value, int page,
                                      unsigned long last_off, const char *help,
                                      const lt_map *defines);
lt_ini_const *ini_parse_pcvar_line(const char *name, const char *value, const char *help,
                                   const lt_map *defines);

/* ---- parser context ---- */
typedef struct ini_strlist { char **v; size_t n; } ini_strlist;
typedef struct ini_limit { unsigned long id; double lo, hi, dflt; } ini_limit;

/* per-file parser state (fresh for every #include) */
typedef struct ini_pstate {
    char *section;                 /* lower-case current section ("" = none/suppressed) */
    int page;                      /* 0-based */
    unsigned long last_offset;
    lt_ini_table *table;
    lt_ini_curve *curve;
    lt_ini_dialog *dialog;
    lt_ini_indicator_panel *ipanel;
    lt_ini_readout_panel *rpanel;
    lt_ini_help *help;
    lt_ini_ref_table *ref;
    lt_ini_setting_group *group;
    lt_vec limits;                 /* ini_limit: pending [ReferenceTables] tableLimits */
    char *category;                /* last gaugeCategory */
} ini_pstate;

/* shared across the root file and its includes (stored in lt_ini.priv) */
typedef struct ini_ctx {
    lt_map defines;                /* name -> ini_strlist* */
    lt_buf logger_raw;             /* raw [LoggerDefinition] lines */
    lt_map included;               /* normalised paths already seen */
    lt_vec std_panels;             /* lt_ini_dialog* synthesised lazily */
    lt_map managed;                /* symbols whose #set/#unset lines are ignored (options) */
} ini_ctx;

#define INI_MAX_INCLUDE_DEPTH 16

/* generic ordered-array + name index insert; replaces an existing element of the
 * same name (old one freed with freefn). Returns the stored pointer. */
void *ini_put(void ***arr, size_t *n, lt_map *idx, const char *name, void *elem,
              void (*freefn)(void *));
/* append a zeroed element slot to a realloc'd array of `elem` byte structs */
void *ini_push(void **arr, size_t *n, size_t elem);
void  ini_str_push(char ***arr, size_t *n, char *s);   /* takes ownership of s */
void  ini_set_str(char **dst, char *val);               /* frees *dst, takes val */

/* section handlers outside ini_parse.c (ini_sect.c) */
void ini_section_line(lt_ini *ini, ini_ctx *cx, ini_pstate *st, const char *key,
                      const char *value);
void ini_flush_limits(lt_ini *ini, ini_pstate *st);
int  ini_proto_key(lt_ini *ini, const char *key, const char *value);
void ini_pstate_free(ini_pstate *st);
void ini_resolve_pcvars(lt_ini *ini);

#endif /* LT_INI_PRIV_H */
