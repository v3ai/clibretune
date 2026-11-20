/* test_project_fuzz.c - mutation/truncation fuzzing of project-module parsers. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "plat/plat.h"
#include "json/json.h"
#include "project/project.h"
#include "project/proj_priv.h"
#include "app/files_logic.h"
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
    static char buf[4096];
    size_t got;
    for (i = 0; i < sizeof SEEDS / sizeof SEEDS[0]; i++) fuzz_text(SEEDS[i]);
    f = fopen("tests/fixtures/project/mini.ini", "rb");
    if (f) { got = fread(buf, 1, 3000, f); fclose(f); buf[got] = 0; fuzz_text(buf); }
    LT_CHECK(1);
}

LT_TEST(fuzz_random_bytes)
{
    char b[200] = { 0 };
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

/* project.json numbers outside the range of unsigned were converted with a plain cast
 * (undefined behaviour, caught by UBSan): they saturate now */
LT_TEST(project_json_number_range)
{
    static const char js[] = "{\"version\":\"1.0\",\"name\":\"n\",\"created\":\"c\",\"modified\":\"m\","
        "\"ecu_definition\":\"d.ini\",\"signature\":\"s\",\"connection\":{\"port\":null,"
        "\"baud_rate\":1e300,\"timeout_ms\":4294967296.5},\"settings\":{\"auto_load_tune\":true,"
        "\"auto_save_tune\":false,\"auto_connect\":false,\"max_restore_points\":-20}}";
    lt_proj_config c;
    lt_file_write("build/tfz_project.json", js, sizeof js - 1);
    LT_CHECK(lt_proj_config_read(&c, "build/tfz_project.json") == LT_OK);
    LT_CHECK(c.conn.baud_rate == UINT_MAX && c.conn.timeout_ms == UINT_MAX);
    LT_CHECK(c.settings.max_restore_points == 0);
    lt_proj_config_clear(&c);
}

/* TunerStudio import: mutated project.properties / INI / tune in a scratch TS project */
#define TS_DIR "build/tfz_ts"
#define TS_OUT "build/tfz_ts_out"

static void ts_write(const char *rel, const char *d, size_t n)
{
    char *p = lt_path_join(TS_DIR, rel);
    lt_file_write(p, d, n);
    lt_free(p);
}

static void ts_import_once(void)
{
    lt_ts_preview pv;
    lt_project p;
    (void)lt_ts_preview_read(TS_DIR, &pv);
    memset(&p, 0, sizeof p);
    if (lt_project_import_ts(TS_DIR, TS_OUT, &p) == LT_OK) lt_project_free(&p);
    lt_proj_rmtree(TS_OUT);
    lt_plat_mkdirs(TS_OUT);
}

LT_TEST(fuzz_ts_import)
{
    static const char *files[3] = { "projectCfg/project.properties", "projectCfg/mainController.ini",
                                    "CurrentTune.msq" };
    static const char *src[3] = { "tests/fixtures/project/ts_proj/projectCfg/project.properties",
                                  "tests/fixtures/project/ts_proj/projectCfg/mainController.ini",
                                  "tests/fixtures/project/ts_proj/CurrentTune.msq" };
    char *orig[3];
    size_t on[3], i;
    int it, k;
    lt_proj_rmtree(TS_DIR);
    lt_plat_mkdirs(TS_DIR "/projectCfg");
    lt_plat_mkdirs(TS_OUT);
    for (i = 0; i < 3; i++) {
        orig[i] = lt_file_read(src[i], &on[i]);
        LT_CHECK(orig[i] != NULL && on[i] > 0);
        if (orig[i] == NULL) return;
        ts_write(files[i], orig[i], on[i]);
    }
    ts_import_once();
    for (it = 0; it < 240; it++) {
        size_t f = (size_t)(it % 3), n = on[f];
        char *m = (char *)lt_malloc(n + 1);
        memcpy(m, orig[f], n);
        for (k = 0; k < 1 + (int)(rnd() % 5); k++) {
            m[rnd() % n] = (rnd() & 1) ? (char)(rnd() & 0xff) : "=\\\n.:/\"#"[rnd() % 8];
        }
        if (it % 4 == 0) n = rnd() % (n + 1);
        ts_write(files[f], m, n);
        ts_import_once();
        ts_write(files[f], orig[f], on[f]);
        lt_free(m);
    }
    for (i = 0; i < 3; i++) lt_free(orig[i]);
    LT_CHECK(1);
}

/* ecuConfigFile is a file name inside projectCfg/: a path in it must not reach outside the
 * imported project (it used to be joined as is, so "../../x" read and copied any file) */
LT_TEST(ts_import_no_traversal)
{
    static const char props[] = "projectName=Trav\necuConfigFile=../../../tests/fixtures/ini/ms3.ini\n";
    static const char abs_props[] = "projectName=Trav\necuConfigFile=/etc/hostname\n";
    lt_ts_preview pv;
    lt_project p;
    lt_proj_rmtree(TS_DIR);
    lt_plat_mkdirs(TS_DIR "/projectCfg");
    lt_plat_mkdirs(TS_OUT);
    ts_write("projectCfg/project.properties", props, sizeof props - 1);
    LT_CHECK(lt_ts_preview_read(TS_DIR, &pv) == 0);
    LT_CHECK(strcmp(pv.ini_file, "ms3.ini") == 0 && !pv.ini_found);
    memset(&p, 0, sizeof p);
    LT_CHECK(lt_project_import_ts(TS_DIR, TS_OUT, &p) == LT_ENOENT);
    ts_write("projectCfg/project.properties", abs_props, sizeof abs_props - 1);
    LT_CHECK(lt_project_import_ts(TS_DIR, TS_OUT, &p) == LT_ENOENT);
    /* the plain name inside projectCfg still works */
    {
        size_t n = 0;
        char *ini = lt_file_read("tests/fixtures/project/ts_proj/projectCfg/mainController.ini", &n);
        LT_CHECK(ini != NULL);
        if (ini) ts_write("projectCfg/ms3.ini", ini, n);
        lt_free(ini);
    }
    ts_write("projectCfg/project.properties", props, sizeof props - 1);
    LT_CHECK(lt_ts_preview_read(TS_DIR, &pv) == 0 && pv.ini_found);
    if (lt_project_import_ts(TS_DIR, TS_OUT, &p) == LT_OK) lt_project_free(&p);
    else LT_CHECK(0);
    lt_proj_rmtree(TS_OUT);
    lt_proj_rmtree(TS_DIR);
}

int main(void)
{
    LT_RUN(fuzz_seeds);
    LT_RUN(fuzz_random_bytes);
    LT_RUN(json_deep_nesting);
    LT_RUN(project_json_number_range);
    LT_RUN(fuzz_ts_import);
    LT_RUN(ts_import_no_traversal);
    LT_TEST_MAIN_END
}
