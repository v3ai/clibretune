/* ini_check_parse.c - helper of tools/ini_corpus_check.sh: parse INI files, print the parse time
 * and a few counts, and list every [Menu] target (submenus followed) as the app sees them.
 *   ini_check_parse FILE...
 * Output, tab separated:
 *   P  <ms>  <file>  consts=N channels=N dialogs=N tables=N curves=N menus=N
 *   T  <file>  <target>            (one per distinct menu target)
 *   E  <file>  <error>             (load failed)
 * Built by the check script against the library sources; not part of ./build.sh. */
#include <stdio.h>
#include <string.h>
#include "lt.h"
#include "ini/ini.h"
#include "plat/plat.h"

static void targets(const char *file, lt_ini_menu_item **items, size_t n, lt_map *seen)
{
    size_t i;
    for (i = 0; i < n; i++) {
        const lt_ini_menu_item *it = items[i];
        if (it->target != NULL && it->target[0] != '\0' && !lt_map_has(seen, it->target) &&
            strcmp(it->target, "std_separator") != 0) {
            lt_map_put(seen, it->target, (void *)it);
            printf("T\t%s\t%s\n", file, it->target);
        }
        if (it->n_items > 0) targets(file, it->items, it->n_items, seen);
    }
}

int main(int argc, char **argv)
{
    int a;
    for (a = 1; a < argc; a++) {
        double t0 = lt_plat_now_ms(), t1;
        lt_ini *ini = lt_ini_load_file(argv[a], NULL);
        lt_map seen;
        size_t i;
        t1 = lt_plat_now_ms();
        if (ini == NULL) {
            printf("E\t%s\t%s\n", argv[a], lt_err_get());
            continue;
        }
        printf("P\t%.0f\t%s\tconsts=%lu channels=%lu dialogs=%lu tables=%lu curves=%lu menus=%lu\n",
               t1 - t0, argv[a], (unsigned long)ini->n_constants, (unsigned long)ini->n_channels,
               (unsigned long)ini->n_dialogs, (unsigned long)ini->n_tables,
               (unsigned long)ini->n_curves, (unsigned long)ini->n_menus);
        lt_map_init(&seen, LT_FALSE);
        for (i = 0; i < ini->n_menus; i++) targets(argv[a], ini->menus[i].items, ini->menus[i].n_items, &seen);
        lt_map_free(&seen);
        lt_ini_free(ini);
        fflush(stdout);
    }
    return 0;
}
