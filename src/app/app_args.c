/* app_args.c - the command line of main.c, parsed here so tests can run it (see app.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "app.h"
#include "../plat/plat.h"

lt_bool lt_app_is_project_dir(const char *dir)
{
    char *f;
    lt_bool ok;
    if (dir == NULL || dir[0] == '\0' || !lt_plat_is_dir(dir)) { return LT_FALSE; }
    f = lt_path_join(dir, "project.json");
    ok = lt_file_exists(f) && !lt_plat_is_dir(f);
    lt_free(f);
    return ok;
}

int lt_app_args_parse(int argc, char **argv, lt_app_args *a, char *err, size_t cap)
{
    const char *folder = NULL;
    int i;
    memset(a, 0, sizeof(*a));
    if (cap > 0) { err[0] = '\0'; }
    for (i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (strcmp(s, "--help") == 0 || strcmp(s, "-h") == 0) { a->help = 1; return 1; }
        if (strcmp(s, "--version") == 0) { a->version = 1; return 1; }
        if (strcmp(s, "--demo") == 0) { a->demo = 1; }
        else if (strcmp(s, "--mcp") == 0) { a->mcp = 1; }
        else if (strcmp(s, "--backend") == 0 && i + 1 < argc) { a->backend = argv[++i]; }
        else if (strcmp(s, "--project") == 0 && i + 1 < argc) { a->project = argv[++i]; }
        else if (strcmp(s, "--ini") == 0 && i + 1 < argc) { a->ini = argv[++i]; }
        else if (s[0] != '-' && folder == NULL) { folder = s; }
        else if (s[0] != '-') {
            lt_snprintf(err, cap, "only one project folder can be opened ('%s', '%s')", folder, s);
            return 2;
        } else {
            lt_snprintf(err, cap, "unknown option '%s' (try --help)", s);
            return 2;
        }
    }
    if (folder != NULL) {
        /* `clibretune <folder>`: what a file manager or a desktop shortcut passes */
        if (a->project != NULL) {
            lt_snprintf(err, cap, "'%s' and --project %s: give one project", folder, a->project);
            return 2;
        }
        if (!lt_app_is_project_dir(folder)) {
            lt_snprintf(err, cap, "'%s' is not a clibretune project folder (it has no project.json)",
                        folder);
            return 2;
        }
        a->project = folder;
    }
    return 0;
}
