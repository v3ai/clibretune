/* test_sync.c - connect/tune sync logic (G-01..G-04): signature matching, INI library offer,
 * project-vs-ECU tune comparison, the on-connect hook choosing a dialog, reconnect scheduling.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/app_ext.h"
#include "../src/app/feat.h"
#include "../src/app/sync_logic.h"

LT_TEST(signature_match)
{
    LT_CHECK(lt_sync_sig_match("speeduino 202305", "speeduino 202305") == LT_SIG_EXACT);
    LT_CHECK(lt_sync_sig_match("Speeduino 202305", "speeduino 202305") == LT_SIG_EXACT);
    LT_CHECK(lt_sync_sig_match("speeduino 202305", "speeduino 202310") == LT_SIG_PARTIAL);
    LT_CHECK(lt_sync_sig_match("speeduino 202305", "speeduino") == LT_SIG_PARTIAL);
    LT_CHECK(lt_sync_sig_match("MS3 1.5", "speeduino 202305") == LT_SIG_NONE);
    LT_CHECK(lt_sync_sig_match("", "speeduino 202305") == LT_SIG_NONE);
    LT_CHECK(lt_sync_sig_match(NULL, "x") == LT_SIG_NONE);
}

LT_TEST(ini_library_offer)
{
    char buf[512], *id = NULL;
    lt_ini_repo r;
    lt_sync_ini_match m[4];
    int n;
    const char *sig;
    lt_ini *ini;
    char other[200];
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    ini = lt_ini_load_file(buf, NULL);
    LT_CHECK(ini != NULL && ini->signature != NULL);
    if (ini == NULL || ini->signature == NULL) { return; }
    sig = ini->signature;
    LT_CHECK(lt_plat_mkdirs("build/test_sync_repo") == 0);
    LT_CHECK(lt_ini_repo_open(&r, "build/test_sync_repo") == 0);
    LT_CHECK(lt_ini_repo_import(&r, buf, &id) == 0);
    lt_free(id);
    n = lt_sync_find_inis(&r, sig, m, 4);
    LT_CHECK(n == 1 && m[0].kind == LT_SIG_EXACT);
    /* a neighbouring build of the same platform is offered as a partial match */
    lt_strlcpy(other, sig, sizeof(other));
    other[strlen(other) - 1] = other[strlen(other) - 1] == '9' ? '8' : '9';
    n = lt_sync_find_inis(&r, other, m, 4);
    LT_CHECK(n == 1 && m[0].kind == LT_SIG_PARTIAL);
    n = lt_sync_find_inis(&r, "something completely different", m, 4);
    LT_CHECK(n == 0);
    lt_ini_repo_close(&r);
    lt_ini_free(ini);
}

LT_TEST(reconnect_schedule)
{
    lt_reconnect rc;
    lt_app_settings s;
    int act;
    memset(&s, 0, sizeof(s));
    s.reconnect_cmd = 1; s.reconnect_fw = 0;
    LT_CHECK(lt_reconnect_arm(&rc, &s, "firmware", "COM3", -1, -1, 1000.0) == -1);   /* disabled */
    LT_CHECK(lt_reconnect_arm(&rc, &s, "controller-command", "COM3", -1, -1, 1000.0) == 0);
    LT_CHECK(rc.max_tries == 4 && rc.next_ms == 3000.0);
    LT_CHECK(lt_reconnect_step(&rc, 2000.0, 0) == 0);        /* still waiting */
    LT_CHECK(lt_reconnect_step(&rc, 3000.0, 0) == 1 && rc.tries == 1);
    LT_CHECK(lt_reconnect_step(&rc, 3100.0, 0) == 0);        /* 400 ms spacing */
    LT_CHECK(lt_reconnect_step(&rc, 3400.0, 0) == 1);
    LT_CHECK(lt_reconnect_step(&rc, 3800.0, 1) == 0 && !rc.armed);   /* connected: stop */
    s.reconnect_fw = 1;
    LT_CHECK(lt_reconnect_arm(&rc, &s, "firmware", "COM3", -1, -1, 0.0) == 0);
    LT_CHECK(rc.max_tries == 10 && rc.next_ms == 8000.0);
    LT_CHECK(lt_reconnect_arm(&rc, NULL, "link-lost", "tcp:h:1", 0, 2, 0.0) == 0);
    LT_CHECK(lt_reconnect_step(&rc, 0.0, 0) == 1);
    LT_CHECK(lt_reconnect_step(&rc, 400.0, 0) == 1);
    act = lt_reconnect_step(&rc, 800.0, 0);
    LT_CHECK(act == 2 && !rc.armed);                          /* gave up */
}

static int n_dialogs_of(lt_app *app, const char *kind)
{
    int i, n = 0;
    for (i = 0; i < app->n_dialogs; i++) {
        if (strcmp(app->dialogs[i]->kind, kind) == 0) { n++; }
    }
    return n;
}

LT_TEST(connect_tune_choice)
{
    char buf[512], tune[300];
    lt_app *app = lt_app_new();
    lt_pagesrc ps;
    const lt_ini_const *c = NULL;
    lt_sync_cmp *cmp;
    size_t i;
    double v = 0.0;
    long nb;
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_plat_setenv("CLIBRETUNE_PROJECTS", "build/test_sync_proj") == 0);
    LT_CHECK(lt_plat_mkdirs("build/test_sync_proj") == 0);
    lt_snprintf(tune, sizeof(tune), "sync%lu", (unsigned long)lt_plat_now_ms());
    LT_CHECK(lt_app_new_project(app, tune, buf) == 0);
    LT_CHECK(app->has_project && app->ini != NULL);
    /* an offline edit that is saved with the project */
    for (i = 0; i < app->ini->n_constants; i++) {
        const lt_ini_const *k = app->ini->constants[i];
        if (!k->is_pc_var && k->shape.cols <= 1 && k->shape.rows <= 1 && k->type != LT_DT_BITS &&
            k->size_bytes > 0 && k->max > k->min) { c = k; break; }
    }
    LT_CHECK(c != NULL);
    if (c == NULL) { lt_app_free(app); return; }
    ps = lt_app_pages(app);
    lt_ini_get(app->ini, &ps, c->name, &v);
    LT_CHECK(lt_ini_set(app->ini, &ps, c->name, v == c->min ? c->max : c->min) == 0);
    lt_app_edited(app);
    LT_CHECK(lt_app_save_tune(app) == 0);

    /* connecting must not overwrite the offline edit, it must ask */
    LT_CHECK(lt_app_connect_demo(app) == 0);
    LT_CHECK(n_dialogs_of(app, "tune_sync") == 1);
    LT_CHECK(n_dialogs_of(app, "sig_mismatch") == 0);
    cmp = lt_sync_compare(app->ini, app->tune, app->mem);
    LT_CHECK(cmp != NULL && !lt_sync_cmp_identical(cmp) && cmp->diff->n >= 1);
    /* "Use clibretune Settings": the ECU image takes the project bytes */
    nb = lt_sync_apply_project(cmp, app->mem);
    LT_CHECK(nb > 0);
    LT_CHECK(lt_proto_write_dirty(app->proto, app->mem) >= 0);
    lt_sync_cmp_free(cmp);
    cmp = lt_sync_compare(app->ini, app->tune, app->mem);
    LT_CHECK(lt_sync_cmp_identical(cmp));
    lt_sync_cmp_free(cmp);
    lt_app_disconnect(app);

    /* a project INI whose signature differs from the ECU's raises the mismatch dialog first */
    app->n_dialogs = 0;
    LT_CHECK(lt_app_connect_demo(app) == 0);
    lt_free(app->ini->signature);
    app->ini->signature = lt_strdup("not the ecu signature");
    app->n_dialogs = 0;
    lt_ext_on_connect(app);
    LT_CHECK(lt_proto_signature_mismatch(app->proto));
    LT_CHECK(n_dialogs_of(app, "sig_mismatch") == 1);
    lt_app_free(app);
}

LT_TEST(use_ecu_replaces_project)
{
    lt_app *app = lt_app_new();
    char buf[512];
    lt_sync_cmp *cmp;
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    /* without a project the ECU simply wins and no dialog appears */
    LT_CHECK(!app->has_project && n_dialogs_of(app, "tune_sync") == 0);
    cmp = lt_sync_compare(app->ini, app->tune, app->mem);
    LT_CHECK(cmp != NULL && lt_sync_cmp_identical(cmp));
    lt_sync_cmp_free(cmp);
    lt_app_free(app);
}

LT_TEST(ignore_keeps_project_tune)
{
    lt_app *app = lt_app_new();
    char buf[512], tmp[512], stamp[32];
    const lt_ini_const *c = NULL;
    lt_pagesrc ps;
    double v, v_after, v_proj, v_ecu;
    size_t i;
    int pass;
    LT_CHECK(lt_app_find_demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    LT_CHECK(lt_plat_setenv("CLIBRETUNE_PROJECTS", "build/test_ignore_proj") == 0);
    LT_CHECK(lt_plat_mkdirs("build/test_ignore_proj") == 0);
    lt_plat_timestamp(stamp, sizeof(stamp));
    for (i = 0; stamp[i] != '\0'; i++) { if (stamp[i] < '0' || stamp[i] > '9') { stamp[i] = '_'; } }
    lt_snprintf(tmp, sizeof(tmp), "ign%s_%lu", stamp, (unsigned long)lt_plat_now_ms());
    LT_CHECK(lt_app_new_project(app, tmp, buf) == 0);
    for (i = 0; i < app->ini->n_constants; i++) {
        const lt_ini_const *k = app->ini->constants[i];
        if (!k->is_pc_var && k->shape.cols <= 1 && k->shape.rows <= 1 && k->type != LT_DT_BITS &&
            k->size_bytes > 0 && k->max > k->min) { c = k; break; }
    }
    LT_CHECK(c != NULL);
    if (c == NULL) { lt_app_free(app); return; }
    for (pass = 0; pass < 2; pass++) {
        ps = lt_app_pages(app);
        lt_ini_get(app->ini, &ps, c->name, &v);
        v_proj = (v == c->min) ? c->max : c->min;
        LT_CHECK(lt_ini_set(app->ini, &ps, c->name, v_proj) == 0);
        lt_app_edited(app);
        LT_CHECK(lt_app_save_tune(app) == 0);
        app->n_dialogs = 0;
        LT_CHECK(lt_app_connect_demo(app) == 0);
        LT_CHECK(n_dialogs_of(app, "tune_sync") == 1);
        ps = lt_app_pages(app);
        lt_ini_get(app->ini, &ps, c->name, &v_ecu);
        if (pass == 0) { LT_CHECK(v_ecu != v_proj); lt_feat_sync_ignore(app); }
        lt_app_disconnect(app);
        ps = lt_app_pages(app);
        lt_ini_get(app->ini, &ps, c->name, &v_after);
        if (pass == 0) { LT_CHECK_NEAR(v_after, v_proj, 1e-6); }     /* Ignore: offline edit survives */
        else { LT_CHECK_NEAR(v_after, v_ecu, 1e-6); }
        LT_CHECK(!app->keep_project_tune);
    }
    lt_app_free(app);
}

/* ---------------------------------------------------------------- dialogs in the terminal UI */
static char g_txt[32000];

/* render app in cell mode (120x40), feeding keys (LT_SHOT_KEYS syntax) first; text in g_txt */
static void tty(lt_app *app, const char *keys)
{
    size_t n = 0;
    char *d;
    lt_plat_setenv("LT_SHOT_TEXT", "build/test_sync_tty.txt");
    lt_plat_setenv("LT_SHOT_KEYS", keys != NULL ? keys : "");
    (void)lt_app_screenshot(app, "", 120, 40, 3);
    lt_plat_setenv("LT_SHOT_TEXT", "");
    lt_plat_setenv("LT_SHOT_KEYS", "");
    d = lt_file_read("build/test_sync_tty.txt", &n);
    g_txt[0] = '\0';
    if (d == NULL) { return; }
    if (n >= sizeof(g_txt)) { n = sizeof(g_txt) - 1; }
    memcpy(g_txt, d, n);
    g_txt[n] = '\0';
    lt_free(d);
}

/* "click:col/row" on the middle of label in g_txt (columns count UTF-8 characters); 0 not found */
static int click_on(const char *label, char *out, size_t cap)
{
    const char *hit = strstr(g_txt, label), *p;
    int row = 0, col = 0;
    if (hit == NULL) { return 0; }
    for (p = g_txt; p < hit; p++) {
        if (*p == '\n') { row++; col = 0; }
        else if (((unsigned char)*p & 0xC0) != 0x80) { col++; }
    }
    lt_snprintf(out, cap, "click:%d/%d", col + (int)strlen(label) / 2, row);
    return 1;
}

/* the connection dialog reached from the start screen (nothing loaded): Demo starts the demo and
 * TCP says what is missing, instead of "No ECU definition loaded" on every button */
LT_TEST(connect_dialog_without_definition)
{
    lt_app *app = lt_app_new();
    char k[64];
    LT_CHECK(lt_app_dialog_push(app, "connect", "", "ECU Connection") != NULL);
    tty(app, NULL);
    LT_CHECK(click_on("TCP connect", k, sizeof(k)));
    tty(app, k);
    LT_CHECK(strstr(g_txt, "No ECU definition yet") != NULL);
    LT_CHECK(n_dialogs_of(app, "connect") == 1 && app->ini == NULL);
    LT_CHECK(click_on("Demo (simulator)", k, sizeof(k)));
    tty(app, k);
    LT_CHECK(app->ini != NULL && app->conn == LT_APP_DEMO);
    LT_CHECK(n_dialogs_of(app, "connect") == 0);
    lt_app_free(app);
}

/* the wizard's "nothing found" note may say "tick Show all ports": the box is there to tick */
LT_TEST(wizard_offers_show_all_ports)
{
    lt_app *app = lt_app_new();
    int i;
    g_txt[0] = '\0';
    LT_CHECK(lt_app_dialog_push(app, "connect_wizard", "", "Connect ECU") != NULL);
    for (i = 0; i < 20 && strstr(g_txt, "Scan again") == NULL; i++) { tty(app, NULL); }
    if (strstr(g_txt, "Show all ports to") != NULL || strstr(g_txt, "ports are hidden") != NULL) {
        LT_CHECK(strstr(g_txt, "Show all ports (also") != NULL);
    }
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(signature_match);
    LT_RUN(ini_library_offer);
    LT_RUN(reconnect_schedule);
    LT_RUN(connect_tune_choice);
    LT_RUN(use_ecu_replaces_project);
    LT_RUN(ignore_keeps_project_tune);
    LT_RUN(connect_dialog_without_definition);
    LT_RUN(wizard_offers_show_all_ports);
    LT_TEST_MAIN_END
}
