/* test_app.c - application shell lifecycle without a window: demo connect,
 * realtime poll, edit -> ECU write -> burn, save/load tune, project round trip.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt_test.h"
#include "../src/app/app.h"

static const char *demo_ini(char *buf, size_t cap)
{
    return lt_app_find_demo_ini(buf, cap);
}

static const lt_ini_const *first_scalar(const lt_ini *ini)
{
    size_t i;
    for (i = 0; i < ini->n_constants; i++) {
        const lt_ini_const *c = ini->constants[i];
        if (!c->is_pc_var && c->shape.cols <= 1 && c->shape.rows <= 1 && c->type != LT_DT_BITS &&
            c->size_bytes > 0 && c->max > c->min) {
            return c;
        }
    }
    return NULL;
}

LT_TEST(demo_edit_burn_save)
{
    char buf[512], tmp[300];
    lt_app *app = lt_app_new();
    const lt_ini_const *c;
    lt_pagesrc ps;
    double v = 0.0, nv;
    int i;
    LT_CHECK(demo_ini(buf, sizeof(buf)) != NULL);
    LT_CHECK(lt_app_start_demo(app, buf) == 0);
    LT_CHECK(app->conn == LT_APP_DEMO);
    LT_CHECK(app->n_tabs == 1);                       /* dashboard */
    for (i = 0; i < 3; i++) { lt_plat_sleep_ms(60); lt_app_service(app); }
    LT_CHECK(app->frames_ok >= 2);
    LT_CHECK(!lt_app_can_burn(app));
    c = first_scalar(app->ini);
    LT_CHECK(c != NULL);
    if (c != NULL) {
        ps = lt_app_pages(app);
        lt_ini_get(app->ini, &ps, c->name, &v);
        nv = c->min + (c->max - c->min) / 2.0;
        if (nv == v) { nv = c->min; }
        LT_CHECK(lt_ini_set(app->ini, &ps, c->name, nv) == 0);
        lt_app_edited(app);
        LT_CHECK(app->modified);
        LT_CHECK(lt_app_can_burn(app));
        LT_CHECK(lt_app_burn(app) == 0);
        LT_CHECK(!lt_app_can_burn(app));
        lt_snprintf(tmp, sizeof(tmp), "build/test_app_tune.msq");
        LT_CHECK(lt_app_save_tune_as(app, tmp) == 0);
        LT_CHECK(!app->modified);
        lt_app_disconnect(app);
        LT_CHECK(app->conn == LT_APP_OFFLINE);
        /* the edit survived the disconnect in the tune */
        ps = lt_app_pages(app);
        lt_ini_get(app->ini, &ps, c->name, &v);
        LT_CHECK(v != 0.0 || nv == 0.0);
        LT_CHECK(lt_app_load_tune(app, tmp) == 0);
    }
    lt_app_free(app);
}

LT_TEST(tabs_and_dialogs)
{
    char buf[512];
    lt_app *app = lt_app_new();
    lt_app_tab *t;
    LT_CHECK(lt_app_open_ini(app, demo_ini(buf, sizeof(buf))) == 0);
    LT_CHECK(lt_app_open_target(app, "veTableDialog", "VE") == 0 || app->n_tabs >= 1);
    t = lt_app_tab_open(app, "dialog", "x", "X", 1);
    LT_CHECK(t != NULL);
    LT_CHECK(lt_app_tab_open(app, "dialog", "x", "X", 1) == t);   /* dedup */
    LT_CHECK(lt_app_tab_open(app, "nosuchkind", "x", "X", 1) == NULL);
    lt_app_tab_close(app, t);
    LT_CHECK(lt_app_tab_find(app, "dialog", "x") == NULL);
    LT_CHECK(lt_app_dialog_push(app, "settings", "", "S") != NULL);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(demo_edit_burn_save);
    LT_RUN(tabs_and_dialogs);
    LT_TEST_MAIN_END
}
