/* tune.h - tune files (.msq), tune diff/merge, INI migration, .table, .tuneView,
 *          and the variable environment that feeds INI expressions.
 *
 * Spec: docs/spec/04-tune-files.md. XML parsing is done with xml/xml.h.
 *
 * A lt_tune holds BOTH representations a .msq can carry:
 *   - raw page images (pageData) - what the ECU actually stores
 *   - named constant values (<constant>) - what TunerStudio shows
 * lt_tune_apply_to_pages() / lt_tune_capture_from_pages() convert between them
 * with the INI (the only place that knows offsets/scales). The editing code
 * works on page images through lt_pagesrc (lt_tune_pagesrc below, or
 * lt_ecu_mem from proto.h when connected); the NAMED values are only used for
 * file IO. The "TuneCache" of spec 04 section 2 (page states + dirty bytes)
 * is lt_ecu_mem in proto.h - do not implement a second one here.
 *
 * Differences from the spec text (deliberate, to keep the struct small):
 *   - constants/pcVariables each live in ONE map name -> lt_tune_entry carrying
 *     value, page and attrs together (spec keeps three parallel maps).
 *   - annotations and the JSON tune format are not implemented.
 *
 * MEMORY: lt_tune owns everything; lt_tune_free(). Entries returned by
 * lt_tune_get* point into the tune and stay valid until that name is replaced
 * or the tune freed.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_TUNE_H
#define LT_TUNE_H

#include "../lt.h"
#include "../lt_codes.h"
#include "../ini/ini.h"
#include "../ini/expr.h"
#include "../xml/xml.h"

/* ====================================================================== */
/* Values                                                                  */
/* ====================================================================== */
typedef enum lt_tv_kind { LT_TV_SCALAR = 0, LT_TV_ARRAY, LT_TV_STRING, LT_TV_BOOL } lt_tv_kind;
typedef struct lt_tune_value {
    lt_tv_kind kind;
    double  scalar;
    double *arr; size_t n;     /* ARRAY: flat, row-major */
    char   *str;               /* STRING (never NULL for STRING kind) */
    int     boolean;
} lt_tune_value;
void lt_tune_value_init(lt_tune_value *v);             /* SCALAR 0 */
void lt_tune_value_clear(lt_tune_value *v);            /* frees arr/str */
void lt_tune_value_copy(lt_tune_value *dst, const lt_tune_value *src);   /* dst is overwritten (not freed) */
lt_bool lt_tune_value_equal(const lt_tune_value *a, const lt_tune_value *b);   /* tolerance 1e-9 */
/* Parse the text of a <constant> element (spec 1.3 parse_value): quoted -> string, numbers,
 * arrays "1 2 3" / "[1, 2]", booleans. */
void lt_tune_value_parse(lt_tune_value *out, const char *text);
/* Shortest round-trip decimal without exponent ("14.7", "1", "-0.3"; 0 for 0/NaN/inf). */
void lt_fmt_f64_shortest(char *dst, size_t cap, double v);

typedef struct lt_const_attrs { char *digits, *units, *rows, *cols; } lt_const_attrs;   /* NULL = absent */

typedef struct lt_tune_entry {
    lt_tune_value value;
    lt_u8 page;                       /* constants only (pc variables: unused) */
    lt_const_attrs attrs;
    lt_bool has_attrs;
} lt_tune_entry;

typedef struct lt_manifest_entry {    /* constantManifest: what the INI looked like at save time */
    char *name, *type; lt_u8 page; lt_u32 offset; double scale, translate;
} lt_manifest_entry;
typedef struct lt_ini_metadata { char *signature, *name, *hash, *spec_version, *saved_at; } lt_ini_metadata;

typedef struct lt_tune {
    char *version;                  /* "1.1" */
    char *signature;                /* "" if unknown */
    char *author, *description, *created, *modified;   /* NULL = none (free-form strings) */
    char *firmware_info;
    lt_u8   *pages[256];            /* raw page images (NULL = none) */
    size_t   page_len[256];
    char    *page_sizes[256];       /* declared size text from <page number size> */
    lt_map   constants;             /* name -> lt_tune_entry* */
    lt_map   pc_variables;          /* name -> lt_tune_entry* */
    char   **ecu_settings; size_t n_ecu_settings;      /* <settings><setting name> in order */
    lt_ini_metadata *ini_metadata;  /* NULL = none */
    lt_manifest_entry *manifest; size_t n_manifest; lt_bool has_manifest;
    char   **sorted_names; size_t n_sorted;   /* private: cache for lt_tune_constant_name */
} lt_tune;

/* ====================================================================== */
/* Lifecycle, load, save                                                   */
/* ====================================================================== */
lt_tune *lt_tune_new(const char *signature);     /* version 1.1, created=modified=now (RFC3339) */
void     lt_tune_free(lt_tune *t);
lt_tune *lt_tune_clone(const lt_tune *t);
/* .msq from disk / memory. Bytes: UTF-8 else Latin-1. Returns NULL + lt_err ("Not a
 * valid MSQ file" when no constants and no signature). Runs sanitize (spec 1.6). */
lt_tune *lt_tune_load(const char *path);
lt_tune *lt_tune_parse_msq(const char *data, size_t len);
/* Serialise in the exact deterministic layout of spec 1.4 (sorted keys). */
void     lt_tune_to_msq(const lt_tune *t, lt_buf *out);
int      lt_tune_save(const lt_tune *t, const char *path);
void     lt_tune_touch(lt_tune *t);              /* modified = now */
int      lt_tune_sanitize(lt_tune *t);           /* returns number of corrupted strings cleared */

/* ---- pages ---- */
void          lt_tune_set_page(lt_tune *t, int page, const lt_u8 *data, size_t len);
const lt_u8  *lt_tune_get_page(const lt_tune *t, int page, size_t *len);
/* no-op if page absent or range exceeds the page (never grows) */
int           lt_tune_patch_page(lt_tune *t, int page, size_t off, const lt_u8 *data, size_t n);
/* Page source over the tune's own images (writes patch the image; no dirty tracking). */
lt_pagesrc    lt_tune_pagesrc(lt_tune *t);

/* ---- named values ---- */
void           lt_tune_set_constant(lt_tune *t, const char *name, const lt_tune_value *v, int page);   /* copies */
const lt_tune_entry *lt_tune_get_constant(const lt_tune *t, const char *name);
void           lt_tune_set_pc_variable(lt_tune *t, const char *name, const lt_tune_value *v);
const lt_tune_entry *lt_tune_get_pc_variable(const lt_tune *t, const char *name);
const lt_tune_entry *lt_tune_get_value(const lt_tune *t, const char *name);   /* constants, then pc vars */
size_t         lt_tune_n_constants(const lt_tune *t);
/* Iterate sorted by name (bytewise): i in [0, n). */
const char    *lt_tune_constant_name(const lt_tune *t, size_t i);
void           lt_tune_set_signature(lt_tune *t, const char *sig);

/* ---- bridge between page images and named values (needs the INI) ---- */
/* Encode every named constant that the INI knows into the page images (creating
 * zeroed pages of the INI's declared size if absent). PC variables are not
 * paged. Strings with option labels ("Speed Density") are mapped to indexes.
 * Returns the number of constants applied. */
int  lt_tune_apply_to_pages(lt_tune *t, const lt_ini *ini);
/* Decode all non-PC INI constants from the page images into named values (and
 * fill ini_metadata / manifest from the INI). Returns count. */
int  lt_tune_capture_from_pages(lt_tune *t, const lt_ini *ini);
/* Fill ini_metadata + constantManifest from the INI (call before saving). */
void lt_tune_stamp_ini(lt_tune *t, const lt_ini *ini, const char *ini_hash);
/* Load page images into an ECU memory shadow as CLEAN pages (offline editing). */
struct lt_ecu_mem;
int  lt_tune_to_mem(const lt_tune *t, struct lt_ecu_mem *mem);
/* Copy loaded pages of `mem` into the tune (before save). */
int  lt_tune_from_mem(lt_tune *t, const struct lt_ecu_mem *mem);

/* New tune for `ini`: zeroed pages of the declared sizes, every constant that has a
 * [Defaults] entry set to it, named values captured, pc variable defaults filled. */
lt_tune *lt_tune_from_defaults(const lt_ini *ini);

/* PC-variable side file projectCfg/pcVariableValues.msq (spec 1.8). */
int  lt_tune_save_pc_variables(const lt_tune *t, const char *path, const char *signature);
int  lt_tune_load_pc_variables(lt_tune *t, const char *path);

/* ====================================================================== */
/* Variable environment for expressions (layering of spec 02 section 1.10) */
/* ====================================================================== */
/* One resolver for ALL { ... } expressions in the UI. Names resolve in this
 * order (first hit wins):  realtime channels (+computed)  >  pc variables of
 * `pcvars` tune  >  constants decoded from `pages`  >  [Defaults].
 * Constant mapping: scalar -> display value; bits -> option index; array ->
 * element 0; string constants are available through get_string; bit option
 * lists through get_bit_options. */
typedef struct lt_varenv {
    const lt_ini *ini;
    lt_pagesrc    pages;           /* ECU memory or tune pages (may be all NULL) */
    lt_ini_rt    *rt;              /* may be NULL (offline) */
    const lt_tune *pcvars;         /* may be NULL */
    lt_inc_cache *inc;             /* .inc tables for table(); may be NULL */
    lt_bool       online;
    double        t0_ms;           /* session start for timeNow() */
    lt_expr_env   env;             /* full resolver (channels first); pass &v.env to lt_expr_* */
    lt_expr_env   consts;          /* same without channels: use as lt_ini_rt_set_parent(rt, &v.consts)
                                      so computed channels can see constants without recursion */
    char          scratch[256];    /* internal */
} lt_varenv;
void lt_varenv_init(lt_varenv *v, const lt_ini *ini, const lt_pagesrc *pages, lt_ini_rt *rt,
                    const lt_tune *pcvars);

/* ====================================================================== */
/* Diff / merge (spec 3)                                                   */
/* ====================================================================== */
typedef struct lt_tune_difference {
    char *name;
    lt_tune_value *value_a, *value_b;       /* NULL = absent on that side */
    lt_bool has_numeric; double numeric_diff;
    lt_bool has_percent; double percent_diff;
} lt_tune_difference;
typedef struct lt_tune_diff {
    lt_tune_difference *diffs; size_t n;    /* sorted by name */
    lt_bool signature_match; char *sig_a, *sig_b;
    size_t sum_a, sum_b;
} lt_tune_diff;
lt_tune_diff *lt_tune_diff_compare(const lt_tune *a, const lt_tune *b);
void          lt_tune_diff_free(lt_tune_diff *d);
lt_bool       lt_tune_diff_identical(const lt_tune_diff *d);
size_t        lt_tune_merge_selected(lt_tune *target, const lt_tune *source, const char *const *names,
                                     size_t n);

/* ====================================================================== */
/* Partial tunes (.msqpart)                                                */
/* ====================================================================== */
/* A partial tune is an ordinary lt_tune holding only some constants (no page images, no
 * manifest). lt_tune_save writes it in the layout TunerStudio uses for .msqpart files: the
 * usual <msq> root with <versionInfo>/<bibliography>, then <page number="N"> blocks carrying just
 * the chosen <constant> elements (pc variables in <page number="-1">). lt_tune_load reads one.
 *
 * One constant against a page source (ECU shadow or tune pages): write the value (0, or
 * LT_EINVAL for a PC variable / bad argument, LT_EDATA when it cannot be encoded), read the
 * current value (LT_FALSE when the page is not available). */
int      lt_tune_write_const(const lt_ini *ini, const lt_ini_const *c, lt_pagesrc *ps, const lt_tune_value *v);
lt_bool  lt_tune_read_const(const lt_ini *ini, const lt_ini_const *c, const lt_pagesrc *ps, lt_tune_value *out);
/* new partial tune with the named constants / pc variables of src (names src lacks are skipped) */
lt_tune *lt_tune_part_extract(const lt_tune *src, const char *const *names, size_t n);
/* new partial tune with the CURRENT values of the named INI constants read from ps (pc variables
 * from pcvars, may be NULL); names the INI does not know are skipped */
lt_tune *lt_tune_part_capture(const lt_ini *ini, const lt_pagesrc *ps, const lt_tune *pcvars,
                              const char *const *names, size_t n);
/* constants of cur whose value differs from ref or that ref lacks: sorted malloc'd names
 * (free each and the array); returns the count */
size_t   lt_tune_part_diff_names(const lt_tune *cur, const lt_tune *ref, char ***names_out);
/* constants edited through an INI table, curve or dialog (embedded panels and tables followed)
 * added to set (key = name, value = non-NULL); returns how many were added */
size_t   lt_tune_part_target_consts(const lt_ini *ini, const char *target, lt_map *set);
/* every constant and pc variable name of a partial tune, sorted (malloc'd, as above) */
size_t   lt_tune_part_names(const lt_tune *part, char ***names_out);
/* Apply ONLY the values in part: INI constants are written through ps, pc variables into pcvars
 * (skipped when NULL). *undo (may be NULL) receives a partial tune with the previous values, so
 * applying it restores them. applied / skipped (may be NULL) count the constants written and
 * those the INI does not know or that failed to encode. Returns 0, or LT_EINVAL. */
int      lt_tune_part_apply(const lt_ini *ini, lt_pagesrc *ps, lt_tune *pcvars, const lt_tune *part,
                            lt_tune **undo, size_t *applied, size_t *skipped);

/* ====================================================================== */
/* INI migration detection (spec 4)                                        */
/* ====================================================================== */
typedef struct lt_constant_change {
    char *name; char *old_type; lt_u8 old_page; lt_u32 old_offset; double old_scale, old_translate;
    char *new_type; lt_u8 new_page; lt_u32 new_offset; double new_scale, new_translate;
    char *description;
} lt_constant_change;
typedef struct lt_migration_report {
    char **missing_in_tune; size_t n_missing_in_tune;
    char **missing_in_ini;  size_t n_missing_in_ini;
    lt_constant_change *type_changed; size_t n_type;
    lt_constant_change *scale_changed; size_t n_scale;
    lt_constant_change *location_changed; size_t n_loc;
    lt_bool can_auto_migrate, requires_user_review;
    char severity[8];                      /* "none","low","medium","high" */
} lt_migration_report;
void  lt_migration_init(lt_migration_report *r);            /* empty report: auto=1, "none" */
void  lt_migration_clear(lt_migration_report *r);
void  lt_migration_compare(lt_migration_report *out, const lt_manifest_entry *saved, size_t n,
                           const lt_ini *ini);
lt_bool lt_migration_has_changes(const lt_migration_report *r);
char *lt_migration_summary(const lt_migration_report *r);   /* "2 new constants, 1 type changes" */

/* ====================================================================== */
/* TunerStudio .table single-table files (spec 5)                          */
/* ====================================================================== */
typedef struct lt_table_file {
    size_t cols, rows;
    double *x, *y;        /* cols / rows axis values */
    double *z;            /* rows*cols row-major */
} lt_table_file;
int   lt_table_file_parse(const char *xml, size_t len, lt_table_file *out);   /* 0 / LT_EDATA with message */
void  lt_table_file_clear(lt_table_file *t);
/* x_name/y_name are the axis labels; z has ny rows x nx cols. */
char *lt_table_file_write(const char *x_name, const char *y_name, const double *x, size_t nx,
                          const double *y, size_t ny, const double *z);   /* malloc'd XML */

/* ====================================================================== */
/* TunerStudio .tuneView files (spec 6) - P2                               */
/* ====================================================================== */
typedef struct lt_kv { char *k, *v; } lt_kv;
typedef struct lt_kvmap { lt_kv *a; size_t n; } lt_kvmap;     /* sorted by key; set replaces */
void lt_kvmap_set(lt_kvmap *m, const char *k, const char *v);
const char *lt_kvmap_get(const lt_kvmap *m, const char *k);
void lt_kvmap_clear(lt_kvmap *m);
typedef struct lt_tune_prop { char *name, *ts_type; lt_kvmap attrs; char *text; } lt_tune_prop;
typedef struct lt_tune_comp { char *comp_type; lt_tune_prop *props; size_t n; } lt_tune_comp;
typedef struct lt_tune_view {
    lt_kvmap bibliography, version_info, tuning_view_attrs;
    char *preview_image;                  /* base64 or NULL */
    lt_tune_comp *comps; size_t ncomps;
} lt_tune_view;
int   lt_tune_view_parse(const char *xml, size_t len, lt_tune_view *out);
void  lt_tune_view_clear(lt_tune_view *v);
char *lt_tune_view_write(const lt_tune_view *v);

#endif /* LT_TUNE_H */
