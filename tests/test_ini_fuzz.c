/* test_ini_fuzz.c - robustness: truncated / mutated INI text and expressions must not crash. */
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "ini/ini.h"
#include "ini/expr.h"
#include "lt_test.h"

static unsigned long rs = 12345UL;
static unsigned rnd(void)
{
    rs = (rs * 1103515245UL + 12345UL) & 0x7fffffffUL;
    return (unsigned)(rs >> 8);
}

static const lt_u8 *pg(void *ud, int page, size_t *len)
{
    static lt_u8 buf[4096];
    (void)ud; (void)page;
    *len = sizeof buf;
    return buf;
}

static void exercise(lt_ini *ini)
{
    size_t i;
    lt_pagesrc src;
    lt_u8 frame[300];
    lt_ini_rt *rt;
    double d;
    src.ud = NULL; src.page = pg; src.write = NULL;
    for (i = 0; i < sizeof frame; i++) frame[i] = (lt_u8)rnd();
    for (i = 0; i < ini->n_constants && i < 400; i++) {
        lt_ini_const_get(ini, ini->constants[i], &src, 0, &d);
        lt_ini_const_get_raw(ini, ini->constants[i], &src, 3, &d);
    }
    rt = lt_ini_rt_new(ini);
    if (rt) {
        lt_ini_rt_update(rt, frame, sizeof frame);
        lt_ini_rt_update(rt, frame, 7);
        lt_ini_rt_free(rt);
    }
    lt_ini_resolve_dynamic(ini, NULL);
}

static void fuzz_file(const char *path, int iters)
{
    size_t n = 0, m, k;
    char *orig = lt_file_read(path, &n);
    char *buf;
    int it;
    LT_CHECK(orig != NULL && n > 100);
    if (!orig) return;
    buf = (char *)malloc(n + 64);
    for (it = 0; it < iters; it++) {
        lt_ini *ini;
        memcpy(buf, orig, n);
        m = n;
        switch (it % 4) {
        case 0: m = rnd() % n; break;                       /* truncation */
        case 1: for (k = 0; k < 40; k++) buf[rnd() % n] = (char)rnd(); break;
        case 2: for (k = 0; k < 40; k++) buf[rnd() % n] = "{}()[],;=\"#$\\ \n"[rnd() % 15]; break;
        default: {                                          /* chop a middle chunk */
            size_t a = rnd() % n, b = a + rnd() % 2000;
            if (b > n) b = n;
            memmove(buf + a, buf + b, n - b);
            m = n - (b - a);
        } break;
        }
        ini = lt_ini_load_string(buf, m, NULL, NULL);
        if (ini) {
            exercise(ini);
            lt_ini_free(ini);
        }
    }
    free(buf);
    free(orig);
}

LT_TEST(fuzz_demo) { fuzz_file("tests/fixtures/ini/demo.ini", 120); LT_CHECK(1); }
LT_TEST(fuzz_ms3) { fuzz_file("tests/fixtures/ini/ms3.ini", 40); LT_CHECK(1); }

LT_TEST(fuzz_expr)
{
    static const char alpha[] = "()+-*/%&|^~!<>=?:,.\"' 0123456789abcxyzeE_$[]{}";
    int it;
    for (it = 0; it < 20000; it++) {
        char e[80];
        size_t len = 1 + rnd() % 70, i;
        lt_value v;
        lt_expr *x;
        for (i = 0; i < len; i++) e[i] = alpha[rnd() % (sizeof alpha - 1)];
        e[len] = 0;
        x = lt_expr_compile(e, NULL, 0);
        if (x) {
            lt_expr_eval(x, NULL, &v);
            lt_value_clear(&v);
            lt_expr_free(x);
        }
    }
    LT_CHECK(1);
}

LT_TEST(deep_nesting)
{
    char *e = (char *)malloc(200001);
    lt_value v;
    lt_expr *x;
    size_t i;
    for (i = 0; i < 100000; i++) { e[i] = '('; e[100000 + i] = ')'; }
    e[200000] = 0;
    x = lt_expr_compile(e, NULL, 0);
    if (x) { lt_expr_eval(x, NULL, &v); lt_value_clear(&v); lt_expr_free(x); }
    for (i = 0; i < 100000; i++) e[i] = '-';
    e[100000] = '1'; e[100001] = 0;
    x = lt_expr_compile(e, NULL, 0);
    if (x) { lt_expr_eval(x, NULL, &v); lt_value_clear(&v); lt_expr_free(x); }
    free(e);
    LT_CHECK(1);
}

LT_TEST(huge_shapes)
{
    const char *t = "[Constants]\npage=1\nw = array, S16, 0, [100000x100000], \"u\", 1,0,0,1,0\n"
                    "v = array, U08, 0, [4000000000], \"u\", 1,0,0,1,0\n";
    lt_ini *ini = lt_ini_load_string(t, strlen(t), NULL, NULL);
    LT_CHECK(ini != NULL);
    if (ini) {
        const lt_ini_const *w = lt_ini_find_const(ini, "w");
        const lt_ini_const *v = lt_ini_find_const(ini, "v");
        LT_CHECK(w && lt_shape_count(&w->shape) <= 65535UL * 65535UL);
        LT_CHECK(v && lt_shape_count(&v->shape) <= 65535UL);
        lt_ini_free(ini);
    }
}

int main(void)
{
    LT_RUN(fuzz_demo);
    LT_RUN(fuzz_ms3);
    LT_RUN(fuzz_expr);
    LT_RUN(deep_nesting);
    LT_RUN(huge_shapes);
    LT_TEST_MAIN_END
}
