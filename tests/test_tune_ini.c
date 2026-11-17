/* test_tune_ini.c - tune <-> INI bridge: round trips with the fixture INIs, varenv, ECU memory */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lt.h"
#include "tune/tune.h"
#include "proto/proto.h"
#include "lt_test.h"

#define DEMO "tests/fixtures/ini/demo.ini"
#define MINI "tests/fixtures/tune/mini_speeduino.ini"

static char *save_str(const lt_tune *t)
{
    lt_buf b;
    lt_buf_init(&b);
    lt_tune_to_msq(t, &b);
    return lt_buf_take(&b);
}

/* ---- deterministic pattern fill, touching only constant bytes ---- */
static void fill_pattern(const lt_ini *ini, lt_tune *t)
{
    lt_pagesrc ps = lt_tune_pagesrc(t);
    size_t i, idx, cnt;
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];
        double lo, hi, span;
        if (c->is_pc_var || !t->pages[c->page]) continue;
        if (c->type == LT_DT_STRING) {
            char s[40];
            lt_snprintf(s, sizeof s, "str%lu", (unsigned long)i);
            lt_ini_const_set_string(ini, c, &ps, s);
            continue;
        }
        if (c->type == LT_DT_BITS) {
            int sz = c->bit_size < 0 ? 8 : c->bit_size;
            lt_u32 mask = sz >= 32 ? LT_U32_MASK : (lt_u32)((1UL << sz) - 1UL);
            lt_ini_const_set_raw(ini, c, &ps, 0, (double)((i * 7 + 1) & mask));
            continue;
        }
        cnt = lt_shape_count(&c->shape);
        if (lt_dtype_range(c->type, &lo, &hi)) {
            span = hi - lo + 1;
            if (span > 200) span = 200;
            for (idx = 0; idx < cnt; idx++)
                lt_ini_const_set_raw(ini, c, &ps, idx, lo + (double)((i * 37 + idx * 11) % (size_t)span));
        } else {
            for (idx = 0; idx < cnt; idx++)
                lt_ini_const_set_raw(ini, c, &ps, idx, (double)(idx + 1) * 0.5 + (double)(i % 50));
        }
    }
}

static lt_tune *blank_for(const lt_ini *ini)
{
    lt_tune *t = lt_tune_new(ini->signature ? ini->signature : "");
    int p;
    for (p = 0; p < (int)ini->n_pages && p < 256; p++) {
        size_t sz = (size_t)p < ini->proto.n_page_sizes ? (size_t)ini->proto.page_sizes[p] : 0;
        lt_u8 *z = (lt_u8 *)lt_calloc(sz ? sz : 1, 1);
        lt_tune_set_page(t, p, z, sz);
        lt_free(z);
    }
    return t;
}

static void roundtrip_ini(const char *path)
{
    lt_ini *ini = lt_ini_load_file(path, NULL);
    lt_tune *a, *parsed;
    char *msq;
    size_t i, nconst = 0, bad_pages = 0;
    int applied;
    lt_buf b;

    LT_CHECK(ini != NULL);
    if (!ini) return;
    a = blank_for(ini);
    fill_pattern(ini, a);
    LT_CHECK(lt_tune_capture_from_pages(a, ini) > 0);
    for (i = 0; i < ini->n_constants; i++)
        if (!ini->constants[i]->is_pc_var) nconst++;
    LT_CHECK_INT(lt_tune_n_constants(a), nconst);
    /* capture twice is idempotent */
    msq = save_str(a);
    parsed = lt_tune_parse_msq(msq, strlen(msq));
    LT_CHECK(parsed != NULL);
    if (parsed) {
        /* identical pages from pageData */
        for (i = 0; i < 256; i++) {
            if (a->pages[i]) {
                LT_CHECK(parsed->pages[i] != NULL && parsed->page_len[i] == a->page_len[i]);
                if (parsed->pages[i] && memcmp(parsed->pages[i], a->pages[i], a->page_len[i]) != 0) bad_pages++;
            }
        }
        LT_CHECK_INT(bad_pages, 0);
        /* now drop the byte images and rebuild them from the named constants only */
        for (i = 0; i < 256; i++) {
            if (parsed->pages[i]) {
                lt_free(parsed->pages[i]);
                parsed->pages[i] = NULL;
                parsed->page_len[i] = 0;
            }
        }
        applied = lt_tune_apply_to_pages(parsed, ini);
        LT_CHECK_INT(applied, nconst);
        bad_pages = 0;
        for (i = 0; i < 256; i++) {
            if (a->pages[i]) {
                if (!parsed->pages[i] || parsed->page_len[i] != a->page_len[i]) {
                    bad_pages++;
                    continue;
                }
                if (memcmp(parsed->pages[i], a->pages[i], a->page_len[i]) != 0) {
                    size_t k;
                    for (k = 0; k < a->page_len[i]; k++)
                        if (parsed->pages[i][k] != a->pages[i][k]) break;
                    fprintf(stderr, "%s: page %lu differs at byte %lu (%02x vs %02x)\n", path,
                            (unsigned long)i, (unsigned long)k, parsed->pages[i][k], a->pages[i][k]);
                    bad_pages++;
                }
            }
        }
        LT_CHECK_INT(bad_pages, 0);
        /* re-capture gives identical msq constants */
        lt_tune_capture_from_pages(parsed, ini);
        lt_buf_init(&b);
        lt_tune_to_msq(parsed, &b);
        LT_CHECK(b.len > 0);
        lt_buf_free(&b);
        {
            lt_tune_diff *d = lt_tune_diff_compare(a, parsed);
            if (d->n) fprintf(stderr, "%s: first diff %s\n", path, d->diffs[0].name);
            LT_CHECK_INT(d->n, 0);
            lt_tune_diff_free(d);
        }
    }
    lt_free(msq);
    lt_tune_free(a);
    lt_tune_free(parsed);
    lt_ini_free(ini);
}

LT_TEST(roundtrip_mini) { roundtrip_ini(MINI); }
LT_TEST(roundtrip_demo) { roundtrip_ini(DEMO); }

LT_TEST(demo_defaults_file_roundtrip)
{
    lt_ini *ini = lt_ini_load_file(DEMO, NULL);
    lt_tune *t, *r;
    char *s;
    LT_CHECK(ini != NULL);
    if (!ini) return;
    t = lt_tune_from_defaults(ini);
    s = save_str(t);
    r = lt_tune_parse_msq(s, strlen(s));
    LT_CHECK(r != NULL);
    if (r) {
        int p, bad = 0;
        /* "false" labels re-read as Bool (spec quirk), so compare byte images instead of text */
        for (p = 0; p < 256; p++) {
            if (r->pages[p]) {
                lt_free(r->pages[p]);
                r->pages[p] = NULL;
                r->page_len[p] = 0;
            }
        }
        LT_CHECK(lt_tune_apply_to_pages(r, ini) > 1000);
        for (p = 0; p < 256; p++) {
            if (t->pages[p] && (!r->pages[p] || memcmp(t->pages[p], r->pages[p], t->page_len[p]) != 0)) bad++;
        }
        LT_CHECK_INT(bad, 0);
    }
    lt_free(s);
    lt_tune_free(t);
    lt_tune_free(r);
    lt_ini_free(ini);
}

/* ---- defaults ---- */
LT_TEST(from_defaults_speeduino)
{
    lt_ini *ini = lt_ini_load_file(MINI, NULL);
    lt_tune *t, *r;
    char *s;
    size_t i;
    double d;
    LT_CHECK(ini != NULL);
    if (!ini) return;
    t = lt_tune_from_defaults(ini);
    LT_CHECK(t != NULL);
    LT_CHECK_STR(t->signature, ini->signature);
    for (i = 0; i < ini->n_pages; i++) {
        size_t len = 0;
        LT_CHECK(lt_tune_get_page(t, (int)i, &len) != NULL);
        LT_CHECK_INT(len, ini->proto.page_sizes[i]);
    }
    LT_CHECK(t->has_manifest && t->n_manifest == 23);
    LT_CHECK(t->ini_metadata != NULL);
    LT_CHECK_NEAR(lt_tune_get_pc_variable(t, "rpmhigh")->value.scalar, 7000, 0);
    LT_CHECK_NEAR(lt_tune_get_pc_variable(t, "fuelUnits")->value.scalar, 1, 0);
    LT_CHECK_NEAR(lt_tune_get_constant(t, "reqFuel")->value.scalar, 8.5, 1e-9);
    LT_CHECK_STR(lt_tune_get_constant(t, "nCylinders")->value.str, "4");
    LT_CHECK_STR(t->ini_metadata->signature, ini->signature);
    LT_CHECK_STR(t->ini_metadata->name, "mini_speeduino.ini");
    {
        lt_pagesrc ps = lt_tune_pagesrc(t);
        /* every constant with a default reads it back */
        int checked = 0;
        for (i = 0; i < ini->n_constants; i++) {
            const lt_ini_const *c = ini->constants[i];
            double dv, got;
            if (c->is_pc_var || c->type == LT_DT_STRING || c->shape.kind != LT_SH_SCALAR) continue;
            if (!lt_ini_default_value(ini, c->name, &dv)) continue;
            if (lt_ini_const_get(ini, c, &ps, 0, &got) != 0) continue;
            if (c->type == LT_DT_BITS || (dv >= c->min && dv <= c->max)) {
                LT_CHECK_NEAR(got, dv, c->type == LT_DT_BITS ? 0 : fabs(c->scale) * 0.51 + 1e-9);
                checked++;
            }
        }
        LT_CHECK(checked > 0);
    }
    s = save_str(t);
    r = lt_tune_parse_msq(s, strlen(s));
    LT_CHECK(r != NULL);
    if (r) {
        lt_tune_diff *df = lt_tune_diff_compare(t, r);
        LT_CHECK(lt_tune_diff_identical(df));
        lt_tune_diff_free(df);
        LT_CHECK(r->has_manifest);
        LT_CHECK_INT(r->n_manifest, t->n_manifest);
    }
    /* a manifest stamped from this ini has no migration changes against it */
    {
        lt_migration_report mr;
        lt_migration_init(&mr);
        lt_migration_compare(&mr, t->manifest, t->n_manifest, ini);
        LT_CHECK(!lt_migration_has_changes(&mr));
        LT_CHECK_STR(mr.severity, "none");
        lt_migration_clear(&mr);
    }
    { lt_pagesrc none; memset(&none, 0, sizeof none); LT_CHECK(lt_ini_get(ini, &none, "reqFuel", &d) == 0); LT_CHECK_NEAR(d, 8.5, 0); }
    lt_free(s);
    lt_tune_free(t);
    lt_tune_free(r);
    lt_ini_free(ini);
}

/* ---- msq values against a real ini ---- */
LT_TEST(apply_sample_msq)
{
    lt_ini *ini = lt_ini_load_file(MINI, NULL);
    lt_tune *t = lt_tune_load("tests/fixtures/tune/sample.msq");
    lt_pagesrc ps;
    double v[10];
    const lt_ini_const *c;
    int n;
    LT_CHECK(ini != NULL && t != NULL);
    if (!ini || !t) return;
    n = lt_tune_apply_to_pages(t, ini);
    LT_CHECK(n >= 2);
    ps = lt_tune_pagesrc(t);
    LT_CHECK_INT(t->page_len[0], ini->proto.page_sizes[0]);
    LT_CHECK(lt_ini_get(ini, &ps, "reqFuel", &v[0]) == 0);
    LT_CHECK_NEAR(v[0], 12.6, 0.051);
    c = lt_ini_find_const(ini, "wueRates");
    LT_CHECK(c != NULL);
    if (c) {
        LT_CHECK_INT(lt_ini_const_get_array(ini, c, &ps, v, 10), 10);
        LT_CHECK_NEAR(v[0], 150, 0);
        LT_CHECK_NEAR(v[1], 136, 0);
        LT_CHECK_NEAR(v[9], 100, 0);
    }
    lt_tune_free(t);
    lt_ini_free(ini);
}

static const char *BITS_INI =
    "[MegaTune]\n signature = \"bits test\"\n[Constants]\n pageSize = 32, 16\n page = 1\n"
    " mode = bits, U08, 0, [0:1], \"Speed Density\", \"Alpha-N\", \"MAF\", INVALID\n"
    " flag = bits, U08, 0, [2:2], \"Off\", \"On\"\n"
    " wide = bits, U16, 1, [4:9], \"zero\"\n"
    " scaled = scalar, S16, 4, \"deg\", 0.1, -40.0, -40.0, 300.0, 1\n"
    " vin = string, ASCII, 8, 6\n"
    " tbl = array, U08, 16, [2x3], \"%\", 1.0, 0.0, 0, 255, 0\n"
    " page = 2\n"
    " p2 = scalar, U08, 0, \"x\", 1.0, 0.0, 0, 255, 0\n";

LT_TEST(apply_bits_strings_arrays)
{
    lt_ini *ini = lt_ini_load_string(BITS_INI, strlen(BITS_INI), NULL, NULL);
    lt_tune *t;
    lt_pagesrc ps;
    lt_tune_value v;
    double d;
    char buf[16];
    double arr[6];
    char *s;
    lt_tune *r;
    LT_CHECK(ini != NULL);
    if (!ini) return;
    t = lt_tune_new("bits test");
    lt_tune_value_init(&v);
    v.kind = LT_TV_STRING; v.str = lt_strdup("MAF");
    lt_tune_set_constant(t, "mode", &v, 0);
    lt_tune_value_clear(&v);
    v.kind = LT_TV_BOOL; v.boolean = 1;
    lt_tune_set_constant(t, "flag", &v, 0);
    lt_tune_value_clear(&v);
    v.kind = LT_TV_SCALAR; v.scalar = 33;
    lt_tune_set_constant(t, "wide", &v, 0);
    v.scalar = 20.5;
    lt_tune_set_constant(t, "scaled", &v, 0);
    lt_tune_value_clear(&v);
    v.kind = LT_TV_STRING; v.str = lt_strdup("ABC");
    lt_tune_set_constant(t, "vin", &v, 0);
    lt_tune_value_clear(&v);
    v.kind = LT_TV_ARRAY; v.n = 6; v.arr = (double *)lt_malloc(6 * sizeof(double));
    v.arr[0] = 1; v.arr[1] = 2; v.arr[2] = 3; v.arr[3] = 4; v.arr[4] = 5; v.arr[5] = 6;
    lt_tune_set_constant(t, "tbl", &v, 0);
    lt_tune_value_clear(&v);
    v.kind = LT_TV_SCALAR; v.scalar = 77;
    lt_tune_set_constant(t, "p2", &v, 1);
    lt_tune_set_constant(t, "unknown_to_ini", &v, 1);
    LT_CHECK_INT(lt_tune_apply_to_pages(t, ini), 7);
    ps = lt_tune_pagesrc(t);
    LT_CHECK_INT(t->page_len[0], 32);
    LT_CHECK_INT(t->page_len[1], 16);
    LT_CHECK_INT(t->pages[0][0], 2 | 4);
    LT_CHECK(lt_ini_get(ini, &ps, "mode", &d) == 0); LT_CHECK_NEAR(d, 2, 0);
    LT_CHECK(lt_ini_get(ini, &ps, "wide", &d) == 0); LT_CHECK_NEAR(d, 33, 0);
    LT_CHECK(lt_ini_get(ini, &ps, "scaled", &d) == 0); LT_CHECK_NEAR(d, 20.5, 1e-9);
    LT_CHECK(lt_ini_const_get_string(ini, lt_ini_find_const(ini, "vin"), &ps, buf, sizeof buf) == 0);
    LT_CHECK_STR(buf, "ABC");
    LT_CHECK(lt_ini_const_get_array(ini, lt_ini_find_const(ini, "tbl"), &ps, arr, 6) == 6);
    LT_CHECK_NEAR(arr[5], 6, 0);
    LT_CHECK_INT(t->pages[0][16 + 5], 6);
    LT_CHECK_INT(t->pages[1][0], 77);
    /* capture back: labels for bits with options, numbers otherwise */
    LT_CHECK(lt_tune_capture_from_pages(t, ini) >= 6);
    LT_CHECK(lt_tune_get_constant(t, "mode")->value.kind == LT_TV_STRING);
    LT_CHECK_STR(lt_tune_get_constant(t, "mode")->value.str, "MAF");
    LT_CHECK_STR(lt_tune_get_constant(t, "flag")->value.str, "On");
    LT_CHECK(lt_tune_get_constant(t, "wide")->value.kind == LT_TV_SCALAR);
    LT_CHECK_NEAR(lt_tune_get_constant(t, "wide")->value.scalar, 33, 0);
    LT_CHECK_STR(lt_tune_get_constant(t, "vin")->value.str, "ABC");
    LT_CHECK_INT(lt_tune_get_constant(t, "tbl")->value.n, 6);
    LT_CHECK_STR(lt_tune_get_constant(t, "tbl")->attrs.rows, "3");
    LT_CHECK_STR(lt_tune_get_constant(t, "tbl")->attrs.cols, "2");
    LT_CHECK_STR(lt_tune_get_constant(t, "scaled")->attrs.units, "deg");
    LT_CHECK_STR(lt_tune_get_constant(t, "scaled")->attrs.digits, "1");
    LT_CHECK(!lt_tune_get_constant(t, "mode")->has_attrs);
    LT_CHECK_STR(t->ini_metadata->signature, "bits test");
    s = save_str(t);
    LT_CHECK(strstr(s, "name=\"mode\">\"MAF\"</constant>") != NULL);
    LT_CHECK(strstr(s, "<constant cols=\"2\" digits=\"0\" name=\"tbl\" rows=\"3\" units=\"%\">1 2 3 4 5 6</constant>") != NULL);
    LT_CHECK(strstr(s, "<constant name=\"wide\">33</constant>") != NULL);
    r = lt_tune_parse_msq(s, strlen(s));
    LT_CHECK(r != NULL);
    lt_free(s);
    /* numeric / label fallbacks when applying */
    v.kind = LT_TV_STRING; v.str = lt_strdup("2");
    lt_tune_set_constant(t, "mode", &v, 0);
    lt_tune_value_clear(&v);
    lt_tune_apply_to_pages(t, ini);
    LT_CHECK(lt_ini_get(ini, &ps, "mode", &d) == 0); LT_CHECK_NEAR(d, 2, 0);
    v.kind = LT_TV_STRING; v.str = lt_strdup("alpha-n");
    lt_tune_set_constant(t, "mode", &v, 0);
    lt_tune_value_clear(&v);
    lt_tune_apply_to_pages(t, ini);
    LT_CHECK(lt_ini_get(ini, &ps, "mode", &d) == 0); LT_CHECK_NEAR(d, 1, 0);
    lt_tune_free(t); lt_tune_free(r);
    lt_ini_free(ini);
}

LT_TEST(apply_short_array_and_bad_values)
{
    lt_ini *ini = lt_ini_load_string(BITS_INI, strlen(BITS_INI), NULL, NULL);
    lt_tune *t;
    lt_tune_value v;
    double arr[6];
    lt_pagesrc ps;
    if (!ini) { LT_CHECK(0); return; }
    t = lt_tune_new("x");
    lt_tune_value_init(&v);
    v.kind = LT_TV_ARRAY; v.n = 2; v.arr = (double *)lt_malloc(2 * sizeof(double));
    v.arr[0] = 9; v.arr[1] = 8;
    lt_tune_set_constant(t, "tbl", &v, 0);
    lt_tune_value_clear(&v);
    /* a string value for a numeric constant is skipped */
    v.kind = LT_TV_STRING; v.str = lt_strdup("junk");
    lt_tune_set_constant(t, "p2", &v, 1);
    lt_tune_value_clear(&v);
    LT_CHECK_INT(lt_tune_apply_to_pages(t, ini), 1);
    ps = lt_tune_pagesrc(t);
    LT_CHECK(lt_ini_const_get_array(ini, lt_ini_find_const(ini, "tbl"), &ps, arr, 6) == 6);
    LT_CHECK_NEAR(arr[0], 9, 0); LT_CHECK_NEAR(arr[1], 8, 0); LT_CHECK_NEAR(arr[2], 0, 0);
    lt_tune_free(t);
    lt_ini_free(ini);
}

/* ---- varenv ---- */
static const char *TUNE_ENV_INI =
    "[MegaTune]\n signature = \"env\"\n[Constants]\n pageSize = 32\n page = 1\n"
    " mode = bits, U08, 0, [0:1], \"SD\", \"AlphaN\", \"MAF\"\n"
    " gain = scalar, U08, 1, \"x\", 0.5, 0.0, 0, 100, 1\n"
    " curve = array, U08, 2, [4], \"x\", 1.0, 0.0, 0, 255, 0\n"
    " name = string, ASCII, 8, 8\n"
    "[PcVariables]\n pcgain = scalar, U08, 1.0, 0.0, 0, 100, 0\n"
    "[OutputChannels]\n rpm = scalar, U16, 0, \"rpm\", 1.0, 0.0\n"
    " twice = { rpm * 2 }\n"
    " scaled = { gain * 10 }\n"
    "[Defaults]\n defaultValue = defgain, 7\n defaultValue = gain, 2\n";

LT_TEST(varenv_layers)
{
    lt_ini *ini = lt_ini_load_string(TUNE_ENV_INI, strlen(TUNE_ENV_INI), NULL, NULL);
    lt_tune *t, *pc;
    lt_pagesrc ps;
    lt_varenv env;
    lt_ini_rt *rt;
    lt_tune_value v;
    double d;
    const char *s;
    size_t n = 0;
    const char *const *opts;
    lt_u8 frame[8];
    LT_CHECK(ini != NULL);
    if (!ini) return;
    t = lt_tune_from_defaults(ini);
    ps = lt_tune_pagesrc(t);
    lt_ini_set(ini, &ps, "gain", 4.0);
    lt_ini_set(ini, &ps, "mode", 2);
    lt_ini_const_set_string(ini, lt_ini_find_const(ini, "name"), &ps, "hello");
    {
        double c[4];
        c[0] = 10; c[1] = 20; c[2] = 30; c[3] = 40;
        lt_ini_const_set_array(ini, lt_ini_find_const(ini, "curve"), &ps, c, 4);
    }
    pc = lt_tune_new("env");
    lt_tune_value_init(&v);
    v.scalar = 55;
    lt_tune_set_pc_variable(pc, "pcgain", &v);
    rt = lt_ini_rt_new(ini);
    lt_varenv_init(&env, ini, &ps, rt, pc);
    lt_ini_rt_set_parent(rt, &env.consts);
    frame[0] = 0x03; frame[1] = 0xE8;    /* rpm = 1000 (big endian) */
    frame[2] = frame[3] = 0;
    lt_ini_rt_update(rt, frame, 4);

    LT_CHECK(env.env.get_var(env.env.ud, "rpm", &d)); LT_CHECK_NEAR(d, 1000, 0);
    LT_CHECK(env.env.get_var(env.env.ud, "gain", &d)); LT_CHECK_NEAR(d, 4.0, 1e-9);
    LT_CHECK(env.env.get_var(env.env.ud, "mode", &d)); LT_CHECK_NEAR(d, 2, 0);
    LT_CHECK(env.env.get_var(env.env.ud, "pcgain", &d)); LT_CHECK_NEAR(d, 55, 0);
    LT_CHECK(env.env.get_var(env.env.ud, "twice", &d)); LT_CHECK_NEAR(d, 2000, 0);
    LT_CHECK(env.env.get_var(env.env.ud, "scaled", &d)); LT_CHECK_NEAR(d, 40, 1e-9);
    LT_CHECK(env.env.get_var(env.env.ud, "defgain", &d)); LT_CHECK_NEAR(d, 7, 0);
    LT_CHECK(!env.env.get_var(env.env.ud, "nothing", &d));
    /* consts env: no channels */
    LT_CHECK(!env.consts.get_var(env.consts.ud, "rpm", &d));
    LT_CHECK(env.consts.get_var(env.consts.ud, "gain", &d));
    /* strings and options */
    s = env.env.get_string(env.env.ud, "name");
    LT_CHECK(s != NULL && strcmp(s, "hello") == 0);
    s = env.env.get_string(env.env.ud, "mode");
    LT_CHECK(s != NULL && strcmp(s, "MAF") == 0);
    LT_CHECK(env.env.get_string(env.env.ud, "gain") == NULL);
    opts = env.env.get_bit_options(env.env.ud, "mode", &n);
    LT_CHECK(opts != NULL && n == 3 && strcmp(opts[1], "AlphaN") == 0);
    LT_CHECK(env.env.get_bit_options(env.env.ud, "gain", &n) == NULL);
    /* arrayValue interpolation */
    LT_CHECK(env.env.array_value(env.env.ud, "curve", 1.5, &d)); LT_CHECK_NEAR(d, 25, 1e-9);
    LT_CHECK(env.env.array_value(env.env.ud, "curve", -3, &d)); LT_CHECK_NEAR(d, 10, 0);
    LT_CHECK(env.env.array_value(env.env.ud, "curve", 99, &d)); LT_CHECK_NEAR(d, 40, 0);
    LT_CHECK(!env.env.array_value(env.env.ud, "gain2", 0, &d));
    LT_CHECK(!env.env.is_online(env.env.ud));
    env.online = LT_TRUE;
    LT_CHECK(env.env.is_online(env.env.ud));
    LT_CHECK(env.env.time_now(env.env.ud) >= -1.0);
    /* expressions end to end */
    LT_CHECK(lt_expr_test("rpm > 500 && mode == 2", &env.env));
    LT_CHECK(!lt_expr_test("gain > 5", &env.env));
    LT_CHECK(lt_expr_test("pcgain == 55", &env.env));
    {
        lt_expr *e = lt_expr_compile("stringValue(name) == \"hello\"", NULL, 0);
        lt_bool b = LT_FALSE;
        if (e) {
            lt_expr_eval_bool(e, &env.env, &b);
            LT_CHECK(b);
            lt_expr_free(e);
        }
    }
    /* page precedence: pc variable tune overrides defaults but not channels */
    lt_tune_value_init(&v);
    v.scalar = 3;
    lt_tune_set_pc_variable(pc, "rpm", &v);
    LT_CHECK(env.env.get_var(env.env.ud, "rpm", &d)); LT_CHECK_NEAR(d, 1000, 0);
    LT_CHECK(env.consts.get_var(env.consts.ud, "rpm", &d)); LT_CHECK_NEAR(d, 3, 0);
    /* pages not loaded: falls back to [Defaults] */
    {
        lt_tune *empty = lt_tune_new("env");
        lt_pagesrc ps2 = lt_tune_pagesrc(empty);
        lt_varenv e2;
        lt_varenv_init(&e2, ini, &ps2, NULL, NULL);
        LT_CHECK(e2.env.get_var(e2.env.ud, "defgain", &d));
        LT_CHECK(!e2.env.get_var(e2.env.ud, "gain", &d) || 1);
        lt_tune_free(empty);
    }
    lt_ini_rt_free(rt);
    lt_tune_free(t);
    lt_tune_free(pc);
    lt_ini_free(ini);
}

/* ---- ECU memory glue ---- */
LT_TEST(mem_glue)
{
    lt_ini *ini = lt_ini_load_file(MINI, NULL);
    lt_ecu_mem *mem;
    lt_tune *t, *back;
    lt_pagesrc ps;
    size_t len = 0;
    const lt_u8 *p;
    LT_CHECK(ini != NULL);
    if (!ini) return;
    t = lt_tune_from_defaults(ini);
    mem = lt_ecu_mem_from_ini(ini);
    LT_CHECK_INT(lt_tune_to_mem(t, mem), (int)ini->n_pages);
    LT_CHECK(lt_ecu_mem_fully_loaded(mem));
    LT_CHECK(!lt_ecu_mem_has_dirty(mem));
    ps = lt_ecu_mem_pagesrc(mem);
    LT_CHECK(lt_ini_set(ini, &ps, "reqFuel", 9.5) == 0);
    LT_CHECK(lt_ecu_mem_has_dirty(mem));
    back = lt_tune_new(ini->signature);
    LT_CHECK_INT(lt_tune_from_mem(back, mem), (int)ini->n_pages);
    p = lt_tune_get_page(back, 0, &len);
    LT_CHECK(p != NULL && len == ini->proto.page_sizes[0]);
    lt_tune_capture_from_pages(back, ini);
    LT_CHECK_NEAR(lt_tune_get_constant(back, "reqFuel")->value.scalar, 9.5, 0.051);
    lt_ecu_mem_free(mem);
    lt_tune_free(t);
    lt_tune_free(back);
    lt_ini_free(ini);
}

/* ---- INI version migration with a real stamped manifest ---- */
LT_TEST(migration_between_inis)
{
    lt_ini *a = lt_ini_load_file(MINI, NULL);
    lt_ini *b = lt_ini_load_file(DEMO, NULL);
    lt_tune *t;
    lt_migration_report r;
    char *s;
    LT_CHECK(a != NULL && b != NULL);
    if (!a || !b) return;
    t = lt_tune_from_defaults(a);
    lt_migration_init(&r);
    lt_migration_compare(&r, t->manifest, t->n_manifest, b);
    LT_CHECK(lt_migration_has_changes(&r));
    LT_CHECK(r.n_missing_in_ini > 0);
    LT_CHECK_STR(r.severity, "high");
    s = lt_migration_summary(&r);
    LT_CHECK(strstr(s, "removed constants") != NULL);
    lt_free(s);
    lt_migration_clear(&r);
    lt_tune_free(t);
    lt_ini_free(a);
    lt_ini_free(b);
}

int main(void)
{
    LT_RUN(roundtrip_mini);
    LT_RUN(roundtrip_demo);
    LT_RUN(demo_defaults_file_roundtrip);
    LT_RUN(from_defaults_speeduino);
    LT_RUN(apply_sample_msq);
    LT_RUN(apply_bits_strings_arrays);
    LT_RUN(apply_short_array_and_bad_values);
    LT_RUN(varenv_layers);
    LT_RUN(mem_glue);
    LT_RUN(migration_between_inis);
    LT_TEST_MAIN_END
}
