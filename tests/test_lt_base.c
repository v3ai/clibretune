/* test_lt_base.c - tests for src/lt_base.c */
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include "lt.h"
#include "lt_test.h"

static char out[1024];

#define F1(fmt, a) lt_snprintf(out, sizeof out, fmt, a)

LT_TEST(fmt_ints)
{
    LT_CHECK_INT(lt_snprintf(out, sizeof out, "%d", 42), 2);
    LT_CHECK_STR(out, "42");
    lt_snprintf(out, sizeof out, "%d %i %u", -7, 8, 4000000000UL);
    LT_CHECK_STR(out, "-7 8 4000000000");
    lt_snprintf(out, sizeof out, "[%5d][%-5d][%05d][%+d][% d]", 42, 42, 42, 42, 42);
    LT_CHECK_STR(out, "[   42][42   ][00042][+42][ 42]");
    lt_snprintf(out, sizeof out, "[%05d][%+05d]", -42, 42);
    LT_CHECK_STR(out, "[-0042][+0042]");
    lt_snprintf(out, sizeof out, "[%.5d][%8.3d][%.0d]", 42, 7, 0);
    LT_CHECK_STR(out, "[00042][     007][]");
    lt_snprintf(out, sizeof out, "%x %X %#x %#X %o %#o %08x", 255, 255, 255, 255, 8, 8, 0xBEEF);
    LT_CHECK_STR(out, "ff FF 0xff 0XFF 10 010 0000beef");
    lt_snprintf(out, sizeof out, "%#x %#o", 0, 0);
    LT_CHECK_STR(out, "0 0");
    lt_snprintf(out, sizeof out, "%ld %lu %lx", -123456789L, 3000000000UL, 0xDEADBEEFUL);
    LT_CHECK_STR(out, "-123456789 3000000000 deadbeef");
    lt_snprintf(out, sizeof out, "%ld", LONG_MIN);
    LT_CHECK(strncmp(out, "-", 1) == 0 && strlen(out) >= 11);
    lt_snprintf(out, sizeof out, "%zu", (size_t)12345);
    LT_CHECK_STR(out, "12345");
    lt_snprintf(out, sizeof out, "[%*d][%-*d][%.*d]", 6, 1, 6, 2, 3, 4);
    LT_CHECK_STR(out, "[     1][2     ][004]");
    lt_snprintf(out, sizeof out, "[%*d]", -4, 5);
    LT_CHECK_STR(out, "[5   ]");
}

LT_TEST(fmt_strings)
{
    lt_snprintf(out, sizeof out, "%s|%c|%%|%5s|%-5s|%.2s|%5.1s", "abc", 'z', "ab", "ab", "xyz", "qq");
    LT_CHECK_STR(out, "abc|z|%|   ab|ab   |xy|    q");
    lt_snprintf(out, sizeof out, "%s", (const char *)NULL);
    LT_CHECK_STR(out, "(null)");
    lt_snprintf(out, sizeof out, "%p", (void *)0);
    LT_CHECK_STR(out, "0");
    lt_snprintf(out, sizeof out, "%p", (void *)0x1234);
    LT_CHECK_STR(out, "0x1234");
}

LT_TEST(fmt_truncation)
{
    char small[6];
    int n = lt_snprintf(small, sizeof small, "hello world");
    LT_CHECK_INT(n, 11);
    LT_CHECK_STR(small, "hello");
    n = lt_snprintf(small, 1, "abc");
    LT_CHECK_INT(n, 3);
    LT_CHECK(small[0] == '\0');
    n = lt_snprintf(NULL, 0, "%d-%s", 123, "xy");
    LT_CHECK_INT(n, 6);
    n = lt_snprintf(small, sizeof small, "");
    LT_CHECK_INT(n, 0);
    LT_CHECK_STR(small, "");
}

LT_TEST(fmt_float_fixed)
{
    F1("%f", 3.14159265);
    LT_CHECK_STR(out, "3.141593");
    F1("%.2f", 3.14159);
    LT_CHECK_STR(out, "3.14");
    F1("%.0f", 2.5);
    LT_CHECK(strcmp(out, "3") == 0 || strcmp(out, "2") == 0);
    F1("%.0f", 0.4);
    LT_CHECK_STR(out, "0");
    F1("%.2f", 9.996);
    LT_CHECK_STR(out, "10.00");
    F1("%.1f", 0.96);
    LT_CHECK_STR(out, "1.0");
    F1("%.3f", 999.9996);
    LT_CHECK_STR(out, "1000.000");
    F1("%.2f", 9.9951);
    LT_CHECK_STR(out, "10.00");
    F1("%.2f", -1.005);
    LT_CHECK(strcmp(out, "-1.00") == 0 || strcmp(out, "-1.01") == 0);
    F1("%f", -0.0);
    LT_CHECK_STR(out, "-0.000000");
    F1("%.1f", -0.04);
    LT_CHECK_STR(out, "-0.0");
    F1("%f", 0.0);
    LT_CHECK_STR(out, "0.000000");
    F1("%.3f", 1234567.891);
    LT_CHECK_STR(out, "1234567.891");
    F1("%.6f", 0.000001);
    LT_CHECK_STR(out, "0.000001");
    F1("%.10f", 0.5);
    LT_CHECK_STR(out, "0.5000000000");
    F1("%f", 1e15);
    LT_CHECK_STR(out, "1000000000000000.000000");
    F1("%.0f", 1e20);
    LT_CHECK_STR(out, "100000000000000000000");
    F1("%#.0f", 3.0);
    LT_CHECK_STR(out, "3.");
    F1("[%8.2f]", 3.14159);
    LT_CHECK_STR(out, "[    3.14]");
    F1("[%-8.2f]", 3.14159);
    LT_CHECK_STR(out, "[3.14    ]");
    F1("[%08.2f]", -3.14159);
    LT_CHECK_STR(out, "[-0003.14]");
    F1("[%+.1f]", 2.0);
    LT_CHECK_STR(out, "[+2.0]");
    F1("[% .1f]", 2.0);
    LT_CHECK_STR(out, "[ 2.0]");
    lt_snprintf(out, sizeof out, "%.*f", 3, 2.0);
    LT_CHECK_STR(out, "2.000");
    F1("%.2f", 1.0e-10);
    LT_CHECK_STR(out, "0.00");
    F1("%.15f", 0.25);
    LT_CHECK_STR(out, "0.250000000000000");
}

LT_TEST(fmt_float_special)
{
    double zero = 0.0;
    double inf = HUGE_VAL;
    double nan_ = zero / zero;
    F1("%f", inf);
    LT_CHECK_STR(out, "inf");
    F1("%f", -inf);
    LT_CHECK_STR(out, "-inf");
    F1("%F", inf);
    LT_CHECK_STR(out, "INF");
    F1("%f", nan_);
    LT_CHECK(strcmp(out, "nan") == 0);
    F1("%e", nan_);
    LT_CHECK(strcmp(out, "nan") == 0);
    F1("[%8f]", inf);
    LT_CHECK_STR(out, "[     inf]");
    F1("[%08f]", inf);
    LT_CHECK_STR(out, "[     inf]");
    F1("[%+f]", inf);
    LT_CHECK_STR(out, "[+inf]");
    F1("%g", inf);
    LT_CHECK_STR(out, "inf");
}

LT_TEST(fmt_float_exp)
{
    F1("%e", 12345.6789);
    LT_CHECK_STR(out, "1.234568e+04");
    F1("%.2e", 0.000123);
    LT_CHECK_STR(out, "1.23e-04");
    F1("%E", 5.0);
    LT_CHECK_STR(out, "5.000000E+00");
    F1("%.0e", 9.6);
    LT_CHECK_STR(out, "1e+01");
    F1("%.3e", 9.9996);
    LT_CHECK_STR(out, "1.000e+01");
    F1("%e", 0.0);
    LT_CHECK_STR(out, "0.000000e+00");
    F1("%.1e", 1e100);
    LT_CHECK_STR(out, "1.0e+100");
    F1("%.1e", 1e-300);
    LT_CHECK_STR(out, "1.0e-300");
    F1("%.2e", -2.5e-7);
    LT_CHECK_STR(out, "-2.50e-07");
    F1("[%12.3e]", 1234.6);
    LT_CHECK_STR(out, "[   1.235e+03]");
    F1("%.1e", DBL_MAX);
    LT_CHECK_STR(out, "1.8e+308");
}

LT_TEST(fmt_float_g)
{
    F1("%g", 100000.0);
    LT_CHECK_STR(out, "100000");
    F1("%g", 1000000.0);
    LT_CHECK_STR(out, "1e+06");
    F1("%g", 0.0001);
    LT_CHECK_STR(out, "0.0001");
    F1("%g", 0.00001);
    LT_CHECK_STR(out, "1e-05");
    F1("%g", 1.5);
    LT_CHECK_STR(out, "1.5");
    F1("%g", 0.0);
    LT_CHECK_STR(out, "0");
    F1("%g", 123456789.0);
    LT_CHECK_STR(out, "1.23457e+08");
    F1("%.3g", 3.14159);
    LT_CHECK_STR(out, "3.14");
    F1("%.10g", 0.1);
    LT_CHECK_STR(out, "0.1");
    F1("%g", 999999.5);
    LT_CHECK_STR(out, "1e+06");
    F1("%G", 1e-10);
    LT_CHECK_STR(out, "1E-10");
    F1("%#g", 1.5);
    LT_CHECK_STR(out, "1.50000");
    F1("%.0g", 123.0);
    LT_CHECK_STR(out, "1e+02");
    F1("%g", -2.25);
    LT_CHECK_STR(out, "-2.25");
}

LT_TEST(fmt_double_helper)
{
    char b[64];
    lt_fmt_double(b, sizeof b, 1.5, 6);
    LT_CHECK_STR(b, "1.5");
    lt_fmt_double(b, sizeof b, 100.0, 3);
    LT_CHECK_STR(b, "100");
    lt_fmt_double(b, sizeof b, -0.25, 4);
    LT_CHECK_STR(b, "-0.25");
    lt_fmt_double(b, sizeof b, -0.0, 3);
    LT_CHECK_STR(b, "0");
    lt_fmt_double(b, sizeof b, -0.0001, 2);
    LT_CHECK_STR(b, "0");
    lt_fmt_double(b, sizeof b, 3.14159, 2);
    LT_CHECK_STR(b, "3.14");
    lt_fmt_double(b, sizeof b, 2.0, 0);
    LT_CHECK_STR(b, "2");
    lt_fmt_double(b, sizeof b, 1234567.125, 3);
    LT_CHECK_STR(b, "1234567.125");
    lt_fmt_double(b, 4, 123456.0, 1);
    LT_CHECK_STR(b, "123");
}

LT_TEST(err_msgs)
{
    lt_err_clear();
    LT_CHECK_STR(lt_err_get(), "");
    lt_err_set("bad %s at %d", "thing", 7);
    LT_CHECK_STR(lt_err_get(), "bad thing at 7");
    lt_err_clear();
    LT_CHECK_STR(lt_err_get(), "");
}

LT_TEST(strings)
{
    char *d, *n;
    char buf[16];
    char t[64];
    d = lt_strdup("hello");
    LT_CHECK_STR(d, "hello");
    lt_free(d);
    n = lt_strndup("hello", 3);
    LT_CHECK_STR(n, "hel");
    lt_free(n);
    n = lt_strndup("hi", 10);
    LT_CHECK_STR(n, "hi");
    lt_free(n);
    LT_CHECK(lt_stricmp("HeLLo", "hello") == 0);
    LT_CHECK(lt_stricmp("abc", "abd") < 0);
    LT_CHECK(lt_stricmp("abd", "ABC") > 0);
    LT_CHECK(lt_stricmp("ab", "abc") < 0);
    LT_CHECK(lt_strnicmp("HELLOx", "helloy", 5) == 0);
    LT_CHECK(lt_strnicmp("HELLOx", "helloy", 6) != 0);
    LT_CHECK(lt_str_starts("foobar", "foo"));
    LT_CHECK(!lt_str_starts("foobar", "bar"));
    LT_CHECK(!lt_str_starts("fo", "foo"));
    LT_CHECK(lt_str_ends("foobar", "bar"));
    LT_CHECK(!lt_str_ends("foobar", "foo"));
    LT_CHECK(!lt_str_ends("ar", "bar"));
    LT_CHECK(lt_str_istarts("FooBar", "fOO"));
    LT_CHECK(!lt_str_istarts("Fo", "foo"));
    strcpy(t, "  \t hi there \r\n");
    LT_CHECK_STR(lt_str_trim(t), "hi there");
    strcpy(t, "   ");
    LT_CHECK_STR(lt_str_trim(t), "");
    strcpy(t, "x");
    LT_CHECK_STR(lt_str_trim(t), "x");
    LT_CHECK_INT(lt_strlcpy(buf, "0123456789ABCDEFGHI", sizeof buf), 19);
    LT_CHECK_STR(buf, "0123456789ABCDE");
    LT_CHECK_INT(lt_strlcpy(buf, "ab", sizeof buf), 2);
    LT_CHECK_STR(buf, "ab");
    LT_CHECK_INT(lt_strlcat(buf, "cd", sizeof buf), 4);
    LT_CHECK_STR(buf, "abcd");
    LT_CHECK_INT(lt_strlcat(buf, "0123456789ABCDEF", sizeof buf), 20);
    LT_CHECK_STR(buf, "abcd0123456789A");
}

LT_TEST(parse_helpers)
{
    double d = 0;
    long l = 0;
    LT_CHECK(lt_parse_double("3.5", &d));
    LT_CHECK_NEAR(d, 3.5, 0);
    LT_CHECK(lt_parse_double("  -1e3  ", &d));
    LT_CHECK_NEAR(d, -1000.0, 0);
    LT_CHECK(lt_parse_double(".5", &d));
    LT_CHECK_NEAR(d, 0.5, 0);
    LT_CHECK(!lt_parse_double("", &d));
    LT_CHECK(!lt_parse_double("   ", &d));
    LT_CHECK(!lt_parse_double("1.5x", &d));
    LT_CHECK(!lt_parse_double("abc", &d));
    LT_CHECK(!lt_parse_double("1 2", &d));
    LT_CHECK(lt_parse_long("123", &l));
    LT_CHECK_INT(l, 123);
    LT_CHECK(lt_parse_long("-45", &l));
    LT_CHECK_INT(l, -45);
    LT_CHECK(lt_parse_long("+7", &l));
    LT_CHECK_INT(l, 7);
    LT_CHECK(lt_parse_long("0x1F", &l));
    LT_CHECK_INT(l, 31);
    LT_CHECK(lt_parse_long("-0xff", &l));
    LT_CHECK_INT(l, -255);
    LT_CHECK(lt_parse_long("  42  ", &l));
    LT_CHECK_INT(l, 42);
    LT_CHECK(lt_parse_long("0", &l));
    LT_CHECK_INT(l, 0);
    LT_CHECK(lt_parse_long("007", &l));
    LT_CHECK_INT(l, 7);
    LT_CHECK(!lt_parse_long("", &l));
    LT_CHECK(!lt_parse_long("-", &l));
    LT_CHECK(!lt_parse_long("0x", &l));
    LT_CHECK(!lt_parse_long("12a", &l));
    LT_CHECK(!lt_parse_long("1.5", &l));
    LT_CHECK(!lt_parse_long("99999999999999999999999", &l));
    LT_CHECK(lt_parse_long("2147483647", &l));
    LT_CHECK_INT(l, 2147483647L);
}

LT_TEST(math_helpers)
{
    double zero = 0.0;
    LT_CHECK_NEAR(lt_round(2.5), 3.0, 0);
    LT_CHECK_NEAR(lt_round(-2.5), -3.0, 0);
    LT_CHECK_NEAR(lt_round(2.4), 2.0, 0);
    LT_CHECK_NEAR(lt_round(-2.4), -2.0, 0);
    LT_CHECK_NEAR(lt_round(0.49999999999999994), 0.0, 0);
    LT_CHECK_NEAR(lt_trunc(2.9), 2.0, 0);
    LT_CHECK_NEAR(lt_trunc(-2.9), -2.0, 0);
    LT_CHECK(lt_isnan(zero / zero));
    LT_CHECK(!lt_isnan(1.0));
    LT_CHECK(!lt_isnan(HUGE_VAL));
    LT_CHECK(lt_isfinite(1.0));
    LT_CHECK(lt_isfinite(0.0));
    LT_CHECK(!lt_isfinite(HUGE_VAL));
    LT_CHECK(!lt_isfinite(-HUGE_VAL));
    LT_CHECK(!lt_isfinite(zero / zero));
}

LT_TEST(buf_basic)
{
    lt_buf b;
    char *s;
    int i;
    lt_buf_init(&b);
    lt_buf_puts(&b, "abc");
    lt_buf_putc(&b, 'd');
    lt_buf_append(&b, "efgh", 2);
    LT_CHECK_INT(b.len, 6);
    LT_CHECK_STR(b.data, "abcdef");
    lt_buf_printf(&b, "-%d-%s", 12, "xy");
    LT_CHECK_STR(b.data, "abcdef-12-xy");
    for (i = 0; i < 500; i++) lt_buf_printf(&b, "%d,", i);
    LT_CHECK(b.len > 1000);
    LT_CHECK(b.data[b.len] == '\0');
    LT_CHECK(strncmp(b.data + 12, "0,1,2,", 6) == 0);
    lt_buf_clear(&b);
    LT_CHECK_INT(b.len, 0);
    LT_CHECK_STR(b.data, "");
    {
        char big[600];
        memset(big, 'q', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        lt_buf_printf(&b, "%s|%s", big, big);
        LT_CHECK_INT(b.len, 1199);
        LT_CHECK(b.data[599] == '|' && b.data[1199] == '\0');
    }
    lt_buf_clear(&b);
    lt_buf_puts(&b, "taken");
    s = lt_buf_take(&b);
    LT_CHECK_STR(s, "taken");
    LT_CHECK(b.data == NULL && b.len == 0);
    lt_free(s);
    s = lt_buf_take(&b);
    LT_CHECK_STR(s, "");
    lt_free(s);
    lt_buf_free(&b);
}

LT_TEST(vec_ops)
{
    lt_vec v;
    int i, *p;
    lt_vec_init(&v, sizeof(int));
    for (i = 0; i < 100; i++) {
        p = (int *)lt_vec_push(&v);
        LT_CHECK(*p == 0);
        *p = i * 10;
    }
    LT_CHECK_INT(v.len, 100);
    LT_CHECK_INT(LT_VEC_AT(&v, int, 5), 50);
    LT_CHECK_INT(*(int *)lt_vec_at(&v, 99), 990);
    LT_CHECK(lt_vec_at(&v, 100) == NULL);
    lt_vec_remove(&v, 0);
    LT_CHECK_INT(v.len, 99);
    LT_CHECK_INT(LT_VEC_AT(&v, int, 0), 10);
    lt_vec_remove(&v, 50);
    LT_CHECK_INT(LT_VEC_AT(&v, int, 49), 500);
    LT_CHECK_INT(LT_VEC_AT(&v, int, 50), 520);
    lt_vec_remove(&v, v.len - 1);
    LT_CHECK_INT(v.len, 97);
    LT_CHECK_INT(LT_VEC_AT(&v, int, 96), 980);
    lt_vec_remove(&v, 1000);
    LT_CHECK_INT(v.len, 97);
    lt_vec_clear(&v);
    LT_CHECK_INT(v.len, 0);
    p = (int *)lt_vec_push(&v);
    LT_CHECK(*p == 0);
    lt_vec_free(&v);
    LT_CHECK(v.data == NULL && v.len == 0);
}

LT_TEST(map_basic)
{
    lt_map m;
    int a = 1, b = 2;
    lt_map_init(&m, LT_FALSE);
    LT_CHECK(lt_map_get(&m, "x") == NULL);
    LT_CHECK(!lt_map_has(&m, "x"));
    lt_map_put(&m, "alpha", &a);
    lt_map_put(&m, "beta", &b);
    LT_CHECK(lt_map_get(&m, "alpha") == &a);
    LT_CHECK(lt_map_get(&m, "beta") == &b);
    LT_CHECK(lt_map_get(&m, "ALPHA") == NULL);
    LT_CHECK(lt_map_has(&m, "beta"));
    lt_map_put(&m, "alpha", &b);
    LT_CHECK(lt_map_get(&m, "alpha") == &b);
    LT_CHECK_INT(m.len, 2);
    lt_map_put(&m, "nullval", NULL);
    LT_CHECK(lt_map_has(&m, "nullval"));
    LT_CHECK(lt_map_get(&m, "nullval") == NULL);
    lt_map_free(&m);
}

LT_TEST(map_icase)
{
    lt_map m;
    int a = 1, b = 2;
    lt_map_init(&m, LT_TRUE);
    lt_map_put(&m, "RpmHigh", &a);
    LT_CHECK(lt_map_get(&m, "rpmhigh") == &a);
    LT_CHECK(lt_map_get(&m, "RPMHIGH") == &a);
    LT_CHECK(lt_map_has(&m, "rPmHiGh"));
    lt_map_put(&m, "RPMHIGH", &b);
    LT_CHECK_INT(m.len, 1);
    LT_CHECK(lt_map_get(&m, "rpmhigh") == &b);
    LT_CHECK(!lt_map_has(&m, "rpmhigh2"));
    LT_CHECK(lt_hash_str("AbC", LT_TRUE) == lt_hash_str("aBc", LT_TRUE));
    LT_CHECK(lt_hash_str("AbC", LT_FALSE) != lt_hash_str("aBc", LT_FALSE));
    lt_map_free(&m);
}

LT_TEST(map_growth)
{
    lt_map m;
    static int vals[10000];
    char key[32];
    int i, found = 0;
    size_t used = 0, s;
    lt_map_init(&m, LT_FALSE);
    for (i = 0; i < 10000; i++) {
        vals[i] = i;
        lt_snprintf(key, sizeof key, "key_%d", i);
        lt_map_put(&m, key, &vals[i]);
    }
    LT_CHECK_INT(m.len, 10000);
    LT_CHECK(m.cap * 7 >= m.len * 10);
    for (i = 0; i < 10000; i++) {
        lt_snprintf(key, sizeof key, "key_%d", i);
        if (lt_map_get(&m, key) == &vals[i]) found++;
    }
    LT_CHECK_INT(found, 10000);
    LT_CHECK(!lt_map_has(&m, "key_10000"));
    for (s = 0; s < m.cap; s++) {
        if (m.slots[s].used == 1) used++;
    }
    LT_CHECK_INT(used, 10000);
    lt_map_free(&m);
    LT_CHECK(m.slots == NULL && m.len == 0);
}

LT_TEST(hash_crc)
{
    LT_CHECK(lt_hash_str("", LT_FALSE) == 0x811C9DC5UL);
    LT_CHECK(lt_hash_str("a", LT_FALSE) == 0xE40C292CUL);
    LT_CHECK(lt_hash_str("foobar", LT_FALSE) == 0xBF9CF968UL);
    LT_CHECK(lt_crc32(0, "123456789", 9) == 0xCBF43926UL);
    LT_CHECK(lt_crc32(0, "", 0) == 0UL);
    {
        lt_u32 c = lt_crc32(0, "1234", 4);
        c = lt_crc32(c, "56789", 5);
        LT_CHECK(c == 0xCBF43926UL);
    }
    LT_CHECK(lt_crc32(0, "a", 1) == 0xE8B7BE43UL);
}

LT_TEST(byte_order)
{
    lt_u8 b[4];
    b[0] = 0x12; b[1] = 0x34; b[2] = 0x56; b[3] = 0x78;
    LT_CHECK(lt_rd_be(b, 4) == 0x12345678UL);
    LT_CHECK(lt_rd_le(b, 4) == 0x78563412UL);
    LT_CHECK(lt_rd_be(b, 2) == 0x1234UL);
    LT_CHECK(lt_rd_le(b, 2) == 0x3412UL);
    LT_CHECK(lt_rd_be(b, 1) == 0x12UL);
    lt_wr_be(b, 4, 0xA1B2C3D4UL);
    LT_CHECK(b[0] == 0xA1 && b[1] == 0xB2 && b[2] == 0xC3 && b[3] == 0xD4);
    lt_wr_le(b, 4, 0xA1B2C3D4UL);
    LT_CHECK(b[0] == 0xD4 && b[1] == 0xC3 && b[2] == 0xB2 && b[3] == 0xA1);
    lt_wr_be(b, 2, 0xFFEEUL);
    LT_CHECK(b[0] == 0xFF && b[1] == 0xEE);
    lt_wr_le(b, 3, 0x010203UL);
    LT_CHECK(b[0] == 3 && b[1] == 2 && b[2] == 1);
    LT_CHECK(lt_rd_be(b, 0) == 0UL);
}

LT_TEST(f32_bits)
{
    double zero = 0.0, inf = HUGE_VAL, nan_ = zero / zero;
    double dn;
    LT_CHECK(lt_f32_to_bits(1.0) == 0x3F800000UL);
    LT_CHECK(lt_f32_to_bits(-2.5) == 0xC0200000UL);
    LT_CHECK(lt_f32_to_bits(0.1) == 0x3DCCCCCDUL);
    LT_CHECK(lt_f32_to_bits(0.0) == 0UL);
    LT_CHECK(lt_f32_to_bits(-0.0) == 0x80000000UL);
    LT_CHECK(lt_f32_to_bits(inf) == 0x7F800000UL);
    LT_CHECK(lt_f32_to_bits(-inf) == 0xFF800000UL);
    LT_CHECK(lt_f32_to_bits(nan_) == 0x7FC00000UL);
    LT_CHECK(lt_f32_to_bits(1e39) == 0x7F800000UL);
    LT_CHECK(lt_f32_to_bits(3.4028234663852886e38) == 0x7F7FFFFFUL);
    LT_CHECK(lt_f32_to_bits(1e-50) == 0UL);
    LT_CHECK(lt_f32_to_bits(1.17549435e-38) == 0x00800000UL);
    LT_CHECK(lt_f32_to_bits(1.4012984643248171e-45) == 1UL);
    LT_CHECK(lt_f32_to_bits(16777217.0) == 0x4B800000UL);
    LT_CHECK_NEAR(lt_f32_from_bits(0x3F800000UL), 1.0, 0);
    LT_CHECK_NEAR(lt_f32_from_bits(0xC0200000UL), -2.5, 0);
    LT_CHECK_NEAR(lt_f32_from_bits(0x3DCCCCCDUL), 0.100000001490116119384765625, 0);
    LT_CHECK_NEAR(lt_f32_from_bits(0x7F7FFFFFUL), 3.4028234663852886e38, 0);
    LT_CHECK(lt_f32_from_bits(0x7F800000UL) == inf);
    LT_CHECK(lt_f32_from_bits(0xFF800000UL) == -inf);
    LT_CHECK(lt_isnan(lt_f32_from_bits(0x7FC00000UL)));
    LT_CHECK(lt_isnan(lt_f32_from_bits(0x7F800001UL)));
    LT_CHECK(lt_f32_from_bits(0x80000000UL) == 0.0);
    F1("%f", lt_f32_from_bits(0x80000000UL));
    LT_CHECK_STR(out, "-0.000000");
    /* denormals */
    LT_CHECK_NEAR(lt_f32_from_bits(1UL), 1.401298464324817e-45, 1e-60);
    LT_CHECK_NEAR(lt_f32_from_bits(0x007FFFFFUL), 1.1754942106924411e-38, 1e-50);
    {
        lt_u32 bits = lt_f32_to_bits(1e-40);
        LT_CHECK(bits > 0UL && bits < 0x00800000UL);
        dn = lt_f32_from_bits(bits);
        LT_CHECK_NEAR(dn, 1e-40, 1e-45);
    }
    /* round trip some floats */
    {
        static const lt_u32 samples[] = { 0x3F800000UL, 0x40490FDBUL, 0x00000001UL,
            0x00800000UL, 0x7F7FFFFFUL, 0xBF000000UL, 0x12345678UL, 0x5F5E1000UL };
        size_t i;
        for (i = 0; i < LT_COUNTOF(samples); i++) {
            LT_CHECK(lt_f32_to_bits(lt_f32_from_bits(samples[i])) == samples[i]);
        }
    }
}

LT_TEST(path_helpers)
{
    char *j;
    j = lt_path_join("a", "b.txt");
    LT_CHECK_STR(j, "a/b.txt");
    lt_free(j);
    j = lt_path_join("a/", "b");
    LT_CHECK_STR(j, "a/b");
    lt_free(j);
    j = lt_path_join("", "b");
    LT_CHECK_STR(j, "b");
    lt_free(j);
    LT_CHECK_STR(lt_path_base("/x/y/file.ini"), "file.ini");
    LT_CHECK_STR(lt_path_base("file.ini"), "file.ini");
    LT_CHECK_STR(lt_path_base("C:\\dir\\f.msq"), "f.msq");
    LT_CHECK_STR(lt_path_base("/x/y/"), "");
    LT_CHECK_STR(lt_path_ext("/x/y/file.ini"), ".ini");
    LT_CHECK_STR(lt_path_ext("a.b/file"), "");
    LT_CHECK_STR(lt_path_ext("archive.tar.gz"), ".gz");
    LT_CHECK_STR(lt_path_ext(".hidden"), "");
    LT_CHECK_STR(lt_path_ext("noext"), "");
}

LT_TEST(file_io)
{
    const char *path = "build/lt_test_file.tmp";
    char data[300];
    char *r;
    size_t n = 0, i;
    for (i = 0; i < sizeof data; i++) data[i] = (char)(i & 0xFF);
    LT_CHECK_INT(lt_file_write(path, data, sizeof data), 0);
    LT_CHECK(lt_file_exists(path));
    r = lt_file_read(path, &n);
    LT_CHECK(r != NULL);
    LT_CHECK_INT(n, sizeof data);
    LT_CHECK(r && memcmp(r, data, sizeof data) == 0);
    LT_CHECK(r && r[n] == '\0');
    lt_free(r);
    LT_CHECK_INT(lt_file_write(path, "", 0), 0);
    r = lt_file_read(path, &n);
    LT_CHECK(r != NULL && n == 0 && r[0] == '\0');
    lt_free(r);
    remove(path);
    LT_CHECK(!lt_file_exists(path));
    lt_err_clear();
    LT_CHECK(lt_file_read("build/does/not/exist", NULL) == NULL);
    LT_CHECK(strlen(lt_err_get()) > 0);
    LT_CHECK(lt_file_write("build/does/not/exist", "x", 1) < 0);
}

LT_TEST(memory)
{
    char *p = (char *)lt_malloc(10);
    int *z;
    LT_CHECK(p != NULL);
    p = (char *)lt_realloc(p, 1000);
    LT_CHECK(p != NULL);
    lt_free(p);
    z = (int *)lt_calloc(16, sizeof(int));
    LT_CHECK(z[0] == 0 && z[15] == 0);
    lt_free(z);
    lt_free(NULL);
    p = (char *)lt_malloc(0);
    LT_CHECK(p != NULL);
    lt_free(p);
}

int main(void)
{
    LT_RUN(fmt_ints);
    LT_RUN(fmt_strings);
    LT_RUN(fmt_truncation);
    LT_RUN(fmt_float_fixed);
    LT_RUN(fmt_float_special);
    LT_RUN(fmt_float_exp);
    LT_RUN(fmt_float_g);
    LT_RUN(fmt_double_helper);
    LT_RUN(err_msgs);
    LT_RUN(strings);
    LT_RUN(parse_helpers);
    LT_RUN(math_helpers);
    LT_RUN(buf_basic);
    LT_RUN(vec_ops);
    LT_RUN(map_basic);
    LT_RUN(map_icase);
    LT_RUN(map_growth);
    LT_RUN(hash_crc);
    LT_RUN(byte_order);
    LT_RUN(f32_bits);
    LT_RUN(path_helpers);
    LT_RUN(file_io);
    LT_RUN(memory);
    LT_TEST_MAIN_END
}
