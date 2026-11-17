/* test_ini_codec.c - data types, bit fields, constant codec, realtime decode,
 * loggers and .inc tables. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lt.h"
#include "ini/ini.h"
#include "ini/ini_priv.h"
#include "lt_test.h"

/* NULL-terminated list of text pieces, concatenated at run time (C89 limits string literals
 * to 509 characters, so long INI texts are passed in several pieces). */
static lt_ini *load_n(const char *first, ...)
{
    lt_buf b;
    va_list ap;
    const char *s = first;
    lt_ini *ini;
    lt_buf_init(&b);
    va_start(ap, first);
    while (s) {
        lt_buf_puts(&b, s);
        s = va_arg(ap, const char *);
    }
    va_end(ap);
    ini = lt_ini_load_string(b.data ? b.data : "", b.len, NULL, NULL);
    lt_buf_free(&b);
    return ini;
}

/* ---- a trivial page source ---- */
typedef struct tpages {
    lt_u8 buf[4][512];
    size_t len[4];
    int writes;
    size_t bytes_written;
} tpages;

static const lt_u8 *tp_page(void *ud, int page, size_t *len)
{
    tpages *t = (tpages *)ud;
    if (page < 0 || page > 3 || t->len[page] == 0) return NULL;
    *len = t->len[page];
    return t->buf[page];
}

static int tp_write(void *ud, int page, size_t off, const lt_u8 *data, size_t n)
{
    tpages *t = (tpages *)ud;
    if (page < 0 || page > 3 || off + n > t->len[page]) return LT_ERANGE;
    memcpy(t->buf[page] + off, data, n);
    t->writes++;
    t->bytes_written += n;
    return 0;
}

static void tp_init(tpages *t, lt_pagesrc *src)
{
    memset(t, 0, sizeof *t);
    t->len[0] = 512;
    t->len[1] = 64;
    src->ud = t;
    src->page = tp_page;
    src->write = tp_write;
}

/* ---------------------------------------------------------------- */

LT_TEST(dtype_read_write)
{
    lt_u8 b[16];
    double v;
    memset(b, 0, sizeof b);
    LT_CHECK(lt_dtype_write(LT_DT_U16, b, 16, 0, LT_ENDIAN_BIG, 0x1234) == 0);
    LT_CHECK(b[0] == 0x12 && b[1] == 0x34);
    LT_CHECK(lt_dtype_write(LT_DT_U16, b, 16, 2, LT_ENDIAN_LITTLE, 0x1234) == 0);
    LT_CHECK(b[2] == 0x34 && b[3] == 0x12);
    LT_CHECK(lt_dtype_read(LT_DT_U16, b, 16, 0, LT_ENDIAN_BIG, &v) == 0 && v == 0x1234);
    LT_CHECK(lt_dtype_read(LT_DT_U16, b, 16, 2, LT_ENDIAN_LITTLE, &v) == 0 && v == 0x1234);
    LT_CHECK(lt_dtype_read(LT_DT_U16, b, 16, 0, LT_ENDIAN_LITTLE, &v) == 0 && v == 0x3412);

    /* signed */
    lt_dtype_write(LT_DT_S16, b, 16, 0, LT_ENDIAN_LITTLE, -2);
    LT_CHECK(b[0] == 0xFE && b[1] == 0xFF);
    lt_dtype_read(LT_DT_S16, b, 16, 0, LT_ENDIAN_LITTLE, &v);
    LT_CHECK_NEAR(v, -2, 0);
    lt_dtype_write(LT_DT_S08, b, 16, 4, LT_ENDIAN_BIG, -128);
    LT_CHECK(b[4] == 0x80);
    lt_dtype_read(LT_DT_S08, b, 16, 4, LT_ENDIAN_BIG, &v);
    LT_CHECK_NEAR(v, -128, 0);
    lt_dtype_write(LT_DT_S32, b, 16, 8, LT_ENDIAN_BIG, -100000);
    lt_dtype_read(LT_DT_S32, b, 16, 8, LT_ENDIAN_BIG, &v);
    LT_CHECK_NEAR(v, -100000, 0);
    lt_dtype_write(LT_DT_U32, b, 16, 8, LT_ENDIAN_LITTLE, 4000000000.0);
    lt_dtype_read(LT_DT_U32, b, 16, 8, LT_ENDIAN_LITTLE, &v);
    LT_CHECK_NEAR(v, 4000000000.0, 0);

    /* integer writes ROUND (half away from zero) then saturate */
    lt_dtype_write(LT_DT_U08, b, 16, 0, LT_ENDIAN_BIG, 14.7 / 0.1);   /* 146.99999999999997 */
    LT_CHECK(b[0] == 147);
    lt_dtype_write(LT_DT_U08, b, 16, 0, LT_ENDIAN_BIG, 2.5);
    LT_CHECK(b[0] == 3);
    lt_dtype_write(LT_DT_S08, b, 16, 0, LT_ENDIAN_BIG, -2.5);
    LT_CHECK(b[0] == 0xFD);
    lt_dtype_write(LT_DT_U08, b, 16, 0, LT_ENDIAN_BIG, 300);
    LT_CHECK(b[0] == 255);
    lt_dtype_write(LT_DT_U08, b, 16, 0, LT_ENDIAN_BIG, -5);
    LT_CHECK(b[0] == 0);
    lt_dtype_write(LT_DT_S08, b, 16, 0, LT_ENDIAN_BIG, 1000);
    LT_CHECK(b[0] == 127);
    lt_dtype_write(LT_DT_S16, b, 16, 0, LT_ENDIAN_BIG, -99999);
    lt_dtype_read(LT_DT_S16, b, 16, 0, LT_ENDIAN_BIG, &v);
    LT_CHECK_NEAR(v, -32768, 0);
    lt_dtype_write(LT_DT_U08, b, 16, 0, LT_ENDIAN_BIG, 0.0 * 1e300 * 1e300 / (0.0 + 1e-300 * 0.0 + 1.0));
    LT_CHECK(b[0] == 0);

    /* floats */
    lt_dtype_write(LT_DT_F32, b, 16, 0, LT_ENDIAN_BIG, 1.5);
    LT_CHECK(b[0] == 0x3F && b[1] == 0xC0 && b[2] == 0 && b[3] == 0);
    lt_dtype_read(LT_DT_F32, b, 16, 0, LT_ENDIAN_BIG, &v);
    LT_CHECK_NEAR(v, 1.5, 0);
    lt_dtype_write(LT_DT_F32, b, 16, 0, LT_ENDIAN_LITTLE, -0.1);
    lt_dtype_read(LT_DT_F32, b, 16, 0, LT_ENDIAN_LITTLE, &v);
    LT_CHECK_NEAR(v, -0.1, 1e-7);
    lt_dtype_write(LT_DT_F64, b, 16, 0, LT_ENDIAN_BIG, 1.0);
    LT_CHECK(b[0] == 0x3F && b[1] == 0xF0 && b[2] == 0);
    lt_dtype_write(LT_DT_F64, b, 16, 0, LT_ENDIAN_LITTLE, 123456.789012345);
    lt_dtype_read(LT_DT_F64, b, 16, 0, LT_ENDIAN_LITTLE, &v);
    LT_CHECK(v == 123456.789012345);
    lt_dtype_write(LT_DT_F64, b, 16, 0, LT_ENDIAN_BIG, -2.5e-300);
    lt_dtype_read(LT_DT_F64, b, 16, 0, LT_ENDIAN_BIG, &v);
    LT_CHECK(v == -2.5e-300);
    lt_dtype_write(LT_DT_F64, b, 16, 0, LT_ENDIAN_BIG, 3.0e10);
    lt_dtype_read(LT_DT_F64, b, 16, 0, LT_ENDIAN_BIG, &v);
    LT_CHECK(v == 3.0e10);
    lt_dtype_write(LT_DT_F64, b, 16, 0, LT_ENDIAN_BIG, 0.0);
    lt_dtype_read(LT_DT_F64, b, 16, 0, LT_ENDIAN_BIG, &v);
    LT_CHECK(v == 0.0);

    /* range checks */
    LT_CHECK(lt_dtype_read(LT_DT_U32, b, 16, 13, LT_ENDIAN_BIG, &v) == LT_ERANGE);
    LT_CHECK(lt_dtype_read(LT_DT_U08, b, 16, 16, LT_ENDIAN_BIG, &v) == LT_ERANGE);
    LT_CHECK(lt_dtype_read(LT_DT_U08, b, 16, 15, LT_ENDIAN_BIG, &v) == 0);
    b[15] = 0x55;
    LT_CHECK(lt_dtype_write(LT_DT_U16, b, 16, 15, LT_ENDIAN_BIG, 1) == LT_ERANGE);
    LT_CHECK(b[15] == 0x55);   /* nothing written */
    LT_CHECK(lt_dtype_read(LT_DT_STRING, b, 16, 0, LT_ENDIAN_BIG, &v) == LT_EINVAL);
    {
        double lo, hi;
        LT_CHECK(lt_dtype_range(LT_DT_S16, &lo, &hi) && lo == -32768 && hi == 32767);
        LT_CHECK(lt_dtype_range(LT_DT_U32, &lo, &hi) && hi == 4294967295.0);
        LT_CHECK(lt_dtype_range(LT_DT_BITS, &lo, &hi) && hi == 255);
        LT_CHECK(!lt_dtype_range(LT_DT_F32, &lo, &hi));
        LT_CHECK(!lt_dtype_range(LT_DT_STRING, &lo, &hi));
    }
}

LT_TEST(bit_extraction)
{
    lt_u8 d[4];
    lt_u32 v;

    /* within one byte */
    d[0] = 0xF5;
    LT_CHECK(lt_bits_get(d, 1, 0, 4, 4, &v) == 0 && v == 0x0F);
    d[0] = 0x3C;
    LT_CHECK(lt_bits_get(d, 1, 0, 2, 4, &v) == 0 && v == 0xF);
    d[0] = 0x80;
    LT_CHECK(lt_bits_get(d, 1, 0, 7, 1, &v) == 0 && v == 1);
    d[0] = 0x7F;
    LT_CHECK(lt_bits_get(d, 1, 0, 7, 1, &v) == 0 && v == 0);
    d[0] = 0xAB;
    LT_CHECK(lt_bits_get(d, 1, 0, 0, 8, &v) == 0 && v == 0xAB);
    d[0] = 0xCD;
    d[1] = 0xAB;
    LT_CHECK(lt_bits_get(d, 2, 0, 0, 16, &v) == 0 && v == 0xABCD);
    /* spanning two bytes: contiguous field value */
    d[0] = 0xC0;
    d[1] = 0x03;
    LT_CHECK(lt_bits_get(d, 2, 0, 6, 4, &v) == 0 && v == 0xF);
    /* pos >= 8 reaches following bytes */
    d[0] = 0x00;
    d[1] = 0x08;
    LT_CHECK(lt_bits_get(d, 2, 0, 11, 1, &v) == 0 && v == 1);
    /* offset */
    d[0] = 0;
    d[1] = 0;
    d[2] = 0x20;
    LT_CHECK(lt_bits_get(d, 3, 1, 13, 1, &v) == 0 && v == 1);
    /* zero size / empty data / short data */
    d[0] = 0xFF;
    LT_CHECK(lt_bits_get(d, 1, 0, 0, 0, &v) == 0 && v == 0);
    LT_CHECK(lt_bits_get(d, 1, 0, 7, 0, &v) == 0 && v == 0);
    LT_CHECK(lt_bits_get(d, 0, 0, 0, 1, &v) == LT_ERANGE && v == 0);
    LT_CHECK(lt_bits_get(d, 1, 0, 6, 4, &v) == LT_ERANGE);
    LT_CHECK(lt_bits_set(d, 1, 0, 0, 0, 5) == 0 && d[0] == 0xFF);
    LT_CHECK(lt_bits_set(d, 0, 0, 0, 1, 1) == LT_ERANGE);

    /* write preserves other bits */
    d[0] = 0xFF;
    LT_CHECK(lt_bits_set(d, 1, 0, 2, 3, 0) == 0);
    LT_CHECK(d[0] == 0xE3);
    LT_CHECK(lt_bits_set(d, 1, 0, 2, 3, 5) == 0);
    LT_CHECK(d[0] == 0xF7);
    LT_CHECK(lt_bits_set(d, 1, 0, 2, 3, 0xFF) == 0);   /* value masked to the field */
    LT_CHECK(d[0] == 0xFF);
}

LT_TEST(bit_roundtrip_all)
{
    lt_u8 d[6];
    lt_u32 v, want;
    int pos, size;
    int bad = 0;
    for (pos = 0; pos < 24; pos++) {
        for (size = 1; size <= 16; size++) {
            lt_u8 before[6];
            int i;
            for (i = 0; i < 6; i++) d[i] = (lt_u8)(0xA5 ^ (i * 37));
            memcpy(before, d, 6);
            want = (lt_u32)(0x1234567 + pos * 131 + size) & (((lt_u32)1 << size) - (lt_u32)1);
            if (lt_bits_set(d, 6, 0, pos, size, want) != 0) bad++;
            if (lt_bits_get(d, 6, 0, pos, size, &v) != 0 || v != want) bad++;
            /* other bits untouched */
            for (i = 0; i < 48; i++) {
                int in_field = i >= pos && i < pos + size;
                int a = (d[i / 8] >> (i % 8)) & 1, b = (before[i / 8] >> (i % 8)) & 1;
                if (!in_field && a != b) bad++;
            }
        }
    }
    LT_CHECK_INT(bad, 0);
}

LT_TEST(const_codec_scalars)
{
    lt_ini *ini = load_n(
        "[MegaTune]\nsignature=\"t\"\n[TunerStudio]\nendianness = big\n",
        "[Constants]\npageSize = 512, 64\npage = 1\n",
        "reqFuel = scalar, U16, 0, \"ms\", 0.1, 0.0, 0, 25.5, 1\n",
        "temp = scalar, S08, 2, \"C\", 1, -40, -40, 100, 0\n",
        "lilEnd = scalar, U16, 4, \"\", 1, 0, 0, 65535, 0\n",
        "bigEnd = scalar, BU16, 6, \"\", 1, 0, 0, 65535, 0\n",
        "mode = bits, U08, 8, [0:1], \"A\", \"B\", \"C\", \"D\"\n",
        "wide = bits, U16, 9, [4:11], \"x\"\n",
        "fl = scalar, F32, 12, \"\", 1, 0, -1000, 1000, 2\n",
        "page = 2\n",
        "p2 = scalar, U08, 3, \"\", 2, 10, 0, 500, 0\n",
        "[PcVariables]\npcv = scalar, U08, \"\", 1, 0, 0, 10, 0\n",
        "[Defaults]\ndefaultValue = pcv, 4\ndefaultValue = reqFuel, 7.5\n", (const char *)NULL);
    tpages tp;
    lt_pagesrc src;
    double v;
    const lt_ini_const *c;
    tp_init(&tp, &src);

    c = lt_ini_find_const(ini, "reqFuel");
    LT_CHECK(lt_ini_const_set(ini, c, &src, 0, 14.7) == 0);
    LT_CHECK(tp.buf[0][0] == 0 && tp.buf[0][1] == 147);   /* big endian, rounded 147 */
    LT_CHECK(lt_ini_const_get(ini, c, &src, 0, &v) == 0);
    LT_CHECK_NEAR(v, 14.7, 1e-9);
    LT_CHECK(lt_ini_const_get_raw(ini, c, &src, 0, &v) == 0 && v == 147);
    LT_CHECK(lt_ini_const_set_raw(ini, c, &src, 0, 300) == 0);
    LT_CHECK(tp.buf[0][0] == 0x01 && tp.buf[0][1] == 0x2C);
    /* display clamps to the INI range */
    lt_ini_const_set(ini, c, &src, 0, 1000.0);
    lt_ini_const_get(ini, c, &src, 0, &v);
    LT_CHECK_NEAR(v, 25.5, 1e-9);
    lt_ini_const_set(ini, c, &src, 0, -3.0);
    lt_ini_const_get(ini, c, &src, 0, &v);
    LT_CHECK_NEAR(v, 0, 1e-9);

    c = lt_ini_find_const(ini, "temp");
    lt_ini_const_set(ini, c, &src, 0, 25);                /* raw 65 */
    LT_CHECK(tp.buf[0][2] == 65);
    lt_ini_const_set(ini, c, &src, 0, -40);
    LT_CHECK(tp.buf[0][2] == 0);
    LT_CHECK(lt_ini_const_set_raw(ini, c, &src, 0, -128) == 0);
    LT_CHECK(tp.buf[0][2] == 0x80);
    lt_ini_const_get(ini, c, &src, 0, &v);
    LT_CHECK_NEAR(v, -168, 1e-9);

    /* endianness: global big, per-field BU16 is big, plain U16 follows global */
    ini->endian = LT_ENDIAN_LITTLE;
    c = lt_ini_find_const(ini, "lilEnd");
    LT_CHECK(lt_ini_const_endian(ini, c) == LT_ENDIAN_LITTLE);
    lt_ini_const_set(ini, c, &src, 0, 0x1234);
    LT_CHECK(tp.buf[0][4] == 0x34 && tp.buf[0][5] == 0x12);
    c = lt_ini_find_const(ini, "bigEnd");
    LT_CHECK(lt_ini_const_endian(ini, c) == LT_ENDIAN_BIG);
    lt_ini_const_set(ini, c, &src, 0, 0x1234);
    LT_CHECK(tp.buf[0][6] == 0x12 && tp.buf[0][7] == 0x34);
    ini->endian = LT_ENDIAN_BIG;

    /* bits */
    c = lt_ini_find_const(ini, "mode");
    tp.buf[0][8] = 0xFC;
    LT_CHECK(lt_ini_const_set(ini, c, &src, 0, 2) == 0);
    LT_CHECK(tp.buf[0][8] == 0xFE);
    lt_ini_const_get(ini, c, &src, 0, &v);
    LT_CHECK_NEAR(v, 2, 0);
    lt_ini_const_set(ini, c, &src, 0, 9);   /* saturates at the field maximum (2 bits) */
    LT_CHECK(tp.buf[0][8] == 0xFF);
    c = lt_ini_find_const(ini, "wide");     /* [4:11] straddles two bytes */
    tp.buf[0][9] = 0x0F;
    tp.buf[0][10] = 0xF0;
    lt_ini_const_get(ini, c, &src, 0, &v);
    LT_CHECK_NEAR(v, 0, 0);
    lt_ini_const_set(ini, c, &src, 0, 0xA5);
    LT_CHECK(tp.buf[0][9] == 0x5F && tp.buf[0][10] == 0xFA);
    lt_ini_const_get(ini, c, &src, 0, &v);
    LT_CHECK_NEAR(v, 0xA5, 0);

    /* float */
    c = lt_ini_find_const(ini, "fl");
    lt_ini_const_set(ini, c, &src, 0, 12.5);
    lt_ini_const_get(ini, c, &src, 0, &v);
    LT_CHECK_NEAR(v, 12.5, 0);

    /* second page + scale/translate */
    c = lt_ini_find_const(ini, "p2");
    LT_CHECK_INT(c->page, 1);
    lt_ini_const_set(ini, c, &src, 0, 110);   /* (110-10)/2 = 50 */
    LT_CHECK(tp.buf[1][3] == 50);

    /* pc variables are not page-backed */
    c = lt_ini_find_const(ini, "pcv");
    LT_CHECK(lt_ini_const_get(ini, c, &src, 0, &v) == LT_ENOENT);

    /* by-name access and default fallback */
    LT_CHECK(lt_ini_get(ini, &src, "mode", &v) == 0);
    LT_CHECK(lt_ini_get(ini, &src, "nope", &v) == LT_ENOENT);
    LT_CHECK(lt_ini_get(ini, &src, "pcv", &v) == 0 && v == 4);
    LT_CHECK(lt_ini_set(ini, &src, "temp", 30) == 0);
    LT_CHECK(lt_ini_set(ini, &src, "nope", 30) == LT_ENOENT);
    tp.len[0] = 0;   /* page not loaded -> defaults */
    LT_CHECK(lt_ini_get(ini, &src, "reqFuel", &v) == 0 && v == 7.5);
    LT_CHECK(lt_ini_get(ini, &src, "temp", &v) != 0);
    LT_CHECK(lt_ini_set(ini, &src, "temp", 30) == LT_ENOENT);
    lt_ini_free(ini);
}

LT_TEST(const_codec_arrays_and_strings)
{
    lt_ini *ini = load_n(
        "[TunerStudio]\nendianness = big\n",
        "[Constants]\npageSize = 512, 64\npage = 1\n",
        "bins = array, U08, 0, [4], \"rpm\", 100, 0, 0, 25500, 0\n",
        "tbl = array, U16, 10, [3x2], \"%\", 0.5, 0, 0, 100, 1\n",
        "name = string, ASCII, 30, 8\n", (const char *)NULL);
    tpages tp;
    lt_pagesrc src;
    double out[8];
    double in[6];
    const lt_ini_const *c;
    char s[16];
    int i;
    tp_init(&tp, &src);

    c = lt_ini_find_const(ini, "bins");
    LT_CHECK_INT(lt_ini_const_get_array(ini, c, &src, NULL, 0), 4);
    in[0] = 500; in[1] = 1500; in[2] = 2500; in[3] = 3500;
    LT_CHECK(lt_ini_const_set_array(ini, c, &src, in, 4) == 0);
    LT_CHECK(tp.buf[0][0] == 5 && tp.buf[0][1] == 15 && tp.buf[0][3] == 35);
    LT_CHECK(tp.writes == 1);   /* a single write for the changed span */
    LT_CHECK(lt_ini_const_set_array(ini, c, &src, in, 4) == 0);
    LT_CHECK(tp.writes == 1);   /* unchanged data writes nothing */
    in[2] = 2600;
    lt_ini_const_set_array(ini, c, &src, in, 4);
    LT_CHECK(tp.writes == 2 && tp.bytes_written == 4 + 1);
    LT_CHECK_INT(lt_ini_const_get_array(ini, c, &src, out, 8), 4);
    LT_CHECK_NEAR(out[2], 2600, 0);
    LT_CHECK(lt_ini_const_get_array(ini, c, &src, out, 2) == LT_ERANGE);

    c = lt_ini_find_const(ini, "tbl");
    for (i = 0; i < 6; i++) in[i] = 10.0 * (i + 1);
    LT_CHECK(lt_ini_const_set_array(ini, c, &src, in, 6) == 0);
    LT_CHECK(tp.buf[0][10] == 0 && tp.buf[0][11] == 20);   /* 10 / 0.5 = 20, big endian */
    LT_CHECK_INT(lt_ini_const_get_array(ini, c, &src, out, 8), 6);
    LT_CHECK_NEAR(out[5], 60, 0);
    /* element access, row-major */
    LT_CHECK(lt_ini_const_get(ini, c, &src, 4, &out[0]) == 0);
    LT_CHECK_NEAR(out[0], 50, 0);
    LT_CHECK(lt_ini_const_set(ini, c, &src, 1, 99) == 0);
    LT_CHECK(tp.buf[0][12] == 0 && tp.buf[0][13] == 198);
    LT_CHECK(lt_ini_const_get(ini, c, &src, 6, &out[0]) == LT_ERANGE);
    /* writing past the page */
    in[0] = 1;
    c = lt_ini_find_const(ini, "bins");
    LT_CHECK(lt_ini_const_set_array(ini, c, &src, in, 1) == 0);

    c = lt_ini_find_const(ini, "name");
    LT_CHECK(lt_ini_const_set_string(ini, c, &src, "HelloWorld") == 0);   /* truncated to 8 */
    LT_CHECK(memcmp(tp.buf[0] + 30, "HelloWor", 8) == 0);
    LT_CHECK(lt_ini_const_get_string(ini, c, &src, s, sizeof s) == 0);
    LT_CHECK_STR(s, "HelloWor");
    LT_CHECK(lt_ini_const_set_string(ini, c, &src, "Hi") == 0);
    LT_CHECK(tp.buf[0][32] == 0 && tp.buf[0][37] == 0);   /* NUL padded */
    lt_ini_const_get_string(ini, c, &src, s, sizeof s);
    LT_CHECK_STR(s, "Hi");
    lt_ini_const_get_string(ini, c, &src, s, 3);
    LT_CHECK_STR(s, "Hi");
    lt_ini_const_get_string(ini, c, &src, s, 2);
    LT_CHECK_STR(s, "H");
    LT_CHECK(lt_ini_const_get_string(ini, lt_ini_find_const(ini, "bins"), &src, s, 8) == LT_EINVAL);
    lt_ini_free(ini);
}

LT_TEST(patch_invalid_sizes)
{
    lt_ini *ini = load_n(
        "[Constants]\npageSize = 512\npage = 1\n",
        "rows = scalar, U08, 0, \"\", 1, 0, 4, 16, 0\n",
        "cols = scalar, U08, 1, \"\", 1, 0, 4, 16, 0\n",
        "other = scalar, U08, 5, \"\", 1, 0, 4, 16, 0\n",
        "tbl = array, U08, 10, [{cols}x{rows}], \"\", 1, 0, 0, 255, 0\n",
        "[ConstantsExtensions]\ndefaultValue = rows, 8\ndefaultValue = cols, 8\ndefaultValue = other, 8\n", (const char *)NULL);
    tpages tp;
    lt_pagesrc src;
    double v;
    tp_init(&tp, &src);
    tp.buf[0][0] = 0;     /* out of range 4..16 */
    tp.buf[0][1] = 10;    /* ok */
    tp.buf[0][5] = 0;     /* not a size constant: untouched */
    LT_CHECK_INT(lt_ini_patch_invalid_sizes(ini, &src), 1);
    lt_ini_get(ini, &src, "rows", &v);
    LT_CHECK_NEAR(v, 8, 0);
    lt_ini_get(ini, &src, "cols", &v);
    LT_CHECK_NEAR(v, 10, 0);
    LT_CHECK(tp.buf[0][5] == 0);
    lt_ini_free(ini);
}

/* ---------------------------------------------------------------- */
/* realtime                                                          */
/* ---------------------------------------------------------------- */

LT_TEST(channel_decode_spec_cases)
{
    lt_ini *ini = load_n(
        "[TunerStudio]\nendianness = little\n",
        "[OutputChannels]\n",
        "rpm = scalar, U16, 0, \"RPM\", 1.0, 0.0\n",
        "crank = bits, U32, 0, [11:11]\n",
        "flag = bits, U32, 1, [8:8]\n",
        "run = bits, U08, 0, [1:1]\n",
        "nSquirts = bits, U08, 0, [5:7]\n",
        "errorNum = bits, U08, 0, [0:1]\n",
        "currentError = bits, U08, 0, [2:7]\n",
        "afr_type = bits, S08, 0, [0:2]\n",
        "launchMode = bits, S08, 0, [6:7]\n",
        "hasSync = bits, U08, 0, [7:7]\n",
        "clt = scalar, S16, 0, \"C\", 0.1, -40.0\n",
        "plainBits = bits, U08, 0\n",
        "f = scalar, F32, 0, \"\", 1, 0\n",
        "afr = { ego1 / 10.0 }, \"AFR\"\n", (const char *)NULL);
    const lt_endian e = LT_ENDIAN_LITTLE;
    lt_u8 b[8];
    double v;
#define CH(n) lt_ini_find_channel(ini, n)
    memset(b, 0, sizeof b);
    b[0] = 0xE8;
    b[1] = 0x03;
    LT_CHECK(lt_ini_channel_decode(CH("rpm"), e, b, 2, &v) == 0);
    LT_CHECK_NEAR(v, 1000, 0);
    LT_CHECK(lt_ini_channel_decode(CH("rpm"), e, b, 1, &v) == LT_ERANGE);
    LT_CHECK(lt_ini_channel_decode(CH("afr"), e, b, 8, &v) == LT_EINVAL);
    LT_CHECK(lt_ini_channel_decode(CH("rpm"), LT_ENDIAN_BIG, b, 2, &v) == 0);
    LT_CHECK_NEAR(v, 0xE803, 0);

    memset(b, 0, sizeof b);
    b[1] = 0x08;
    LT_CHECK(lt_ini_channel_decode(CH("crank"), e, b, 4, &v) == 0);
    LT_CHECK_NEAR(v, 1, 0);
    LT_CHECK(lt_ini_channel_decode(CH("flag"), e, b, 2, &v) == LT_ERANGE);   /* 2-byte block */

    b[0] = 0x02;
    LT_CHECK(lt_ini_channel_decode(CH("run"), e, b, 1, &v) == 0 && v == 1);
    b[0] = 0x40;
    lt_ini_channel_decode(CH("nSquirts"), e, b, 1, &v);
    LT_CHECK_NEAR(v, 2, 0);
    b[0] = 0xE0;
    lt_ini_channel_decode(CH("nSquirts"), e, b, 1, &v);
    LT_CHECK_NEAR(v, 7, 0);
    b[0] = (lt_u8)((42 << 2) | 3);
    lt_ini_channel_decode(CH("errorNum"), e, b, 1, &v);
    LT_CHECK_NEAR(v, 3, 0);
    lt_ini_channel_decode(CH("currentError"), e, b, 1, &v);
    LT_CHECK_NEAR(v, 42, 0);
    b[0] = 0xC5;   /* 0b11000101 */
    lt_ini_channel_decode(CH("afr_type"), e, b, 1, &v);
    LT_CHECK_NEAR(v, 5, 0);
    lt_ini_channel_decode(CH("launchMode"), e, b, 1, &v);
    LT_CHECK_NEAR(v, 3, 0);
    b[0] = 0x80;
    lt_ini_channel_decode(CH("hasSync"), e, b, 1, &v);
    LT_CHECK_NEAR(v, 1, 0);
    b[0] = 0;
    lt_ini_channel_decode(CH("hasSync"), e, b, 1, &v);
    LT_CHECK_NEAR(v, 0, 0);
    /* bits without a position decode as a plain byte */
    b[0] = 0x2A;
    lt_ini_channel_decode(CH("plainBits"), e, b, 1, &v);
    LT_CHECK_NEAR(v, 42, 0);
    /* scale + translate */
    b[0] = 0x90;
    b[1] = 0x01;   /* 400 */
    lt_ini_channel_decode(CH("clt"), e, b, 2, &v);
    LT_CHECK_NEAR(v, 0, 1e-9);
    b[0] = 0x00;
    b[1] = 0x00;
    lt_ini_channel_decode(CH("clt"), e, b, 2, &v);
    LT_CHECK_NEAR(v, -40, 1e-9);
    lt_dtype_write(LT_DT_F32, b, 8, 0, e, 3.25);
    lt_ini_channel_decode(CH("f"), e, b, 4, &v);
    LT_CHECK_NEAR(v, 3.25, 0);
#undef CH
    lt_ini_free(ini);
}

LT_TEST(realtime_evaluator)
{
    lt_ini *ini = load_n(
        "[TunerStudio]\nendianness = little\n",
        "[OutputChannels]\nochBlockSize = 3\n",
        "rpm = scalar, U16, 0, \"RPM\", 1.0, 0.0\n",
        "flag = scalar, U08, 2, \"\", 1, 0\n",
        "rpm_x4 = { rpm_x2 * 2 }, \"\"\n",
        "rpm_x2 = { rpm * 2 }, \"\"\n",
        "status = { flag > 0 ? 100 : 0 }, \"\"\n",
        "viaConst = { rpm + mult }, \"\"\n",
        "bad = { unknownFn(1) }, \"\"\n",
        "huge = { 1 / 0 + sqrt(0 - 1) }, \"\"\n", (const char *)NULL);
    lt_ini_rt *rt = lt_ini_rt_new(ini);
    lt_u8 frame[3];
    double v;
    lt_map pv;
    double mult = 5.0;
    lt_expr_env parent;
    const lt_ini_channel *ch;

    LT_CHECK_INT(lt_ini_rt_count(rt), 8);
    LT_CHECK_INT(lt_ini_realtime_size(ini), 3);
    frame[0] = 0xE8; frame[1] = 0x03; frame[2] = 1;
    LT_CHECK(lt_ini_rt_update(rt, frame, 3) == 0);
    LT_CHECK(lt_ini_rt_get(rt, "rpm", &v) && v == 1000);
    LT_CHECK(lt_ini_rt_get(rt, "rpm_x2", &v) && v == 2000);
    LT_CHECK(lt_ini_rt_get(rt, "rpm_x4", &v) && v == 4000);   /* declared before its input */
    LT_CHECK(lt_ini_rt_get(rt, "status", &v) && v == 100);
    LT_CHECK(!lt_ini_rt_get(rt, "bad", &v));                   /* eval error: no value */
    LT_CHECK(lt_ini_rt_get(rt, "huge", &v) && v == 0);         /* non-finite stored as 0 */
    LT_CHECK(!lt_ini_rt_get(rt, "nothing", &v));
    ch = lt_ini_find_channel(ini, "rpm_x2");
    LT_CHECK_NEAR(lt_ini_rt_value(rt, ch->index), 2000, 0);
    LT_CHECK(lt_ini_rt_valid(rt, ch->index));
    LT_CHECK(!lt_ini_rt_valid(rt, lt_ini_find_channel(ini, "bad")->index));
    frame[2] = 0;
    lt_ini_rt_update(rt, frame, 3);
    LT_CHECK(lt_ini_rt_get(rt, "status", &v) && v == 0);

    /* parent env supplies constants for computed channels */
    lt_map_init(&pv, LT_FALSE);
    lt_map_put(&pv, "mult", &mult);
    lt_expr_env_from_map(&parent, &pv);
    lt_ini_rt_set_parent(rt, &parent);
    lt_ini_rt_update(rt, frame, 3);
    LT_CHECK(lt_ini_rt_get(rt, "viaConst", &v) && v == 1005);
    mult = 6.0;
    lt_ini_rt_update(rt, frame, 3);
    LT_CHECK(lt_ini_rt_get(rt, "viaConst", &v) && v == 1006);
    {
        const lt_expr_env *env = lt_ini_rt_env(rt);
        LT_CHECK(env->get_var(env->ud, "rpm", &v) && v == 1000);
        LT_CHECK(env->get_var(env->ud, "mult", &v) && v == 6.0);    /* falls through to parent */
        LT_CHECK(!env->get_var(env->ud, "zzz", &v));
    }

    /* short frame: channels that fit are still decoded */
    LT_CHECK(lt_ini_rt_update(rt, frame, 2) == LT_ERANGE);
    LT_CHECK(lt_ini_rt_get(rt, "rpm", &v) && v == 1000);
    LT_CHECK(!lt_ini_rt_valid(rt, lt_ini_find_channel(ini, "flag")->index));

    /* direct override (demo mode) */
    lt_ini_rt_set(rt, lt_ini_find_channel(ini, "rpm")->index, 3000);
    LT_CHECK(lt_ini_rt_get(rt, "rpm", &v) && v == 3000);
    lt_map_free(&pv);
    lt_ini_rt_free(rt);
    lt_ini_free(ini);
}

LT_TEST(realtime_size_fallback)
{
    lt_ini *ini = load_n(
        "[OutputChannels]\na = scalar, U16, 4, \"\", 1, 0\nb = bits, U32, 10, [0:1]\nc = { a + 1 }\n", (const char *)NULL);
    LT_CHECK_INT(lt_ini_realtime_size(ini), 14);
    lt_ini_free(ini);
}

/* ---------------------------------------------------------------- */
/* loggers                                                           */
/* ---------------------------------------------------------------- */

#define LOGGER_INI_LOAD() load_n(\
    "[LoggerDefinition]\n",\
    "loggerDef = tooth, \"Tooth Logger\", tooth\n",\
    "    startCommand       = \"H\"\n",\
    "    stopCommand        = \"h\"\n",\
    "    dataReadCommand    = \"T\\$tsCanId\\x00\" ; trailing comment\n",\
    "    ;dataReadCommand   = \"r\\x00\\x0f\"\n",\
    "    dataReadTimeout    = 5000\n",\
    "    continuousRead     = true\n",\
    "    recordDef          = 0, 0, 4\n",\
    "    recordField = toothTime, \"Tooth Time\", 0, 32, 1.0, \"us\"\n",\
    "loggerDef = compositeLogger, \"Composite Logger\", composite\n",\
    "    startCommand       = \"J\"\n",\
    "    stopCommand        = \"j\"\n",\
    "    dataReadCommand    = \"O\"\n",\
    "    dataReadyCondition = { dataReady > 0 }\n",\
    "    recordDef          = 0, 0, 5\n",\
    "    recordField = priLevel, \"Primary\", 0, 1, 1.0\n",\
    "    recordField = secLevel, \"Secondary\", 1, 1, 1.0\n",\
    "    recordField = sync, \"Sync\", 4, 1, 1.0\n",\
    "    recordField = refTime, \"Ref Time\", 8, 32, 0.001, \"ms\"\n",\
    "    calcField = ignored\n",\
    "[PcVariables]\ntsCanId = bits, U08, [0:3], \"A\"\n", (const char *)NULL)

LT_TEST(loggers)
{
    lt_ini *ini = LOGGER_INI_LOAD();
    const lt_ini_logger *t, *c;
    double vals[8];
    int pres[8];
    lt_u8 rec[8];
    LT_CHECK_INT(ini->n_loggers, 2);
    t = &ini->loggers[0];
    c = &ini->loggers[1];
    LT_CHECK_STR(t->name, "tooth");
    LT_CHECK_STR(t->label, "Tooth Logger");
    LT_CHECK_STR(t->kind, "tooth");
    LT_CHECK_STR(t->start_command.p, "H");
    LT_CHECK_STR(t->stop_command.p, "h");
    LT_CHECK(t->data_read_command.p[0] == 'T' && t->data_read_command.p[1] == 0);
    LT_CHECK_STR(t->data_read_command.p + 2, "\\x00");
    LT_CHECK(t->continuous_read);
    LT_CHECK_INT(t->data_read_timeout_ms, 5000);
    LT_CHECK_INT(t->record_len, 4);
    LT_CHECK_INT(t->n_fields, 1);
    LT_CHECK_STR(t->fields[0].units, "us");
    LT_CHECK_STR(c->start_command.p, "J");
    LT_CHECK_STR(c->stop_command.p, "j");
    LT_CHECK_STR(c->data_read_command.p, "O");
    LT_CHECK_STR(c->data_ready_condition, "dataReady > 0");
    LT_CHECK(!c->continuous_read);
    LT_CHECK_INT(c->n_fields, 4);
    LT_CHECK_INT(c->record_len, 5);

    rec[0] = 0; rec[1] = 0; rec[2] = 0x04; rec[3] = 0x94;
    LT_CHECK_INT(lt_ini_logger_decode(t, rec, 4, vals, pres), 1);
    LT_CHECK_NEAR(vals[0], 1172, 0);
    rec[3] = 0x90;
    lt_ini_logger_decode(t, rec, 4, vals, pres);
    LT_CHECK_NEAR(vals[0], 1168, 0);
    rec[2] = 0x09; rec[3] = 0x20;
    lt_ini_logger_decode(t, rec, 4, vals, pres);
    LT_CHECK_NEAR(vals[0], 2336, 0);
    LT_CHECK_INT(lt_ini_logger_record_count(t, 508), 127);
    LT_CHECK_INT(lt_ini_logger_record_count(c, 635), 127);
    LT_CHECK_INT(lt_ini_logger_record_count(t, 0), 0);
    {
        lt_u8 two[2] = {1, 2};
        LT_CHECK_INT(lt_ini_logger_decode(t, two, 2, vals, pres), 0);
        LT_CHECK(pres[0] == 0);
    }
    rec[0] = 0; rec[1] = 0; rec[2] = 0; rec[3] = 0;
    LT_CHECK(lt_ini_logger_is_empty(t, rec, 4));
    rec[0] = 0x45; rec[1] = 0x23; rec[2] = 0x01; rec[3] = 0x00;
    LT_CHECK(!lt_ini_logger_is_empty(t, rec, 4));

    rec[0] = 0x00; rec[1] = 0x01; rec[2] = 0xED; rec[3] = 0x28; rec[4] = 0x19;
    LT_CHECK_INT(lt_ini_logger_decode(c, rec, 5, vals, pres), 4);
    LT_CHECK_NEAR(vals[0], 1, 0);
    LT_CHECK_NEAR(vals[1], 0, 0);
    LT_CHECK_NEAR(vals[2], 1, 0);
    LT_CHECK_NEAR(vals[3], 126.248, 1e-9);
    rec[0] = 0x00; rec[1] = 0x01; rec[2] = 0xEE; rec[3] = 0xF4; rec[4] = 0x10;
    lt_ini_logger_decode(c, rec, 5, vals, pres);
    LT_CHECK_NEAR(vals[0], 0, 0);
    LT_CHECK_NEAR(vals[2], 1, 0);
    LT_CHECK_NEAR(vals[3], 126.708, 1e-9);
    memset(rec, 0, 5);
    LT_CHECK(lt_ini_logger_is_empty(c, rec, 5));
    rec[1] = 0xE8; rec[2] = 0x03;
    LT_CHECK(!lt_ini_logger_is_empty(c, rec, 5));
    lt_ini_free(ini);
}

/* ---------------------------------------------------------------- */
/* .inc tables                                                       */
/* ---------------------------------------------------------------- */

LT_TEST(inc_tables)
{
    lt_inc_table *t;
    double v;

    t = lt_inc_parse("; header\n0.039 19.17\n0.085 17.24\n5.0 160\n# c\n' c\n: c\n", "demo");
    LT_CHECK(t != NULL && !t->direct && t->n == 3);
    LT_CHECK_STR(t->name, "demo");
    LT_CHECK(lt_inc_lookup(t, 0.0, &v) && v == 19.17);          /* clamps low */
    LT_CHECK(lt_inc_lookup(t, 99.0, &v) && v == 160);           /* clamps high */
    LT_CHECK(lt_inc_lookup(t, 0.085, &v) && fabs(v - 17.24) < 1e-9);
    LT_CHECK(lt_inc_lookup(t, 0.062, &v));
    LT_CHECK_NEAR(v, 19.17 + (0.062 - 0.039) / (0.085 - 0.039) * (17.24 - 19.17), 1e-9);
    LT_CHECK(lt_inc_lookup(t, 2.5425, &v));
    LT_CHECK_NEAR(v, 17.24 + (2.5425 - 0.085) / (5.0 - 0.085) * (160 - 17.24), 1e-9);
    lt_inc_free(t);

    /* unsorted input is sorted; tab separated; inline comments */
    t = lt_inc_parse("10\t100 ; ten\n0\t0\n5\t50\n\n", "x");
    LT_CHECK(t->n == 3 && t->keys[0] == 0 && t->keys[2] == 10);
    LT_CHECK(lt_inc_lookup(t, 7.5, &v) && v == 75);
    lt_inc_free(t);

    t = lt_inc_parse("junk\n1\nabc def\n3 4 5\n", "j");
    LT_CHECK(t->n == 1 && t->keys[0] == 3 && t->vals[0] == 4);
    lt_inc_free(t);

    /* direct index tables */
    t = lt_inc_parse("DB 210T ; 0 - sensor failure\nDB 475T\nDW 123\nDB bad\ndb 7\n", "d");
    LT_CHECK(t->direct && t->n == 4);
    LT_CHECK(lt_inc_lookup(t, 0, &v) && v == 210);
    LT_CHECK(lt_inc_lookup(t, 1, &v) && v == 475);
    LT_CHECK(lt_inc_lookup(t, 2, &v) && v == 123);
    LT_CHECK(lt_inc_lookup(t, 2.6, &v) && v == 7);    /* rounded to index 3 */
    LT_CHECK(lt_inc_lookup(t, -5, &v) && v == 210);   /* negative saturates to 0 */
    LT_CHECK(!lt_inc_lookup(t, 4, &v));
    lt_inc_free(t);

    t = lt_inc_parse("", "empty");
    LT_CHECK(!lt_inc_lookup(t, 1, &v));
    lt_inc_free(t);
}

LT_TEST(inc_cache)
{
    lt_inc_cache c;
    const lt_inc_table *t, *t2;
    double v;
    LT_CHECK(lt_file_write("build/thermtest.inc", "0 0\n10 100\n", 11) == 0);
    lt_inc_cache_init(&c);
    LT_CHECK(lt_inc_cache_get(&c, "thermtest.inc") == NULL);   /* no search paths */
    lt_inc_cache_add_path(&c, "build");
    lt_inc_cache_add_path(&c, "build");
    LT_CHECK_INT(c.n_paths, 1);
    t = lt_inc_cache_get(&c, "thermtest.inc");
    LT_CHECK(t != NULL);
    LT_CHECK_STR(t->name, "thermtest");
    t2 = lt_inc_cache_get(&c, "thermtest.inc");
    LT_CHECK(t == t2);
    t2 = lt_inc_cache_get(&c, "thermtest");   /* .inc appended */
    LT_CHECK(t2 != NULL && lt_inc_lookup(t2, 5, &v) && v == 50);
    LT_CHECK(lt_inc_cache_get(&c, "missing.inc") == NULL);
    lt_inc_cache_free(&c);
}

int main(void)
{
    LT_RUN(dtype_read_write);
    LT_RUN(bit_extraction);
    LT_RUN(bit_roundtrip_all);
    LT_RUN(const_codec_scalars);
    LT_RUN(const_codec_arrays_and_strings);
    LT_RUN(patch_invalid_sizes);
    LT_RUN(channel_decode_spec_cases);
    LT_RUN(realtime_evaluator);
    LT_RUN(realtime_size_fallback);
    LT_RUN(loggers);
    LT_RUN(inc_tables);
    LT_RUN(inc_cache);
    LT_TEST_MAIN_END
}
