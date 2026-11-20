/* test_tune_fuzz.c - truncation/mutation robustness of the tune parsers. */
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "tune/tune.h"
#include "tune/tune_csv.h"
#include "lt_test.h"

#define FIX "tests/fixtures/tune/"

static unsigned long rng_s = 987654UL;
static unsigned long rnd(unsigned long n)
{
    rng_s = (rng_s * 1103515245UL + 12345UL) & 0x7fffffffUL;
    return (rng_s >> 8) % n;
}

static void one_msq(const char *d, size_t n)
{
    lt_tune *t = lt_tune_parse_msq(d, n);
    if (t) {
        lt_buf b;
        lt_tune_sanitize(t);
        lt_buf_init(&b);
        lt_tune_to_msq(t, &b);
        lt_buf_free(&b);
        lt_tune_free(t);
    }
}

static void one_table(const char *d, size_t n)
{
    lt_table_file t;
    memset(&t, 0, sizeof t);
    if (lt_table_file_parse(d, n, &t) == 0) lt_table_file_clear(&t);
}

static void one_view(const char *d, size_t n)
{
    lt_tune_view v;
    memset(&v, 0, sizeof v);
    if (lt_tune_view_parse(d, n, &v) == 0) {
        char *s = lt_tune_view_write(&v);
        lt_free(s);
        lt_tune_view_clear(&v);
    }
}

static void run_file(const char *name, void (*fn)(const char *, size_t))
{
    size_t n = 0, i, k, m;
    char *src = lt_file_read(name, &n);
    char *buf;
    LT_CHECK(src != NULL);
    if (!src) return;
    buf = (char *)lt_malloc(n + 1);
    for (i = 0; i <= n; i += (n > 400 ? n / 300 : 1)) {
        memcpy(buf, src, i);
        buf[i] = 0;
        fn(buf, i);
    }
    for (i = 0; i < 400; i++) {
        memcpy(buf, src, n);
        m = 1 + rnd(8);
        for (k = 0; k < m; k++) buf[rnd((unsigned long)n)] = (char)rnd(256);
        buf[n] = 0;
        fn(buf, n);
    }
    for (i = 0; i < 100; i++) {
        size_t a = rnd((unsigned long)n), l = rnd(64);
        if (a + l > n) l = n - a;
        memcpy(buf, src, n);
        memmove(buf + a, buf + a + l, n - a - l);
        buf[n - l] = 0;
        fn(buf, n - l);
    }
    lt_free(buf);
    lt_free(src);
}

LT_TEST(fuzz_msq) { run_file(FIX "sample.msq", one_msq); }
LT_TEST(fuzz_table) { run_file(FIX "real_export.table", one_table); }
LT_TEST(fuzz_tuneview) { run_file(FIX "sample.tuneView", one_view); }

/* pages of the small Speeduino INI for the apply paths */
static lt_ini *g_ini;
static lt_u8 g_pages[4][512];
static const lt_u8 *pg_read(void *ud, int page, size_t *len)
{
    LT_UNUSED(ud);
    if (page < 0 || page >= 4) return NULL;
    *len = sizeof g_pages[0];
    return g_pages[page];
}
static int pg_write(void *ud, int page, size_t off, const lt_u8 *d, size_t n)
{
    LT_UNUSED(ud);
    if (page < 0 || page >= 4 || off > sizeof g_pages[0] || n > sizeof g_pages[0] - off) return -1;
    memcpy(g_pages[page] + off, d, n);
    return 0;
}
static lt_pagesrc pages(void)
{
    lt_pagesrc ps;
    ps.ud = NULL; ps.page = pg_read; ps.write = pg_write;
    return ps;
}

/* .msqpart: parse, apply with undo, apply the undo, extract */
static void one_part(const char *d, size_t n)
{
    lt_tune *part = lt_tune_parse_msq(d, n);
    if (part) {
        lt_pagesrc ps = pages();
        lt_tune *pcv = lt_tune_new("x"), *undo = NULL, *ex;
        char **names = NULL;
        size_t k, i, ap = 0, sk = 0;
        lt_tune_part_apply(g_ini, &ps, pcv, part, &undo, &ap, &sk);
        if (undo) {
            lt_tune_part_apply(g_ini, &ps, pcv, undo, NULL, NULL, NULL);
            lt_tune_free(undo);
        }
        k = lt_tune_part_names(part, &names);
        ex = lt_tune_part_extract(part, (const char *const *)names, k);
        if (ex) lt_tune_free(ex);
        for (i = 0; i < k; i++) lt_free(names[i]);
        lt_free(names);
        lt_tune_free(pcv);
        lt_tune_free(part);
    }
}

/* tune CSV import */
static void one_csv(const char *d, size_t n)
{
    lt_pagesrc ps = pages();
    lt_csv_import r;
    memset(&r, 0, sizeof r);
    lt_tune_csv_import(g_ini, &ps, d, n, &r);
    lt_csv_import_free(&r);
}

LT_TEST(fuzz_msqpart)
{
    lt_tune *t, *part;
    lt_buf b;
    static const char *const names[5] = { "veTable", "reqFuel", "nCylinders", "tsPcVar", "nope" };
    lt_tune_value v;
    g_ini = lt_ini_load_file("tests/fixtures/tune/mini_speeduino.ini", NULL);
    LT_CHECK(g_ini != NULL);
    if (!g_ini) return;
    t = lt_tune_from_defaults(g_ini);
    LT_CHECK(t != NULL);
    if (!t) return;
    lt_tune_value_init(&v);
    v.scalar = 2.0;
    lt_tune_set_pc_variable(t, "tsPcVar", &v);
    part = lt_tune_part_extract(t, names, 5);
    lt_buf_init(&b);
    lt_tune_to_msq(part, &b);
    lt_file_write("build/tune_fz.msqpart", b.data, b.len);
    run_file("build/tune_fz.msqpart", one_part);
    lt_buf_free(&b);
    lt_tune_free(part);
    lt_tune_free(t);
}

LT_TEST(fuzz_tune_csv)
{
    lt_pagesrc ps;
    char *txt;
    size_t cnt = 0;
    if (!g_ini) return;
    ps = pages();
    txt = lt_tune_csv_export(g_ini, &ps, &cnt);
    LT_CHECK(txt != NULL && cnt > 3);
    if (!txt) return;
    lt_file_write("build/tune_fz.csv", txt, strlen(txt));
    run_file("build/tune_fz.csv", one_csv);
    lt_free(txt);
    lt_ini_free(g_ini);
    g_ini = NULL;
}

LT_TEST(fuzz_values)
{
    int i, k;
    char s[64];
    lt_tune_value v;
    for (i = 0; i < 3000; i++) {
        int l = (int)rnd(40);
        for (k = 0; k < l; k++) s[k] = rnd(4) ? " \"0123456789.,-+eE{}ab\\"[rnd(23)] : (char)rnd(256);
        s[l] = 0;
        for (k = 0; k < l; k++) if (s[k] == 0) s[k] = 'x';
        lt_tune_value_parse(&v, s);
        lt_tune_value_clear(&v);
    }
}

int main(void)
{
    LT_RUN(fuzz_msq);
    LT_RUN(fuzz_table);
    LT_RUN(fuzz_tuneview);
    LT_RUN(fuzz_msqpart);
    LT_RUN(fuzz_tune_csv);
    LT_RUN(fuzz_values);
    LT_TEST_MAIN_END
}
