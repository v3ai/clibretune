/* dl_log.c - in-memory log model, format dispatch, CSV reader/writer.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "datalog.h"

/* ---------------------------------------------------------------- model */

void lt_log_init(lt_log *l)
{
    memset(l, 0, sizeof *l);
}

static void free_strs(char **v, int n)
{
    int i;
    if (!v) {
        return;
    }
    for (i = 0; i < n; i++) {
        lt_free(v[i]);
    }
    lt_free(v);
}

void lt_log_free(lt_log *l)
{
    free_strs(l->names, l->n_ch);
    free_strs(l->units, l->n_ch);
    lt_free(l->t);
    lt_free(l->v);
    lt_free(l->markers);
    lt_free(l->header);
    lt_log_init(l);
}

void lt_log_set_header(lt_log *l, const char *header)
{
    lt_free(l->header);
    l->header = header != NULL ? lt_strdup(header) : NULL;
}

void lt_log_set_channels(lt_log *l, const char *const *names, const char *const *units, int n)
{
    int i;
    free_strs(l->names, l->n_ch);
    free_strs(l->units, l->n_ch);
    lt_free(l->t);
    lt_free(l->v);
    lt_free(l->markers);
    lt_free(l->header);
    lt_log_init(l);
    if (n < 0) {
        n = 0;
    }
    l->n_ch = n;
    if (n > 0) {
        l->names = (char **)lt_calloc((size_t)n, sizeof(char *));
        l->units = (char **)lt_calloc((size_t)n, sizeof(char *));
    }
    for (i = 0; i < n; i++) {
        l->names[i] = lt_strdup(names && names[i] ? names[i] : "");
        l->units[i] = lt_strdup(units && units[i] ? units[i] : "");
    }
}

void lt_log_append(lt_log *l, double t, const double *values)
{
    int c;
    if (l->n >= l->cap) {
        long ncap = l->cap ? l->cap * 2 : 64;
        l->t = (double *)lt_realloc(l->t, (size_t)ncap * sizeof(double));
        if (l->n_ch > 0) {
            l->v = (double *)lt_realloc(l->v, (size_t)ncap * (size_t)l->n_ch * sizeof(double));
        }
        l->cap = ncap;
    }
    l->t[l->n] = t;
    for (c = 0; c < l->n_ch; c++) {
        l->v[(size_t)l->n * (size_t)l->n_ch + (size_t)c] = values ? values[c] : 0.0;
    }
    l->n++;
}

void lt_log_add_marker(lt_log *l, double t, const char *text)
{
    l->markers = (lt_log_marker *)lt_realloc(l->markers,
                                             (size_t)(l->n_markers + 1) * sizeof(lt_log_marker));
    memset(&l->markers[l->n_markers], 0, sizeof(lt_log_marker));
    l->markers[l->n_markers].t = t;
    lt_strlcpy(l->markers[l->n_markers].text, text ? text : "",
               sizeof l->markers[l->n_markers].text);
    l->n_markers++;
}

int lt_log_channel_index(const lt_log *l, const char *name)
{
    int i;
    for (i = 0; i < l->n_ch; i++) {
        if (strcmp(l->names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

double lt_log_duration(const lt_log *l)
{
    return l->n > 0 ? l->t[l->n - 1] : 0.0;
}

void lt_strv_free(char **v, size_t n)
{
    size_t i;
    if (!v) {
        return;
    }
    for (i = 0; i < n; i++) {
        lt_free(v[i]);
    }
    lt_free(v);
}

/* ------------------------------------------------------- format dispatch */

lt_log_format lt_log_format_from_ext(const char *path)
{
    const char *e = path ? lt_path_ext(path) : "";
    if (lt_stricmp(e, ".csv") == 0) {
        return LT_FMT_CSV;
    }
    if (lt_stricmp(e, ".mlg") == 0) {
        return LT_FMT_MLG;
    }
    if (lt_stricmp(e, ".msl") == 0) {
        return LT_FMT_MSL;
    }
    return LT_FMT_NONE;
}

const char *lt_log_filter_all(void)
{
    return "Datalogs|*.mlg;*.msl;*.csv|MegaLogViewer binary|*.mlg|MegaLogViewer text|*.msl|"
           "CSV|*.csv|All files|*";
}

const char *lt_log_format_ext(lt_log_format f)
{
    switch (f) {
    case LT_FMT_CSV: return "csv";
    case LT_FMT_MLG: return "mlg";
    case LT_FMT_MSL: return "msl";
    default: return "";
    }
}

int lt_log_read(const char *path, lt_log *out)
{
    switch (lt_log_format_from_ext(path)) {
    case LT_FMT_CSV: return lt_csv_read(path, out);
    case LT_FMT_MLG: return lt_mlg_read(path, out);
    case LT_FMT_MSL: return lt_msl_read(path, out);
    default:
        lt_err_set("not a log format clibretune reads: %s", path ? path : "(null)");
        return LT_EINVAL;
    }
}

int lt_log_write(const char *path, const lt_log *log)
{
    switch (lt_log_format_from_ext(path)) {
    case LT_FMT_CSV: return lt_csv_write(path, log);
    case LT_FMT_MLG: return lt_mlg_write(path, log, NULL);
    case LT_FMT_MSL: return lt_msl_write(path, log);
    default:
        lt_err_set("not a log format clibretune writes: %s", path ? path : "(null)");
        return LT_EINVAL;
    }
}

/* ------------------------------------------------------------------ CSV */

/* Split `line` in place at `delim`; returns the field count, fields[] filled up to max. */
static int split_line(char *line, char delim, char **fields, int max)
{
    int n = 0;
    char *p = line;
    for (;;) {
        char *q = strchr(p, delim);
        if (n < max) {
            fields[n] = p;
        }
        n++;
        if (!q) {
            break;
        }
        *q = '\0';
        p = q + 1;
    }
    return n;
}

static int count_char(const char *s, char c)
{
    int n = 0;
    for (; *s; s++) {
        if (*s == c) {
            n++;
        }
    }
    return n;
}

static char *unquote(char *s)
{
    size_t n;
    s = lt_str_trim(s);
    n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = '\0';
        s++;
        s = lt_str_trim(s);
    }
    return s;
}

static lt_bool is_num(char *s)
{
    double d;
    return lt_parse_double(unquote(s), &d);
}

/* "MARK 003 - text" -> text (or the whole line when no text follows); NULL when not a MARK line */
static const char *mark_text(const char *line)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (strncmp(p, "MARK", 4) != 0 || (p[4] != '\0' && p[4] != ' ' && p[4] != '\t' &&
                                       !isdigit((unsigned char)p[4]))) {
        return NULL;
    }
    p += 4;
    while (*p == ' ' || *p == '\t' || isdigit((unsigned char)*p)) {
        p++;
    }
    if (*p == '-') {
        p++;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
    }
    return *p != '\0' ? p : line;
}

/* quoted title line before the header: returns 1 (and the text without quotes) unless its
 * first cell is the "Time" column */
static int title_line(char *line, char **text)
{
    char *p = lt_str_trim(line), *q;
    size_t n;
    if (p[0] != '"') {
        return 0;
    }
    q = strchr(p + 1, '"');
    if (q != NULL && (size_t)(q - (p + 1)) >= 4 && lt_strnicmp(p + 1, "time", 4) == 0) {
        return 0;
    }
    if (q == NULL && lt_strnicmp(p + 1, "time", 4) == 0) {
        return 0;
    }
    n = strlen(p);
    if (n >= 2 && p[n - 1] == '"') {
        p[n - 1] = '\0';
    }
    *text = p + 1;
    return 1;
}

static void lower_inplace(char *s)
{
    for (; *s; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

int lt_csv_parse(const char *text, size_t len, lt_log *out)
{
    const char *p = text, *end = text + len;
    char *line = NULL;
    size_t linecap = 0;
    char **fields = NULL;
    int maxf = 0, nf, i, n_ch, dropped = 0, ms = 0, have_header = 0;
    char delim = ',';
    long skipped = 0;
    char **names = NULL, **units = NULL;
    double *vals = NULL;
    int units_possible = 1, rc = 0;
    lt_log tmp;
    lt_buf title;

    lt_log_init(&tmp);
    lt_log_init(out);
    lt_buf_init(&title);
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) {
        p += 3;
    }
    n_ch = 0;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *next = nl ? nl + 1 : end;
        char *h;

        if (ll > 0 && p[ll - 1] == '\r') {
            ll--;
        }
        if (ll + 1 > linecap) {
            linecap = ll + 64;
            line = (char *)lt_realloc(line, linecap);
        }
        memcpy(line, p, ll);
        line[ll] = '\0';
        p = next;

        if (!have_header) {
            char *tt;
            if (lt_str_trim(line)[0] == '\0') {
                continue;
            }
            if (title_line(line, &tt)) {
                if (title.len > 0) {
                    lt_buf_putc(&title, '\n');
                }
                lt_buf_puts(&title, tt);
                continue;
            }
            have_header = 1;
            if (count_char(line, '\t') > count_char(line, ',')) {
                delim = '\t';
            }
            nf = count_char(line, delim) + 1;
            maxf = nf + 8;
            fields = (char **)lt_malloc((size_t)maxf * sizeof(char *));
            nf = split_line(line, delim, fields, maxf);
            for (i = 0; i < nf && i < maxf; i++) {
                fields[i] = unquote(fields[i]);
            }
            h = fields[0];
            {
                char *lc = lt_strdup(h);
                lower_inplace(lc);
                if (strncmp(lc, "time", 4) != 0) {
                    lt_err_set("unexpected first column \"%s\"; expected 'Time'", h);
                    lt_free(lc);
                    rc = LT_EDATA;
                    goto done;
                }
                if (strstr(lc, "ms") != NULL) {
                    ms = 1;
                }
                lt_free(lc);
            }
            n_ch = nf - 1;
            while (n_ch > 0 && unquote(fields[n_ch])[0] == '\0') {
                n_ch--;
                dropped++;
            }
            if (n_ch <= 0) {
                lt_err_set("log header has no channel columns");
                rc = LT_EDATA;
                goto done;
            }
            names = (char **)lt_calloc((size_t)n_ch, sizeof(char *));
            units = (char **)lt_calloc((size_t)n_ch, sizeof(char *));
            for (i = 0; i < n_ch; i++) {
                names[i] = lt_strdup(unquote(fields[i + 1]));
                units[i] = lt_strdup("");
            }
            lt_log_set_channels(&tmp, (const char *const *)names, (const char *const *)units,
                                n_ch);
            if (title.len > 0) {
                lt_log_set_header(&tmp, (const char *)title.data);
            }
            vals = (double *)lt_malloc((size_t)n_ch * sizeof(double));
            continue;
        }
        h = lt_str_trim(line);
        if (*h == '\0') {
            continue;
        }
        {
            const char *mt = mark_text(h);
            if (mt != NULL) {
                lt_log_add_marker(&tmp, tmp.n > 0 ? tmp.t[tmp.n - 1] : 0.0, mt);
                continue;
            }
        }
        nf = split_line(h, delim, fields, maxf);
        if (nf > maxf) {
            nf = maxf;   /* absurdly wide row: will fail the count check below */
        }
        for (i = 0; i < nf; i++) {
            fields[i] = unquote(fields[i]);
        }
        /* units line right after the header */
        if (units_possible && tmp.n == 0) {
            int all_text = !is_num(fields[0]);
            for (i = 1; all_text && i < nf; i++) {
                if (is_num(fields[i])) {
                    all_text = 0;
                }
            }
            if (all_text && nf >= 2) {
                for (i = 0; i < n_ch && i + 1 < nf; i++) {
                    lt_free(tmp.units[i]);
                    tmp.units[i] = lt_strdup(unquote(fields[i + 1]));
                }
                if (lt_stricmp(unquote(fields[0]), "ms") == 0) {
                    ms = 1;
                }
                units_possible = 0;
                continue;
            }
        }
        units_possible = 0;
        {
            double t;
            int ok = 1, extra_ok = 1;
            if (!lt_parse_double(unquote(fields[0]), &t)) {
                skipped++;
                continue;
            }
            if (ms) {
                t /= 1000.0;
            }
            for (i = n_ch + 1; i < nf; i++) {
                if (unquote(fields[i])[0] != '\0') {
                    extra_ok = 0;
                }
            }
            if (nf - 1 < n_ch || !extra_ok) {
                skipped++;
                continue;
            }
            for (i = 0; i < n_ch && ok; i++) {
                if (!lt_parse_double(unquote(fields[i + 1]), &vals[i])) {
                    ok = 0;
                }
            }
            if (!ok) {
                skipped++;
                continue;
            }
            if (!(t >= 0.0)) {   /* also maps NaN to 0, like Rust f64::max(0.0) */
                t = 0.0;
            }
            lt_log_append(&tmp, t, vals);
        }
    }
    if (!have_header) {
        lt_err_set("log file has no header row");
        rc = LT_EDATA;
        goto done;
    }
    if (tmp.n == 0) {
        lt_err_set("log has no parsable data rows (%ld skipped)", skipped);
        rc = LT_EDATA;
        goto done;
    }
    *out = tmp;
    lt_log_init(&tmp);
done:
    lt_free(line);
    lt_free(fields);
    lt_free(vals);
    lt_buf_free(&title);
    free_strs(names, n_ch);
    free_strs(units, n_ch);
    lt_log_free(&tmp);
    LT_UNUSED(dropped);
    return rc;
}

int lt_csv_read(const char *path, lt_log *out)
{
    size_t len = 0;
    char *buf;
    int rc;
    lt_log_init(out);
    buf = lt_file_read(path, &len);
    if (!buf) {
        return lt_file_exists(path) ? LT_EIO : LT_ENOENT;
    }
    rc = lt_csv_parse(buf, len, out);
    lt_free(buf);
    return rc;
}

static void put_num(lt_buf *b, double v, int decimals)
{
    char tmp[64];
    if (!lt_isfinite(v)) {
        v = 0.0;
    }
    lt_snprintf(tmp, sizeof tmp, "%.*f", decimals, v);
    lt_buf_puts(b, tmp);
}

int lt_csv_write(const char *path, const lt_log *log)
{
    lt_buf b;
    long i;
    int c, has_units = 0, rc;
    lt_buf_init(&b);
    lt_buf_puts(&b, "Time");
    for (c = 0; c < log->n_ch; c++) {
        lt_buf_putc(&b, ',');
        lt_buf_puts(&b, log->names[c]);
        if (log->units && log->units[c] && log->units[c][0]) {
            has_units = 1;
        }
    }
    lt_buf_putc(&b, '\n');
    if (has_units) {
        lt_buf_puts(&b, "s");
        for (c = 0; c < log->n_ch; c++) {
            lt_buf_putc(&b, ',');
            lt_buf_puts(&b, log->units[c] ? log->units[c] : "");
        }
        lt_buf_putc(&b, '\n');
    }
    for (i = 0; i < log->n; i++) {
        put_num(&b, log->t[i], 3);
        for (c = 0; c < log->n_ch; c++) {
            lt_buf_putc(&b, ',');
            put_num(&b, LT_LOG_AT(log, i, c), 4);
        }
        lt_buf_putc(&b, '\n');
    }
    rc = lt_file_write(path, b.data ? b.data : "", b.len);
    lt_buf_free(&b);
    return rc == 0 ? 0 : LT_EIO;
}
