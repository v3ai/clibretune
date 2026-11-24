/* test_projfiles.c - projects and files: recent projects stored as absolute paths (and the
 * migration of older relative entries), the command line (`libretune <folder>`), project folder
 * names that work on every OS, no half-created project when creating fails.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/plat/plat.h"
#include "../src/project/proj_priv.h"

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

static void safe_is(const char *name, const char *want)
{
    char *r = lt_proj_safe_name(name, LT_TRUE);
    LT_CHECK_STR(r, want);
    lt_free(r);
}

LT_TEST(safe_names)
{
    lt_project p;
    char *x;
    safe_is("My Car", "My Car");
    safe_is("a/b:c*d?", "a_b_c_d_");
    safe_is("con", "con_");                     /* Windows device names, any case, on every OS */
    safe_is("CON", "CON_");
    safe_is("Nul", "Nul_");
    safe_is("aux", "aux_");
    safe_is("prn", "prn_");
    safe_is("com1", "com1_");
    safe_is("LPT9", "LPT9_");
    safe_is("COM\xC2\xB9", "COM\xC2\xB9_");     /* COM with a superscript one */
    safe_is("com0", "com0");                    /* not a device */
    safe_is("COM10", "COM10");
    safe_is("console", "console");
    safe_is("con.txt", "con_txt");              /* the dot becomes '_': no extension is left */
    safe_is("con   ", "con_");                  /* trailing spaces go first */
    safe_is("Track day...", "Track day___");    /* dots are mapped, so none can trail */
    safe_is("   ", "_");
    safe_is("\xC3\x9C" "bung", "\xC3\x9C" "bung");      /* UTF-8 kept */
    /* lt_project_create makes the folder from the safe name and keeps the name as typed */
    x = lt_path_join(TMP, "safe/nul_");
    (void)lt_proj_rmtree(x);
    (void)lt_plat_mkdirs(TMP "/safe");
    LT_CHECK(lt_project_create("nul", DEMO, "sig", TMP "/safe", &p) == 0);
    LT_CHECK(lt_plat_is_dir(x));
    LT_CHECK_STR(p.cfg.name, "nul");
    lt_project_close(&p);
    (void)lt_proj_rmtree(x);
    lt_free(x);
}

/* creating fails after the folder was made (disk full while writing project.json): nothing is
 * left behind, not even the projects folder the create made; an existing parent stays */
LT_TEST(create_cleanup)
{
    lt_project p;
    const char *parent = TMP "/half/new_parent";
    (void)lt_proj_rmtree(TMP "/half");
    LT_CHECK(lt_plat_mkdirs(TMP "/half") == 0);
    lt_plat_fault_write_limit(0);
    LT_CHECK(lt_project_create("Half", DEMO, "sig", parent, &p) == LT_EIO);
    lt_plat_fault_write_limit(-1);
    LT_CHECK(lt_err_get()[0] != '\0');
    LT_CHECK(!lt_plat_is_dir(TMP "/half/new_parent/Half"));
    LT_CHECK(!lt_plat_is_dir(parent));
    LT_CHECK(lt_plat_is_dir(TMP "/half"));
    /* an existing parent with other projects in it is kept */
    LT_CHECK(lt_project_create("Kept", DEMO, "sig", parent, &p) == 0);
    lt_project_close(&p);
    lt_plat_fault_write_limit(0);
    LT_CHECK(lt_project_create("Half", DEMO, "sig", parent, &p) == LT_EIO);
    lt_plat_fault_write_limit(-1);
    LT_CHECK(!lt_plat_is_dir(TMP "/half/new_parent/Half"));
    LT_CHECK(lt_app_is_project_dir(TMP "/half/new_parent/Kept"));
    /* and the same name works once the disk has room again */
    LT_CHECK(lt_project_create("Half", DEMO, "sig", parent, &p) == 0);
    lt_project_close(&p);
    LT_CHECK(lt_app_is_project_dir(TMP "/half/new_parent/Half"));
    (void)lt_proj_rmtree(TMP "/half");
}

int main(void)
{
    (void)lt_plat_mkdirs(TMP);
    LT_RUN(args);
    LT_RUN(recent_abs);
    LT_RUN(recent_migrate);
    LT_RUN(safe_names);
    LT_RUN(create_cleanup);
    LT_TEST_MAIN_END
}
