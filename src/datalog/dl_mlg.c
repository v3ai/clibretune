/* dl_mlg.c - MegaLogViewer MLG v2 reader and writer (big endian), spec 06 section 4.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "datalog.h"

#define MLG_HDR 24
#define MLG_FLD 89
#define MLG_MARK 50

static const lt_u8 mlg_magic[6] = { 'M', 'L', 'V', 'L', 'G', 0 };

static int type_width(int t)
{
    static const int w[8] = { 1, 1, 2, 2, 4, 4, 8, 4 };
    return (t >= 0 && t < 8) ? w[t] : 0;
}

static double decode_raw(int type, const lt_u8 *p)
{
    lt_u32 u;
    switch (type) {
    case LT_MLG_U08: return (double)p[0];
    case LT_MLG_S08: return (double)(p[0] >= 128 ? (int)p[0] - 256 : (int)p[0]);
    case LT_MLG_U16: return (double)lt_rd_be(p, 2);
    case LT_MLG_S16: {
        long v = (long)lt_rd_be(p, 2);
        return (double)(v >= 32768L ? v - 65536L : v);
    }
    case LT_MLG_U32: return (double)lt_rd_be(p, 4);
    case LT_MLG_S32:
        u = lt_rd_be(p, 4) & LT_U32_MASK;
        return u >= 2147483648UL ? (double)u - 4294967296.0 : (double)u;
    case LT_MLG_S64: {
        double hi = decode_raw(LT_MLG_S32, p);
        double lo = (double)(lt_rd_be(p + 4, 4) & LT_U32_MASK);
        return hi * 4294967296.0 + lo;
    }
    default: return lt_f32_from_bits(lt_rd_be(p, 4) & LT_U32_MASK);
    }
}

static void field_str(char *dst, size_t cap, const lt_u8 *src, size_t width)
{
    size_t n = 0;
    while (n < width && src[n] != 0) {
        n++;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    lt_str_trim(dst);
}

/* -------------------------------------------------------------- reader */

typedef struct mlg_clock {
    int have_start;
    double start;       /* absolute ticks */
    long previous;
    double wraps;
} mlg_clock;

static double clock_advance(mlg_clock *c, long raw)
{
    double absolute, el;
    if (c->have_start && c->previous > raw && c->previous - raw > 32768L) {
        c->wraps += 1.0;
    }
    c->previous = raw;
    absolute = c->wraps * 65536.0 + (double)raw;
    if (!c->have_start) {
        c->start = absolute;
        c->have_start = 1;
    }
    el = absolute - c->start;
    return el < 0.0 ? 0.0 : el;
}

int lt_mlg_read_buf(const void *data, size_t len, lt_log *out)
{
    const lt_u8 *d = (const lt_u8 *)data;
    lt_mlg_field *fl = NULL;
    int *off = NULL;
    int nf, i, rec_len, sum = 0, time_idx = -1, n_ch = 0;
    size_t data_off, pos, rec_idx = 0;
    long dropped = 0;
    mlg_clock clk;
    double base = 0.0, last_t = 0.0, last_ticks = 0.0;
    int have_base = 0, rc = 0, version;
    int *col = NULL;     /* field index per log channel */
    double *vals = NULL;
    lt_log tmp;
    const char **names = NULL, **units = NULL;

    lt_log_init(out);
    lt_log_init(&tmp);
    memset(&clk, 0, sizeof clk);
    if (len < MLG_HDR) {
        lt_err_set("file is too short to be an MLG log");
        return LT_EDATA;
    }
    if (memcmp(d, mlg_magic, 6) != 0) {
        lt_err_set("not an MLG log (bad file signature)");
        return LT_EDATA;
    }
    version = (int)lt_rd_be(d + 6, 2);
    if (version != 2) {
        lt_err_set("MLG format version %d is not supported, expected 2", version);
        return LT_EDATA;
    }
    data_off = (size_t)(lt_rd_be(d + 16, 4) & LT_U32_MASK);
    rec_len = (int)lt_rd_be(d + 20, 2);
    nf = (int)lt_rd_be(d + 22, 2);
    if (len < (size_t)MLG_HDR + (size_t)MLG_FLD * (size_t)nf) {
        lt_err_set("log ends inside its field descriptors");
        return LT_EDATA;
    }
    if (data_off < (size_t)MLG_HDR + (size_t)MLG_FLD * (size_t)nf) {
        lt_err_set("log data would start inside its own header");
        return LT_EDATA;
    }
    fl = (lt_mlg_field *)lt_calloc((size_t)(nf > 0 ? nf : 1), sizeof *fl);
    off = (int *)lt_calloc((size_t)(nf > 0 ? nf : 1), sizeof(int));
    for (i = 0; i < nf; i++) {
        const lt_u8 *f = d + MLG_HDR + (size_t)i * MLG_FLD;
        int t = f[0];
        if (type_width(t) == 0) {
            lt_err_set("field %d has unknown storage type %d", i, t);
            rc = LT_EDATA;
            goto done;
        }
        fl[i].type = (lt_mlg_type)t;
        field_str(fl[i].name, sizeof fl[i].name, f + 1, 34);
        field_str(fl[i].units, sizeof fl[i].units, f + 35, 10);
        fl[i].scale = (float)lt_f32_from_bits(lt_rd_be(f + 46, 4) & LT_U32_MASK);
        fl[i].transform = (float)lt_f32_from_bits(lt_rd_be(f + 50, 4) & LT_U32_MASK);
        fl[i].digits = f[54];
        if (!lt_isfinite((double)fl[i].scale) || !lt_isfinite((double)fl[i].transform)) {
            lt_err_set("field %d has a non-finite scale", i);
            rc = LT_EDATA;
            goto done;
        }
        off[i] = sum;
        sum += type_width(t);
        if (time_idx < 0 && lt_stricmp(fl[i].name, "time") == 0 &&
            strcmp(fl[i].units, "s") == 0) {
            time_idx = i;
        }
    }
    if (sum != rec_len) {
        lt_err_set("header declares %d-byte records but the fields need %d", rec_len, sum);
        rc = LT_EDATA;
        goto done;
    }
    /* channels = all fields except a Time field used as the time base */
    col = (int *)lt_calloc((size_t)(nf > 0 ? nf : 1), sizeof(int));
    names = (const char **)lt_calloc((size_t)(nf > 0 ? nf : 1), sizeof(char *));
    units = (const char **)lt_calloc((size_t)(nf > 0 ? nf : 1), sizeof(char *));
    for (i = 0; i < nf; i++) {
        if (i == time_idx) {
            continue;
        }
        col[n_ch] = i;
        names[n_ch] = fl[i].name;
        units[n_ch] = fl[i].units;
        n_ch++;
    }
    lt_log_set_channels(&tmp, names, units, n_ch);
    vals = (double *)lt_calloc((size_t)(n_ch > 0 ? n_ch : 1), sizeof(double));

    pos = data_off;
    while (pos < len) {
        int kind;
        long raw_clock;
        double ticks;
        if (len - pos < 4) {
            lt_err_set("log ends part-way through record %lu", (unsigned long)rec_idx);
            rc = LT_EDATA;
            goto done;
        }
        kind = d[pos];
        raw_clock = (long)lt_rd_be(d + pos + 2, 2);
        ticks = clock_advance(&clk, raw_clock);
        pos += 4;
        if (kind == 0) {
            const lt_u8 *pl;
            lt_u32 sumb = 0;
            int k;
            double tval = 0.0;
            if (len - pos < (size_t)rec_len + 1) {
                lt_err_set("log ends part-way through record %lu", (unsigned long)rec_idx);
                rc = LT_EDATA;
                goto done;
            }
            pl = d + pos;
            for (k = 0; k < rec_len; k++) {
                sumb += pl[k];
            }
            if ((sumb & 0xFFu) != pl[rec_len]) {
                dropped++;
                pos += (size_t)rec_len + 1;
                rec_idx++;
                continue;
            }
            pos += (size_t)rec_len + 1;
            rec_idx++;
            if (time_idx >= 0) {
                tval = (decode_raw(fl[time_idx].type, pl + off[time_idx]) +
                        (double)fl[time_idx].transform) * (double)fl[time_idx].scale;
                if (!lt_isfinite(tval)) {
                    dropped++;
                    continue;
                }
                if (!have_base) {
                    base = tval;
                    have_base = 1;
                }
                tval -= base;
                if (!(tval >= 0.0)) {
                    tval = 0.0;
                }
            } else {
                tval = ticks * 1e-5;
            }
            for (k = 0; k < n_ch; k++) {
                int fi = col[k];
                vals[k] = (decode_raw(fl[fi].type, pl + off[fi]) + (double)fl[fi].transform) *
                          (double)fl[fi].scale;
            }
            lt_log_append(&tmp, tval, vals);
            last_t = tval;
            last_ticks = ticks;
        } else if (kind == 1) {
            char text[MLG_MARK + 1];
            if (len - pos < MLG_MARK) {
                lt_err_set("log ends part-way through record %lu", (unsigned long)rec_idx);
                rc = LT_EDATA;
                goto done;
            }
            field_str(text, sizeof text, d + pos, MLG_MARK);
            pos += MLG_MARK;
            rec_idx++;
            lt_log_add_marker(&tmp, time_idx >= 0 ? last_t + (ticks > last_ticks ?
                              (ticks - last_ticks) * 1e-5 : 0.0) : ticks * 1e-5, text);
        } else {
            lt_err_set("record kind %d at record %lu is not one this reader knows", kind,
                       (unsigned long)rec_idx);
            rc = LT_EDATA;
            goto done;
        }
    }
    if (tmp.n == 0) {
        lt_err_set("MLG log contains no readable records");
        rc = LT_EDATA;
        goto done;
    }
    *out = tmp;
    lt_log_init(&tmp);
done:
    lt_log_free(&tmp);
    lt_free(fl);
    lt_free(off);
    lt_free(col);
    lt_free(names);
    lt_free(units);
    lt_free(vals);
    LT_UNUSED(dropped);
    return rc;
}

int lt_mlg_read(const char *path, lt_log *out)
{
    size_t len = 0;
    char *buf;
    int rc;
    lt_log_init(out);
    buf = lt_file_read(path, &len);
    if (!buf) {
        return lt_file_exists(path) ? LT_EIO : LT_ENOENT;
    }
    rc = lt_mlg_read_buf(buf, len, out);
    lt_free(buf);
    return rc;
}

/* -------------------------------------------------------------- writer */

#define W_BATCH 8192

struct lt_mlg_writer {
    FILE *fp;
    int n_ch;
    int rec_len;          /* 4 * (n_ch + 1) */
    lt_u8 counter;
    int failed;
    lt_buf pend;
};

static void put_fixed(lt_u8 *dst, size_t width, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    memset(dst, 0, width);
    if (n > width - 1) {
        n = width - 1;
    }
    memcpy(dst, s, n);
}

static int w_flush_pending(lt_mlg_writer *w)
{
    if (w->pend.len > 0) {
        if (fwrite(w->pend.data, 1, w->pend.len, w->fp) != w->pend.len) {
            w->failed = 1;
        }
        lt_buf_clear(&w->pend);
    }
    return w->failed ? LT_EIO : 0;
}

lt_mlg_writer *lt_mlg_open(const char *path, const char *const *names, const char *const *units,
                           int n_ch, const char *info)
{
    lt_mlg_writer *w;
    FILE *fp;
    lt_u8 hdr[MLG_HDR], fd[MLG_FLD];
    size_t info_len = info ? strlen(info) : 0;
    int i, nf = n_ch + 1;
    long tsec;

    if (n_ch < 0 || nf > 65535) {
        lt_err_set("invalid MLG channel count %d", n_ch);
        return NULL;
    }
    fp = fopen(path, "wb");
    if (!fp) {
        lt_err_set("cannot open '%s' for writing", path);
        return NULL;
    }
    w = (lt_mlg_writer *)lt_calloc(1, sizeof *w);
    w->fp = fp;
    w->n_ch = n_ch;
    w->rec_len = 4 * nf;
    lt_buf_init(&w->pend);

    tsec = (long)time(NULL);   /* capture time, unix seconds */
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, mlg_magic, 6);
    lt_wr_be(hdr + 6, 2, 2);
    lt_wr_be(hdr + 8, 4, (lt_u32)tsec & LT_U32_MASK);
    lt_wr_be(hdr + 12, 4, (lt_u32)(MLG_HDR + MLG_FLD * nf));
    lt_wr_be(hdr + 16, 4, (lt_u32)(MLG_HDR + MLG_FLD * nf + (long)info_len));
    lt_wr_be(hdr + 20, 2, (lt_u32)w->rec_len);
    lt_wr_be(hdr + 22, 2, (lt_u32)nf);
    lt_buf_append(&w->pend, hdr, sizeof hdr);
    for (i = 0; i < nf; i++) {
        memset(fd, 0, sizeof fd);
        fd[0] = (lt_u8)LT_MLG_F32;
        if (i == 0) {
            put_fixed(fd + 1, 34, "Time");
            put_fixed(fd + 35, 10, "s");
        } else {
            put_fixed(fd + 1, 34, names && names[i - 1] ? names[i - 1] : "");
            put_fixed(fd + 35, 10, units && units[i - 1] ? units[i - 1] : "");
        }
        fd[45] = 0;
        lt_wr_be(fd + 46, 4, lt_f32_to_bits(1.0));
        lt_wr_be(fd + 50, 4, lt_f32_to_bits(0.0));
        fd[54] = 3;
        lt_buf_append(&w->pend, fd, sizeof fd);
    }
    if (info_len > 0) {
        lt_buf_append(&w->pend, info, info_len);
    }
    /* the header stays buffered until the first flush (so open never fails late) */
    return w;
}

static void put_rec_head(lt_mlg_writer *w, int kind, double t_sec)
{
    lt_u8 h[4];
    double ticks = t_sec * 100000.0;
    double m;
    long clk;
    if (!(ticks >= 0.0)) {
        ticks = 0.0;
    }
    m = fmod(floor(ticks + 0.5), 65536.0);
    clk = (long)m;
    h[0] = (lt_u8)kind;
    h[1] = w->counter++;
    lt_wr_be(h + 2, 2, (lt_u32)clk);
    lt_buf_append(&w->pend, h, 4);
}

int lt_mlg_put(lt_mlg_writer *w, double t_sec, const double *values)
{
    lt_u8 payload[4];
    lt_u32 sum = 0;
    int c, k;
    lt_u8 ck;
    if (!w || w->failed) {
        return LT_EIO;
    }
    put_rec_head(w, 0, t_sec);
    for (c = 0; c <= w->n_ch; c++) {
        double v = c == 0 ? t_sec : (values ? values[c - 1] : 0.0);
        lt_wr_be(payload, 4, lt_f32_to_bits(v));
        for (k = 0; k < 4; k++) {
            sum += payload[k];
        }
        lt_buf_append(&w->pend, payload, 4);
    }
    ck = (lt_u8)(sum & 0xFFu);
    lt_buf_append(&w->pend, &ck, 1);
    if (w->pend.len >= W_BATCH) {
        return w_flush_pending(w);
    }
    return 0;
}

int lt_mlg_put_marker(lt_mlg_writer *w, double t_sec, const char *text)
{
    lt_u8 body[MLG_MARK];
    if (!w || w->failed) {
        return LT_EIO;
    }
    put_rec_head(w, 1, t_sec);
    memset(body, 0, sizeof body);
    if (text) {
        size_t n = strlen(text);
        memcpy(body, text, n > MLG_MARK ? MLG_MARK : n);
    }
    lt_buf_append(&w->pend, body, sizeof body);
    if (w->pend.len >= W_BATCH) {
        return w_flush_pending(w);
    }
    return 0;
}

int lt_mlg_flush(lt_mlg_writer *w)
{
    if (!w) {
        return LT_EINVAL;
    }
    if (w_flush_pending(w) != 0) {
        return LT_EIO;
    }
    if (fflush(w->fp) != 0) {
        w->failed = 1;
        return LT_EIO;
    }
    return 0;
}

int lt_mlg_close(lt_mlg_writer *w)
{
    int rc;
    if (!w) {
        return 0;
    }
    rc = lt_mlg_flush(w);
    if (fclose(w->fp) != 0) {
        rc = LT_EIO;
    }
    lt_buf_free(&w->pend);
    lt_free(w);
    if (rc != 0) {
        lt_err_set("write error on MLG file");
    }
    return rc;
}

int lt_mlg_write(const char *path, const lt_log *log, const char *info)
{
    lt_mlg_writer *w;
    long i, m = 0;
    int rc;
    w = lt_mlg_open(path, (const char *const *)log->names, (const char *const *)log->units,
                    log->n_ch, info);
    if (!w) {
        return LT_EIO;
    }
    for (i = 0; i < log->n; i++) {
        while (m < log->n_markers && log->markers[m].t <= log->t[i]) {
            lt_mlg_put_marker(w, log->markers[m].t, log->markers[m].text);
            m++;
        }
        lt_mlg_put(w, log->t[i], log->v + (size_t)i * (size_t)log->n_ch);
    }
    while (m < log->n_markers) {
        lt_mlg_put_marker(w, log->markers[m].t, log->markers[m].text);
        m++;
    }
    rc = lt_mlg_close(w);
    return rc;
}
