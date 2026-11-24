/* dl_msl.c - MegaLogViewer / TunerStudio ASCII logs (.msl): whole-log writer and the
 * streaming writer used by the recorder.  Reading goes through lt_csv_parse, which accepts
 * the MSL extras (quoted title lines, units line, MARK lines).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include "datalog.h"
#include "../plat/plat.h"

struct lt_msl_writer {
    FILE *fp;
    int n_ch;
    long marks;
    lt_buf b;             /* pending text */
};

/* compact number: fixed decimals with trailing zeros removed ("12.5", "0", "-3.25") */
static void put_val(lt_buf *b, double v, int decimals)
{
    char tmp[64];
    size_t n;
    if (!lt_isfinite(v)) {
        v = 0.0;
    }
    lt_snprintf(tmp, sizeof tmp, "%.*f", decimals, v);
    n = strlen(tmp);
    if (strchr(tmp, '.') != NULL) {
        while (n > 0 && tmp[n - 1] == '0') {
            tmp[--n] = '\0';
        }
        if (n > 0 && tmp[n - 1] == '.') {
            tmp[--n] = '\0';
        }
    }
    if (strcmp(tmp, "-0") == 0) {
        lt_strlcpy(tmp, "0", sizeof tmp);
    }
    lt_buf_puts(b, tmp);
}

/* a cell without tabs / newlines (names and units never contain them in practice) */
static void put_cell(lt_buf *b, const char *s)
{
    for (; s != NULL && *s; s++) {
        lt_buf_putc(b, (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : (unsigned char)*s);
    }
}

static void put_titles(lt_buf *b, const char *title)
{
    const char *p = title;
    if (p == NULL || *p == '\0') {
        char ts[40];
        lt_plat_timestamp(ts, sizeof ts);
        lt_buf_puts(b, "\"clibretune datalog\"\n\"Capture Date: ");
        lt_buf_puts(b, ts);
        lt_buf_puts(b, "\"\n");
        return;
    }
    while (*p) {
        lt_buf_putc(b, '"');
        while (*p && *p != '\n') {
            lt_buf_putc(b, *p == '"' ? '\'' : (*p == '\r' ? ' ' : (unsigned char)*p));
            p++;
        }
        lt_buf_puts(b, "\"\n");
        if (*p == '\n') {
            p++;
        }
    }
}

static void put_head(lt_buf *b, const char *const *names, const char *const *units, int n_ch,
                     const char *title)
{
    int c;
    put_titles(b, title);
    lt_buf_puts(b, "Time");
    for (c = 0; c < n_ch; c++) {
        lt_buf_putc(b, '\t');
        put_cell(b, names[c]);
    }
    lt_buf_puts(b, "\ns");
    for (c = 0; c < n_ch; c++) {
        lt_buf_putc(b, '\t');
        put_cell(b, units != NULL ? units[c] : "");
    }
    lt_buf_putc(b, '\n');
}

static void put_row(lt_buf *b, double t, const double *v, int n_ch)
{
    int c;
    put_val(b, t, 3);
    for (c = 0; c < n_ch; c++) {
        lt_buf_putc(b, '\t');
        put_val(b, v[c], 4);
    }
    lt_buf_putc(b, '\n');
}

static void put_mark(lt_buf *b, long k, const char *text)
{
    char tmp[24];
    lt_snprintf(tmp, sizeof tmp, "MARK %03ld - ", k);
    lt_buf_puts(b, tmp);
    put_cell(b, text != NULL && text[0] ? text : "Manual");
    lt_buf_putc(b, '\n');
}

void lt_msl_format(lt_buf *b, const lt_log *log)
{
    long i, m = 0;
    put_head(b, (const char *const *)log->names, (const char *const *)log->units, log->n_ch,
             log->header);
    /* markers before the first row */
    while (m < log->n_markers && log->n > 0 && log->markers[m].t < log->t[0]) {
        put_mark(b, m, log->markers[m].text);
        m++;
    }
    for (i = 0; i < log->n; i++) {
        put_row(b, log->t[i], log->v + (size_t)i * (size_t)log->n_ch, log->n_ch);
        while (m < log->n_markers && (i + 1 >= log->n || log->markers[m].t < log->t[i + 1])) {
            put_mark(b, m, log->markers[m].text);
            m++;
        }
    }
    while (m < log->n_markers) {
        put_mark(b, m, log->markers[m].text);
        m++;
    }
}

int lt_msl_write(const char *path, const lt_log *log)
{
    lt_buf b;
    int rc;
    lt_buf_init(&b);
    lt_msl_format(&b, log);
    rc = lt_file_write(path, b.data != NULL ? b.data : "", b.len);
    lt_buf_free(&b);
    if (rc != 0) {
        lt_err_set("cannot write '%s'", path);
        return LT_EIO;
    }
    return 0;
}

int lt_msl_parse(const char *text, size_t len, lt_log *out)
{
    return lt_csv_parse(text, len, out);
}

int lt_msl_read(const char *path, lt_log *out)
{
    return lt_csv_read(path, out);
}

/* ---- streaming ---- */

lt_msl_writer *lt_msl_open(const char *path, const char *const *names, const char *const *units,
                           int n_ch, const char *title)
{
    lt_msl_writer *w;
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        lt_err_set("cannot open '%s' for writing", path);
        return NULL;
    }
    w = (lt_msl_writer *)lt_calloc(1, sizeof *w);
    w->fp = fp;
    w->n_ch = n_ch < 0 ? 0 : n_ch;
    lt_buf_init(&w->b);
    put_head(&w->b, names, units, w->n_ch, title);
    if (lt_msl_flush(w) != 0) {
        lt_msl_close(w);
        return NULL;
    }
    return w;
}

int lt_msl_put(lt_msl_writer *w, double t_sec, const double *values)
{
    put_row(&w->b, t_sec, values, w->n_ch);
    return 0;
}

int lt_msl_put_marker(lt_msl_writer *w, const char *text)
{
    put_mark(&w->b, w->marks++, text);
    return 0;
}

int lt_msl_flush(lt_msl_writer *w)
{
    int rc = 0;
    if (w->b.len > 0 && fwrite(w->b.data, 1, w->b.len, w->fp) != w->b.len) {
        rc = LT_EIO;
    }
    lt_buf_clear(&w->b);
    if (fflush(w->fp) != 0) {
        rc = LT_EIO;
    }
    if (rc != 0) {
        lt_err_set("MSL write error");
    }
    return rc;
}

int lt_msl_close(lt_msl_writer *w)
{
    int rc;
    if (w == NULL) {
        return 0;
    }
    rc = lt_msl_flush(w);
    if (fclose(w->fp) != 0) {
        rc = LT_EIO;
    }
    lt_buf_free(&w->b);
    lt_free(w);
    return rc;
}
