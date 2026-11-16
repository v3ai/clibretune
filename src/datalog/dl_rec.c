/* dl_rec.c - live recorder (streams MLG) and playback cursor. Spec 06 sections 5 and 6.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "datalog.h"
#include "../plat/plat.h"

#define REC_FLUSH_ROWS 16

struct lt_recorder {
    int      n_ch;
    char   **names, **units;
    double   hz;
    lt_bool  recording;
    lt_mlg_writer *w;
    char    *path;        /* current file or "" */
    char    *finished;    /* last finished file or "" */
    char    *dir;         /* directory of the streaming file, or NULL */
    lt_bool  have_start, have_last;
    double   start_ms, last_ms, last_ts;
    long     rows, malformed;
    double  *tt;          /* ring: LT_REC_TAIL timestamps */
    double  *tv;          /* ring: LT_REC_TAIL * n_ch values */
    long     tail_len;
    long     tail_head;   /* slot of the oldest entry */
};

static char *dup_or_empty(const char *s)
{
    return lt_strdup(s ? s : "");
}

lt_recorder *lt_rec_new(const char *const *names, const char *const *units, int n_ch)
{
    lt_recorder *r = (lt_recorder *)lt_calloc(1, sizeof *r);
    int i;
    if (n_ch < 0) {
        n_ch = 0;
    }
    r->n_ch = n_ch;
    r->names = (char **)lt_calloc((size_t)(n_ch + 1), sizeof(char *));
    r->units = (char **)lt_calloc((size_t)(n_ch + 1), sizeof(char *));
    for (i = 0; i < n_ch; i++) {
        r->names[i] = dup_or_empty(names ? names[i] : NULL);
        r->units[i] = dup_or_empty(units ? units[i] : NULL);
    }
    r->hz = 10.0;
    r->path = lt_strdup("");
    r->finished = lt_strdup("");
    r->tt = (double *)lt_calloc(LT_REC_TAIL, sizeof(double));
    r->tv = (double *)lt_calloc((size_t)LT_REC_TAIL * (size_t)(n_ch > 0 ? n_ch : 1),
                                sizeof(double));
    return r;
}

static int rec_close_writer(lt_recorder *r)
{
    int rc = 0;
    if (r->w) {
        rc = lt_mlg_close(r->w);
        r->w = NULL;
        lt_free(r->finished);
        r->finished = lt_strdup(r->path);
    }
    return rc;
}

void lt_rec_free(lt_recorder *r)
{
    int i;
    if (!r) {
        return;
    }
    rec_close_writer(r);
    for (i = 0; i < r->n_ch; i++) {
        lt_free(r->names[i]);
        lt_free(r->units[i]);
    }
    lt_free(r->names);
    lt_free(r->units);
    lt_free(r->path);
    lt_free(r->finished);
    lt_free(r->dir);
    lt_free(r->tt);
    lt_free(r->tv);
    lt_free(r);
}

void lt_rec_set_sample_rate(lt_recorder *r, double hz)
{
    if (!(hz >= 1.0)) {
        hz = 1.0;
    }
    if (hz > 200.0) {
        hz = 200.0;
    }
    r->hz = hz;
}

static char *dir_of(const char *path)
{
    const char *p = path + strlen(path);
    while (p > path && p[-1] != '/' && p[-1] != '\\') {
        p--;
    }
    if (p == path) {
        return NULL;
    }
    return lt_strndup(path, (size_t)(p - path - 1 > 0 ? p - path - 1 : 1));
}

static void reset_timeline(lt_recorder *r)
{
    r->have_start = LT_FALSE;
    r->have_last = LT_FALSE;
    r->start_ms = r->last_ms = r->last_ts = 0.0;
    r->rows = 0;
    r->tail_len = 0;
    r->tail_head = 0;
}

static int rec_open_file(lt_recorder *r, const char *path)
{
    char *d = dir_of(path);
    if (d) {
        lt_plat_mkdirs(d);
    }
    r->w = lt_mlg_open(path, (const char *const *)r->names, (const char *const *)r->units,
                       r->n_ch, NULL);
    if (!r->w) {
        lt_free(d);
        return LT_EIO;
    }
    lt_free(r->dir);
    r->dir = d;
    lt_free(r->path);
    r->path = lt_strdup(path);
    return 0;
}

int lt_rec_start(lt_recorder *r, const char *path)
{
    int rc = 0;
    if (r->recording) {
        lt_rec_stop(r);
    }
    rec_close_writer(r);
    reset_timeline(r);
    r->malformed = 0;
    if (path && path[0]) {
        rc = rec_open_file(r, path);
        if (rc != 0) {
            return rc;
        }
    }
    r->recording = LT_TRUE;
    return 0;
}

int lt_rec_stop(lt_recorder *r)
{
    int rc;
    r->recording = LT_FALSE;
    rc = rec_close_writer(r);
    return rc;
}

lt_bool lt_rec_is_recording(const lt_recorder *r)
{
    return r->recording;
}

static void tail_push(lt_recorder *r, double ts, const double *values)
{
    long slot;
    int c;
    if (r->tail_len < LT_REC_TAIL) {
        slot = (r->tail_head + r->tail_len) % LT_REC_TAIL;
        r->tail_len++;
    } else {
        slot = r->tail_head;
        r->tail_head = (r->tail_head + 1) % LT_REC_TAIL;
    }
    r->tt[slot] = ts;
    for (c = 0; c < r->n_ch; c++) {
        r->tv[(size_t)slot * (size_t)r->n_ch + (size_t)c] = values[c];
    }
}

int lt_rec_record_at(lt_recorder *r, double now_ms, const double *values, int n)
{
    double ts;
    if (!r->recording) {
        return 0;
    }
    if (n != r->n_ch || (n > 0 && !values)) {
        r->malformed++;
        return LT_EINVAL;
    }
    if (r->have_last && now_ms - r->last_ms < 1000.0 / r->hz - 1e-6) {
        return 0;
    }
    if (!r->have_start) {
        r->start_ms = now_ms;
        r->have_start = LT_TRUE;
    }
    ts = (now_ms - r->start_ms) / 1000.0;
    if (ts < r->last_ts) {
        ts = r->last_ts;
    }
    if (r->w) {
        int rc = lt_mlg_put(r->w, ts, values);
        if (rc == 0 && (r->rows + 1) % REC_FLUSH_ROWS == 0) {
            rc = lt_mlg_flush(r->w);
        }
        if (rc != 0) {
            /* disk failure: stop recording, keep what is in memory */
            r->recording = LT_FALSE;
            rec_close_writer(r);
            lt_err_set("recorder write error; recording stopped");
            return LT_EIO;
        }
    }
    tail_push(r, ts, values);
    r->rows++;
    r->last_ts = ts;
    r->last_ms = now_ms;
    r->have_last = LT_TRUE;
    return 1;
}

int lt_rec_record(lt_recorder *r, const double *values, int n)
{
    return lt_rec_record_at(r, lt_plat_now_ms(), values, n);
}

void lt_rec_marker_at(lt_recorder *r, double now_ms, const char *text)
{
    double ts = r->have_start ? (now_ms - r->start_ms) / 1000.0 : 0.0;
    if (!r->recording || !r->w) {
        return;
    }
    if (ts < r->last_ts) {
        ts = r->last_ts;
    }
    if (lt_mlg_put_marker(r->w, ts, text) != 0) {
        r->recording = LT_FALSE;
        rec_close_writer(r);
    }
}

void lt_rec_marker(lt_recorder *r, const char *text)
{
    lt_rec_marker_at(r, lt_plat_now_ms(), text);
}

static char *unique_path(const char *dir)
{
    char *base = lt_rec_default_path(dir);
    char *cand = lt_strdup(base);
    int k = 1;
    while (lt_file_exists(cand)) {
        size_t bl = strlen(base);
        lt_free(cand);
        cand = (char *)lt_malloc(bl + 16);
        memcpy(cand, base, bl - 4);   /* strip ".mlg" */
        lt_snprintf(cand + bl - 4, 20, "_%d.mlg", k++);
    }
    lt_free(base);
    return cand;
}

void lt_rec_clear(lt_recorder *r)
{
    reset_timeline(r);
    r->malformed = 0;
    if (r->recording && r->dir) {
        char *d = lt_strdup(r->dir);
        char *np = unique_path(d);
        rec_close_writer(r);
        if (rec_open_file(r, np) != 0) {
            r->recording = LT_FALSE;
        }
        lt_free(np);
        lt_free(d);
    }
}

long lt_rec_count(const lt_recorder *r)
{
    return r->rows;
}

long lt_rec_malformed(const lt_recorder *r)
{
    return r->malformed;
}

double lt_rec_duration(const lt_recorder *r)
{
    return r->last_ts;
}

const char *lt_rec_path(const lt_recorder *r)
{
    if (r->w && r->path[0]) {
        return r->path;
    }
    return r->finished[0] ? r->finished : r->path;
}

void lt_rec_live_window(const lt_recorder *r, long start, long count, lt_log *out)
{
    long total = r->rows, origin, end, from, skip, take, i;
    lt_log_set_channels(out, (const char *const *)r->names, (const char *const *)r->units,
                        r->n_ch);
    origin = total - r->tail_len;
    if (origin < 0) {
        origin = 0;
    }
    if (start < 0) {
        start = 0;
    }
    if (start > total) {
        start = total;
    }
    end = (count > total - start) ? total : start + (count < 0 ? 0 : count);
    from = start > origin ? start : origin;
    skip = from - origin;
    take = end > from ? end - from : 0;
    for (i = 0; i < take; i++) {
        long slot = (r->tail_head + skip + i) % LT_REC_TAIL;
        lt_log_append(out, r->tt[slot], r->tv + (size_t)slot * (size_t)r->n_ch);
    }
}

char *lt_rec_default_path(const char *dir)
{
    char ts[40], name[64];
    size_t i;
    lt_plat_timestamp(ts, sizeof ts);
    for (i = 0; ts[i]; i++) {
        if (ts[i] == ' ') {
            ts[i] = '_';
        } else if (ts[i] == ':') {
            ts[i] = '.';
        }
    }
    lt_snprintf(name, sizeof name, "%s.mlg", ts);
    if (!dir || !dir[0]) {
        return lt_strdup(name);
    }
    return lt_path_join(dir, name);
}

/* ------------------------------------------------------------- player */

void lt_player_init(lt_player *p, const lt_log *log)
{
    p->log = log;
    p->pos = 0;
    p->ptime = (log && log->n > 0) ? log->t[0] : 0.0;
    p->rate = 1.0;
    p->loop = LT_FALSE;
}

long lt_player_len(const lt_player *p)
{
    return p->log ? p->log->n : 0;
}

double lt_player_duration(const lt_player *p)
{
    return p->log ? lt_log_duration(p->log) : 0.0;
}

void lt_player_seek(lt_player *p, long pos)
{
    long n = lt_player_len(p);
    if (n == 0 || pos < 0) {
        p->pos = 0;
    } else {
        p->pos = pos > n - 1 ? n - 1 : pos;
    }
    if (n > 0) {
        p->ptime = p->log->t[p->pos];
    }
}

void lt_player_seek_time(lt_player *p, double t)
{
    long n = lt_player_len(p), lo = 0, hi;
    if (n == 0) {
        p->pos = 0;
        return;
    }
    hi = n;   /* first index with t[i] >= t in [0, n] */
    while (lo < hi) {
        long mid = lo + (hi - lo) / 2;
        if (p->log->t[mid] >= t) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    p->pos = lo >= n ? n - 1 : lo;
    p->ptime = t;
    if (lo >= n) {
        p->ptime = p->log->t[n - 1];
    }
}

lt_bool lt_player_next(lt_player *p)
{
    if (p->pos + 1 < lt_player_len(p)) {
        p->pos++;
        p->ptime = p->log->t[p->pos];
        return LT_TRUE;
    }
    return LT_FALSE;
}

lt_bool lt_player_prev(lt_player *p)
{
    if (p->pos > 0) {
        p->pos--;
        p->ptime = p->log->t[p->pos];
        return LT_TRUE;
    }
    return LT_FALSE;
}

double lt_player_time(const lt_player *p)
{
    return lt_player_len(p) > 0 ? p->log->t[p->pos] : 0.0;
}

const double *lt_player_row(const lt_player *p)
{
    if (lt_player_len(p) == 0 || p->log->n_ch == 0) {
        return NULL;
    }
    return p->log->v + (size_t)p->pos * (size_t)p->log->n_ch;
}

long lt_player_channel(const lt_player *p, const char *name, double *out, long cap)
{
    long i, n = 0;
    int c;
    if (!p->log) {
        return 0;
    }
    c = lt_log_channel_index(p->log, name);
    if (c < 0) {
        return 0;
    }
    for (i = 0; i < p->log->n && n < cap; i++) {
        out[n++] = LT_LOG_AT(p->log, i, c);
    }
    return n;
}

void lt_player_set_rate(lt_player *p, double rate)
{
    if (!(rate >= 0.01)) {
        rate = 0.01;
    }
    if (rate > 100.0) {
        rate = 100.0;
    }
    p->rate = rate;
}

void lt_player_set_loop(lt_player *p, lt_bool loop)
{
    p->loop = loop ? LT_TRUE : LT_FALSE;
}

lt_bool lt_player_advance(lt_player *p, double dt_s)
{
    long n = lt_player_len(p), lo, hi;
    double t0, t1, tt;
    if (n == 0) {
        return LT_FALSE;
    }
    if (!lt_isfinite(dt_s)) {
        return LT_FALSE;
    }
    t0 = p->log->t[0];
    t1 = p->log->t[n - 1];
    tt = p->ptime + dt_s * p->rate;
    if (tt > t1) {
        if (p->loop && t1 > t0) {
            tt = t0 + fmod(tt - t0, t1 - t0);
        } else {
            p->ptime = t1;
            p->pos = n - 1;
            return LT_FALSE;
        }
    }
    if (tt < t0) {
        tt = t0;
    }
    p->ptime = tt;
    /* last index with t[i] <= tt */
    lo = 0;
    hi = n;
    while (lo < hi) {
        long mid = lo + (hi - lo) / 2;
        if (p->log->t[mid] <= tt) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    p->pos = lo > 0 ? lo - 1 : 0;
    return LT_TRUE;
}
