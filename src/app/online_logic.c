/* online_logic.c - see online_logic.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "online_logic.h"
#include "../plat/plat.h"
#include "../project/project.h"

static char **grow_ptrs(char **p, size_t n)
{
    size_t bytes;
    if (!lt_mul_size(n, sizeof(char *), &bytes)) { bytes = (size_t)-1; }     /* OOM abort */
    return (char **)lt_realloc(p, bytes);
}

static const lt_ionline_src sources[] = {
    { "speeduino", "Speeduino", "https://github.com/speeduino/speeduino/tree/master/reference",
      "Open the .ini file for your firmware version and use the download button." },
    { "fome", "FOME",
      "https://github.com/FOME-Tech/fome-fw/tree/master/firmware/tunerstudio/generated",
      "Pick the .ini that matches your board and download it." },
    { "rusefi", "rusEFI", "https://rusefi.com/online/ini/rusefi/",
      "Download the .ini matching your firmware build." },
    { "epicefi", "epicEFI", "https://content.epicefi.com/firmware/ini/",
      "Download the .ini matching your firmware build." }
};

int lt_ionline_n(void)
{
    return (int)LT_COUNTOF(sources);
}

const lt_ionline_src *lt_ionline_get(int i)
{
    if (i < 0 || i >= lt_ionline_n()) { return NULL; }
    return &sources[i];
}

static int starts_ci(const char *s, const char *pre)
{
    size_t n = strlen(pre), i;
    for (i = 0; i < n; i++) {
        char a = s[i], b = pre[i];
        if (a == '\0') { return 0; }
        if (a >= 'A' && a <= 'Z') { a = (char)(a - 'A' + 'a'); }
        if (a != b) { return 0; }
    }
    return 1;
}

int lt_ionline_pick(const char *signature)
{
    int i;
    if (signature == NULL) { return -1; }
    while (*signature == ' ') { signature++; }
    for (i = 0; i < lt_ionline_n(); i++) {
        if (starts_ci(signature, sources[i].id)) { return i; }
    }
    return -1;
}

char *lt_ionline_downloads_dir(void)
{
    char *env = lt_plat_getenv("LT_DOWNLOADS_DIR");
    char *home, *d;
    if (env != NULL && env[0] != '\0') { return env; }
    lt_free(env);
    home = lt_plat_home_dir();
    d = lt_path_join(home, "Downloads");
    lt_free(home);
    if (d != NULL && !lt_plat_is_dir(d)) { lt_free(d); d = NULL; }
    return d;
}

void lt_ionline_watch_init(lt_ionline_watch *w)
{
    memset(w, 0, sizeof(*w));
}

void lt_ionline_watch_free(lt_ionline_watch *w)
{
    size_t i;
    for (i = 0; i < w->n_seen; i++) { lt_free(w->seen[i]); }
    lt_free(w->seen);
    lt_free(w->dir);
    memset(w, 0, sizeof(*w));
}

static int is_ini(const char *name)
{
    const char *e = lt_path_ext(name);
    return e != NULL && lt_stricmp(e, ".ini") == 0;
}

static int seen_has(const lt_ionline_watch *w, const char *name)
{
    size_t i;
    for (i = 0; i < w->n_seen; i++) {
        if (strcmp(w->seen[i], name) == 0) { return 1; }
    }
    return 0;
}

static void seen_add(lt_ionline_watch *w, const char *name)
{
    if (w->n_seen == w->cap_seen) {
        size_t nc = w->cap_seen == 0 ? 32 : w->cap_seen * 2;
        w->seen = (char **)grow_ptrs(w->seen, nc);
        w->cap_seen = nc;
    }
    w->seen[w->n_seen++] = lt_strdup(name);
}

int lt_ionline_watch_start(lt_ionline_watch *w, const char *dir)
{
    char **names = NULL;
    int n, i;
    lt_ionline_watch_free(w);
    if (dir == NULL) { return -1; }
    n = lt_plat_list_dir(dir, &names);
    if (n < 0) { return -1; }
    w->dir = lt_strdup(dir);
    for (i = 0; i < n; i++) {
        if (is_ini(names[i])) { seen_add(w, names[i]); }
    }
    lt_plat_free_list(names, n);
    return 0;
}

size_t lt_ionline_watch_poll(lt_ionline_watch *w, char ***paths)
{
    char **names = NULL, **out = NULL;
    int n, i;
    size_t got = 0;
    *paths = NULL;
    if (w->dir == NULL) { return 0; }
    n = lt_plat_list_dir(w->dir, &names);
    if (n < 0) { return 0; }
    for (i = 0; i < n; i++) {
        char *full, *sig = NULL, *nm = NULL, *txt;
        size_t len = 0;
        if (!is_ini(names[i]) || seen_has(w, names[i])) { continue; }
        full = lt_path_join(w->dir, names[i]);
        txt = full != NULL ? lt_file_read(full, &len) : NULL;
        if (txt != NULL && len > 0) {
            (void)lt_ini_extract_info(txt, &sig, &nm);
        }
        lt_free(txt);
        lt_free(nm);
        if (sig == NULL || sig[0] == '\0') {      /* empty or still downloading, or not an ECU INI */
            lt_free(sig);
            lt_free(full);
            continue;
        }
        lt_free(sig);
        seen_add(w, names[i]);
        out = (char **)grow_ptrs(out, got + 1);
        out[got++] = full;
    }
    lt_plat_free_list(names, n);
    *paths = out;
    return got;
}

size_t lt_ionline_list(const char *dir, char ***paths)
{
    lt_ionline_watch w;
    size_t n;
    lt_ionline_watch_init(&w);
    w.dir = lt_strdup(dir);
    n = lt_ionline_watch_poll(&w, paths);
    lt_ionline_watch_free(&w);
    return n;
}
