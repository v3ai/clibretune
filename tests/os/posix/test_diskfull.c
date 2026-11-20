/* test_diskfull.c - a real "file too large / disk full" error from the kernel (RLIMIT_FSIZE)
 * while a tune, settings file or project.json is being replaced must leave the previous file
 * complete and byte-identical. (Before atomic saves the file was truncated in place.)
 *
 * PLATFORM FILE (tests/os/posix, built only for POSIX targets): includes sys/resource.h and
 * signal.h for setrlimit/SIGXFSZ; feature-test macros come from the build (tools/target.sh).
 */
#include "lt_test.h"
#include "../../../src/plat/plat.h"
#include "../../../src/tune/tune.h"
#include "../../../src/project/project.h"
#include <signal.h>
#include <string.h>
#include <sys/resource.h>

#define DIR "build/tdiskfull"

static int same_file(const char *path, const char *data, size_t n)
{
    size_t m = 0;
    char *d = lt_file_read(path, &m);
    int same = d != NULL && m == n && memcmp(d, data, n) == 0;
    lt_free(d);
    return same;
}

static struct rlimit g_old;

static int limit_file_size(rlim_t bytes)
{
    struct rlimit r;
    if (getrlimit(RLIMIT_FSIZE, &g_old) != 0) return -1;
    r = g_old;
    r.rlim_cur = bytes;
    return setrlimit(RLIMIT_FSIZE, &r);
}

static void unlimit(void)
{
    (void)setrlimit(RLIMIT_FSIZE, &g_old);
}

LT_TEST(tune_and_settings_survive_efbig)
{
    const char *tp = DIR "/CurrentTune.msq", *sp = DIR "/settings.conf";
    lt_tune *t = lt_tune_load("tests/fixtures/tune/sample.msq");
    lt_tune_value v;
    lt_props pr;
    char *orig_t, *orig_s, key[32];
    size_t nt = 0, ns = 0;
    int i;
    LT_CHECK(t != NULL);
    if (t == NULL) return;
    signal(SIGXFSZ, SIG_IGN);              /* get EFBIG from write() instead of a signal */
    lt_plat_mkdirs(DIR);
    LT_CHECK(lt_tune_save(t, tp) == 0);
    lt_props_init(&pr);
    lt_props_set(&pr, "theme", "dark");
    LT_CHECK(lt_props_save(&pr, sp) == 0);
    orig_t = lt_file_read(tp, &nt);
    orig_s = lt_file_read(sp, &ns);
    LT_CHECK(orig_t != NULL && orig_s != NULL && nt < 3000 && ns < 3000);
    /* both grow well past the limit */
    lt_tune_value_init(&v);
    for (i = 0; i < 200; i++) {
        lt_snprintf(key, sizeof(key), "growConstant%d", i);
        v.scalar = i;
        lt_tune_set_constant(t, key, &v, 0);
        lt_props_set(&pr, key, "some value that takes room");
    }
    LT_CHECK(limit_file_size(3000) == 0);
    LT_CHECK(lt_tune_save(t, tp) != 0);
    LT_CHECK(lt_props_save(&pr, sp) != 0);
    unlimit();
    LT_CHECK(same_file(tp, orig_t, nt));
    LT_CHECK(same_file(sp, orig_s, ns));
    {
        lt_tune *r = lt_tune_load(tp);
        LT_CHECK(r != NULL);
        if (r) lt_tune_free(r);
    }
    LT_CHECK(lt_tune_save(t, tp) == 0);    /* with room it works */
    lt_props_free(&pr);
    lt_free(orig_t);
    lt_free(orig_s);
    lt_tune_free(t);
}

int main(void)
{
    LT_RUN(tune_and_settings_survive_efbig);
    LT_TEST_MAIN_END
}
