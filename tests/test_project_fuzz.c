/* test_project_fuzz.c - mutation/truncation fuzzing of project-module parsers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "plat/plat.h"
#include "json/json.h"
#include "project/project.h"
#include "lt_test.h"

static unsigned long rng = 12345UL;
static unsigned rnd(void) { rng = rng * 1103515245UL + 12345UL; return (unsigned)((rng >> 16) & 0x7fff); }

static const char *SEEDS[] = {
    "{\"a\":[1,2.5,-3e2,true,null,\"x\\u00e9\\ud83d\\ude00\"],\"b\":{\"c\":\"d\"}}",
    "{\"name\":\"n\",\"description\":\"d\",\"created\":\"2024-01-01T00:00:00Z\",\"created_by\":\"u\","
    "\"actions\":[{\"type\":\"TableEdit\",\"table\":\"veTable\",\"x\":1,\"y\":2,\"value\":3.5,\"old_value\":1.0},"
    "{\"Pause\":{\"ms\":100}},{\"type\":\"Pause\",\"ms\":5}]}"
};

static void fuzz_one(const char *s, size_t n)
{
    char err[256];
    lt_json *j = lt_json_parse(s, n, err, sizeof err);
    char *z, *t;
    lt_action_set as;
    lt_props pr;
    char *sig = NULL, *nm = NULL;
    char ts[21];
    if (j) { z = lt_json_to_string(j, 1); lt_free(z); lt_json_free(j); }
    t = (char *)lt_malloc(n + 1);
    memcpy(t, s, n); t[n] = 0;
    lt_action_set_init(&as);
    if (lt_action_set_deserialize(t, &as, err, sizeof err) == 0) {
        z = lt_action_set_serialize(&as, err, sizeof err);
        lt_free(z);
    }
    lt_action_set_free(&as);
    lt_props_init(&pr);
    lt_props_parse(&pr, t);
    lt_props_free(&pr);
    if (lt_ini_extract_info(t, &sig, &nm) == 0) { lt_free(sig); lt_free(nm); }
    (void)lt_restore_parse_timestamp(t, ts);
    lt_free(t);
}

static void fuzz_text(const char *seed)
{
    size_t n = strlen(seed), i, k;
    char *m = (char *)lt_malloc(n + 8);
    for (i = 0; i <= n; i++) fuzz_one(seed, i);
    for (k = 0; k < 1500; k++) {
        int c, nm = 1 + (int)(rnd() % 4);
        memcpy(m, seed, n);
        for (c = 0; c < nm; c++) m[rnd() % n] = (char)(rnd() & 0xff);
        fuzz_one(m, n);
    }
    lt_free(m);
}

LT_TEST(fuzz_seeds)
{
    size_t i;
    FILE *f;
    static char buf[65536];
    size_t got;
    for (i = 0; i < sizeof SEEDS / sizeof SEEDS[0]; i++) fuzz_text(SEEDS[i]);
    f = fopen("tests/fixtures/project/mini.ini", "rb");
    if (f) { got = fread(buf, 1, 3000, f); fclose(f); buf[got] = 0; fuzz_text(buf); }
    LT_CHECK(1);
}

LT_TEST(fuzz_random_bytes)
{
    char b[200];
    int k, i;
    for (k = 0; k < 3000; k++) {
        size_t n = rnd() % 200;
        for (i = 0; i < (int)n; i++) b[i] = (char)(rnd() & 0xff);
        fuzz_one(b, n);
    }
    LT_CHECK(1);
}

LT_TEST(json_deep_nesting)
{
    size_t n = 200000, i;
    char *s = (char *)lt_malloc(n + 1);
    char err[64];
    lt_json *j;
    for (i = 0; i < n; i++) s[i] = '[';
    s[n] = 0;
    j = lt_json_parse(s, n, err, sizeof err);   /* must not overflow the stack */
    if (j) lt_json_free(j);
    lt_free(s);
    LT_CHECK(1);
}

int main(void)
{
    LT_RUN(fuzz_seeds);
    LT_RUN(fuzz_random_bytes);
    LT_RUN(json_deep_nesting);
    LT_TEST_MAIN_END
}
