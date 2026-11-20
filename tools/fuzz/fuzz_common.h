/* fuzz_common.h - shared helpers for the libFuzzer harnesses in tools/fuzz/.
 * The harnesses are clang-only (-fsanitize=fuzzer) and never part of the normal build;
 * tools/fuzz/run.sh builds and runs them from the repository root. */
#ifndef LT_FUZZ_COMMON_H
#define LT_FUZZ_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lt.h"
#include "ini/ini.h"

/* NUL-terminated private copy of the input (most text parsers want one) */
static char *fz_cstr(const uint8_t *d, size_t n)
{
    char *s = (char *)malloc(n + 1);
    if (n) memcpy(s, d, n);
    s[n] = 0;
    return s;
}

static lt_ini *fz_load_ini(const char *path)
{
    lt_ini *ini = lt_ini_load_file(path, NULL);
    if (!ini) {
        fprintf(stderr, "fuzz: cannot load %s (run from the repository root)\n", path);
        abort();
    }
    return ini;
}

/* the demo INI (rusEFI, ~5000 constants), loaded once */
static lt_ini *fz_demo_ini(void)
{
    static lt_ini *ini;
    if (!ini) ini = fz_load_ini("tests/fixtures/ini/demo.ini");
    return ini;
}

/* a small Speeduino INI: cheap enough to apply every input to */
static lt_ini *fz_small_ini(void)
{
    static lt_ini *ini;
    if (!ini) ini = fz_load_ini("tests/fixtures/tune/mini_speeduino.ini");
    return ini;
}

/* per-process scratch directory below $FUZZ_TMP (run.sh sets it) or build/fuzz/tmp */
static const char *fz_tmpdir(void)
{
    static char dir[256];
    if (!dir[0]) {
        char cmd[400];
        const char *root = getenv("FUZZ_TMP");
        snprintf(dir, sizeof dir, "%s/p%ld", root && root[0] ? root : "build/fuzz/tmp", (long)getpid());
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
        if (system(cmd) != 0) abort();
    }
    return dir;
}

static void fz_write(const char *path, const void *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    if (n) fwrite(d, 1, n, f);
    fclose(f);
}

/* in-memory pages sized from an INI */
#define FZ_MAXPG 64
typedef struct fz_pages { lt_u8 *p[FZ_MAXPG]; size_t n[FZ_MAXPG]; } fz_pages;

static const lt_u8 *fz_page(void *ud, int page, size_t *len)
{
    fz_pages *pg = (fz_pages *)ud;
    if (page < 0 || page >= FZ_MAXPG || !pg->p[page]) return NULL;
    *len = pg->n[page];
    return pg->p[page];
}

static int fz_page_write(void *ud, int page, size_t off, const lt_u8 *data, size_t n)
{
    fz_pages *pg = (fz_pages *)ud;
    if (page < 0 || page >= FZ_MAXPG || !pg->p[page]) return -1;
    if (off > pg->n[page] || n > pg->n[page] - off) return -1;
    memcpy(pg->p[page] + off, data, n);
    return 0;
}

static void fz_pages_init(fz_pages *pg, const lt_ini *ini, int fill)
{
    size_t i;
    memset(pg, 0, sizeof *pg);
    for (i = 0; i < ini->proto.n_page_sizes && i < FZ_MAXPG; i++) {
        size_t sz = (size_t)ini->proto.page_sizes[i];
        if (sz > (1u << 20)) sz = 1u << 20;
        pg->n[i] = sz;
        pg->p[i] = (lt_u8 *)malloc(sz ? sz : 1);
        memset(pg->p[i], fill, sz);
    }
}

static void fz_pages_free(fz_pages *pg)
{
    int i;
    for (i = 0; i < FZ_MAXPG; i++) free(pg->p[i]);
}

static lt_pagesrc fz_pagesrc(fz_pages *pg)
{
    lt_pagesrc s;
    s.ud = pg;
    s.page = fz_page;
    s.write = fz_page_write;
    return s;
}

#endif
