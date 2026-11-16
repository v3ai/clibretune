/* test_datalog.c - tests for src/datalog (MLG, CSV, recorder, player, dyno, math). */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "lt.h"
#include "lt_codes.h"
#include "datalog/datalog.h"
#include "plat/plat.h"
#include "lt_test.h"

#define TMP "build/dl_tmp_"

/* ------------------------------------------------------------ MLG builders */

typedef struct tfield { int type; const char *name, *units; double scale, transform; } tfield;

static void b_f32(lt_buf *b, double v)
{
    lt_u8 x[4];
    lt_wr_be(x, 4, lt_f32_to_bits(v));
    lt_buf_append(b, x, 4);
}
static void b_be(lt_buf *b, int n, lt_u32 v)
{
    lt_u8 x[4];
    lt_wr_be(x, n, v);
    lt_buf_append(b, x, (size_t)n);
}

static void mlg_head(lt_buf *b, int version, const tfield *f, int nf, const char *info,
                     int rec_len)
{
    int i;
    lt_u8 d[89];
    size_t il = info ? strlen(info) : 0;
    lt_buf_append(b, "MLVLG\0", 6);
    b_be(b, 2, (lt_u32)version);
    b_be(b, 4, 1700000000UL);
    b_be(b, 4, (lt_u32)(24 + 89 * nf));
    b_be(b, 4, (lt_u32)(24 + 89 * nf + (int)il));
    b_be(b, 2, (lt_u32)rec_len);
    b_be(b, 2, (lt_u32)nf);
    for (i = 0; i < nf; i++) {
        memset(d, 0, sizeof d);
        d[0] = (lt_u8)f[i].type;
        strncpy((char *)d + 1, f[i].name, 33);
        strncpy((char *)d + 35, f[i].units, 9);
        lt_wr_be(d + 46, 4, lt_f32_to_bits(f[i].scale));
        lt_wr_be(d + 50, 4, lt_f32_to_bits(f[i].transform));
        d[54] = 3;
        lt_buf_append(b, d, sizeof d);
    }
    if (il) {
        lt_buf_append(b, info, il);
    }
}

static int widths(const tfield *f, int nf)
{
    static const int w[8] = { 1, 1, 2, 2, 4, 4, 8, 4 };
    int i, s = 0;
    for (i = 0; i < nf; i++) {
        s += w[f[i].type];
    }
    return s;
}

/* record with prebuilt payload; corrupt != 0 flips the checksum */
static void add_rec(lt_buf *b, int ctr, int clk, const lt_buf *payload, int corrupt)
{
    size_t i;
    lt_u32 sum = 0;
    lt_buf_putc(b, 0);
    lt_buf_putc(b, ctr);
    b_be(b, 2, (lt_u32)clk);
    lt_buf_append(b, payload->data, payload->len);
    for (i = 0; i < payload->len; i++) {
        sum += (lt_u8)payload->data[i];
    }
    lt_buf_putc(b, (int)((sum + (corrupt ? 1 : 0)) & 0xFF));
}

static void add_marker(lt_buf *b, int ctr, int clk, const char *text)
{
    char t[50];
    memset(t, 0, sizeof t);
    memcpy(t, text, strlen(text) < 50 ? strlen(text) : 50);
    lt_buf_putc(b, 1);
    lt_buf_putc(b, ctr);
    b_be(b, 2, (lt_u32)clk);
    lt_buf_append(b, t, 50);
}

static const tfield F_TIME = { LT_MLG_F32, "Time", "s", 1.0, 0.0 };
static const tfield F_CLT = { LT_MLG_U16, "CLT", "deg C", 0.01, 0.0 };
static const tfield F_STFT = { LT_MLG_F32, "STFT: Bank 1", "%", 100.0, -1.0 };

/* payload Time,CLT,STFT */
static void pl3(lt_buf *p, double t, int clt, double stft)
{
    lt_buf_clear(p);
    b_f32(p, t);
    b_be(p, 2, (lt_u32)clt);
    b_f32(p, stft);
}

static void base_file(lt_buf *b, int version)
{
    tfield f[3];
    f[0] = F_TIME; f[1] = F_CLT; f[2] = F_STFT;
    mlg_head(b, version, f, 3, NULL, widths(f, 3));
}

LT_TEST(mlg_spec_basic)
{
    lt_buf b, p;
    lt_log l;
    lt_buf_init(&b); lt_buf_init(&p);
    base_file(&b, 2);
    pl3(&p, 0.0, 8741, 0.9352285); add_rec(&b, 0, 1000, &p, 0);
    pl3(&p, 0.01, 8747, 1.0);      add_rec(&b, 1, 2000, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), 0);
    LT_CHECK_INT(l.n_ch, 2);
    LT_CHECK_STR(l.names[0], "CLT");
    LT_CHECK_STR(l.names[1], "STFT: Bank 1");
    LT_CHECK_STR(l.units[0], "deg C");
    LT_CHECK_INT(l.n, 2);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 0, 0), 87.41, 1e-4);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 0), 87.47, 1e-4);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 0, 1), (0.9352285 - 1.0) * 100.0, 1e-4);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 1), 0.0, 1e-9);
    LT_CHECK_NEAR(l.t[0], 0.0, 1e-9);
    LT_CHECK_NEAR(l.t[1], 0.01, 1e-6);
    lt_log_free(&l);
    lt_buf_free(&b); lt_buf_free(&p);
}

static void clock_file(lt_buf *b)
{
    tfield f[2];
    f[0] = F_CLT; f[1] = F_STFT;
    mlg_head(b, 2, f, 2, NULL, widths(f, 2));
}
static void pl2(lt_buf *p, int clt, double stft)
{
    lt_buf_clear(p);
    b_be(p, 2, (lt_u32)clt);
    b_f32(p, stft);
}

LT_TEST(mlg_clock_timestamps)
{
    lt_buf b, p;
    lt_log l;
    lt_buf_init(&b); lt_buf_init(&p);
    clock_file(&b);
    pl2(&p, 8741, 1.0); add_rec(&b, 0, 1000, &p, 0);
    pl2(&p, 8747, 1.0); add_rec(&b, 1, 2000, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), 0);
    LT_CHECK_INT(l.n_ch, 2);
    LT_CHECK_NEAR(l.t[0], 0.0, 1e-12);
    LT_CHECK_NEAR(l.t[1], 0.010, 1e-9);   /* 1000 ticks of 10 us */
    lt_log_free(&l);

    /* marker first: clock starts at the first record of any kind */
    lt_buf_clear(&b);
    clock_file(&b);
    add_marker(&b, 0, 1000, "MARK 000 - Going Online");
    pl2(&p, 8741, 1.0); add_rec(&b, 1, 3000, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), 0);
    LT_CHECK_INT(l.n, 1);
    LT_CHECK_NEAR(l.t[0], 0.020, 1e-9);
    LT_CHECK_INT(l.n_markers, 1);
    LT_CHECK_STR(l.markers[0].text, "MARK 000 - Going Online");
    LT_CHECK_NEAR(l.markers[0].t, 0.0, 1e-12);
    lt_log_free(&l);

    /* wrap: 65000 then 200 */
    lt_buf_clear(&b);
    clock_file(&b);
    pl2(&p, 1, 1.0); add_rec(&b, 0, 65000, &p, 0);
    add_rec(&b, 1, 200, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), 0);
    LT_CHECK_NEAR(l.t[1], 0.00736, 1e-9);
    lt_log_free(&l);

    /* small backward step is not a wrap: 1000, 900, 1100 */
    lt_buf_clear(&b);
    clock_file(&b);
    add_rec(&b, 0, 1000, &p, 0);
    add_rec(&b, 1, 900, &p, 0);
    add_rec(&b, 2, 1100, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), 0);
    LT_CHECK_INT(l.n, 3);
    LT_CHECK_NEAR(l.t[1], 0.0, 1e-12);
    LT_CHECK_NEAR(l.t[2], 0.001, 1e-9);
    lt_log_free(&l);

    /* corrupt record at clock 60000 between 1000 and 200: clock still advances */
    lt_buf_clear(&b);
    clock_file(&b);
    add_rec(&b, 0, 1000, &p, 0);
    add_rec(&b, 1, 60000, &p, 1);
    add_rec(&b, 2, 200, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), 0);
    LT_CHECK_INT(l.n, 2);
    LT_CHECK_NEAR(l.t[1], 0.64736, 1e-9);
    lt_log_free(&l);

    /* marker at 60000 gives the same result */
    lt_buf_clear(&b);
    clock_file(&b);
    add_rec(&b, 0, 1000, &p, 0);
    add_marker(&b, 1, 60000, "x");
    add_rec(&b, 2, 200, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), 0);
    LT_CHECK_INT(l.n, 2);
    LT_CHECK_INT(l.n_markers, 1);
    LT_CHECK_NEAR(l.t[1], 0.64736, 1e-9);
    lt_log_free(&l);
    lt_buf_free(&b); lt_buf_free(&p);
}

LT_TEST(mlg_errors)
{
    lt_buf b, p;
    lt_log l;
    size_t full;
    lt_buf_init(&b); lt_buf_init(&p);
    base_file(&b, 1);
    pl3(&p, 0, 1, 1); add_rec(&b, 0, 1, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "version 1") != NULL);
    LT_CHECK_INT(lt_mlg_read_buf("not an mlg file at all, definitely not", 38, &l), LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "signature") != NULL);
    LT_CHECK_INT(lt_mlg_read_buf("MLV", 3, &l), LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "too short") != NULL);

    /* zero records */
    lt_buf_clear(&b);
    base_file(&b, 2);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "no readable records") != NULL);

    /* truncated mid record */
    pl3(&p, 0, 1, 1); add_rec(&b, 0, 1, &p, 0);
    full = b.len;
    LT_CHECK_INT(lt_mlg_read_buf(b.data, full, &l), 0);
    lt_log_free(&l);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, full - 3, &l), LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "part-way") != NULL);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, full + 2, &l), LT_EDATA);   /* 2 stray bytes */

    /* unknown kind */
    lt_buf_putc(&b, 7); lt_buf_putc(&b, 0); b_be(&b, 2, 5);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "kind 7") != NULL);

    /* header problems */
    lt_buf_clear(&b);
    base_file(&b, 2);
    b.data[22] = 0; b.data[23] = 3;
    b.data[20] = 0; b.data[21] = 13;
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), LT_EDATA);   /* sum is 10 */
    LT_CHECK(strstr(lt_err_get(), "13-byte") != NULL && strstr(lt_err_get(), "10") != NULL);
    b.data[21] = 10;
    b.data[19] = 24;   /* data offset inside header */
    b.data[18] = 0; b.data[17] = 0; b.data[16] = 0;
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "inside its own header") != NULL);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, 24 + 89, &l), LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "field descriptors") != NULL);

    lt_buf_clear(&b);
    base_file(&b, 2);
    b.data[24] = 9;   /* bad storage type */
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "unknown storage type") != NULL);
    b.data[24] = 0;
    lt_wr_be((lt_u8 *)b.data + 24 + 46, 4, 0x7F800000UL);   /* inf scale */
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "non-finite") != NULL);
    lt_buf_free(&b); lt_buf_free(&p);
}

LT_TEST(mlg_checksum_marker_info_and_files)
{
    lt_buf b, p;
    lt_log l;
    tfield f[2];
    lt_buf_init(&b); lt_buf_init(&p);
    clock_file(&b);
    pl2(&p, 8741, 1.0); add_rec(&b, 0, 1000, &p, 0);
    pl2(&p, 8745, 1.0); add_rec(&b, 1, 1100, &p, 1);   /* bad checksum: skipped */
    pl2(&p, 8747, 1.0); add_rec(&b, 2, 1200, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), 0);
    LT_CHECK_INT(l.n, 2);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 0), 87.47, 1e-4);
    lt_log_free(&l);

    lt_buf_clear(&b);
    clock_file(&b);
    pl2(&p, 8741, 1.0); add_rec(&b, 0, 1000, &p, 0);
    add_marker(&b, 1, 1050, "between");
    pl2(&p, 8747, 1.0); add_rec(&b, 2, 1100, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), 0);
    LT_CHECK_INT(l.n, 2);
    LT_CHECK_INT(l.n_markers, 1);
    LT_CHECK_NEAR(l.markers[0].t, 0.0005, 1e-9);
    lt_log_free(&l);

    /* info text between descriptors and data */
    lt_buf_clear(&b);
    f[0] = F_CLT; f[1] = F_STFT;
    mlg_head(&b, 2, f, 2, "<msq>tune text</msq>", widths(f, 2));
    pl2(&p, 8741, 1.0); add_rec(&b, 0, 1000, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), 0);
    LT_CHECK_INT(l.n, 1);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 0, 0), 87.41, 1e-4);
    lt_log_free(&l);
    /* via file API and dispatch */
    LT_CHECK_INT(lt_file_write(TMP "a.MLG", b.data, b.len), 0);
    LT_CHECK_INT(lt_log_read(TMP "a.MLG", &l), 0);
    LT_CHECK_INT(l.n, 1);
    lt_log_free(&l);
    LT_CHECK_INT(lt_log_read(TMP "a.ltlog", &l), LT_EINVAL);
    LT_CHECK_INT(lt_mlg_read(TMP "missing.mlg", &l), LT_ENOENT);
    lt_plat_remove(TMP "a.MLG");
    lt_buf_free(&b); lt_buf_free(&p);
}

LT_TEST(mlg_time_channel)
{
    lt_buf b, p;
    lt_log l;
    tfield f[2];
    lt_buf_init(&b); lt_buf_init(&p);
    f[0] = F_TIME; f[1] = F_CLT;
    mlg_head(&b, 2, f, 2, NULL, widths(f, 2));
    lt_buf_clear(&p); b_f32(&p, 10.0); b_be(&p, 2, 100); add_rec(&b, 0, 5, &p, 0);
    lt_buf_clear(&p); b_f32(&p, 4010.0); b_be(&p, 2, 200); add_rec(&b, 1, 6, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), 0);
    LT_CHECK_NEAR(l.t[0], 0.0, 1e-9);
    LT_CHECK_NEAR(l.t[1], 4000.0, 1e-9);     /* clock ignored */
    lt_log_free(&l);

    /* NaN first record dropped, baseline from the first finite value */
    lt_buf_clear(&b);
    mlg_head(&b, 2, f, 2, NULL, widths(f, 2));
    lt_buf_clear(&p); b_be(&p, 4, 0x7FC00000UL); b_be(&p, 2, 1); add_rec(&b, 0, 1, &p, 0);
    lt_buf_clear(&p); b_f32(&p, 10.0); b_be(&p, 2, 2); add_rec(&b, 1, 2, &p, 0);
    lt_buf_clear(&p); b_f32(&p, 12.0); b_be(&p, 2, 3); add_rec(&b, 2, 3, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), 0);
    LT_CHECK_INT(l.n, 2);
    LT_CHECK_NEAR(l.t[0], 0.0, 1e-9);
    LT_CHECK_NEAR(l.t[1], 2.0, 1e-9);
    lt_log_free(&l);

    /* "Time" with other units is an ordinary channel and the clock is used */
    lt_buf_clear(&b);
    f[0].units = "ms";
    mlg_head(&b, 2, f, 2, NULL, widths(f, 2));
    lt_buf_clear(&p); b_f32(&p, 10.0); b_be(&p, 2, 1); add_rec(&b, 0, 100, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), 0);
    LT_CHECK_INT(l.n_ch, 2);
    LT_CHECK_STR(l.names[0], "Time");
    lt_buf_free(&b); lt_buf_free(&p);
    lt_log_free(&l);
}

LT_TEST(mlg_scalar_types)
{
    tfield f[8];
    lt_buf b, p;
    lt_log l;
    int i;
    static const char *nm[8] = { "u8", "i8", "u16", "i16", "u32", "i32", "i64", "f32" };
    lt_buf_init(&b); lt_buf_init(&p);
    for (i = 0; i < 8; i++) {
        f[i].type = i; f[i].name = nm[i]; f[i].units = ""; f[i].scale = 1.0; f[i].transform = 0.0;
    }
    f[1].scale = 2.0; f[1].transform = 1.0;   /* (raw + 1) * 2 */
    mlg_head(&b, 2, f, 8, NULL, widths(f, 8));
    lt_buf_clear(&p);
    lt_buf_putc(&p, 200);                 /* u8 */
    lt_buf_putc(&p, 0xFE);                /* i8 = -2 -> (-2+1)*2 = -2 */
    b_be(&p, 2, 0xFFFE);                  /* u16 65534 */
    b_be(&p, 2, 0xFFFE);                  /* i16 -2 */
    b_be(&p, 4, 0xFFFFFFFEUL);            /* u32 */
    b_be(&p, 4, 0xFFFFFFFEUL);            /* i32 -2 */
    b_be(&p, 4, 0xFFFFFFFFUL); b_be(&p, 4, 0xFFFFFFFDUL);   /* i64 -3 */
    b_f32(&p, -1.5);
    add_rec(&b, 0, 1, &p, 0);
    lt_buf_clear(&p);
    lt_buf_putc(&p, 1); lt_buf_putc(&p, 0x7F);
    b_be(&p, 2, 1); b_be(&p, 2, 0x7FFF); b_be(&p, 4, 70000UL); b_be(&p, 4, 0x7FFFFFFFUL);
    b_be(&p, 4, 1); b_be(&p, 4, 5);   /* i64 = 2^32 + 5 */
    b_f32(&p, 2.25);
    add_rec(&b, 1, 2, &p, 0);
    LT_CHECK_INT(lt_mlg_read_buf(b.data, b.len, &l), 0);
    LT_CHECK_INT(l.n_ch, 8);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 0, 0), 200, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 0, 1), -2, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 0, 2), 65534, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 0, 3), -2, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 0, 4), 4294967294.0, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 0, 5), -2, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 0, 6), -3, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 0, 7), -1.5, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 1), 256, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 3), 32767, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 4), 70000, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 5), 2147483647.0, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 6), 4294967301.0, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 7), 2.25, 0);
    lt_log_free(&l);
    lt_buf_free(&b); lt_buf_free(&p);
}

/* ------------------------------------------------------------ MLG writer */

static lt_log make_log(int n_ch, long n)
{
    static const char *names[3] = { "RPM", "MAP", "AFR" };
    static const char *units[3] = { "rpm", "kPa", "" };
    lt_log l;
    long i;
    int c;
    double v[3];
    lt_log_init(&l);
    lt_log_set_channels(&l, names, units, n_ch);
    for (i = 0; i < n; i++) {
        for (c = 0; c < n_ch; c++) {
            v[c] = (c == 0) ? 800.0 + 25.0 * (double)i : (c == 1 ? 30.5 + (double)i : 14.7);
        }
        lt_log_append(&l, 0.05 * (double)i, v);
    }
    return l;
}

LT_TEST(mlg_writer_bytes_and_roundtrip)
{
    lt_log l = make_log(3, 4), r;
    size_t len = 0, i;
    lt_u8 *f;
    lt_u32 sum = 0;
    lt_log_add_marker(&l, 0.06, "hello marker");
    LT_CHECK_INT(lt_mlg_write(TMP "w.mlg", &l, "INFO"), 0);
    f = (lt_u8 *)lt_file_read(TMP "w.mlg", &len);
    LT_CHECK(f != NULL);
    /* header, 4 fields (Time + 3), info "INFO", 4 data records of 4*4+1+4, 1 marker 54 */
    LT_CHECK_INT(len, 24 + 89 * 4 + 4 + 4 * 21 + 54);
    LT_CHECK(memcmp(f, "MLVLG\0", 6) == 0);
    LT_CHECK_INT(lt_rd_be(f + 6, 2), 2);
    LT_CHECK_INT(lt_rd_be(f + 12, 4), 24 + 89 * 4);
    LT_CHECK_INT(lt_rd_be(f + 16, 4), 24 + 89 * 4 + 4);
    LT_CHECK_INT(lt_rd_be(f + 20, 2), 16);
    LT_CHECK_INT(lt_rd_be(f + 22, 2), 4);
    LT_CHECK_INT(f[24], LT_MLG_F32);
    LT_CHECK_STR((const char *)f + 25, "Time");
    LT_CHECK_STR((const char *)f + 59, "s");
    LT_CHECK_INT(lt_rd_be(f + 24 + 46, 4), 0x3F800000UL);   /* scale 1.0 */
    LT_CHECK_INT(lt_rd_be(f + 24 + 50, 4), 0);
    LT_CHECK_STR((const char *)f + 24 + 89 + 1, "RPM");
    LT_CHECK_STR((const char *)f + 24 + 89 + 35, "rpm");
    LT_CHECK(memcmp(f + 24 + 89 * 4, "INFO", 4) == 0);
    {
        const lt_u8 *rec = f + 24 + 89 * 4 + 4;
        LT_CHECK_INT(rec[0], 0);
        LT_CHECK_INT(rec[1], 0);                       /* counter */
        LT_CHECK_INT(lt_rd_be(rec + 2, 2), 0);         /* clock */
        LT_CHECK_INT(lt_rd_be(rec + 4 + 4, 4), lt_f32_to_bits(800.0));
        for (i = 0; i < 16; i++) {
            sum += rec[4 + i];
        }
        LT_CHECK_INT(rec[20], (int)(sum & 0xFF));
        rec += 21;                                      /* record 1: t = 0.05 -> 5000 ticks */
        LT_CHECK_INT(rec[1], 1);
        LT_CHECK_INT(lt_rd_be(rec + 2, 2), 5000);
        rec += 21 * 3;                                  /* marker follows the 4th record? */
    }
    lt_free(f);
    LT_CHECK_INT(lt_mlg_read(TMP "w.mlg", &r), 0);
    LT_CHECK_INT(r.n_ch, 3);
    LT_CHECK_INT(r.n, 4);
    LT_CHECK_STR(r.names[1], "MAP");
    LT_CHECK_STR(r.units[0], "rpm");
    LT_CHECK_STR(r.units[2], "");
    for (i = 0; i < 4; i++) {
        LT_CHECK_NEAR(r.t[i], 0.05 * (double)i, 1e-6);
        LT_CHECK_NEAR(LT_LOG_AT(&r, i, 0), 800.0 + 25.0 * (double)i, 1e-3);
        LT_CHECK_NEAR(LT_LOG_AT(&r, i, 1), 30.5 + (double)i, 1e-4);
        LT_CHECK_NEAR(LT_LOG_AT(&r, i, 2), 14.7, 1e-5);
    }
    LT_CHECK_INT(r.n_markers, 1);
    LT_CHECK_STR(r.markers[0].text, "hello marker");
    LT_CHECK_NEAR(r.markers[0].t, 0.06, 1e-6);
    lt_log_free(&r);

    /* long names truncated, empty log with channels fails to read (no records) */
    lt_log_free(&l);
    lt_log_init(&l);
    {
        const char *nm[1];
        nm[0] = "0123456789012345678901234567890123456789";
        lt_log_set_channels(&l, nm, NULL, 1);
        {
            double v = 1.0;
            lt_log_append(&l, 0.0, &v);
        }
        LT_CHECK_INT(lt_log_write(TMP "w2.mlg", &l), 0);
        LT_CHECK_INT(lt_log_read(TMP "w2.mlg", &r), 0);
        LT_CHECK_INT(strlen(r.names[0]), 33);
        lt_log_free(&r);
    }
    lt_log_free(&l);
    lt_plat_remove(TMP "w.mlg");
    lt_plat_remove(TMP "w2.mlg");
}

LT_TEST(mlg_writer_large_streaming)
{
    lt_mlg_writer *w;
    const char *names[2];
    double v[2];
    long i;
    lt_log r;
    names[0] = "a"; names[1] = "b";
    w = lt_mlg_open(TMP "big.mlg", names, NULL, 2, NULL);
    LT_CHECK(w != NULL);
    for (i = 0; i < 5000; i++) {
        v[0] = (double)i; v[1] = -(double)i;
        LT_CHECK_INT(lt_mlg_put(w, (double)i * 0.01, v), 0);
        if (i == 2500) {
            LT_CHECK_INT(lt_mlg_flush(w), 0);
        }
    }
    LT_CHECK_INT(lt_mlg_close(w), 0);
    LT_CHECK_INT(lt_mlg_read(TMP "big.mlg", &r), 0);
    LT_CHECK_INT(r.n, 5000);
    LT_CHECK_NEAR(LT_LOG_AT(&r, 4999, 1), -4999.0, 1e-6);
    /* the 8-bit record counter wraps and the 16-bit clock wraps (655 s span here is 50 s) */
    LT_CHECK_NEAR(r.t[4999], 49.99, 1e-3);
    lt_log_free(&r);
    LT_CHECK(lt_mlg_open("build/no_such_dir_xyz/f.mlg", names, NULL, 2, NULL) == NULL);
    lt_plat_remove(TMP "big.mlg");
}

/* ------------------------------------------------------------------ CSV */

LT_TEST(csv_spec_cases)
{
    lt_log l;
    const char *t1 = "\xEF\xBB\xBFTime,rpm,map\n0.0,900,50\n\nnot-a-number,1,2\n1.0,1000\n"
                     "1.5,1100,60\n";
    LT_CHECK_INT(lt_csv_parse(t1, strlen(t1), &l), 0);
    LT_CHECK_INT(l.n_ch, 2);
    LT_CHECK_STR(l.names[0], "rpm");
    LT_CHECK_STR(l.names[1], "map");
    LT_CHECK_INT(l.n, 2);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 0), 1100, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 1), 60, 0);
    LT_CHECK_NEAR(l.t[1], 1.5, 0);
    lt_log_free(&l);

    LT_CHECK_INT(lt_csv_parse("", 0, &l), LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "no header") != NULL);
    LT_CHECK_INT(lt_csv_parse("Time,rpm\n", 9, &l), LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "no parsable data rows") != NULL);
    LT_CHECK_INT(lt_csv_parse("Foo,rpm\n1,2\n", 12, &l), LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "unexpected first column") != NULL);
    LT_CHECK_INT(lt_csv_parse("Time\n1\n", 7, &l), LT_EDATA);
    LT_CHECK(strstr(lt_err_get(), "no channel columns") != NULL);
    LT_CHECK_INT(lt_csv_parse("Time,rpm\n0.0,900\n", 17, &l), 0);
    LT_CHECK_INT(l.n, 1);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 0, 0), 900, 0);
    lt_log_free(&l);
    LT_CHECK_INT(lt_csv_read(TMP "none.csv", &l), LT_ENOENT);
}

LT_TEST(csv_tolerance_and_units)
{
    lt_log l;
    const char *ms = "Time (ms),rpm,\r\n0,900,\r\n500,1000,\r\n-5,1,\r\n";
    const char *u = "Time,RPM,MAP\ns,rpm,kPa\n0.0,900,50\n0.5,950,51\n";
    const char *tab = "Time\tRPM\n0\t100\n1\t200\n";
    const char *q = "\"Time\",\"RPM\"\n0,5\n";
    const char *msu = "Time,RPM\nms,rpm\n0,5\n250,6\n";
    LT_CHECK_INT(lt_csv_parse(ms, strlen(ms), &l), 0);
    LT_CHECK_INT(l.n_ch, 1);
    LT_CHECK_INT(l.n, 3);
    LT_CHECK_NEAR(l.t[1], 0.5, 1e-12);
    LT_CHECK_NEAR(l.t[2], 0.0, 0);   /* negative clamped */
    lt_log_free(&l);
    LT_CHECK_INT(lt_csv_parse(u, strlen(u), &l), 0);
    LT_CHECK_INT(l.n, 2);
    LT_CHECK_STR(l.units[0], "rpm");
    LT_CHECK_STR(l.units[1], "kPa");
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 1), 51, 0);
    lt_log_free(&l);
    LT_CHECK_INT(lt_csv_parse(tab, strlen(tab), &l), 0);
    LT_CHECK_INT(l.n, 2);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 0), 200, 0);
    lt_log_free(&l);
    LT_CHECK_INT(lt_csv_parse(q, strlen(q), &l), 0);
    LT_CHECK_STR(l.names[0], "RPM");
    lt_log_free(&l);
    LT_CHECK_INT(lt_csv_parse(msu, strlen(msu), &l), 0);
    LT_CHECK_NEAR(l.t[1], 0.25, 1e-12);
    lt_log_free(&l);
}

LT_TEST(csv_write_roundtrip)
{
    lt_log l, r;
    size_t len = 0;
    char *txt;
    static const char *names[2] = { "rpm", "map" };
    double v[2];
    lt_log_init(&l);
    lt_log_set_channels(&l, names, NULL, 2);
    v[0] = 900; v[1] = 50.12345;  lt_log_append(&l, 0.0, v);
    v[0] = 1000.5; v[1] = -3;     lt_log_append(&l, 0.5, v);
    LT_CHECK_INT(lt_log_write(TMP "o.csv", &l), 0);
    txt = lt_file_read(TMP "o.csv", &len);
    LT_CHECK_STR(txt, "Time,rpm,map\n0.000,900.0000,50.1235\n0.500,1000.5000,-3.0000\n");
    lt_free(txt);
    LT_CHECK_INT(lt_log_read(TMP "o.csv", &r), 0);
    LT_CHECK_INT(r.n, 2);
    LT_CHECK_NEAR(LT_LOG_AT(&r, 1, 0), 1000.5, 0);
    LT_CHECK_NEAR(r.t[1], 0.5, 0);
    lt_log_free(&r);
    lt_log_free(&l);

    /* with units: TunerStudio-style second line, and it round-trips */
    l = make_log(2, 2);
    LT_CHECK_INT(lt_csv_write(TMP "u.csv", &l), 0);
    txt = lt_file_read(TMP "u.csv", &len);
    LT_CHECK(strncmp(txt, "Time,RPM,MAP\ns,rpm,kPa\n0.000,800.0000,30.5000\n", 44) == 0);
    lt_free(txt);
    LT_CHECK_INT(lt_csv_read(TMP "u.csv", &r), 0);
    LT_CHECK_STR(r.units[1], "kPa");
    LT_CHECK_INT(r.n, 2);
    lt_log_free(&r);
    lt_log_free(&l);
    lt_plat_remove(TMP "o.csv");
    lt_plat_remove(TMP "u.csv");
}

LT_TEST(csv_fixture)
{
    lt_log l;
    LT_CHECK_INT(lt_log_read("tests/fixtures/datalog/ts_style.csv", &l), 0);
    LT_CHECK_INT(l.n_ch, 3);
    LT_CHECK_INT(l.n, 3);
    LT_CHECK_STR(l.units[1], "kPa");
    LT_CHECK_NEAR(LT_LOG_AT(&l, 2, 2), 13.2, 0);
    LT_CHECK_NEAR(lt_log_duration(&l), 0.2, 0);
    lt_log_free(&l);
}

LT_TEST(log_model_and_format)
{
    lt_log l = make_log(3, 3);
    LT_CHECK_INT(lt_log_channel_index(&l, "MAP"), 1);
    LT_CHECK_INT(lt_log_channel_index(&l, "map"), -1);
    LT_CHECK_NEAR(lt_log_duration(&l), 0.1, 1e-12);
    LT_CHECK_INT(lt_log_format_from_ext("a/b.CSV"), LT_FMT_CSV);
    LT_CHECK_INT(lt_log_format_from_ext("x.mlg"), LT_FMT_MLG);
    LT_CHECK_INT(lt_log_format_from_ext("x.ltlog"), LT_FMT_NONE);
    LT_CHECK_INT(lt_log_format_from_ext("noext"), LT_FMT_NONE);
    LT_CHECK_STR(lt_log_format_ext(LT_FMT_MLG), "mlg");
    LT_CHECK_STR(lt_log_format_ext(LT_FMT_CSV), "csv");
    LT_CHECK_INT(lt_log_write("x.bin", &l), LT_EINVAL);
    lt_log_set_channels(&l, NULL, NULL, 0);
    LT_CHECK_INT(l.n, 0);
    LT_CHECK_NEAR(lt_log_duration(&l), 0.0, 0);
    lt_log_free(&l);
}

/* ------------------------------------------------------------- recorder */

static lt_recorder *new_rec(void)
{
    static const char *names[2] = { "rpm", "map" };
    static const char *units[2] = { "rpm", "kPa" };
    return lt_rec_new(names, units, 2);
}

LT_TEST(recorder_basic_rate_malformed)
{
    lt_recorder *r = new_rec();
    double v[2], bad1[1], bad3[3];
    lt_log log;
    v[0] = 1000; v[1] = 50;
    bad1[0] = 1000; bad3[0] = bad3[1] = bad3[2] = 1;
    LT_CHECK_INT(lt_rec_record_at(r, 0, v, 2), 0);   /* not recording */
    LT_CHECK_INT(lt_rec_start(r, TMP "r1.mlg"), 0);
    LT_CHECK(lt_rec_is_recording(r));
    LT_CHECK_INT(lt_rec_record_at(r, 1000.0, v, 2), 1);
    LT_CHECK_INT(lt_rec_record_at(r, 1050.0, v, 2), 0);   /* 10 Hz: dropped */
    LT_CHECK_INT(lt_rec_record_at(r, 1099.0, v, 2), 0);
    LT_CHECK_INT(lt_rec_record_at(r, 1100.0, v, 2), 1);
    LT_CHECK_INT(lt_rec_record_at(r, 1100.0, bad1, 1), LT_EINVAL);   /* before rate limiter */
    LT_CHECK_INT(lt_rec_record_at(r, 1300.0, bad3, 3), LT_EINVAL);
    LT_CHECK_INT(lt_rec_count(r), 2);
    LT_CHECK_INT(lt_rec_malformed(r), 2);
    LT_CHECK_NEAR(lt_rec_duration(r), 0.1, 1e-9);
    LT_CHECK_STR(lt_rec_path(r), TMP "r1.mlg");
    lt_rec_marker_at(r, 1150.0, "go");
    LT_CHECK_INT(lt_rec_stop(r), 0);
    LT_CHECK(!lt_rec_is_recording(r));
    LT_CHECK_STR(lt_rec_path(r), TMP "r1.mlg");
    LT_CHECK_INT(lt_rec_record_at(r, 5000.0, v, 2), 0);
    LT_CHECK_INT(lt_mlg_read(TMP "r1.mlg", &log), 0);
    LT_CHECK_INT(log.n, 2);
    LT_CHECK_INT(log.n_markers, 1);
    LT_CHECK_NEAR(log.t[1], 0.1, 1e-6);
    LT_CHECK_STR(log.units[1], "kPa");
    lt_log_free(&log);
    lt_rec_free(r);
    lt_plat_remove(TMP "r1.mlg");
}

LT_TEST(recorder_rate_clamp)
{
    lt_recorder *r = new_rec();
    double v[2];
    int i, ok = 0;
    v[0] = 1; v[1] = 2;
    lt_rec_set_sample_rate(r, 0.0);   /* clamps to 1 Hz */
    lt_rec_start(r, NULL);
    for (i = 0; i < 40; i++) {
        ok += lt_rec_record_at(r, 100.0 * i, v, 2);   /* 10 Hz offered, 4 s */
    }
    LT_CHECK_INT(ok, 4);
    lt_rec_start(r, NULL);
    lt_rec_set_sample_rate(r, 1000.0);   /* clamps to 200 Hz = 5 ms */
    ok = 0;
    for (i = 0; i < 40; i++) {
        ok += lt_rec_record_at(r, 1.0 * i, v, 2);
    }
    LT_CHECK_INT(ok, 8);   /* t = 0,5,...,35 */
    lt_rec_free(r);
}

LT_TEST(recorder_tail_and_live_window)
{
    lt_recorder *r = new_rec();
    double v[2];
    long i;
    lt_log w, f;
    lt_rec_set_sample_rate(r, 200.0);
    LT_CHECK_INT(lt_rec_start(r, TMP "r2.mlg"), 0);
    for (i = 0; i < 2080; i++) {
        v[0] = (double)i; v[1] = 0;
        LT_CHECK_INT(lt_rec_record_at(r, 5.0 * (double)i, v, 2), 1);
    }
    LT_CHECK_INT(lt_rec_count(r), 2080);
    lt_log_init(&w);
    lt_rec_live_window(r, 0, 100000, &w);
    LT_CHECK_INT(w.n, 2048);
    LT_CHECK_NEAR(LT_LOG_AT(&w, 0, 0), 32, 0);
    LT_CHECK_NEAR(LT_LOG_AT(&w, 2047, 0), 2079, 0);
    lt_rec_live_window(r, 2070, 5, &w);
    LT_CHECK_INT(w.n, 5);
    LT_CHECK_NEAR(LT_LOG_AT(&w, 0, 0), 2070, 0);
    lt_rec_live_window(r, 10, 30, &w);    /* entirely evicted: starts at origin */
    LT_CHECK_INT(w.n, 8);                 /* rows 32..39 are within [10,40) */
    LT_CHECK_NEAR(LT_LOG_AT(&w, 0, 0), 32, 0);
    lt_rec_live_window(r, 5000, 5, &w);
    LT_CHECK_INT(w.n, 0);
    LT_CHECK_STR(w.names[1], "map");
    LT_CHECK_INT(lt_rec_stop(r), 0);
    LT_CHECK_INT(lt_mlg_read(TMP "r2.mlg", &f), 0);
    LT_CHECK_INT(f.n, 2080);
    LT_CHECK_NEAR(LT_LOG_AT(&f, 2079, 0), 2079, 1e-3);
    lt_log_free(&f);
    lt_log_free(&w);
    lt_rec_free(r);
    lt_plat_remove(TMP "r2.mlg");
}

LT_TEST(recorder_restart_and_clear)
{
    lt_recorder *r = new_rec();
    double v[2];
    v[0] = 1; v[1] = 2;
    lt_rec_set_sample_rate(r, 200.0);
    lt_rec_start(r, NULL);
    lt_rec_record_at(r, 100.0, v, 2);
    LT_CHECK_INT(lt_rec_count(r), 1);
    lt_rec_stop(r);
    lt_rec_start(r, NULL);
    LT_CHECK_INT(lt_rec_count(r), 0);
    lt_rec_record_at(r, 900.0, v, 2);
    LT_CHECK_INT(lt_rec_count(r), 1);
    LT_CHECK_NEAR(lt_rec_duration(r), 0.0, 0);   /* new timeline begins at the first sample */
    lt_rec_record_at(r, 1900.0, v, 2);
    LT_CHECK_NEAR(lt_rec_duration(r), 1.0, 1e-9);
    lt_rec_clear(r);
    LT_CHECK_INT(lt_rec_count(r), 0);
    LT_CHECK_NEAR(lt_rec_duration(r), 0.0, 0);
    lt_rec_start(r, NULL);
    lt_rec_record_at(r, 10.0, v, 2);
    LT_CHECK_INT(lt_rec_count(r), 1);
    lt_rec_free(r);
}

LT_TEST(recorder_clear_starts_new_file)
{
    lt_recorder *r = new_rec();
    double v[2];
    lt_log a, b;
    char *p1;
    v[0] = 7; v[1] = 8;
    lt_rec_set_sample_rate(r, 200.0);
    LT_CHECK_INT(lt_rec_start(r, TMP "r3.mlg"), 0);
    lt_rec_record_at(r, 0.0, v, 2);
    lt_rec_clear(r);
    LT_CHECK(lt_rec_is_recording(r));
    p1 = lt_strdup(lt_rec_path(r));
    LT_CHECK(strcmp(p1, TMP "r3.mlg") != 0);
    LT_CHECK(lt_str_ends(p1, ".mlg"));
    lt_rec_record_at(r, 0.0, v, 2);
    lt_rec_record_at(r, 10.0, v, 2);
    LT_CHECK_INT(lt_rec_stop(r), 0);
    LT_CHECK_INT(lt_mlg_read(TMP "r3.mlg", &a), 0);
    LT_CHECK_INT(a.n, 1);
    LT_CHECK_INT(lt_mlg_read(p1, &b), 0);
    LT_CHECK_INT(b.n, 2);
    lt_log_free(&a); lt_log_free(&b);
    lt_plat_remove(p1);
    lt_plat_remove(TMP "r3.mlg");
    lt_free(p1);
    lt_rec_free(r);
}

LT_TEST(recorder_disk_failure_stops)
{
    if (lt_file_exists("/dev/full")) {
        lt_recorder *r = new_rec();
        double v[2];
        int i, rc = 0;
        v[0] = 1; v[1] = 2;
        lt_rec_set_sample_rate(r, 200.0);
        LT_CHECK_INT(lt_rec_start(r, "/dev/full"), 0);
        for (i = 0; i < 100 && rc >= 0; i++) {
            rc = lt_rec_record_at(r, 5.0 * i, v, 2);
        }
        LT_CHECK_INT(rc, LT_EIO);
        LT_CHECK(!lt_rec_is_recording(r));
        LT_CHECK_INT(lt_rec_record_at(r, 9999.0, v, 2), 0);
        lt_rec_free(r);
    }
}

LT_TEST(recorder_default_path)
{
    char *p = lt_rec_default_path("some/dir");
    size_t n = strlen(p);
    LT_CHECK(strncmp(p, "some/dir/", 9) == 0);
    LT_CHECK(lt_str_ends(p, ".mlg"));
    /* YYYY-MM-DD_HH.MM.SS.mlg */
    LT_CHECK_INT(n - 9, 23);
    LT_CHECK(p[9 + 4] == '-' && p[9 + 10] == '_' && p[9 + 13] == '.' && p[9 + 16] == '.');
    lt_free(p);
    p = lt_rec_default_path(NULL);
    LT_CHECK_INT(strlen(p), 23);
    lt_free(p);
}

/* --------------------------------------------------------------- player */

LT_TEST(player_basic)
{
    lt_log l;
    lt_player p;
    double out[8];
    static const char *names[1] = { "rpm" };
    double v;
    lt_log_init(&l);
    lt_log_set_channels(&l, names, NULL, 1);
    v = 1000; lt_log_append(&l, 0.0, &v);
    v = 2000; lt_log_append(&l, 1.0, &v);
    v = 3000; lt_log_append(&l, 2.0, &v);
    lt_player_init(&p, &l);
    LT_CHECK_INT(lt_player_len(&p), 3);
    LT_CHECK_NEAR(lt_player_duration(&p), 2.0, 0);
    LT_CHECK_NEAR(lt_player_row(&p)[0], 1000, 0);
    LT_CHECK(lt_player_next(&p));
    LT_CHECK_INT(p.pos, 1);
    LT_CHECK_NEAR(lt_player_time(&p), 1.0, 0);
    lt_player_seek_time(&p, 1.5);
    LT_CHECK_INT(p.pos, 2);
    LT_CHECK(!lt_player_next(&p));
    LT_CHECK_INT(p.pos, 2);
    lt_player_seek_time(&p, 1.0);
    LT_CHECK_INT(p.pos, 1);
    lt_player_seek_time(&p, -4.0);
    LT_CHECK_INT(p.pos, 0);
    lt_player_seek_time(&p, 99.0);
    LT_CHECK_INT(p.pos, 2);
    LT_CHECK(lt_player_prev(&p));
    LT_CHECK(lt_player_prev(&p));
    LT_CHECK(!lt_player_prev(&p));
    LT_CHECK_INT(p.pos, 0);
    lt_player_seek(&p, 1000000L);
    LT_CHECK_INT(p.pos, 2);
    LT_CHECK(!lt_player_next(&p));
    LT_CHECK_INT(lt_player_channel(&p, "rpm", out, 8), 3);
    LT_CHECK_NEAR(out[0], 1000, 0);
    LT_CHECK_NEAR(out[2], 3000, 0);
    LT_CHECK_INT(lt_player_channel(&p, "rpm", out, 2), 2);
    LT_CHECK_INT(lt_player_channel(&p, "nope", out, 8), 0);
    lt_log_free(&l);
}

LT_TEST(player_empty_and_single)
{
    lt_log l;
    lt_player p;
    static const char *names[1] = { "rpm" };
    double v = 5;
    lt_log_init(&l);
    lt_log_set_channels(&l, names, NULL, 1);
    lt_player_init(&p, &l);
    LT_CHECK(lt_player_row(&p) == NULL);
    LT_CHECK(!lt_player_next(&p));
    LT_CHECK(!lt_player_prev(&p));
    lt_player_seek(&p, 5);
    LT_CHECK_INT(p.pos, 0);
    lt_player_seek_time(&p, 1.0);
    LT_CHECK_INT(p.pos, 0);
    LT_CHECK_NEAR(lt_player_duration(&p), 0.0, 0);
    LT_CHECK(!lt_player_advance(&p, 1.0));
    lt_log_append(&l, 0.0, &v);
    LT_CHECK(!lt_player_next(&p));
    LT_CHECK_INT(p.pos, 0);
    lt_log_free(&l);
}

LT_TEST(player_rate_and_loop)
{
    lt_log l;
    lt_player p;
    static const char *names[1] = { "x" };
    double v;
    int i;
    lt_log_init(&l);
    lt_log_set_channels(&l, names, NULL, 1);
    for (i = 0; i <= 10; i++) {
        v = i;
        lt_log_append(&l, 0.5 + 0.1 * i, &v);   /* t from 0.5 to 1.5 */
    }
    lt_player_init(&p, &l);
    LT_CHECK(lt_player_advance(&p, 0.25));
    LT_CHECK_INT(p.pos, 2);                 /* clock 0.75: last t <= 0.75 is 0.7 (index 2) */
    lt_player_set_rate(&p, 2.0);
    LT_CHECK(lt_player_advance(&p, 0.25));  /* +0.5 -> 1.25 */
    LT_CHECK_INT(p.pos, 7);
    LT_CHECK(!lt_player_advance(&p, 1.0));  /* beyond the end */
    LT_CHECK_INT(p.pos, 10);
    lt_player_set_loop(&p, LT_TRUE);
    lt_player_seek(&p, 10);
    LT_CHECK(lt_player_advance(&p, 0.25));  /* 1.5 + 0.5 = 2.0 -> 0.5 + fmod(1.5, 1.0) = 1.0 */
    LT_CHECK_INT(p.pos, 5);
    lt_player_set_rate(&p, -3.0);
    LT_CHECK_NEAR(p.rate, 0.01, 0);
    lt_player_set_rate(&p, 1e9);
    LT_CHECK_NEAR(p.rate, 100.0, 0);
    lt_log_free(&l);
}

/* ---------------------------------------------------------------- dyno */

static lt_dyno_run mkrun(const double *rpm, const double *hp, const double *tq, size_t n)
{
    lt_dyno_run r;
    size_t i;
    memset(&r, 0, sizeof r);
    r.data = (lt_dyno_point *)lt_calloc(n, sizeof(lt_dyno_point));
    r.n = n;
    for (i = 0; i < n; i++) {
        r.data[i].rpm = rpm[i];
        if (hp) { r.data[i].has_hp = LT_TRUE; r.data[i].hp = hp[i]; }
        if (tq) { r.data[i].has_tq = LT_TRUE; r.data[i].torque = tq[i]; }
    }
    return r;
}

LT_TEST(dyno_csv_import)
{
    const char *csv = "Time,RPM,HP,Torque,AFR,Boost\n# comment\n0.0,3000,100,175,12.5,5\n"
                      "0.5,4000,150,197,12.0,6\n\n1.0,5000,200,210,11.8,7\n"
                      "1.5,6000,220,192,11.9,8\n2.0,50,5,5,1,1\n2.5,abc,5,5,1,1\n"
                      "3.0,25000,5,5,1,1\n3.5,5500,x,y,z,w\n";
    lt_dyno_run r;
    char **h;
    size_t nh, i;
    LT_CHECK_INT(lt_file_write(TMP "d.csv", csv, strlen(csv)), 0);
    LT_CHECK_INT(lt_dyno_from_csv(TMP "d.csv", "run1", &r), 0);
    LT_CHECK_INT(r.n, 5);
    LT_CHECK_STR(r.name, "run1");
    LT_CHECK_STR(r.source_file, TMP "d.csv");
    LT_CHECK_STR(r.color, "#4fc3f7");
    LT_CHECK_NEAR(r.data[3].rpm, 5500, 0);      /* sorted by rpm */
    LT_CHECK(!r.data[3].has_hp);
    LT_CHECK(r.data[0].has_time && r.data[0].has_boost && r.data[0].has_afr);
    LT_CHECK_NEAR(r.data[0].boost, 5, 0);
    LT_CHECK(r.has_peak_hp && r.has_peak_tq);
    LT_CHECK_NEAR(r.peak_hp, 220, 0);
    LT_CHECK_NEAR(r.peak_hp_rpm, 6000, 0);
    LT_CHECK_NEAR(r.peak_tq, 210, 0);
    LT_CHECK_NEAR(r.peak_tq_rpm, 5000, 0);
    lt_dyno_run_free(&r);
    LT_CHECK(r.data == NULL);

    LT_CHECK_INT(lt_dyno_csv_headers(TMP "d.csv", &h, &nh), 0);
    LT_CHECK_INT(nh, 6);
    LT_CHECK_STR(h[1], "RPM");
    for (i = 0; i < nh; i++) {
        lt_free(h[i]);
    }
    lt_free(h);
    {
        const char *bad = "hp,torque\n1,2\n";
        lt_file_write(TMP "d2.csv", bad, strlen(bad));
        LT_CHECK_INT(lt_dyno_from_csv(TMP "d2.csv", "x", &r), LT_EDATA);
        LT_CHECK(strstr(lt_err_get(), "RPM column") != NULL);
        lt_file_write(TMP "d2.csv", "", 0);
        LT_CHECK_INT(lt_dyno_from_csv(TMP "d2.csv", "x", &r), LT_EDATA);
        LT_CHECK(strstr(lt_err_get(), "Empty CSV file") != NULL);
        LT_CHECK_INT(lt_dyno_csv_headers(TMP "d2.csv", &h, &nh), LT_EDATA);
        {
            const char *q = "Engine Speed;x\ttime,\"quoted,value\",power\n";
            lt_file_write(TMP "d2.csv", q, strlen(q));
            LT_CHECK_INT(lt_dyno_csv_headers(TMP "d2.csv", &h, &nh), 0);
            LT_CHECK_INT(nh, 4);
            LT_CHECK_STR(h[2], "quoted,value");
            for (i = 0; i < nh; i++) {
                lt_free(h[i]);
            }
            lt_free(h);
        }
        {   /* column detection chain: "speed" and "Horsepower" */
            const char *c2 = "speed,Horsepower,Torque (Nm),Lambda,MAP kPa\n3000,90,160,0.85,120\n";
            lt_file_write(TMP "d2.csv", c2, strlen(c2));
            LT_CHECK_INT(lt_dyno_from_csv(TMP "d2.csv", "y", &r), 0);
            LT_CHECK_INT(r.n, 1);
            LT_CHECK(r.data[0].has_hp && r.data[0].has_tq && r.data[0].has_afr);
            LT_CHECK(r.data[0].has_boost && !r.data[0].has_time);
            LT_CHECK_NEAR(r.data[0].afr, 0.85, 0);
            lt_dyno_run_free(&r);
        }
    }
    LT_CHECK_INT(lt_dyno_from_csv(TMP "none.csv", "x", &r), LT_ENOENT);
    lt_plat_remove(TMP "d.csv");
    lt_plat_remove(TMP "d2.csv");
}

LT_TEST(dyno_peaks_tie_last_wins)
{
    static const double rpm[3] = { 3000, 4000, 5000 };
    static const double hp[3] = { 150, 200, 200 };
    lt_dyno_run r = mkrun(rpm, hp, NULL, 3);
    lt_dyno_compute_peaks(&r);
    LT_CHECK(r.has_peak_hp && !r.has_peak_tq);
    LT_CHECK_NEAR(r.peak_hp_rpm, 5000, 0);
    lt_dyno_run_free(&r);
    r = mkrun(rpm, NULL, NULL, 3);
    lt_dyno_compute_peaks(&r);
    LT_CHECK(!r.has_peak_hp);
    lt_dyno_run_free(&r);
}

LT_TEST(dyno_interpolate_and_compare)
{
    static const double rpm[3] = { 1000, 2000, 3000 };
    static const double hp[3] = { 50, 100, 150 };
    lt_dyno_run a = mkrun(rpm, hp, NULL, 3);
    double v;
    LT_CHECK(lt_dyno_interpolate(&a, 1500, 0, &v));
    LT_CHECK_NEAR(v, 75, 1e-9);
    LT_CHECK(lt_dyno_interpolate(&a, 2500, 0, &v));
    LT_CHECK_NEAR(v, 125, 1e-9);
    LT_CHECK(lt_dyno_interpolate(&a, 3000, 0, &v));
    LT_CHECK_NEAR(v, 150, 1e-9);
    LT_CHECK(!lt_dyno_interpolate(&a, 999, 0, &v));
    LT_CHECK(!lt_dyno_interpolate(&a, 3001, 0, &v));
    LT_CHECK(!lt_dyno_interpolate(&a, 1500, 1, &v));   /* no torque */
    lt_dyno_run_free(&a);
    {
        static const double r3[2] = { 3000, 6000 };
        static const double h3[2] = { 100, 200 };
        lt_dyno_run d = mkrun(r3, h3, NULL, 2);
        LT_CHECK(lt_dyno_interpolate(&d, 3000, 0, &v));
        LT_CHECK_NEAR(v, 100, 1e-9);
        LT_CHECK(lt_dyno_interpolate(&d, 6000, 0, &v));
        LT_CHECK_NEAR(v, 200, 1e-9);
        LT_CHECK(!lt_dyno_interpolate(&d, 2999, 0, &v));
        LT_CHECK(!lt_dyno_interpolate(&d, 6001, 0, &v));
        lt_dyno_run_free(&d);
    }
    {
        static const double ra[2] = { 3000, 5000 }, ha[2] = { 100, 200 };
        static const double rb[2] = { 3000, 5000 }, hb[2] = { 110, 215 };
        static const double ta[2] = { 150, 160 }, tb[2] = { 140, 170 };
        lt_dyno_run x = mkrun(ra, ha, ta, 2), y = mkrun(rb, hb, tb, 2);
        lt_dyno_cmp c;
        LT_CHECK_INT(lt_dyno_compare(&x, &y, &c), 0);
        LT_CHECK_INT(c.n_hp, 2);
        LT_CHECK_NEAR(c.hp_diff[0], 3000, 0);
        LT_CHECK_NEAR(c.hp_diff[1], 10, 1e-9);
        LT_CHECK_NEAR(c.hp_diff[2], 5000, 0);
        LT_CHECK_NEAR(c.hp_diff[3], 15, 1e-9);
        LT_CHECK(c.has_total_hp);
        LT_CHECK_NEAR(c.total_hp_change, 12.5, 1e-9);
        LT_CHECK_INT(c.n_tq, 2);
        LT_CHECK_NEAR(c.total_tq_change, 0.0, 1e-9);   /* -10 and +10 */
        lt_dyno_cmp_free(&c);
        lt_dyno_run_free(&x);
        lt_dyno_run_free(&y);
    }
    {   /* range test: only overlapping rpms compared */
        static const double ra[4] = { 2000, 3000, 6000, 7000 }, ha[4] = { 80, 100, 200, 210 };
        static const double rb[2] = { 3000, 6000 }, hb[2] = { 110, 215 };
        lt_dyno_run x = mkrun(ra, ha, NULL, 4), y = mkrun(rb, hb, NULL, 2);
        lt_dyno_cmp c;
        LT_CHECK_INT(lt_dyno_compare(&x, &y, &c), 0);
        LT_CHECK_INT(c.n_hp, 2);
        LT_CHECK_NEAR(c.hp_diff[0], 3000, 0);
        LT_CHECK_NEAR(c.hp_diff[2], 6000, 0);
        LT_CHECK_NEAR(c.total_hp_change, 12.5, 1e-9);
        LT_CHECK(!c.has_total_tq);
        lt_dyno_cmp_free(&c);
        lt_dyno_run_free(&x);
        lt_dyno_run_free(&y);
    }
}

LT_TEST(dyno_map_to_table)
{
    static const double bins[4] = { 500, 1000, 1500, 2000 };
    static const double yb[5] = { 20, 40, 60, 80, 100 };
    lt_dyno_run r;
    lt_dyno_cell cells[32];
    size_t n;
    LT_CHECK_INT(lt_dyno_nearest_bin(bins, 4, 800), 1);
    LT_CHECK_INT(lt_dyno_nearest_bin(bins, 4, 500), 0);
    LT_CHECK_INT(lt_dyno_nearest_bin(bins, 4, 1999), 3);
    LT_CHECK_INT(lt_dyno_nearest_bin(bins, 4, 1250), 1);   /* tie -> first */
    LT_CHECK_INT(lt_dyno_nearest_bin(bins, 0, 1250), 0);
    {
        static const double rpm[4] = { 500, 520, 1990, 1000 };
        static const double hp[4] = { 10, 20, 100, 50 };
        r = mkrun(rpm, hp, NULL, 4);
        r.data[3].has_hp = LT_FALSE;
        r.data[3].has_afr = LT_TRUE;
        r.data[3].afr = 12.0;
        n = lt_dyno_map_to_table(&r, bins, 4, yb, 5, 0.0, cells, 32);
        LT_CHECK_INT(n, 3);
        LT_CHECK_INT(cells[0].row, 4);          /* WOT assumption: last row */
        LT_CHECK_INT(cells[0].col, 0);
        LT_CHECK_NEAR(cells[0].hp, 15, 1e-12);
        LT_CHECK_INT(cells[0].samples, 2);
        LT_CHECK(cells[0].has_hp && !cells[0].has_tq && !cells[0].has_afr);
        LT_CHECK_INT(cells[1].col, 1);
        LT_CHECK(cells[1].has_afr && !cells[1].has_hp);
        LT_CHECK_NEAR(cells[1].afr, 12.0, 0);
        LT_CHECK_INT(cells[2].col, 3);
        /* capacity limit */
        LT_CHECK_INT(lt_dyno_map_to_table(&r, bins, 4, yb, 5, 0.0, cells, 1), 1);
        /* boost based row: boost 30 kPa * 2 = 60 -> row 2 */
        r.data[0].has_boost = LT_TRUE;
        r.data[0].boost = 30;
        n = lt_dyno_map_to_table(&r, bins, 4, yb, 5, 2.0, cells, 32);
        LT_CHECK_INT(n, 4);
        LT_CHECK_INT(cells[0].row, 2);
        LT_CHECK_INT(cells[0].samples, 1);
        LT_CHECK_INT(lt_dyno_map_to_table(&r, bins, 4, yb, 0, 0.0, cells, 32), 0);
        lt_dyno_run_free(&r);
    }
}

/* ------------------------------------------------- fake expression engine */

struct lt_expr { char *src; };

typedef struct fx { const char *p; const lt_expr_env *env; int err; int run; } fx;

static void fx_ws(fx *f) { while (*f->p == ' ') f->p++; }
static double fx_expr(fx *f);

static double fx_primary(fx *f)
{
    double v = 0;
    fx_ws(f);
    if (*f->p == '(') {
        f->p++;
        v = fx_expr(f);
        fx_ws(f);
        if (*f->p != ')') f->err = 1; else f->p++;
    } else if (isdigit((unsigned char)*f->p) || *f->p == '.') {
        char *e;
        v = strtod(f->p, &e);
        f->p = e;
    } else if (isalpha((unsigned char)*f->p) || *f->p == '_') {
        char nm[64];
        size_t n = 0;
        while (isalnum((unsigned char)*f->p) || *f->p == '_') {
            if (n < 63) nm[n++] = *f->p;
            f->p++;
        }
        nm[n] = '\0';
        if (f->run) {
            if (!f->env || !f->env->get_var || !f->env->get_var(f->env->ud, nm, &v)) f->err = 1;
        }
    } else {
        f->err = 1;
    }
    return v;
}
static double fx_unary(fx *f)
{
    fx_ws(f);
    if (*f->p == '-') { f->p++; return -fx_unary(f); }
    return fx_primary(f);
}
static double fx_mul(fx *f)
{
    double v = fx_unary(f);
    for (;;) {
        fx_ws(f);
        if (*f->p == '*') { f->p++; v *= fx_unary(f); }
        else if (*f->p == '/') { double d; f->p++; d = fx_unary(f); v = d != 0 ? v / d : 0; }
        else return v;
    }
}
static double fx_add(fx *f)
{
    double v = fx_mul(f);
    for (;;) {
        fx_ws(f);
        if (*f->p == '+') { f->p++; v += fx_mul(f); }
        else if (*f->p == '-') { f->p++; v -= fx_mul(f); }
        else return v;
    }
}
static double fx_cmp(fx *f)
{
    double v = fx_add(f);
    fx_ws(f);
    if (*f->p == '>') { f->p++; return v > fx_add(f) ? 1 : 0; }
    if (*f->p == '<') { f->p++; return v < fx_add(f) ? 1 : 0; }
    return v;
}
static double fx_expr(fx *f)
{
    double c = fx_cmp(f), a, b;
    fx_ws(f);
    if (*f->p == '?') {
        f->p++;
        a = fx_expr(f);
        fx_ws(f);
        if (*f->p != ':') { f->err = 1; return 0; }
        f->p++;
        b = fx_expr(f);
        return c != 0 ? a : b;
    }
    return c;
}

static lt_expr *fx_compile(const char *src, char *err, size_t errlen)
{
    fx f;
    lt_expr *e;
    f.p = src; f.env = NULL; f.err = 0; f.run = 0;
    fx_expr(&f);
    fx_ws(&f);
    if (f.err || *f.p) {
        if (err && errlen) lt_strlcpy(err, "syntax error", errlen);
        return NULL;
    }
    e = (lt_expr *)lt_malloc(sizeof *e);
    e->src = lt_strdup(src);
    return e;
}
static void fx_free(lt_expr *e) { lt_free(e->src); lt_free(e); }
static int fx_eval(const lt_expr *e, const lt_expr_env *env, double *out)
{
    fx f;
    f.p = e->src; f.env = env; f.err = 0; f.run = 1;
    *out = fx_expr(&f);
    return f.err ? LT_EDATA : 0;
}
static void fx_vars(const lt_expr *e, lt_map *set)
{
    const char *p = e->src;
    while (*p) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            char nm[64];
            size_t n = 0;
            while (isalnum((unsigned char)*p) || *p == '_') {
                if (n < 63) nm[n++] = *p;
                p++;
            }
            nm[n] = '\0';
            lt_map_put(set, nm, (void *)1);
        } else if (isdigit((unsigned char)*p) || *p == '.') {
            while (isalnum((unsigned char)*p) || *p == '.') p++;
        } else {
            p++;
        }
    }
}

static void install_fake(void)
{
    lt_math_hooks h;
    h.compile = fx_compile;
    h.free_expr = fx_free;
    h.eval = fx_eval;
    h.collect_vars = fx_vars;
    lt_math_set_hooks(&h);
}

static lt_math_set mkset(const char *const *nm, const char *const *ex, size_t n)
{
    lt_math_set s;
    size_t i;
    s.n = n;
    s.ch = (lt_math_channel *)lt_calloc(n, sizeof(lt_math_channel));
    for (i = 0; i < n; i++) {
        s.ch[i].name = lt_strdup(nm[i]);
        s.ch[i].units = lt_strdup("u");
        s.ch[i].expression = lt_strdup(ex[i]);
    }
    return s;
}

typedef struct vars { const char *n[4]; double v[4]; int cnt; } vars;
static lt_bool vars_get(void *ud, const char *name, double *out)
{
    vars *vs = (vars *)ud;
    int i;
    for (i = 0; i < vs->cnt; i++) {
        if (strcmp(vs->n[i], name) == 0) { *out = vs->v[i]; return LT_TRUE; }
    }
    return LT_FALSE;
}

LT_TEST(math_compile_and_order)
{
    lt_math_channel c;
    char err[64];
    size_t order[8], i;
    int pos[4];
    lt_math_set s;
    install_fake();
    memset(&c, 0, sizeof c);
    c.expression = lt_strdup("(map - 100) * 0.1");
    LT_CHECK_INT(lt_math_compile(&c, err, sizeof err), 0);
    LT_CHECK(c.ast != NULL);
    lt_free(c.expression);
    c.expression = lt_strdup("map + ");
    LT_CHECK_INT(lt_math_compile(&c, err, sizeof err), LT_EDATA);
    LT_CHECK(c.ast == NULL);
    LT_CHECK(err[0] != '\0');
    lt_free(c.expression);

    {
        const char *nm[2] = { "Boost_Warn", "Boost_PSI" };
        const char *ex[2] = { "Boost_PSI > 5 ? 1 : 0", "(map - 100) * 0.145" };
        s = mkset(nm, ex, 2);
        lt_math_eval_order(&s, order);
        LT_CHECK_INT(order[0], 1);
        LT_CHECK_INT(order[1], 0);
        {
            vars vs;
            double out[2];
            lt_expr_env env;
            vs.n[0] = "map"; vs.v[0] = 150; vs.cnt = 1;
            memset(&env, 0, sizeof env);
            env.ud = &vs;
            env.get_var = vars_get;
            LT_CHECK_INT(lt_math_eval_all(&s, &env, out), 0);
            LT_CHECK_NEAR(out[1], 7.25, 1e-9);
            LT_CHECK_NEAR(out[0], 1.0, 0);
            vs.v[0] = 120;
            LT_CHECK_INT(lt_math_eval_all(&s, &env, out), 0);
            LT_CHECK_NEAR(out[0], 0.0, 0);
        }
        lt_math_set_free(&s);
    }
    {
        const char *nm[4] = { "c", "x", "b", "a" };
        const char *ex[4] = { "b + 1", "map * 2", "a + 1", "rpm / 100" };
        s = mkset(nm, ex, 4);
        lt_math_eval_order(&s, order);
        LT_CHECK_INT(order[0], 1);
        LT_CHECK_INT(order[1], 3);
        LT_CHECK_INT(order[2], 2);
        LT_CHECK_INT(order[3], 0);
        for (i = 0; i < 4; i++) pos[order[i]] = (int)i;
        LT_CHECK(pos[3] < pos[2] && pos[2] < pos[0] && pos[1] < pos[2]);
        lt_math_set_free(&s);
    }
    {
        const char *nm[3] = { "p", "q", "solo" };
        const char *ex[3] = { "q + 1", "p + 1", "rpm" };
        s = mkset(nm, ex, 3);
        lt_math_eval_order(&s, order);
        LT_CHECK_INT(order[0], 2);
        LT_CHECK_INT(order[1], 0);
        LT_CHECK_INT(order[2], 1);
        lt_math_set_free(&s);
    }
    {
        const char *nm[2] = { "bad", "ok" };
        const char *ex[2] = { "map + ", "rpm * 2" };
        s = mkset(nm, ex, 2);
        lt_math_eval_order(&s, order);
        LT_CHECK(order[0] + order[1] == 1);
        {
            double out[2];
            vars vs;
            lt_expr_env env;
            vs.n[0] = "rpm"; vs.v[0] = 4; vs.cnt = 1;
            memset(&env, 0, sizeof env);
            env.ud = &vs; env.get_var = vars_get;
            LT_CHECK_INT(lt_math_eval_all(&s, &env, out), LT_EDATA);
            LT_CHECK(lt_isnan(out[0]));
            LT_CHECK_NEAR(out[1], 8, 0);
        }
        lt_math_set_free(&s);
    }
    s.n = 0; s.ch = NULL;
    lt_math_eval_order(&s, order);   /* n = 0: no-op */
    lt_math_set_hooks(NULL);
}

LT_TEST(math_persistence)
{
    lt_math_set s, l;
    const char *nm[2] = { "A", "B\"q" };
    const char *ex[2] = { "1+1", "2*2" };
    size_t len = 0;
    char *txt;
    install_fake();
    s = mkset(nm, ex, 2);
    LT_CHECK_INT(lt_math_save(TMP "m.json", &s), 0);
    txt = lt_file_read(TMP "m.json", &len);
    LT_CHECK_STR(txt, "[\n  {\n    \"name\": \"A\",\n    \"units\": \"u\",\n"
                      "    \"expression\": \"1+1\"\n  },\n  {\n    \"name\": \"B\\\"q\",\n"
                      "    \"units\": \"u\",\n    \"expression\": \"2*2\"\n  }\n]");
    lt_free(txt);
    LT_CHECK_INT(lt_math_load(TMP "m.json", &l), 0);
    LT_CHECK_INT(l.n, 2);
    LT_CHECK_STR(l.ch[0].name, "A");
    LT_CHECK_STR(l.ch[0].expression, "1+1");
    LT_CHECK(l.ch[0].ast != NULL);
    LT_CHECK_STR(l.ch[1].name, "B\"q");
    lt_math_set_free(&l);
    lt_math_set_free(&s);

    /* empty set, missing file, unknown keys, invalid expressions load anyway, bad JSON */
    s.n = 0; s.ch = NULL;
    LT_CHECK_INT(lt_math_save(TMP "m.json", &s), 0);
    txt = lt_file_read(TMP "m.json", &len);
    LT_CHECK_STR(txt, "[]");
    lt_free(txt);
    LT_CHECK_INT(lt_math_load(TMP "m.json", &l), 0);
    LT_CHECK_INT(l.n, 0);
    LT_CHECK_INT(lt_math_load(TMP "nonexistent.json", &l), 0);
    LT_CHECK_INT(l.n, 0);
    {
        const char *j = "[ {\"name\":\"X\", \"extra\":[1,{\"a\":null}], \"units\":\"\\u00b0C\", "
                        "\"expression\":\"map + \", \"n\": -1.5e3 }, {} ]";
        lt_file_write(TMP "m.json", j, strlen(j));
        LT_CHECK_INT(lt_math_load(TMP "m.json", &l), 0);
        LT_CHECK_INT(l.n, 2);
        LT_CHECK_STR(l.ch[0].units, "\xC2\xB0" "C");
        LT_CHECK(l.ch[0].ast == NULL);
        LT_CHECK_STR(l.ch[1].name, "");
        lt_math_set_free(&l);
        lt_file_write(TMP "m.json", "[ {\"name\": ", 12);
        LT_CHECK_INT(lt_math_load(TMP "m.json", &l), LT_EDATA);
        lt_file_write(TMP "m.json", "{}", 2);
        LT_CHECK_INT(lt_math_load(TMP "m.json", &l), LT_EDATA);
    }
    lt_plat_remove(TMP "m.json");
    lt_math_set_hooks(NULL);
}

LT_TEST(math_apply_to_log)
{
    lt_log l = make_log(2, 3);   /* RPM, MAP */
    const char *nm[2] = { "RPM_k", "Boost" };
    const char *ex[2] = { "RPM / 1000", "(MAP - 100) * 0.145 + RPM_k" };
    lt_math_set s;
    install_fake();
    s = mkset(nm, ex, 2);
    LT_CHECK_INT(lt_math_apply_to_log(&s, &l), 0);
    LT_CHECK_INT(l.n_ch, 4);
    LT_CHECK_STR(l.names[2], "RPM_k");
    LT_CHECK_STR(l.units[3], "u");
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 2), 0.825, 1e-9);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 1, 3), (31.5 - 100) * 0.145 + 0.825, 1e-9);
    LT_CHECK_NEAR(LT_LOG_AT(&l, 2, 0), 850.0, 0);
    {
        double row[4];
        memcpy(row, l.v, sizeof row);
        lt_log_append(&l, 1.0, row);   /* appending still works afterwards */
    }
    LT_CHECK_INT(l.n, 4);
    lt_math_set_free(&s);
    lt_log_free(&l);
    lt_math_set_hooks(NULL);
    /* without engine */
    l = make_log(2, 1);
    s = mkset(nm, ex, 2);
    LT_CHECK_INT(lt_math_apply_to_log(&s, &l), LT_EUNSUPPORTED);
    LT_CHECK_INT(lt_math_compile(&s.ch[0], NULL, 0), LT_EUNSUPPORTED);
    lt_math_set_free(&s);
    lt_log_free(&l);
}

/* ------------------------------------------------------------ ini glue */

static double g_set_vals[8];
static void fake_rt_set(lt_ini_rt *rt, size_t idx, double v)
{
    LT_UNUSED(rt);
    if (idx < 8) g_set_vals[idx] = v;
}

LT_TEST(ini_glue)
{
    lt_ini ini;
    lt_ini_channel c0, c1, c2;
    lt_ini_channel *chs[3];
    lt_ini_dl_entry ents[3];
    char **names, **units;
    size_t n;
    lt_log l = make_log(3, 2);
    lt_player p;
    memset(&ini, 0, sizeof ini);
    memset(&c0, 0, sizeof c0); memset(&c1, 0, sizeof c1); memset(&c2, 0, sizeof c2);
    c0.name = (char *)"RPM"; c0.units = (char *)"rpm"; c0.index = 0;
    c1.name = (char *)"MAP"; c1.units = (char *)"kPa"; c1.index = 1;
    c2.name = (char *)"AFR"; c2.units = NULL; c2.index = 2;
    chs[0] = &c0; chs[1] = &c1; chs[2] = &c2;
    ini.channels = chs;
    ini.n_channels = 3;
    lt_map_init(&ini.channel_idx, LT_FALSE);
    lt_map_put(&ini.channel_idx, "RPM", &c0);
    lt_map_put(&ini.channel_idx, "MAP", &c1);
    lt_map_put(&ini.channel_idx, "AFR", &c2);
    n = lt_datalog_channels_from_ini(&ini, &names, &units);
    LT_CHECK_INT(n, 3);
    LT_CHECK_STR(names[2], "AFR");
    LT_CHECK_STR(units[2], "");
    lt_strv_free(names, n); lt_strv_free(units, n);

    memset(ents, 0, sizeof ents);
    ents[0].channel = (char *)"MAP"; ents[0].enabled = LT_TRUE;
    ents[1].channel = (char *)"GHOST"; ents[1].enabled = LT_TRUE;
    ents[2].channel = (char *)"RPM"; ents[2].enabled = LT_FALSE;
    ini.dl_entries = ents;
    ini.n_dl_entries = 3;
    n = lt_datalog_channels_from_ini(&ini, &names, NULL);
    LT_CHECK_INT(n, 1);
    LT_CHECK_STR(names[0], "MAP");
    lt_strv_free(names, n);

    lt_player_init(&p, &l);
    lt_player_next(&p);
    lt_player_apply_to_rt(&p, &ini, NULL);   /* no setter, no rt: nothing */
    lt_datalog_set_rt_setter(fake_rt_set);
    lt_player_apply_to_rt(&p, &ini, (lt_ini_rt *)&ini);
    LT_CHECK_NEAR(g_set_vals[0], 825.0, 0);
    LT_CHECK_NEAR(g_set_vals[1], 31.5, 0);
    lt_datalog_set_rt_setter(NULL);
    lt_map_free(&ini.channel_idx);
    lt_log_free(&l);
}

static void review_fixes(void)
{
    lt_dyno_run r;
    lt_log l;
    lt_player p;
    static const char *csv = "Time,rpm\nnan,900\n1,1000\n";
    char big[40000];
    size_t off = 0;
    int i;
    /* NaN timestamp clamps to 0 */
    LT_CHECK_INT(lt_csv_parse(csv, strlen(csv), &l), 0);
    LT_CHECK_INT((int)l.n, 2);
    LT_CHECK_NEAR(l.t[0], 0.0, 0);
    lt_log_free(&l);
    /* NaN rpm rejected, stable sort by rpm with many rows (reverse order, ties kept) */
    off += (size_t)lt_snprintf(big + off, sizeof big - off, "rpm,hp\nnan,5\n");
    for (i = 1500; i >= 1; i--) {
        off += (size_t)lt_snprintf(big + off, sizeof big - off, "%d,%d\n", 1000 + i / 2, i);
    }
    LT_CHECK_INT(lt_file_write(TMP "sort.csv", big, off), 0);
    LT_CHECK_INT(lt_dyno_from_csv(TMP "sort.csv", "s", &r), 0);
    LT_CHECK_INT((int)r.n, 1500);
    for (i = 1; i < (int)r.n; i++) {
        LT_CHECK(r.data[i - 1].rpm <= r.data[i].rpm);
        if (r.data[i - 1].rpm == r.data[i].rpm) {
            LT_CHECK(r.data[i - 1].hp > r.data[i].hp);   /* input order preserved */
        }
    }
    lt_dyno_run_free(&r);
    /* advance with NaN dt does not corrupt the cursor */
    lt_log_init(&l);
    {
        const char *nm[1] = { "a" };
        double v = 1.0;
        lt_log_set_channels(&l, nm, NULL, 1);
        lt_log_append(&l, 0.0, &v);
        lt_log_append(&l, 1.0, &v);
    }
    lt_player_init(&p, &l);
    LT_CHECK(!lt_player_advance(&p, 0.0 / (volatile double)0.0 * 0.0 + (double)HUGE_VAL - (double)HUGE_VAL));
    LT_CHECK(p.ptime == p.ptime);
    lt_log_free(&l);
}

int main(void)
{
    LT_RUN(review_fixes);
    LT_RUN(mlg_spec_basic);
    LT_RUN(mlg_clock_timestamps);
    LT_RUN(mlg_errors);
    LT_RUN(mlg_checksum_marker_info_and_files);
    LT_RUN(mlg_time_channel);
    LT_RUN(mlg_scalar_types);
    LT_RUN(mlg_writer_bytes_and_roundtrip);
    LT_RUN(mlg_writer_large_streaming);
    LT_RUN(csv_spec_cases);
    LT_RUN(csv_tolerance_and_units);
    LT_RUN(csv_write_roundtrip);
    LT_RUN(csv_fixture);
    LT_RUN(log_model_and_format);
    LT_RUN(recorder_basic_rate_malformed);
    LT_RUN(recorder_rate_clamp);
    LT_RUN(recorder_tail_and_live_window);
    LT_RUN(recorder_restart_and_clear);
    LT_RUN(recorder_clear_starts_new_file);
    LT_RUN(recorder_disk_failure_stops);
    LT_RUN(recorder_default_path);
    LT_RUN(player_basic);
    LT_RUN(player_empty_and_single);
    LT_RUN(player_rate_and_loop);
    LT_RUN(dyno_csv_import);
    LT_RUN(dyno_peaks_tie_last_wins);
    LT_RUN(dyno_interpolate_and_compare);
    LT_RUN(dyno_map_to_table);
    LT_RUN(math_compile_and_order);
    LT_RUN(math_persistence);
    LT_RUN(math_apply_to_log);
    LT_RUN(ini_glue);
    LT_TEST_MAIN_END
}
