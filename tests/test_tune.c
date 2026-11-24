/* test_tune.c - .msq read/write, values, diff/merge, migration, .table, .tuneView */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lt.h"
#include "tune/tune.h"
#include "lt_test.h"

#define FIX "tests/fixtures/tune/"

static lt_tune *P(const char *s)
{
    return lt_tune_parse_msq(s, strlen(s));
}

static lt_tune_value sc(double d)
{
    lt_tune_value v;
    lt_tune_value_init(&v);
    v.scalar = d;
    return v;
}

static lt_tune_value str(const char *s)
{
    lt_tune_value v;
    lt_tune_value_init(&v);
    v.kind = LT_TV_STRING;
    v.str = lt_strdup(s);
    return v;
}

static void setsc(lt_tune *t, const char *name, double d, int page)
{
    lt_tune_value v = sc(d);
    lt_tune_set_constant(t, name, &v, page);
}

static char *save_str(const lt_tune *t)
{
    lt_buf b;
    lt_buf_init(&b);
    lt_tune_to_msq(t, &b);
    return lt_buf_take(&b);
}

static size_t count_sub(const char *h, const char *n)
{
    size_t c = 0, l = strlen(n);
    const char *p = h;
    while ((p = strstr(p, n)) != NULL) {
        c++;
        p += l;
    }
    return c;
}

/* ---------------- numbers ---------------- */
LT_TEST(number_format)
{
    char b[400];
    lt_fmt_f64_shortest(b, sizeof b, 14.7); LT_CHECK_STR(b, "14.7");
    lt_fmt_f64_shortest(b, sizeof b, 0.1); LT_CHECK_STR(b, "0.1");
    lt_fmt_f64_shortest(b, sizeof b, 0.0); LT_CHECK_STR(b, "0");
    lt_fmt_f64_shortest(b, sizeof b, -0.0); LT_CHECK_STR(b, "0");
    lt_fmt_f64_shortest(b, sizeof b, 1.0); LT_CHECK_STR(b, "1");
    lt_fmt_f64_shortest(b, sizeof b, -0.3); LT_CHECK_STR(b, "-0.3");
    lt_fmt_f64_shortest(b, sizeof b, 12.0); LT_CHECK_STR(b, "12");
    lt_fmt_f64_shortest(b, sizeof b, 1e21); LT_CHECK_STR(b, "1000000000000000000000");
    lt_fmt_f64_shortest(b, sizeof b, 1e-7); LT_CHECK_STR(b, "0.0000001");
    lt_fmt_f64_shortest(b, sizeof b, 123456.789); LT_CHECK_STR(b, "123456.789");
    lt_fmt_f64_shortest(b, sizeof b, 0.30000000000000004); LT_CHECK_STR(b, "0.30000000000000004");
    lt_fmt_f64_shortest(b, sizeof b, 100.0); LT_CHECK_STR(b, "100");
    lt_fmt_f64_shortest(b, sizeof b, 2.5e-3); LT_CHECK_STR(b, "0.0025");
    {
        volatile double z = 0.0;
        double nan = z / z;
        lt_fmt_f64_shortest(b, sizeof b, nan); LT_CHECK_STR(b, "0");
        lt_fmt_f64_shortest(b, sizeof b, HUGE_VAL); LT_CHECK_STR(b, "0");
        lt_fmt_f64_shortest(b, sizeof b, -HUGE_VAL); LT_CHECK_STR(b, "0");
    }
}

LT_TEST(number_roundtrip)
{
    double vals[8];
    size_t i;
    char b[400];
    vals[0] = 0.1; vals[1] = 1.0 / 3.0; vals[2] = 123456789.123456789; vals[3] = 1e-10;
    vals[4] = 6.02e23; vals[5] = 255.5; vals[6] = -4.9e-5; vals[7] = 65535.0;
    for (i = 0; i < 8; i++) {
        lt_fmt_f64_shortest(b, sizeof b, vals[i]);
        LT_CHECK(strtod(b, NULL) == vals[i]);
        LT_CHECK(strchr(b, 'e') == NULL);
    }
}

LT_TEST(value_parse)
{
    lt_tune_value v;
    lt_tune_value_parse(&v, "\"1\"");
    LT_CHECK(v.kind == LT_TV_STRING); LT_CHECK_STR(v.str, "1");
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "\"1.0\"");
    LT_CHECK(v.kind == LT_TV_STRING); LT_CHECK_STR(v.str, "1.0");
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "\"Speed Density\"");
    LT_CHECK(v.kind == LT_TV_STRING); LT_CHECK_STR(v.str, "Speed Density");
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "12.6");
    LT_CHECK(v.kind == LT_TV_SCALAR); LT_CHECK_NEAR(v.scalar, 12.6, 1e-12);
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "\"true\"");
    LT_CHECK(v.kind == LT_TV_BOOL); LT_CHECK(v.boolean);
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "false");
    LT_CHECK(v.kind == LT_TV_BOOL); LT_CHECK(!v.boolean);
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "  1 2 3 \n 4 ");
    LT_CHECK(v.kind == LT_TV_ARRAY); LT_CHECK_INT(v.n, 4); LT_CHECK_NEAR(v.arr[3], 4, 0);
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "[1, 2.5]");
    LT_CHECK(v.kind == LT_TV_ARRAY); LT_CHECK_INT(v.n, 2); LT_CHECK_NEAR(v.arr[1], 2.5, 0);
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "[7]");
    LT_CHECK(v.kind == LT_TV_SCALAR); LT_CHECK_NEAR(v.scalar, 7, 0);
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "[1 x 3]");
    LT_CHECK(v.kind == LT_TV_ARRAY); LT_CHECK_INT(v.n, 2);
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "[abc]");
    LT_CHECK(v.kind == LT_TV_STRING); LT_CHECK_STR(v.str, "[abc]");
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "Speed Density");
    LT_CHECK(v.kind == LT_TV_STRING); LT_CHECK_STR(v.str, "Speed Density");
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "1 2 x");
    LT_CHECK(v.kind == LT_TV_STRING);
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "0x10");
    LT_CHECK(v.kind == LT_TV_STRING);
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "-1e3");
    LT_CHECK(v.kind == LT_TV_SCALAR); LT_CHECK_NEAR(v.scalar, -1000, 0);
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "inf");
    LT_CHECK(v.kind == LT_TV_SCALAR); LT_CHECK(!lt_isfinite(v.scalar));
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "");
    LT_CHECK(v.kind == LT_TV_STRING); LT_CHECK_STR(v.str, "");
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "\"a &lt; b &quot;q&quot;\"");
    LT_CHECK_STR(v.str, "a < b \"q\"");
    lt_tune_value_clear(&v);
    lt_tune_value_parse(&v, "1\t2");
    LT_CHECK(v.kind == LT_TV_ARRAY);
    lt_tune_value_clear(&v);
}

LT_TEST(value_equal_copy)
{
    lt_tune_value a, b, c;
    double arr[3];
    arr[0] = 1; arr[1] = 2; arr[2] = 3;
    lt_tune_value_init(&a);
    a.kind = LT_TV_ARRAY;
    a.arr = (double *)lt_malloc(sizeof arr);
    memcpy(a.arr, arr, sizeof arr);
    a.n = 3;
    lt_tune_value_copy(&b, &a);
    LT_CHECK(lt_tune_value_equal(&a, &b));
    b.arr[2] = 3.0000001;
    LT_CHECK(!lt_tune_value_equal(&a, &b));
    b.arr[2] = 3.0 + 1e-10;
    LT_CHECK(lt_tune_value_equal(&a, &b));
    c = sc(1);
    LT_CHECK(!lt_tune_value_equal(&a, &c));
    lt_tune_value_clear(&a); lt_tune_value_clear(&b);
    a = str("x"); b = str("x");
    LT_CHECK(lt_tune_value_equal(&a, &b));
    lt_tune_value_clear(&b);
    b = str("y");
    LT_CHECK(!lt_tune_value_equal(&a, &b));
    lt_tune_value_clear(&a); lt_tune_value_clear(&b);
}

/* ---------------- msq ---------------- */
LT_TEST(determinism)
{
    lt_tune *t = lt_tune_new("speeduino 202501");
    char *s1, *s2, *s3;
    lt_u8 buf[32];
    int p;
    const char *q;
    int last = -1;
    for (p = 0; p < 12; p++) {
        memset(buf, p, sizeof buf);
        lt_tune_set_page(t, p, buf, sizeof buf);
    }
    setsc(t, "reqFuel", 12.5, 0);
    setsc(t, "stoich", 14.7, 0);
    s1 = save_str(t); s2 = save_str(t); s3 = save_str(t);
    LT_CHECK_STR(s1, s2);
    LT_CHECK_STR(s2, s3);
    LT_CHECK_INT(count_sub(s1, "<pageData page=\""), 12);
    for (q = s1; (q = strstr(q, "<pageData page=\"")) != NULL; q++) {
        int n = atoi(q + 16);
        LT_CHECK(n > last);
        last = n;
    }
    LT_CHECK(strstr(s1, "<pageData page=\"3\">03030303") != NULL);
    LT_CHECK(strstr(s1, "<versionInfo signature=\"speeduino 202501\" firmwareInfo=\"speeduino 202501\" nPages=\"12\"/>") != NULL);
    lt_free(s1); lt_free(s2); lt_free(s3);
    lt_tune_free(t);
}

LT_TEST(new_and_get)
{
    lt_tune *t = lt_tune_new("speeduino 202310");
    const lt_tune_entry *e;
    LT_CHECK_STR(t->signature, "speeduino 202310");
    LT_CHECK_STR(t->version, "1.1");
    LT_CHECK(t->created != NULL && t->modified != NULL);
    LT_CHECK_INT(strlen(t->created), 25);
    setsc(t, "reqFuel", 10.5, 0);
    e = lt_tune_get_constant(t, "reqFuel");
    LT_CHECK(e != NULL);
    if (e) LT_CHECK_NEAR(e->value.scalar, 10.5, 0);
    LT_CHECK(lt_tune_get_constant(t, "nope") == NULL);
    LT_CHECK(lt_tune_get_value(t, "reqFuel") == e);
    LT_CHECK_INT(lt_tune_n_constants(t), 1);
    setsc(t, "reqFuel", 11, 2);
    LT_CHECK_INT(lt_tune_n_constants(t), 1);
    LT_CHECK_INT(lt_tune_get_constant(t, "reqFuel")->page, 2);
    lt_tune_free(t);
}

LT_TEST(sorted_names)
{
    lt_tune *t = lt_tune_new("s");
    setsc(t, "b", 1, 0); setsc(t, "a", 1, 0); setsc(t, "c", 1, 0);
    LT_CHECK_STR(lt_tune_constant_name(t, 0), "a");
    LT_CHECK_STR(lt_tune_constant_name(t, 2), "c");
    setsc(t, "Z", 1, 0);
    LT_CHECK_STR(lt_tune_constant_name(t, 0), "Z");
    LT_CHECK(lt_tune_constant_name(t, 4) == NULL);
    lt_tune_free(t);
}

LT_TEST(self_closing)
{
    static const char *m =
        "<?xml version=\"1.0\"?>\n<!DOCTYPE msq PUBLIC \"-//rusEfi//DTD msq//EN\" \"msq.dtd\">\n"
        "<msq><versionInfo firmwareInfo=\"rusEFI test\"/><page number=\"0\">"
        "<constant name=\"vinNumber\"/><constant name=\"engineType\">25</constant>"
        "<constant name=\"tuneHidingKey\"/><constant name=\"someValue\">42.5</constant></page></msq>";
    lt_tune *t = P(m);
    LT_CHECK(t != NULL);
    if (!t) return;
    LT_CHECK_STR(t->signature, "");
    LT_CHECK(lt_tune_get_constant(t, "vinNumber")->value.kind == LT_TV_STRING);
    LT_CHECK_STR(lt_tune_get_constant(t, "vinNumber")->value.str, "");
    LT_CHECK_NEAR(lt_tune_get_constant(t, "engineType")->value.scalar, 25, 0);
    LT_CHECK_NEAR(lt_tune_get_constant(t, "someValue")->value.scalar, 42.5, 0);
    LT_CHECK(t->firmware_info == NULL);
    lt_tune_free(t);
}

LT_TEST(pagedata)
{
    static const char *m =
        "<msq><versionInfo signature=\"x\"/><pageData page=\"0\">0102030405060708</pageData>"
        "<pageData page=\"1\"> aa bb\n cc dd </pageData><pageData page=\"2\">zz</pageData>"
        "<pageData page=\"3\">0g1</pageData><pageData page=\"x\">00</pageData>"
        "<pageData page=\"4\">ABcd9</pageData>"
        "<page number=\"0\"><constant name=\"testValue\">100</constant></page></msq>";
    lt_tune *t = P(m);
    size_t len = 0;
    const lt_u8 *p;
    LT_CHECK(t != NULL);
    if (!t) return;
    p = lt_tune_get_page(t, 0, &len);
    LT_CHECK(p != NULL && len == 8 && p[0] == 1 && p[7] == 8);
    p = lt_tune_get_page(t, 1, &len);
    LT_CHECK(p != NULL && len == 4 && p[0] == 0xaa && p[3] == 0xdd);
    LT_CHECK(lt_tune_get_page(t, 2, &len) == NULL);
    LT_CHECK(lt_tune_get_page(t, 3, &len) == NULL);
    p = lt_tune_get_page(t, 4, &len);
    LT_CHECK(p != NULL && len == 2 && p[0] == 0xab && p[1] == 0xcd);
    LT_CHECK_NEAR(lt_tune_get_constant(t, "testValue")->value.scalar, 100, 0);
    lt_tune_free(t);
}

LT_TEST(quoted_bits)
{
    static const char *m =
        "<msq><versionInfo signature=\"x\"/><page number=\"0\">"
        "<constant name=\"a\">\"1\"</constant><constant name=\"b\">\"1.0\"</constant>"
        "<constant name=\"c\">\"Speed Density\"</constant><constant name=\"d\">12.6</constant>"
        "<constant name=\"e\">\"true\"</constant></page></msq>";
    lt_tune *t = P(m);
    char *s;
    LT_CHECK(t != NULL);
    if (!t) return;
    LT_CHECK(lt_tune_get_constant(t, "a")->value.kind == LT_TV_STRING);
    LT_CHECK_STR(lt_tune_get_constant(t, "a")->value.str, "1");
    LT_CHECK_STR(lt_tune_get_constant(t, "b")->value.str, "1.0");
    LT_CHECK_STR(lt_tune_get_constant(t, "c")->value.str, "Speed Density");
    LT_CHECK(lt_tune_get_constant(t, "d")->value.kind == LT_TV_SCALAR);
    LT_CHECK(lt_tune_get_constant(t, "e")->value.kind == LT_TV_BOOL);
    s = save_str(t);
    LT_CHECK(strstr(s, ">\"1.0\"</constant>") != NULL);
    LT_CHECK(strstr(s, "name=\"e\">true</constant>") != NULL);
    lt_free(s);
    lt_tune_free(t);
}

LT_TEST(sample_file)
{
    lt_tune *t = lt_tune_load(FIX "sample.msq");
    lt_tune *t2;
    char *s, *s2;
    const lt_tune_entry *e;
    lt_tune_diff *d;
    LT_CHECK(t != NULL);
    if (!t) return;
    LT_CHECK_STR(t->signature, "speeduino 202501");
    LT_CHECK_STR(t->author, "LibreTune");
    LT_CHECK_STR(t->modified, "Sun May 03 20:46:57 NZST 2026");
    LT_CHECK(t->created == NULL);
    LT_CHECK_INT(t->n_ecu_settings, 2);
    LT_CHECK_STR(t->ecu_settings[0], "CELSIUS");
    LT_CHECK_STR(t->ecu_settings[1], "AFR");
    LT_CHECK_STR(t->page_sizes[1], "288");
    LT_CHECK(t->page_sizes[0] == NULL);
    e = lt_tune_get_constant(t, "wueRates");
    LT_CHECK(e != NULL && e->value.kind == LT_TV_ARRAY && e->value.n == 10);
    LT_CHECK(e->has_attrs);
    LT_CHECK_STR(e->attrs.cols, "1"); LT_CHECK_STR(e->attrs.rows, "10");
    LT_CHECK_STR(e->attrs.units, "%"); LT_CHECK_STR(e->attrs.digits, "0");
    LT_CHECK_INT(e->page, 1);
    LT_CHECK_NEAR(lt_tune_get_constant(t, "reqFuel")->value.scalar, 12.6, 0);
    LT_CHECK_NEAR(lt_tune_get_pc_variable(t, "rpmhigh")->value.scalar, 7000, 0);
    LT_CHECK_STR(lt_tune_get_pc_variable(t, "rpmhigh")->attrs.units, "rpm");
    LT_CHECK(lt_tune_get_constant(t, "rpmhigh") == NULL);
    LT_CHECK(lt_tune_get_value(t, "rpmhigh") != NULL);
    s = save_str(t);
    LT_CHECK(strstr(s, "<constant cols=\"1\" digits=\"0\" name=\"wueRates\" rows=\"10\" units=\"%\">150 136 131") != NULL);
    LT_CHECK(strstr(s, "<pcVariable digits=\"0\" name=\"rpmhigh\" units=\"rpm\">7000</pcVariable>") != NULL);
    LT_CHECK(strstr(s, "<setting name=\"CELSIUS\"/>") != NULL);
    LT_CHECK(strstr(s, "<page number=\"1\" size=\"288\">") != NULL);
    LT_CHECK(strstr(s, "<page number=\"-1\">") != NULL);
    LT_CHECK(strstr(s, "<bibliography author=\"LibreTune\" writeDate=\"Sun May 03 20:46:57 NZST 2026\"/>") != NULL);
    t2 = P(s);
    LT_CHECK(t2 != NULL);
    d = lt_tune_diff_compare(t, t2);
    LT_CHECK(lt_tune_diff_identical(d));
    lt_tune_diff_free(d);
    LT_CHECK_STR(t2->page_sizes[1], "288");
    LT_CHECK_INT(t2->n_ecu_settings, 2);
    LT_CHECK_STR(lt_tune_get_constant(t2, "wueRates")->attrs.units, "%");
    s2 = save_str(t2);
    LT_CHECK_STR(s, s2);
    lt_free(s); lt_free(s2);
    lt_tune_free(t); lt_tune_free(t2);
}

LT_TEST(minimal_file)
{
    static const char *m = "<msq><versionInfo signature=\"speeduino 202501\"/>"
        "<page number=\"0\"><constant name=\"reqFuel\">12.6</constant></page></msq>";
    lt_tune *t = P(m);
    char *s;
    LT_CHECK(t != NULL);
    if (!t) return;
    s = save_str(t);
    LT_CHECK(strstr(s, "<constant name=\"reqFuel\">12.6</constant>") != NULL);
    LT_CHECK(strstr(s, "<settings") == NULL);
    LT_CHECK(strstr(s, "<bibliography") == NULL);
    lt_free(s);
    lt_tune_free(t);
}

LT_TEST(invalid_files)
{
    lt_tune *t;
    t = P("<msq><page number=\"-1\"><pcVariable name=\"a\">1</pcVariable></page></msq>");
    LT_CHECK(t == NULL);
    LT_CHECK_STR(lt_err_get(), "Not a valid MSQ file");
    t = P("garbage");
    LT_CHECK(t == NULL);
    t = P("<msq></msq>");
    LT_CHECK(t == NULL);
    t = P("<msq signature=\"abc\"></msq>");
    LT_CHECK(t != NULL);
    if (t) LT_CHECK_STR(t->signature, "abc");
    lt_tune_free(t);
    t = lt_tune_load("tests/fixtures/tune/does_not_exist.msq");
    LT_CHECK(t == NULL);
}

LT_TEST(page_walk)
{
    static const char *m =
        "<msq><versionInfo signature=\"s\"/>"
        "<page number=\"2\"><constant name=\"a\">1</constant></page>"
        "<page number=\"-1\"><constant name=\"b\">2</constant><pcVariable name=\"p\">3</pcVariable></page>"
        "<page><constant name=\"c\">3</constant></page>"
        "<page number=\"300\"><constant name=\"d\">4</constant></page>"
        "<page number=\"5\"><constant name=\"e\">5</constant></page></msq>";
    lt_tune *t = P(m);
    LT_CHECK(t != NULL);
    if (!t) return;
    LT_CHECK_INT(lt_tune_get_constant(t, "a")->page, 2);
    LT_CHECK_INT(lt_tune_get_constant(t, "b")->page, 2);
    LT_CHECK_INT(lt_tune_get_constant(t, "c")->page, 2);
    LT_CHECK_INT(lt_tune_get_constant(t, "d")->page, 2);
    LT_CHECK_INT(lt_tune_get_constant(t, "e")->page, 5);
    LT_CHECK(lt_tune_get_pc_variable(t, "p") != NULL);
    lt_tune_free(t);
}

LT_TEST(legacy_fallback)
{
    lt_tune *t = P("<msq><constant name=\"x\">1</constant><constant name=\"y\">2</constant>"
                   "<pcVariable name=\"p\">1</pcVariable></msq>");
    LT_CHECK(t != NULL);
    if (!t) return;
    LT_CHECK_INT(lt_tune_n_constants(t), 2);
    LT_CHECK_INT(lt_tune_get_constant(t, "x")->page, 0);
    LT_CHECK(lt_tune_get_pc_variable(t, "p") == NULL);
    lt_tune_free(t);
    t = P("<msq signature=\"s\"><pcVariable name=\"p\">1</pcVariable></msq>");
    LT_CHECK(t != NULL);
    if (!t) return;
    LT_CHECK(lt_tune_get_pc_variable(t, "p") != NULL);
    lt_tune_free(t);
}

LT_TEST(signature_fallbacks)
{
    lt_tune *t = P("<msq signature=\"fromroot\"><page number=\"0\"><constant name=\"a\">1</constant></page></msq>");
    LT_CHECK(t && strcmp(t->signature, "fromroot") == 0);
    lt_tune_free(t);
    t = P("<msq timestamp=\"T1\"><versionInfo signature=\"q\"/></msq>");
    LT_CHECK(t != NULL);
    if (t) LT_CHECK_STR(t->modified, "T1");
    lt_tune_free(t);
}

LT_TEST(escaping)
{
    lt_tune *t = lt_tune_new("sig<&>");
    lt_tune *r;
    lt_tune_value v = str("R&D \"quoted\" <x> 'y'");
    char *s;
    t->author = lt_strdup("R&D \"quoted\" <lab>");
    lt_tune_set_constant(t, "label", &v, 0);
    lt_tune_value_clear(&v);
    s = save_str(t);
    LT_CHECK(strstr(s, "&amp;") != NULL);
    LT_CHECK(strstr(s, "&lt;") != NULL);
    LT_CHECK(strstr(s, "&quot;") != NULL);
    LT_CHECK(strstr(s, "R&D") == NULL);
    LT_CHECK(strstr(s, "<lab>") == NULL);
    r = P(s);
    LT_CHECK(r != NULL);
    if (r) {
        LT_CHECK_STR(r->author, "R&D \"quoted\" <lab>");
        LT_CHECK_STR(lt_tune_get_constant(r, "label")->value.str, "R&D \"quoted\" <x> 'y'");
        LT_CHECK_STR(r->signature, "sig<&>");
    }
    lt_free(s);
    lt_tune_free(t);
    lt_tune_free(r);
}

LT_TEST(number_output)
{
    lt_tune *t = lt_tune_new("s");
    lt_tune_value v;
    char *s;
    lt_tune *r;
    setsc(t, "stoich", 14.7, 0);
    setsc(t, "tiny", 0.1, 0);
    setsc(t, "zero", 0.0, 0);
    lt_tune_value_init(&v);
    v.kind = LT_TV_ARRAY;
    v.arr = (double *)lt_malloc(3 * sizeof(double));
    v.arr[0] = 14.7; v.arr[1] = 1.0; v.arr[2] = -0.3; v.n = 3;
    lt_tune_set_constant(t, "arr", &v, 0);
    lt_tune_value_clear(&v);
    v = sc(14.7);
    lt_tune_set_pc_variable(t, "pcstoich", &v);
    s = save_str(t);
    LT_CHECK(strstr(s, ">14.7<") != NULL);
    LT_CHECK(strstr(s, ">0.1<") != NULL);
    LT_CHECK(strstr(s, ">0<") != NULL);
    LT_CHECK(strstr(s, ">14.7 1 -0.3<") != NULL);
    LT_CHECK(count_sub(s, ">14.7<") >= 2);
    r = P(s);
    LT_CHECK(r != NULL);
    if (r) LT_CHECK_NEAR(lt_tune_get_constant(r, "stoich")->value.scalar, 14.7, 1e-12);
    lt_free(s);
    lt_tune_free(t);
    lt_tune_free(r);
}

LT_TEST(sanitize)
{
    lt_tune *t = lt_tune_new("s");
    lt_tune_value v = str("<constant name=x>");
    lt_tune_value w = str("<pcVariable junk");
    lt_tune_value ok = str("<b>fine</b>");
    char *s;
    lt_tune *r;
    lt_tune_set_constant(t, "bad", &v, 0);
    lt_tune_set_constant(t, "bad2", &w, 0);
    lt_tune_set_constant(t, "ok", &ok, 0);
    LT_CHECK_INT(lt_tune_sanitize(t), 2);
    LT_CHECK_STR(lt_tune_get_constant(t, "bad")->value.str, "");
    LT_CHECK_STR(lt_tune_get_constant(t, "ok")->value.str, "<b>fine</b>");
    /* same through load: bypass the writer escaping with raw text */
    r = P("<msq><versionInfo signature=\"s\"/><page number=\"0\"><constant name=\"bad\">\"&lt;constant name=x&gt;\"</constant></page></msq>");
    LT_CHECK(r != NULL);
    if (r) LT_CHECK_STR(lt_tune_get_constant(r, "bad")->value.str, "");
    s = save_str(t);
    lt_free(s);
    lt_tune_value_clear(&v); lt_tune_value_clear(&w); lt_tune_value_clear(&ok);
    lt_tune_free(t); lt_tune_free(r);
}

LT_TEST(pages_api)
{
    lt_tune *t = lt_tune_new("s");
    lt_u8 d[8], x[2];
    size_t len = 0;
    const lt_u8 *p;
    lt_pagesrc ps;
    memset(d, 7, sizeof d);
    x[0] = 1; x[1] = 2;
    LT_CHECK(lt_tune_patch_page(t, 0, 0, x, 2) < 0);
    lt_tune_set_page(t, 0, d, sizeof d);
    LT_CHECK_INT(lt_tune_patch_page(t, 0, 6, x, 2), 0);
    LT_CHECK(lt_tune_patch_page(t, 0, 7, x, 2) < 0);
    p = lt_tune_get_page(t, 0, &len);
    LT_CHECK(len == 8 && p[6] == 1 && p[7] == 2 && p[5] == 7);
    ps = lt_tune_pagesrc(t);
    LT_CHECK(ps.page(ps.ud, 0, &len) == p);
    LT_CHECK_INT(ps.write(ps.ud, 0, 0, x, 1), 0);
    LT_CHECK(p[0] == 1);
    LT_CHECK(ps.page(ps.ud, 1, &len) == NULL);
    LT_CHECK(lt_tune_get_page(t, 300, &len) == NULL);
    lt_tune_set_page(t, 0, x, 2);
    LT_CHECK(lt_tune_get_page(t, 0, &len) != NULL && len == 2);
    lt_tune_free(t);
}

LT_TEST(empty_pagedata_and_latin1)
{
    lt_tune *t = lt_tune_new("s");
    char *s;
    char raw[200];
    lt_tune *r;
    lt_tune_set_page(t, 3, (const lt_u8 *)"", 0);
    setsc(t, "a", 1, 0);
    s = save_str(t);
    LT_CHECK(strstr(s, "<pageData page=\"3\"></pageData>") != NULL);
    lt_free(s);
    lt_tune_free(t);
    /* Latin-1 encoded byte 0xE9 in an author attribute becomes UTF-8 */
    lt_snprintf(raw, sizeof raw, "<msq><versionInfo signature=\"s\"/><bibliography author=\"Ren%c\"/></msq>", 0xE9);
    r = lt_tune_parse_msq(raw, strlen(raw));
    LT_CHECK(r != NULL);
    if (r) LT_CHECK_STR(r->author, "Ren\xC3\xA9");
    lt_tune_free(r);
    lt_snprintf(raw, sizeof raw, "<msq><versionInfo signature=\"s\"/><bibliography author=\"Ren\xC3\xA9\"/></msq>");
    r = lt_tune_parse_msq(raw, strlen(raw));
    LT_CHECK(r != NULL);
    if (r) LT_CHECK_STR(r->author, "Ren\xC3\xA9");
    lt_tune_free(r);
}

LT_TEST(description_comment_and_meta)
{
    lt_tune *t = lt_tune_new("s");
    lt_tune *r;
    char *s;
    t->description = lt_strdup("a--b-");
    t->ini_metadata = (lt_ini_metadata *)lt_calloc(1, sizeof *t->ini_metadata);
    t->ini_metadata->signature = lt_strdup("sig");
    t->ini_metadata->name = lt_strdup("x.ini");
    t->ini_metadata->hash = lt_strdup("abc");
    t->ini_metadata->spec_version = lt_strdup("3.0");
    t->ini_metadata->saved_at = lt_strdup("now");
    t->has_manifest = LT_TRUE;
    t->manifest = (lt_manifest_entry *)lt_calloc(2, sizeof(lt_manifest_entry));
    t->n_manifest = 2;
    t->manifest[0].name = lt_strdup("m1"); t->manifest[0].type = lt_strdup("U08");
    t->manifest[0].page = 3; t->manifest[0].offset = 50; t->manifest[0].scale = 1.0;
    t->manifest[1].name = lt_strdup("m2"); t->manifest[1].type = lt_strdup("S16");
    t->manifest[1].page = 0; t->manifest[1].offset = 7; t->manifest[1].scale = 0.1;
    t->manifest[1].translate = -40;
    setsc(t, "a", 1, 0);
    s = save_str(t);
    LT_CHECK(strstr(s, "<!-- a- -b-  -->") != NULL);
    LT_CHECK(strstr(s, "  <iniMetadata signature=\"sig\" name=\"x.ini\" hash=\"abc\" specVersion=\"3.0\" savedAt=\"now\"/>") != NULL);
    LT_CHECK(strstr(s, "  <constantManifest>\n    <entry name=\"m1\" type=\"U08\" page=\"3\" offset=\"50\" scale=\"1\" translate=\"0\"/>") != NULL);
    LT_CHECK(strstr(s, "<entry name=\"m2\" type=\"S16\" page=\"0\" offset=\"7\" scale=\"0.1\" translate=\"-40\"/>") != NULL);
    r = P(s);
    LT_CHECK(r != NULL);
    if (r) {
        LT_CHECK(r->ini_metadata != NULL);
        LT_CHECK_STR(r->ini_metadata->hash, "abc");
        LT_CHECK_INT(r->n_manifest, 2);
        LT_CHECK_NEAR(r->manifest[1].scale, 0.1, 0);
        LT_CHECK_NEAR(r->manifest[1].translate, -40, 0);
        LT_CHECK_INT(r->manifest[0].page, 3);
        LT_CHECK_INT(r->manifest[0].offset, 50);
        LT_CHECK(r->description == NULL);
    }
    lt_free(s);
    lt_tune_free(t);
    lt_tune_free(r);
}

LT_TEST(manifest_edge_cases)
{
    lt_tune *t = P("<msq><versionInfo signature=\"s\"/><constantManifest>"
                   "<entry name=\"a\" type=\"U08\" page=\"x\" offset=\"y\" scale=\"z\" translate=\"w\"/>"
                   "<entry type=\"U08\"/><entry name=\"\"/><entry name=\"b\"/></constantManifest></msq>");
    LT_CHECK(t != NULL);
    if (!t) return;
    LT_CHECK_INT(t->n_manifest, 2);
    LT_CHECK_STR(t->manifest[0].name, "a");
    LT_CHECK_INT(t->manifest[0].page, 0);
    LT_CHECK_NEAR(t->manifest[0].scale, 1.0, 0);
    LT_CHECK_NEAR(t->manifest[0].translate, 0.0, 0);
    LT_CHECK_STR(t->manifest[1].type, "");
    lt_tune_free(t);
    t = P("<msq><versionInfo signature=\"s\"/><constantManifest></constantManifest></msq>");
    LT_CHECK(t != NULL);
    if (t) LT_CHECK(!t->has_manifest);
    lt_tune_free(t);
}

LT_TEST(clone_and_save_load)
{
    lt_tune *t = lt_tune_load(FIX "sample.msq");
    lt_tune *c;
    lt_tune *l;
    lt_tune_diff *d;
    lt_u8 b[4];
    char *s1, *s2;
    memset(b, 9, 4);
    lt_tune_set_page(t, 1, b, 4);
    c = lt_tune_clone(t);
    s1 = save_str(t); s2 = save_str(c);
    LT_CHECK_STR(s1, s2);
    lt_free(s1); lt_free(s2);
    LT_CHECK_INT(lt_tune_save(c, "build/lt_tune_test_out.msq"), 0);
    l = lt_tune_load("build/lt_tune_test_out.msq");
    LT_CHECK(l != NULL);
    if (l) {
        d = lt_tune_diff_compare(t, l);
        LT_CHECK(lt_tune_diff_identical(d));
        lt_tune_diff_free(d);
        LT_CHECK(lt_tune_get_page(l, 1, NULL) != NULL);
    }
    LT_CHECK(lt_tune_save(c, "build/x.json") < 0);
    lt_tune_free(t); lt_tune_free(c); lt_tune_free(l);
}

LT_TEST(pc_variable_file)
{
    lt_tune *t = lt_tune_new("sig");
    lt_tune *u = lt_tune_new("sig");
    lt_tune_value v = sc(7000);
    lt_tune_value s = str("hello");
    char *txt;
    size_t n;
    lt_tune_set_pc_variable(t, "rpmhigh", &v);
    lt_tune_set_pc_variable(t, "name", &s);
    LT_CHECK_INT(lt_tune_save_pc_variables(t, "build/lt_pcvars_test.msq", "sig<1>"), 0);
    txt = lt_file_read("build/lt_pcvars_test.msq", &n);
    LT_CHECK(txt != NULL);
    if (txt) {
        LT_CHECK(strstr(txt, "<versionInfo fileFormat=\"4.0\" signature=\"sig&lt;1&gt;\"/>") != NULL);
        LT_CHECK(strstr(txt, "<page number=\"-1\">\n<constant name=\"name\">\"hello\"</constant>\n<constant name=\"rpmhigh\">7000</constant>\n</page>") != NULL);
        LT_CHECK(strstr(txt, "<bibliography author=\"clibretune\" writeDate=\"") != NULL);
        lt_free(txt);
    }
    LT_CHECK_INT(lt_tune_load_pc_variables(u, "build/lt_pcvars_test.msq"), 0);
    LT_CHECK(lt_tune_get_pc_variable(u, "rpmhigh") != NULL);
    LT_CHECK_NEAR(lt_tune_get_pc_variable(u, "rpmhigh")->value.scalar, 7000, 0);
    LT_CHECK_STR(lt_tune_get_pc_variable(u, "name")->value.str, "hello");
    LT_CHECK(lt_tune_get_constant(u, "rpmhigh") == NULL);
    /* empty pc variable set still loads (signature present) */
    {
        lt_tune *e = lt_tune_new("sig");
        LT_CHECK_INT(lt_tune_save_pc_variables(e, "build/lt_pcvars_empty.msq", "sig"), 0);
        LT_CHECK_INT(lt_tune_load_pc_variables(u, "build/lt_pcvars_empty.msq"), 0);
        lt_tune_free(e);
    }
    lt_tune_value_clear(&s);
    lt_tune_free(t); lt_tune_free(u);
}

/* ---------------- diff/merge ---------------- */
LT_TEST(diff_basic)
{
    lt_tune *a = lt_tune_new("test"), *b = lt_tune_new("test");
    lt_tune_diff *d = lt_tune_diff_compare(a, b);
    LT_CHECK(lt_tune_diff_identical(d));
    lt_tune_diff_free(d);
    setsc(a, "reqFuel", 10, 0); setsc(b, "reqFuel", 12, 0);
    d = lt_tune_diff_compare(a, b);
    LT_CHECK_INT(d->n, 1);
    LT_CHECK(!lt_tune_diff_identical(d));
    LT_CHECK(d->diffs[0].has_numeric && d->diffs[0].has_percent);
    LT_CHECK_NEAR(d->diffs[0].numeric_diff, 2.0, 1e-12);
    LT_CHECK_NEAR(d->diffs[0].percent_diff, 20.0, 1e-9);
    LT_CHECK_INT(d->sum_a, 1);
    lt_tune_diff_free(d);
    lt_tune_free(a); lt_tune_free(b);
}

LT_TEST(diff_union_and_kinds)
{
    lt_tune *a = lt_tune_new("A"), *b = lt_tune_new("B");
    lt_tune_value s1 = str("x"), s2 = str("y");
    lt_tune_diff *d;
    setsc(a, "onlyA", 1, 0); setsc(b, "onlyB", 2, 0); setsc(a, "zero", 0, 0); setsc(b, "zero", 5, 0);
    lt_tune_set_constant(a, "s", &s1, 0); lt_tune_set_constant(b, "s", &s2, 0);
    setsc(a, "mix", 1, 0); lt_tune_set_constant(b, "mix", &s1, 0);
    setsc(a, "same", 3, 0); setsc(b, "same", 3.0000000001, 0);
    d = lt_tune_diff_compare(a, b);
    LT_CHECK_INT(d->n, 5);
    LT_CHECK(!d->signature_match);
    LT_CHECK_STR(d->sig_a, "A");
    LT_CHECK_STR(d->diffs[0].name, "mix");
    LT_CHECK(!d->diffs[0].has_numeric);
    LT_CHECK_STR(d->diffs[1].name, "onlyA");
    LT_CHECK(d->diffs[1].value_a != NULL && d->diffs[1].value_b == NULL);
    LT_CHECK_STR(d->diffs[2].name, "onlyB");
    LT_CHECK(d->diffs[2].value_a == NULL && d->diffs[2].value_b != NULL);
    LT_CHECK_STR(d->diffs[3].name, "s");
    LT_CHECK_STR(d->diffs[4].name, "zero");
    LT_CHECK(d->diffs[4].has_numeric && !d->diffs[4].has_percent);
    lt_tune_diff_free(d);
    lt_tune_value_clear(&s1); lt_tune_value_clear(&s2);
    lt_tune_free(a); lt_tune_free(b);
}

LT_TEST(merge)
{
    lt_tune *t = lt_tune_new("t"), *s = lt_tune_new("t");
    const char *names[3];
    size_t n;
    setsc(t, "reqFuel", 10, 0); setsc(t, "advance", 15, 0);
    setsc(s, "reqFuel", 12, 3); setsc(s, "advance", 20, 0); setsc(s, "newConst", 5, 4);
    names[0] = "reqFuel"; names[1] = "newConst"; names[2] = "missing";
    n = lt_tune_merge_selected(t, s, names, 3);
    LT_CHECK_INT(n, 2);
    LT_CHECK_NEAR(lt_tune_get_constant(t, "reqFuel")->value.scalar, 12, 0);
    LT_CHECK_INT(lt_tune_get_constant(t, "reqFuel")->page, 3);
    LT_CHECK_NEAR(lt_tune_get_constant(t, "advance")->value.scalar, 15, 0);
    LT_CHECK(lt_tune_get_constant(t, "newConst") != NULL);
    LT_CHECK_INT(lt_tune_merge_selected(t, s, names + 2, 1), 0);
    lt_tune_free(t); lt_tune_free(s);
}

/* ---------------- migration ---------------- */
static const char *MIG_INI =
    "[MegaTune]\n signature = \"sig\"\n[Constants]\n"
    "pageSize = 128, 128, 128, 128, 128, 128\n page = 5\n"
    " egoCorrection = scalar, U08, 100, \"%\", 1.0, 0.0, 0, 255, 0\n"
    " rpmLimit = scalar, U16, 100, \"rpm\", 1.0, 0.0, 0, 255, 0\n"
    " scaled = scalar, U08, 110, \"%\", 0.5, 0.0, 0, 255, 0\n"
    " fresh = scalar, U08, 111, \"%\", 1.0, 0.0, 0, 255, 0\n";

static lt_manifest_entry me(const char *n, const char *ty, lt_u8 p, lt_u32 o, double sc_, double tr)
{
    lt_manifest_entry m;
    m.name = (char *)n; m.type = (char *)ty; m.page = p; m.offset = o; m.scale = sc_; m.translate = tr;
    return m;
}

LT_TEST(migration)
{
    lt_ini *ini = lt_ini_load_string(MIG_INI, strlen(MIG_INI), NULL, NULL);
    lt_migration_report r;
    lt_manifest_entry m[1];
    char *sum;
    LT_CHECK(ini != NULL);
    if (!ini) return;
    lt_migration_init(&r);
    LT_CHECK(!lt_migration_has_changes(&r));
    LT_CHECK_STR(r.severity, "none");
    LT_CHECK(r.can_auto_migrate && !r.requires_user_review);
    /* all four ini constants unknown to an empty manifest */
    lt_migration_compare(&r, NULL, 0, ini);
    LT_CHECK_INT(r.n_missing_in_tune, 4);
    LT_CHECK_STR(r.missing_in_tune[0], "egoCorrection");
    LT_CHECK_STR(r.missing_in_tune[3], "scaled");
    LT_CHECK_STR(r.severity, "low");
    sum = lt_migration_summary(&r);
    LT_CHECK_STR(sum, "4 new constants");
    lt_free(sum);

    m[0] = me("egoCorrection", "U08", 3, 50, 1.0, 0.0);
    lt_migration_compare(&r, m, 1, ini);
    LT_CHECK_INT(r.n_loc, 1);
    LT_CHECK_INT(r.n_type, 0);
    LT_CHECK_INT(r.n_scale, 0);
    LT_CHECK_STR(r.severity, "low");
    LT_CHECK(r.can_auto_migrate && !r.requires_user_review);
    LT_CHECK_INT(r.location_changed[0].old_page, 3);
    LT_CHECK_INT(r.location_changed[0].old_offset, 50);
    LT_CHECK_INT(r.location_changed[0].new_page, 4);
    LT_CHECK_INT(r.location_changed[0].new_offset, 100);
    LT_CHECK_STR(r.location_changed[0].description, "Moved from page 3 offset 50 to page 4 offset 100");

    m[0] = me("rpmLimit", "U08", 3, 50, 1.0, 0.0);
    lt_migration_compare(&r, m, 1, ini);
    LT_CHECK_INT(r.n_type, 1);
    LT_CHECK_INT(r.n_loc, 0);
    LT_CHECK_STR(r.severity, "high");
    LT_CHECK(!r.can_auto_migrate && r.requires_user_review);
    LT_CHECK_STR(r.type_changed[0].description, "Type changed from U08 to U16");
    LT_CHECK_STR(r.type_changed[0].new_type, "U16");

    m[0] = me("scaled", "U08", 4, 110, 0.1, -40.0);
    lt_migration_compare(&r, m, 1, ini);
    LT_CHECK_INT(r.n_scale, 1);
    LT_CHECK_STR(r.severity, "medium");
    LT_CHECK(r.can_auto_migrate && r.requires_user_review);
    LT_CHECK_STR(r.scale_changed[0].description, "Scale changed from 0.1*x+-40 to 0.5*x+0");

    m[0] = me("gone", "U08", 0, 0, 1.0, 0.0);
    lt_migration_compare(&r, m, 1, ini);
    LT_CHECK_INT(r.n_missing_in_ini, 1);
    LT_CHECK_STR(r.severity, "high");
    sum = lt_migration_summary(&r);
    LT_CHECK(strstr(sum, "1 removed constants") != NULL);
    LT_CHECK(strstr(sum, "4 new constants") != NULL);
    LT_CHECK(strncmp(sum, "4 new constants, 1 removed constants", 36) == 0);
    lt_free(sum);
    lt_migration_clear(&r);
    LT_CHECK(!lt_migration_has_changes(&r));
    sum = lt_migration_summary(&r);
    LT_CHECK_STR(sum, "No changes detected");
    lt_free(sum);
    lt_ini_free(ini);
}

LT_TEST(migration_summary_counts)
{
    lt_migration_report r;
    char *s;
    lt_migration_init(&r);
    r.missing_in_tune = (char **)lt_calloc(2, sizeof(char *));
    r.missing_in_tune[0] = lt_strdup("a"); r.missing_in_tune[1] = lt_strdup("b");
    r.n_missing_in_tune = 2;
    r.missing_in_ini = (char **)lt_calloc(1, sizeof(char *));
    r.missing_in_ini[0] = lt_strdup("c");
    r.n_missing_in_ini = 1;
    s = lt_migration_summary(&r);
    LT_CHECK(strstr(s, "2 new constants") != NULL);
    LT_CHECK(strstr(s, "1 removed constants") != NULL);
    lt_free(s);
    LT_CHECK(lt_migration_has_changes(&r));
    lt_migration_clear(&r);
}

/* ---------------- .table ---------------- */
static const char *T_BAD = "<notATable/>";

LT_TEST(table_real_export)
{
    size_t n;
    char *x = lt_file_read(FIX "real_export.table", &n);
    lt_table_file t;
    LT_CHECK(x != NULL);
    if (!x) return;
    LT_CHECK_INT(lt_table_file_parse(x, n, &t), 0);
    LT_CHECK_INT(t.cols, 8);
    LT_CHECK_INT(t.rows, 2);
    LT_CHECK_NEAR(t.x[0], 6, 0); LT_CHECK_NEAR(t.x[7], 15, 0);
    LT_CHECK_NEAR(t.y[0], 206.8, 0); LT_CHECK_NEAR(t.y[1], 413.7, 0);
    LT_CHECK_NEAR(t.z[0], 4.0, 0);
    LT_CHECK_NEAR(t.z[1 * 8 + 7], 0.6, 0);
    LT_CHECK_NEAR(t.z[1 * 8 + 3], 1.19, 0);
    lt_table_file_clear(&t);
    LT_CHECK(t.x == NULL);
    lt_free(x);
}

static char *replace_all(const char *s, const char *a, const char *b)
{
    lt_buf buf;
    size_t la = strlen(a);
    const char *p = s, *q;
    lt_buf_init(&buf);
    while ((q = strstr(p, a)) != NULL) {
        lt_buf_append(&buf, p, (size_t)(q - p));
        lt_buf_puts(&buf, b);
        p = q + la;
    }
    lt_buf_puts(&buf, p);
    return lt_buf_take(&buf);
}

LT_TEST(table_errors)
{
    size_t n;
    char *x = lt_file_read(FIX "real_export.table", &n);
    char *y;
    lt_table_file t;
    LT_CHECK(x != NULL);
    if (!x) return;
    LT_CHECK(lt_table_file_parse(T_BAD, strlen(T_BAD), &t) < 0);
    LT_CHECK(strstr(lt_err_get(), "missing <tableData>") != NULL);
    y = replace_all(x, "cols=\"8\"", "cols=\"9\"");
    LT_CHECK(lt_table_file_parse(y, strlen(y), &t) < 0);
    LT_CHECK(strstr(lt_err_get(), "xAxis has 8 values, table declares 9 cols") != NULL);
    lt_free(y);
    y = replace_all(x, "rows=\"2\"", "rows=\"3\"");
    LT_CHECK(lt_table_file_parse(y, strlen(y), &t) < 0);
    LT_CHECK(strstr(lt_err_get(), "yAxis has 2 values, table declares 3 rows") != NULL);
    lt_free(y);
    y = replace_all(x, "0.6\n", "0.6 7\n");
    LT_CHECK(lt_table_file_parse(y, strlen(y), &t) < 0);
    LT_CHECK(strstr(lt_err_get(), "zValues is not a 8x2 grid") != NULL);
    lt_free(y);
    y = replace_all(x, "<table cols=\"8\" rows=\"2\">", "<table rows=\"2\">");
    LT_CHECK(lt_table_file_parse(y, strlen(y), &t) < 0);
    LT_CHECK(strstr(lt_err_get(), "missing <table cols=...>") != NULL);
    lt_free(y);
    y = replace_all(x, "<table cols=\"8\" rows=\"2\">", "<table cols=\"8\">");
    LT_CHECK(lt_table_file_parse(y, strlen(y), &t) < 0);
    LT_CHECK(strstr(lt_err_get(), "missing <table rows=...>") != NULL);
    lt_free(y);
    LT_CHECK(lt_table_file_parse("<tableData><table cols=\"1\"", 26, &t) < 0);
    lt_free(x);
}

LT_TEST(table_roundtrip)
{
    double x[3], y[2], z[6];
    char *xml;
    lt_table_file t;
    size_t i;
    x[0] = 600; x[1] = 1200; x[2] = 3000;
    y[0] = 10; y[1] = 50;
    for (i = 0; i < 6; i++) z[i] = (double)(i + 1);
    z[4] = 5.25;
    xml = lt_table_file_write("RPM <x>", "MAP & co", x, 3, y, 2, z);
    LT_CHECK(strstr(xml, "<tableData xmlns=\"http://www.EFIAnalytics.com/:table\">") != NULL);
    LT_CHECK(strstr(xml, "<xAxis cols=\"3\" name=\"RPM &lt;x&gt;\">600\n1200\n3000\n</xAxis>") != NULL);
    LT_CHECK(strstr(xml, "<yAxis name=\"MAP &amp; co\" rows=\"2\">10\n50\n</yAxis>") != NULL);
    LT_CHECK(strstr(xml, "<zValues cols=\"3\" rows=\"2\">1 2 3\n4 5.25 6\n</zValues>") != NULL);
    LT_CHECK(strstr(xml, "<table cols=\"3\" rows=\"2\">") != NULL);
    LT_CHECK(strstr(xml, "encoding=\"UTF-8\" standalone=\"no\"") != NULL);
    LT_CHECK(strstr(xml, "writeDate=\"epoch ") != NULL);
    LT_CHECK_INT(lt_table_file_parse(xml, strlen(xml), &t), 0);
    LT_CHECK_INT(t.cols, 3); LT_CHECK_INT(t.rows, 2);
    for (i = 0; i < 3; i++) LT_CHECK_NEAR(t.x[i], x[i], 0);
    for (i = 0; i < 2; i++) LT_CHECK_NEAR(t.y[i], y[i], 0);
    for (i = 0; i < 6; i++) LT_CHECK_NEAR(t.z[i], z[i], 0);
    lt_table_file_clear(&t);
    lt_free(xml);
}

/* ---------------- .tuneView ---------------- */
static void tv_equal(const lt_tune_view *a, const lt_tune_view *b)
{
    size_t i, j, k;
    LT_CHECK_INT(a->bibliography.n, b->bibliography.n);
    for (i = 0; i < a->bibliography.n && i < b->bibliography.n; i++) {
        LT_CHECK_STR(a->bibliography.a[i].k, b->bibliography.a[i].k);
        LT_CHECK_STR(a->bibliography.a[i].v, b->bibliography.a[i].v);
    }
    LT_CHECK_INT(a->version_info.n, b->version_info.n);
    LT_CHECK_INT(a->tuning_view_attrs.n, b->tuning_view_attrs.n);
    LT_CHECK((a->preview_image == NULL) == (b->preview_image == NULL));
    if (a->preview_image && b->preview_image) LT_CHECK_STR(a->preview_image, b->preview_image);
    LT_CHECK_INT(a->ncomps, b->ncomps);
    for (i = 0; i < a->ncomps && i < b->ncomps; i++) {
        LT_CHECK_STR(a->comps[i].comp_type, b->comps[i].comp_type);
        LT_CHECK_INT(a->comps[i].n, b->comps[i].n);
        for (j = 0; j < a->comps[i].n && j < b->comps[i].n; j++) {
            const lt_tune_prop *p = &a->comps[i].props[j], *q = &b->comps[i].props[j];
            LT_CHECK_STR(p->name, q->name);
            LT_CHECK((p->ts_type == NULL) == (q->ts_type == NULL));
            if (p->ts_type && q->ts_type) LT_CHECK_STR(p->ts_type, q->ts_type);
            LT_CHECK_STR(p->text, q->text);
            LT_CHECK_INT(p->attrs.n, q->attrs.n);
            for (k = 0; k < p->attrs.n && k < q->attrs.n; k++) {
                LT_CHECK_STR(p->attrs.a[k].k, q->attrs.a[k].k);
                LT_CHECK_STR(p->attrs.a[k].v, q->attrs.a[k].v);
            }
        }
    }
}

LT_TEST(tuneview_sample)
{
    size_t n;
    char *x = lt_file_read(FIX "sample.tuneView", &n);
    lt_tune_view v, v2;
    char *w, *w2;
    const lt_tune_prop *p;
    LT_CHECK(x != NULL);
    if (!x) return;
    LT_CHECK_INT(lt_tune_view_parse(x, n, &v), 0);
    LT_CHECK_STR(lt_kvmap_get(&v.bibliography, "author"), "Test");
    LT_CHECK_STR(lt_kvmap_get(&v.bibliography, "viewName"), "Demo");
    LT_CHECK_STR(lt_kvmap_get(&v.version_info, "firmwareSignature"), "MS3 Format 0545.02 ");
    LT_CHECK_STR(lt_kvmap_get(&v.version_info, "enabledCondition"), "");
    LT_CHECK(lt_kvmap_get(&v.version_info, "nope") == NULL);
    LT_CHECK_STR(v.preview_image, "QUJD");
    LT_CHECK_STR(lt_kvmap_get(&v.tuning_view_attrs, "Id"), "42");
    LT_CHECK_INT(v.ncomps, 2);
    LT_CHECK_STR(v.comps[0].comp_type, "TuneSettingsPanel");
    LT_CHECK_INT(v.comps[0].n, 5);
    p = &v.comps[0].props[2];
    LT_CHECK_STR(p->name, "Id");
    LT_CHECK_STR(p->ts_type, "String");
    LT_CHECK_STR(p->text, "");
    p = &v.comps[0].props[4];
    LT_CHECK_STR(p->name, "WarnColor");
    LT_CHECK_STR(p->ts_type, "Color");
    LT_CHECK_STR(lt_kvmap_get(&p->attrs, "alpha"), "255");
    LT_CHECK_INT(p->attrs.n, 4);
    LT_CHECK_STR(p->text, "-85552");
    LT_CHECK_STR(v.comps[1].props[0].text, "als_timing_tbl");
    w = lt_tune_view_write(&v);
    LT_CHECK(strstr(w, "<bibliography author=\"Test\" company=\"LibreTune\" viewName=\"Demo\" writeDate=\"2026-04-29\"/>") != NULL);
    LT_CHECK(strstr(w, "<WarnColor type=\"Color\" alpha=\"255\" blue=\"0\" green=\"242\" red=\"242\">-85552</WarnColor>") != NULL);
    LT_CHECK(strstr(w, "<Id type=\"String\"/>") != NULL);
    LT_CHECK(strstr(w, "<previewImage>QUJD</previewImage>") != NULL);
    LT_CHECK(strstr(w, "<tuningView Id=\"42\" ShieldedDuringEdit=\"false\">") != NULL);
    LT_CHECK_INT(lt_tune_view_parse(w, strlen(w), &v2), 0);
    tv_equal(&v, &v2);
    w2 = lt_tune_view_write(&v2);
    LT_CHECK_STR(w, w2);
    lt_free(w); lt_free(w2); lt_free(x);
    lt_tune_view_clear(&v); lt_tune_view_clear(&v2);
}

LT_TEST(tuneview_prefix_and_invalid)
{
    static const char *pfx =
        "<tv:tuneView xmlns:tv=\"http://www.EFIAnalytics.com/:tuneView\"><tv:bibliography author=\"x\"/>"
        "<tv:tuningView><tv:tuneComp type=\"X\"><tv:Foo type=\"String\">bar</tv:Foo></tv:tuneComp>"
        "</tv:tuningView></tv:tuneView>";
    lt_tune_view v;
    LT_CHECK_INT(lt_tune_view_parse(pfx, strlen(pfx), &v), 0);
    LT_CHECK_INT(v.ncomps, 1);
    LT_CHECK_INT(v.comps[0].n, 1);
    LT_CHECK_STR(v.comps[0].props[0].text, "bar");
    LT_CHECK_STR(v.comps[0].props[0].name, "Foo");
    lt_tune_view_clear(&v);
    LT_CHECK(lt_tune_view_parse("<?xml version=\"1.0\"?><other/>", 29, &v) < 0);
    LT_CHECK(strstr(lt_err_get(), "Invalid tuneView document") != NULL);
    LT_CHECK(lt_tune_view_parse("<tuneView><a>", 13, &v) < 0);
}

LT_TEST(tuneview_escapes)
{
    lt_tune_view v;
    lt_tune_view v2;
    char *w;
    lt_tune_comp *c;
    lt_tune_prop *p;
    memset(&v, 0, sizeof v);
    lt_kvmap_set(&v.bibliography, "zeta", "z");
    lt_kvmap_set(&v.bibliography, "alpha", "multi\nline \"q\" <&>\r");
    lt_kvmap_set(&v.bibliography, "alpha", "multi\nline \"q\" <&>\r");
    LT_CHECK_INT(v.bibliography.n, 2);
    LT_CHECK_STR(v.bibliography.a[0].k, "alpha");
    v.comps = (lt_tune_comp *)lt_calloc(1, sizeof(lt_tune_comp));
    v.ncomps = 1;
    c = &v.comps[0];
    c->comp_type = lt_strdup("T");
    c->props = (lt_tune_prop *)lt_calloc(1, sizeof(lt_tune_prop));
    c->n = 1;
    p = &c->props[0];
    p->name = lt_strdup("Prop");
    p->ts_type = lt_strdup("String");
    p->text = lt_strdup("a<b>&\n\"c\"\r");
    lt_kvmap_set(&p->attrs, "k", "v w");
    w = lt_tune_view_write(&v);
    LT_CHECK(strstr(w, "alpha=\"multi&#10;line &quot;q&quot; &lt;&amp;&gt;&#13;\"") != NULL);
    LT_CHECK(strstr(w, ">a&lt;b&gt;&amp;\n\"c\"&#13;</Prop>") != NULL);
    LT_CHECK_INT(lt_tune_view_parse(w, strlen(w), &v2), 0);
    tv_equal(&v, &v2);
    LT_CHECK_STR(lt_kvmap_get(&v2.bibliography, "alpha"), "multi\nline \"q\" <&>\r");
    lt_free(w);
    lt_tune_view_clear(&v); lt_tune_view_clear(&v2);
}

LT_TEST(tuneview_nonselfclosing_bibliography)
{
    static const char *x =
        "<tuneView><bibliography a=\"1\"></bibliography><tuningView><tuneComp type=\"A\"/>"
        "<tuneComp type=\"B\"><P>  keep  </P><Q/></tuneComp></tuningView></tuneView>";
    lt_tune_view v;
    LT_CHECK_INT(lt_tune_view_parse(x, strlen(x), &v), 0);
    LT_CHECK_STR(lt_kvmap_get(&v.bibliography, "a"), "1");
    LT_CHECK_INT(v.ncomps, 2);
    LT_CHECK_INT(v.comps[0].n, 0);
    LT_CHECK_STR(v.comps[1].props[0].text, "  keep  ");
    LT_CHECK_STR(v.comps[1].props[1].text, "");
    LT_CHECK(v.comps[1].props[1].ts_type == NULL);
    lt_tune_view_clear(&v);
}

int main(void)
{
    LT_RUN(number_format);
    LT_RUN(number_roundtrip);
    LT_RUN(value_parse);
    LT_RUN(value_equal_copy);
    LT_RUN(determinism);
    LT_RUN(new_and_get);
    LT_RUN(sorted_names);
    LT_RUN(self_closing);
    LT_RUN(pagedata);
    LT_RUN(quoted_bits);
    LT_RUN(sample_file);
    LT_RUN(minimal_file);
    LT_RUN(invalid_files);
    LT_RUN(page_walk);
    LT_RUN(legacy_fallback);
    LT_RUN(signature_fallbacks);
    LT_RUN(escaping);
    LT_RUN(number_output);
    LT_RUN(sanitize);
    LT_RUN(pages_api);
    LT_RUN(empty_pagedata_and_latin1);
    LT_RUN(description_comment_and_meta);
    LT_RUN(manifest_edge_cases);
    LT_RUN(clone_and_save_load);
    LT_RUN(pc_variable_file);
    LT_RUN(diff_basic);
    LT_RUN(diff_union_and_kinds);
    LT_RUN(merge);
    LT_RUN(migration);
    LT_RUN(migration_summary_counts);
    LT_RUN(table_real_export);
    LT_RUN(table_errors);
    LT_RUN(table_roundtrip);
    LT_RUN(tuneview_sample);
    LT_RUN(tuneview_prefix_and_invalid);
    LT_RUN(tuneview_escapes);
    LT_RUN(tuneview_nonselfclosing_bibliography);
    LT_TEST_MAIN_END
}
