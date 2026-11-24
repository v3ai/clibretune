/* test_projfiles.c - projects and files: recent projects stored as absolute paths (and the
 * migration of older relative entries), the command line (`libretune <folder>`).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/plat/plat.h"

#define TMP "build/test_projfiles"
#define DEMO "tests/fixtures/ini/demo.ini"

/* a fresh project under TMP/<sub>; returns its path as created (relative, malloc'd) */
static char *make_project(const char *sub, const char *name)
{
    lt_project p;
    char *parent = lt_path_join(TMP, sub), *dir;
    (void)lt_plat_mkdirs(parent);
    dir = lt_path_join(parent, name);
    if (!lt_app_is_project_dir(dir)) {
        LT_CHECK(lt_project_create(name, DEMO, "sig", parent, &p) == 0);
        lt_project_close(&p);
    }
    lt_free(parent);
    return dir;
}

LT_TEST(args)
{
    lt_app_args a;
    char err[600];
    char *dir = make_project("args", "Proj");
    char *argv[6];
    argv[0] = (char *)"libretune";
    /* no arguments */
    LT_CHECK(lt_app_args_parse(1, argv, &a, err, sizeof(err)) == 0);
    LT_CHECK(a.project == NULL && !a.demo && !a.mcp);
    /* a single project folder: like --project */
    argv[1] = dir;
    LT_CHECK(lt_app_args_parse(2, argv, &a, err, sizeof(err)) == 0);
    LT_CHECK(a.project != NULL && strcmp(a.project, dir) == 0);
    /* with options around it */
    argv[1] = (char *)"--demo"; argv[2] = dir; argv[3] = (char *)"--backend"; argv[4] = (char *)"tty";
    LT_CHECK(lt_app_args_parse(5, argv, &a, err, sizeof(err)) == 0);
    LT_CHECK(a.demo && a.project == dir && a.backend != NULL && strcmp(a.backend, "tty") == 0);
    /* a folder without project.json is refused with the reason */
    argv[1] = (char *)TMP;
    LT_CHECK(lt_app_args_parse(2, argv, &a, err, sizeof(err)) == 2);
    LT_CHECK(strstr(err, "project.json") != NULL);
    argv[1] = (char *)TMP "/no/such/folder";
    LT_CHECK(lt_app_args_parse(2, argv, &a, err, sizeof(err)) == 2);
    /* two folders, or a folder and --project */
    argv[1] = dir; argv[2] = dir;
    LT_CHECK(lt_app_args_parse(3, argv, &a, err, sizeof(err)) == 2);
    argv[1] = (char *)"--project"; argv[2] = dir; argv[3] = dir;
    LT_CHECK(lt_app_args_parse(4, argv, &a, err, sizeof(err)) == 2);
    /* the other options */
    argv[1] = (char *)"--project"; argv[2] = dir;
    LT_CHECK(lt_app_args_parse(3, argv, &a, err, sizeof(err)) == 0 && a.project == dir);
    argv[1] = (char *)"--bogus";
    LT_CHECK(lt_app_args_parse(2, argv, &a, err, sizeof(err)) == 2);
    LT_CHECK(strstr(err, "--bogus") != NULL);
    argv[1] = (char *)"--help";
    LT_CHECK(lt_app_args_parse(2, argv, &a, err, sizeof(err)) == 1 && a.help);
    argv[1] = (char *)"--version";
    LT_CHECK(lt_app_args_parse(2, argv, &a, err, sizeof(err)) == 1 && a.version);
    LT_CHECK(lt_app_is_project_dir(dir));
    LT_CHECK(!lt_app_is_project_dir(TMP));
    LT_CHECK(!lt_app_is_project_dir(NULL));
    lt_free(dir);
}

/* recent projects are stored resolved: one project reached by different spellings is one entry */
LT_TEST(recent_abs)
{
    lt_app *app = lt_app_new();
    char *dir = make_project("rec", "Alpha"), *other = make_project("rec", "Beta");
    char *abs = lt_plat_abspath(dir), *abs2 = lt_plat_abspath(other);
    char *dotted = lt_path_join(TMP, "rec/../rec/./Alpha/");
    app->settings.n_recent = 0;
    LT_CHECK(lt_app_open_project(app, dir) == 0);
    LT_CHECK_STR(app->project.path, abs);                  /* the project knows its absolute path */
    LT_CHECK_INT(app->settings.n_recent, 1);
    LT_CHECK_STR(app->settings.recent[0], abs);
    LT_CHECK_STR(app->settings.last_project, abs);
    lt_app_recent_add(app, other);
    lt_app_recent_add(app, dotted);                        /* same folder, other spelling */
    LT_CHECK_INT(app->settings.n_recent, 2);
    LT_CHECK_STR(app->settings.recent[0], abs);
    LT_CHECK_STR(app->settings.recent[1], abs2);
    lt_app_free(app);
    lt_free(dir); lt_free(other); lt_free(abs); lt_free(abs2); lt_free(dotted);
}

/* settings.conf of an older build with relative and duplicate recent entries */
LT_TEST(recent_migrate)
{
    lt_app *app = lt_app_new();
    char *dir = make_project("mig", "Gamma");
    char *abs = lt_plat_abspath(dir), *gone = lt_plat_abspath(TMP "/mig/gone");
    char *path = lt_path_join(app->config_dir, "settings.conf");
    lt_props p;
    lt_props_init(&p);
    lt_props_set(&p, "recent0", TMP "/mig/Gamma");
    lt_props_set(&p, "recent1", TMP "/mig/./Gamma/");
    lt_props_set(&p, "recent2", TMP "/mig/x/../gone");      /* missing folders stay listed */
    lt_props_set(&p, "recent3", TMP "/mig/../mig/Gamma");
    lt_props_set(&p, "last_project", TMP "/mig/Gamma/.");
    LT_CHECK(lt_props_save(&p, path) == 0);
    lt_props_free(&p);
    lt_app_free(app);
    app = lt_app_new();
    LT_CHECK_INT(app->settings.n_recent, 2);
    LT_CHECK_STR(app->settings.recent[0], abs);
    LT_CHECK_STR(app->settings.recent[1], gone);
    LT_CHECK_STR(app->settings.last_project, abs);
    lt_app_free(app);
    lt_free(dir); lt_free(abs); lt_free(gone); lt_free(path);
}

int main(void)
{
    (void)lt_plat_mkdirs(TMP);
    LT_RUN(args);
    LT_RUN(recent_abs);
    LT_RUN(recent_migrate);
    LT_TEST_MAIN_END
}
