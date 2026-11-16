/* test_tune_fuzz.c - truncation/mutation robustness of the tune parsers. */
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "tune/tune.h"
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
    LT_RUN(fuzz_values);
    LT_TEST_MAIN_END
}
