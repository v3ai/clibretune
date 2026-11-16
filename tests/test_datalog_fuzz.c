/* test_datalog_fuzz.c - mutation/truncation robustness of the datalog parsers. */
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "lt_codes.h"
#include "datalog/datalog.h"
#include "plat/plat.h"
#include "lt_test.h"

#define TMP "build/dl_fz_"

static unsigned long rng_s = 12345UL;
static unsigned long rnd(unsigned long n)
{
    rng_s = (rng_s * 1103515245UL + 12345UL) & 0x7fffffffUL;
    return (rng_s >> 8) % n;
}

static void mutate(lt_u8 *b, size_t n, int count)
{
    int i;
    for (i = 0; i < count && n > 0; i++) {
        b[rnd((unsigned long)n)] = (lt_u8)rnd(256);
    }
}

static void make_mlg(lt_buf *out)
{
    static const char *nm[3] = { "rpm", "map", "clt" };
    static const char *un[3] = { "rpm", "kPa", "C" };
    lt_log l;
    int i;
    double v[3];
    lt_buf_init(out);
    lt_log_init(&l);
    lt_log_set_channels(&l, nm, un, 3);
    for (i = 0; i < 40; i++) {
        v[0] = 800 + i; v[1] = 30 + i; v[2] = 90;
        lt_log_append(&l, i * 0.05, v);
    }
    lt_log_add_marker(&l, 0.5, "hello");
    lt_mlg_write(TMP "a.mlg", &l, "info");
    {
        size_t n = 0;
        char *p = lt_file_read(TMP "a.mlg", &n);
        lt_buf_append(out, p, n);
        lt_free(p);
    }
    lt_log_free(&l);
}

static void mlg_fuzz(void)
{
    lt_buf b;
    size_t i;
    int it;
    make_mlg(&b);
    for (i = 0; i <= b.len; i++) {
        lt_log l;
        lt_mlg_read_buf(b.data, i, &l);
        lt_log_free(&l);
    }
    for (it = 0; it < 3000; it++) {
        lt_u8 *c = (lt_u8 *)lt_malloc(b.len);
        size_t n = b.len;
        lt_log l;
        memcpy(c, b.data, b.len);
        /* bias toward header bytes */
        if (it & 1) {
            size_t k;
            for (k = 0; k < 3; k++) c[rnd(200)] = (lt_u8)rnd(256);
        }
        mutate(c, n, 1 + (int)rnd(6));
        if (it % 5 == 0) n = (size_t)rnd((unsigned long)n + 1);
        lt_mlg_read_buf(c, n, &l);
        lt_log_free(&l);
        lt_free(c);
    }
    lt_buf_free(&b);
    LT_CHECK(1);
}

static void csv_fuzz(void)
{
    static const char *seed =
        "\xEF\xBB\xBFTime (ms),rpm,\"map\",clt,\n0,900,50,80,\n10,1000,51,81,\n\n20,x,1,2\n"
        "30,1100,52,82\n";
    size_t sl = strlen(seed);
    int it;
    for (it = 0; it < 3000; it++) {
        char *c = (char *)lt_malloc(sl + 1);
        size_t n = sl;
        lt_log l;
        memcpy(c, seed, sl);
        mutate((lt_u8 *)c, n, 1 + (int)rnd(5));
        if (it % 3 == 0) n = (size_t)rnd((unsigned long)n + 1);
        lt_csv_parse(c, n, &l);
        lt_log_free(&l);
        lt_free(c);
    }
    LT_CHECK(1);
}

static void file_fuzz(void)
{
    static const char *dy = "RPM,HP,Torque,AFR\n3000,100,175,13\n4000,150,197,12.5\n5000,180,189,12\n";
    static const char *mj =
        "[\n  {\n    \"name\": \"x\",\n    \"units\": \"u\",\n    \"expression\": \"a + 1\",\n"
        "    \"enabled\": true\n  }\n]";
    int pass;
    int it;
    for (pass = 0; pass < 2; pass++) {
        const char *seed = pass ? mj : dy;
        size_t sl = strlen(seed);
        for (it = 0; it < 1500; it++) {
            char *c = (char *)lt_malloc(sl + 1);
            size_t n = sl;
            memcpy(c, seed, sl);
            mutate((lt_u8 *)c, n, 1 + (int)rnd(4));
            if (it % 3 == 0) n = (size_t)rnd((unsigned long)n + 1);
            if (pass == 0) {
                lt_dyno_run r;
                char **h = NULL;
                size_t hn = 0;
                memset(&r, 0, sizeof r);
                lt_file_write(TMP "d.csv", c, n);
                if (lt_dyno_from_csv(TMP "d.csv", "r", &r) == 0) {
                    lt_dyno_run_free(&r);
                }
                if (lt_dyno_csv_headers(TMP "d.csv", &h, &hn) == 0) {
                    lt_strv_free(h, hn);
                }
            } else {
                lt_math_set s;
                memset(&s, 0, sizeof s);
                lt_file_write(TMP "m.json", c, n);
                if (lt_math_load(TMP "m.json", &s) == 0) {
                    lt_math_set_free(&s);
                }
            }
            lt_free(c);
        }
    }
    LT_CHECK(1);
}

int main(void)
{
    LT_RUN(mlg_fuzz);
    LT_RUN(csv_fuzz);
    LT_RUN(file_fuzz);
    LT_TEST_MAIN_END
}
