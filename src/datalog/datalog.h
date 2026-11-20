/* datalog.h - data logs: in-memory model, CSV + MLG v2 read/write, live
 *             recorder, playback, dyno runs, math channels.
 *
 * Spec: docs/spec/06-datalog.md. DEVIATIONS (see task brief):
 *   - Our native log format is MegaLogViewer MLG v2 (spec section 4): we READ
 *     and WRITE it (spec section 4.3 recipe). The .ltlog format / zstd / JSON
 *     schema (spec section 3) is out of scope; `LT_FMT_LTLOG` does not exist.
 *   - Time is `double` seconds (no 64-bit ints in C89). MLG ticks are 10 us.
 *   - lt_log stores samples in flat arrays instead of an array of entries.
 *
 * MEMORY: lt_log owns its arrays (lt_log_free). lt_mlg_writer / lt_recorder
 * are heap objects with explicit close/free. Single threaded: the recorder is
 * fed from the main loop with each realtime frame.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_DATALOG_H
#define LT_DATALOG_H

#include "../lt.h"
#include "../lt_codes.h"
#include "../ini/ini.h"
#include "../ini/expr.h"

/* ====================================================================== */
/* In-memory log                                                           */
/* ====================================================================== */
typedef struct lt_log_marker { double t; char text[51]; } lt_log_marker;   /* MLG kind-1 records */

typedef struct lt_log {
    int     n_ch;
    char  **names;       /* n_ch channel names (a "Time" column is NOT included here) */
    char  **units;       /* n_ch unit strings ("" if unknown) */
    long    n;           /* number of samples */
    double *t;           /* n timestamps in seconds from log start (>= 0, non decreasing) */
    double *v;           /* n * n_ch values, row-major: v[i*n_ch + c] */
    lt_log_marker *markers; long n_markers;
    long    cap;         /* allocated sample capacity (builders only) */
    char   *header;      /* free-text header lines ('\n' separated, no quotes): the quoted title
                            lines of an MSL/CSV file; NULL when none. Owned (lt_log_free). */
} lt_log;
#define LT_LOG_AT(log, i, c) ((log)->v[(size_t)(i) * (size_t)(log)->n_ch + (size_t)(c)])

void lt_log_init(lt_log *l);
void lt_log_free(lt_log *l);
/* Define channels (copies names/units; units may be NULL). Resets samples. */
void lt_log_set_channels(lt_log *l, const char *const *names, const char *const *units, int n);
void lt_log_append(lt_log *l, double t, const double *values);        /* n_ch values */
int  lt_log_channel_index(const lt_log *l, const char *name);         /* exact, first; -1 if absent */
double lt_log_duration(const lt_log *l);
/* ADDITIVE: append a marker (text truncated to 50 chars). */
void lt_log_add_marker(lt_log *l, double t, const char *text);

typedef enum lt_log_format { LT_FMT_NONE = 0, LT_FMT_CSV, LT_FMT_MLG, LT_FMT_MSL } lt_log_format;
lt_log_format lt_log_format_from_ext(const char *path);   /* .csv / .mlg / .msl, case-insensitive */
const char   *lt_log_format_ext(lt_log_format f);          /* "csv","mlg","msl" */
/* file dialog filter listing every log format we read ("Datalogs|*.mlg;*.msl;*.csv|...") */
const char   *lt_log_filter_all(void);
/* ADDITIVE: set l->header (copied, may be NULL) */
void lt_log_set_header(lt_log *l, const char *header);
/* Dispatch by extension only (content is never sniffed). Errors: LT_EINVAL for
 * other extensions ("not a log format LibreTune reads"), LT_EDATA with a
 * message for corrupt files. */
int lt_log_read(const char *path, lt_log *out);
int lt_log_write(const char *path, const lt_log *log);    /* CSV or MLG by extension */

/* ---- CSV (spec 2): "Time" first column, tolerant reader ---- */
int lt_csv_read(const char *path, lt_log *out);
/* "%.3f" time, "%.4f" values, '.' decimal always. TunerStudio style: when any channel has
 * units, a second line with the units ("s" for Time) follows the header (DEVIATION from
 * spec 2, which writes none; the reader accepts both). */
int lt_csv_write(const char *path, const lt_log *log);
/* ADDITIVE: parse CSV text in memory (reader behind lt_csv_read). Accepts comma or tab
 * separated, optional UTF-8 BOM, quoted header cells, "Time (ms)" / units "ms" -> /1000,
 * and a units line right after the header. */
int lt_csv_parse(const char *text, size_t len, lt_log *out);
/* The reader also accepts the MegaLogViewer/TunerStudio extras (used by .msl): quoted title
 * lines before the header (kept in log->header) and "MARK nnn - text" lines between data rows
 * (a marker at the time of the preceding row). */

/* ---- MSL (MegaLogViewer / TunerStudio ASCII log) ----
 * Layout written:   "title line"            (log->header lines, else "LibreTune datalog" +
 *                   "Capture Date: ...")    the capture date)
 *                   Time<TAB>ch1<TAB>...     channel names
 *                   s<TAB>unit1<TAB>...      units line
 *                   0.000<TAB>v1<TAB>...     data rows (time %.3f, values compact)
 *                   MARK 000 - text          after the last row at or before the marker time
 * Reading is lt_csv_parse (tab separated, quoted titles, units line, MARK lines). */
int lt_msl_parse(const char *text, size_t len, lt_log *out);
int lt_msl_read(const char *path, lt_log *out);
int lt_msl_write(const char *path, const lt_log *log);
/* in-memory form of lt_msl_write (appends to b) */
void lt_msl_format(lt_buf *b, const lt_log *log);
/* Streaming MSL writer (recorder). title may be NULL (default title lines). */
typedef struct lt_msl_writer lt_msl_writer;
lt_msl_writer *lt_msl_open(const char *path, const char *const *names, const char *const *units,
                           int n_ch, const char *title);
int  lt_msl_put(lt_msl_writer *w, double t_sec, const double *values);
int  lt_msl_put_marker(lt_msl_writer *w, const char *text);
int  lt_msl_flush(lt_msl_writer *w);
int  lt_msl_close(lt_msl_writer *w);

/* ====================================================================== */
/* MLG v2 (spec 4), big endian                                             */
/* ====================================================================== */
typedef enum lt_mlg_type {
    LT_MLG_U08 = 0, LT_MLG_S08, LT_MLG_U16, LT_MLG_S16, LT_MLG_U32, LT_MLG_S32, LT_MLG_S64, LT_MLG_F32
} lt_mlg_type;
typedef struct lt_mlg_field {
    char name[35];           /* <= 33 chars used */
    char units[11];
    lt_mlg_type type;
    float scale, transform;  /* value = (raw + transform) * scale */
    int digits;
} lt_mlg_field;
/* The reader returns the Time channel too when present as a field (it is also
 * used for timestamps if its units are "s"); lt_log.names then excludes it. */
int lt_mlg_read(const char *path, lt_log *out);
/* ADDITIVE: same on an in-memory image. */
int lt_mlg_read_buf(const void *data, size_t len, lt_log *out);
/* Whole-log writer: a "Time" field (F32, "s") followed by one F32 field per channel
 * (scale 1, transform 0). info may be NULL (optional embedded tune text). */
int lt_mlg_write(const char *path, const lt_log *log, const char *info);

/* Streaming writer used by the recorder. The header (with descriptors) is
 * written at open; records are appended and flushed in batches. */
typedef struct lt_mlg_writer lt_mlg_writer;
lt_mlg_writer *lt_mlg_open(const char *path, const char *const *names, const char *const *units,
                           int n_ch, const char *info);
int  lt_mlg_put(lt_mlg_writer *w, double t_sec, const double *values);   /* one data record */
int  lt_mlg_put_marker(lt_mlg_writer *w, double t_sec, const char *text);   /* <= 50 chars */
int  lt_mlg_flush(lt_mlg_writer *w);
int  lt_mlg_close(lt_mlg_writer *w);                                     /* flush + free */

/* Infer channel names/units from the INI [Datalog] section (entries with
 * `enabled`): fills arrays of malloc'd strings (free with lt_strv_free). Returns
 * count. If the INI has no [Datalog] section all computed+raw channels are used. */
size_t lt_datalog_channels_from_ini(const lt_ini *ini, char ***names, char ***units);
void   lt_strv_free(char **v, size_t n);

/* ====================================================================== */
/* Recorder (spec 5)                                                       */
/* ====================================================================== */
#define LT_REC_TAIL 2048        /* in-memory ring for live graphs */
typedef struct lt_recorder lt_recorder;
lt_recorder *lt_rec_new(const char *const *names, const char *const *units, int n_ch);
void   lt_rec_free(lt_recorder *r);                     /* stops and closes any open file */
void   lt_rec_set_sample_rate(lt_recorder *r, double hz);   /* clamp [1, 200], default 10 */
/* ADDITIVE: file format of the next lt_rec_start / lt_rec_clear file: LT_FMT_MLG (default) or
 * LT_FMT_MSL; a .mlg/.msl/.csv extension of the path passed to lt_rec_start is replaced to match. */
void   lt_rec_set_format(lt_recorder *r, lt_log_format fmt);
lt_log_format lt_rec_format(const lt_recorder *r);
/* Begin writing an MLG file immediately (every accepted record is appended).
 * The caller picks the path (typically <project>/datalogs/YYYY-MM-DD_HH.MM.SS.mlg,
 * see lt_rec_default_path). */
int    lt_rec_start(lt_recorder *r, const char *path);
int    lt_rec_stop(lt_recorder *r);                      /* closes the file; log_path stays */
lt_bool lt_rec_is_recording(const lt_recorder *r);
/* Feed one sample (n must equal n_ch else counted as malformed and dropped).
 * Rate limited to sample_rate. Uses the plat monotonic clock; _at takes an
 * explicit time in ms for tests/replay. Disk failure stops recording. */
/* Return: 1 stored, 0 dropped (not recording / rate limited), LT_EINVAL malformed (counted),
 * LT_EIO disk failure (recording is stopped). The time base (t = 0) is the first accepted
 * sample after lt_rec_start / lt_rec_clear. */
int    lt_rec_record(lt_recorder *r, const double *values, int n);
int    lt_rec_record_at(lt_recorder *r, double now_ms, const double *values, int n);
void   lt_rec_marker(lt_recorder *r, const char *text);   /* MLG marker at "now" */
void   lt_rec_marker_at(lt_recorder *r, double now_ms, const char *text);   /* ADDITIVE */
void   lt_rec_clear(lt_recorder *r);                     /* restart timeline (new file if recording, name from dir) */
long   lt_rec_count(const lt_recorder *r);               /* rows recorded since start */
long   lt_rec_malformed(const lt_recorder *r);
double lt_rec_duration(const lt_recorder *r);
const char *lt_rec_path(const lt_recorder *r);           /* current or last finished file, or "" */
/* Live window of the tail ring: samples [start, start+count) in recorder row
 * numbering, clipped to what the ring still holds; copies into `out` (a log
 * with the recorder's channels). */
void   lt_rec_live_window(const lt_recorder *r, long start, long count, lt_log *out);
/* "<dir>/YYYY-MM-DD_HH.MM.SS.mlg" (local time), malloc'd */
char  *lt_rec_default_path(const char *dir);

/* ====================================================================== */
/* Playback (spec 6)                                                       */
/* ====================================================================== */
typedef struct lt_player {
    const lt_log *log;       /* borrowed, must outlive the player */
    long pos;
    double ptime;            /* play clock (log time, seconds) used by lt_player_advance */
    double rate;             /* playback speed multiplier, default 1.0, clamped [0.01, 100] */
    lt_bool loop;            /* wrap to the start at the end (default off) */
} lt_player;
void   lt_player_set_rate(lt_player *p, double rate);          /* ADDITIVE */
void   lt_player_set_loop(lt_player *p, lt_bool loop);         /* ADDITIVE */
/* ADDITIVE: advance the play clock by dt_s * rate and move pos to the last sample with
 * t <= clock. Returns LT_FALSE once the end is reached without loop (pos = last). */
lt_bool lt_player_advance(lt_player *p, double dt_s);
void   lt_player_init(lt_player *p, const lt_log *log);
long   lt_player_len(const lt_player *p);
double lt_player_duration(const lt_player *p);
void   lt_player_seek(lt_player *p, long pos);                /* clamp to [0, len-1] */
void   lt_player_seek_time(lt_player *p, double t);           /* first sample with t >= given, else last */
lt_bool lt_player_next(lt_player *p);                         /* LT_FALSE at the end (no move) */
lt_bool lt_player_prev(lt_player *p);
double lt_player_time(const lt_player *p);
const double *lt_player_row(const lt_player *p);              /* n_ch values of the current sample or NULL */
/* Copy channel `name` over [t0,t1] (inclusive; t1 < t0 = whole log). Returns count (NaN-free). */
long   lt_player_channel(const lt_player *p, const char *name, double *out, long cap);
/* Load the current row into a realtime-values object so gauges/tables follow the log: */
/* Needs lt_ini_rt_set, which this module does not link directly: the app calls
 * lt_datalog_set_rt_setter(lt_ini_rt_set) once at startup (no-op until then). */
typedef void (*lt_rt_set_fn)(lt_ini_rt *rt, size_t channel_index, double v);
void   lt_datalog_set_rt_setter(lt_rt_set_fn fn);
void   lt_player_apply_to_rt(const lt_player *p, const lt_ini *ini, lt_ini_rt *rt);

/* ====================================================================== */
/* Dyno runs (spec 7)                                                      */
/* ====================================================================== */
typedef struct lt_dyno_point {
    double rpm;
    lt_bool has_hp, has_tq, has_afr, has_boost, has_time;
    double hp, tq_val, afr, boost, time;
} lt_dyno_point;
typedef struct lt_dyno_run {
    char *name; char *source_file; char color[16];            /* default "#4fc3f7" */
    lt_dyno_point *data; size_t n;                            /* sorted by rpm */
    lt_bool has_peak_hp; double peak_hp, peak_hp_rpm;
    lt_bool has_peak_tq; double peak_tq, peak_tq_rpm;
} lt_dyno_run;
void lt_dyno_run_free(lt_dyno_run *r);
/* ADDITIVE helpers (spec 7.2/7.3). which: 0 = hp, 1 = torque. Linear interpolation at an
 * rpm between existing points only (no extrapolation); LT_FALSE if not possible. */
lt_bool lt_dyno_interpolate(const lt_dyno_run *r, double rpm, int which, double *out);
size_t  lt_dyno_nearest_bin(const double *bins, size_t n, double v);   /* ties -> first; 0 if n=0 */
/* csv import with auto column detection; headers lists the original header cells. */
int  lt_dyno_from_csv(const char *path, const char *name, lt_dyno_run *out);
int  lt_dyno_csv_headers(const char *path, char ***headers, size_t *n);
void lt_dyno_compute_peaks(lt_dyno_run *r);
typedef struct lt_dyno_cell {
    size_t row, col; lt_bool has_hp, has_tq, has_afr; double hp, tq_val, afr; size_t samples;
} lt_dyno_cell;
/* Project dyno points onto a (rpm x load) table grid: cells with data returned in row-major order. */
size_t lt_dyno_map_to_table(const lt_dyno_run *r, const double *x_bins, size_t nx,
                            const double *y_bins, size_t ny, double boost_to_load /* 0 = none */,
                            lt_dyno_cell *out, size_t cap);
typedef struct lt_dyno_cmp {
    double *hp_diff, *tq_diff; size_t n_hp, n_tq;             /* pairs (rpm, diff) flattened: 2*n doubles */
    lt_bool has_total_hp, has_total_tq; double hp_change_sum, tq_change_sum;
} lt_dyno_cmp;
/* Chart scale for the runs (power and torque share the vertical axis): rpm rounded outward to 500,
 * the value maximum rounded up to 50. LT_FALSE (outputs untouched) when no run has a usable span. */
lt_bool lt_dyno_chart_range(const lt_dyno_run *runs, size_t n, double *rpm0, double *rpm1,
                            double *vmax);
int  lt_dyno_compare(const lt_dyno_run *a, const lt_dyno_run *b, lt_dyno_cmp *out);
void lt_dyno_cmp_free(lt_dyno_cmp *c);

/* ====================================================================== */
/* Math channels (spec 8): user-defined computed log channels              */
/* ====================================================================== */
typedef struct lt_math_channel {
    char *name, *units, *expression;
    lt_expr *ast;            /* compiled lazily by lt_math_compile; may be NULL if invalid */
} lt_math_channel;
typedef struct lt_math_set { lt_math_channel *ch; size_t n; } lt_math_set;
/* Expression engine hooks: this module does not link against src/ini. The integration
 * (app) calls lt_math_set_hooks(LT_MATH_HOOKS_INI) once at startup, or tests install a fake.
 * eval returns 0 and sets *out, or negative on evaluation error. */
typedef lt_expr *(*lt_expr_compile_fn)(const char *src, char *err, size_t errlen);
typedef void     (*lt_expr_free_fn)(lt_expr *e);
typedef int      (*lt_expr_eval_fn)(const lt_expr *e, const lt_expr_env *env, double *out);
typedef void     (*lt_expr_vars_fn)(const lt_expr *e, lt_map *set);
typedef struct lt_math_hooks {
    lt_expr_compile_fn compile;
    lt_expr_free_fn    free_expr;
    lt_expr_eval_fn    eval;
    lt_expr_vars_fn    collect_vars;
} lt_math_hooks;
#define LT_MATH_HOOKS_INI { lt_expr_compile, lt_expr_free, lt_expr_eval_f64, lt_expr_collect_vars }
void lt_math_set_hooks(const lt_math_hooks *h);          /* copied; NULL clears */
void lt_math_set_free(lt_math_set *s);
int  lt_math_compile(lt_math_channel *ch, char *err, size_t errlen);    /* 0 or LT_EDATA */
/* Evaluation order (dependencies first, cycles last in stored order); out has n entries. */
void lt_math_eval_order(lt_math_set *s, size_t *order);
/* Evaluate all channels in order for one sample: `env` resolves the log's channels; results are
 * both written to out[i] (stored order) and visible to later channels. */
int  lt_math_eval_all(lt_math_set *s, const lt_expr_env *env, double *out);
/* JSON persistence (array of {name, units, expression}); missing file -> empty set, ok. */
int  lt_math_load(const char *path, lt_math_set *out);
int  lt_math_save(const char *path, const lt_math_set *s);
/* Append computed channels to a log (new columns). */
int  lt_math_apply_to_log(lt_math_set *s, lt_log *log);

#endif /* LT_DATALOG_H */
