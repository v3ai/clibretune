/* test_script_app.c - the Lua screens inside the application shell: registration, the luaScript
 * constant lookup, opening both tabs offscreen, and the output pane helpers.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/app_ext.h"
#include "../src/app/lua_ui.h"

static int has_label(lt_app *app, const char *label)
{
    int i, n = lt_ext_n_menu_items(app, LT_EXT_MENU_TOOLS);
    for (i = 0; i < n; i++) {
        if (strcmp(lt_ext_menu_item_label(app, LT_EXT_MENU_TOOLS, i), label) == 0) { return 1; }
    }
    return 0;
}

LT_TEST(registration)
{
    lt_app *app = lt_app_new();
    LT_CHECK(lt_ext_has_target(app, "lua_console"));
    LT_CHECK(lt_ext_has_target(app, "lua-console"));
    LT_CHECK(has_label(app, "&Lua Console"));
    LT_CHECK(has_label(app, "ECU Lua &Editor"));
    /* no definition open: the ECU editor refuses, the sandbox console opens */
    LT_CHECK(lt_lua_ecu_constant(app) == NULL);
    LT_CHECK(lt_ext_dispatch_target(app, "lua-console") < 0);
    LT_CHECK(lt_ext_dispatch_target(app, "lua_console") == 0);
    LT_CHECK(lt_app_tab_find(app, "lua_console", "lua_console") != NULL);
    LT_CHECK(lt_app_screenshot(app, "build/test_script_app_con.ppm", 1000, 700, 3) == 0);
    lt_app_free(app);
}

LT_TEST(ecu_editor_with_demo_definition)
{
    char buf[512];
    lt_app *app = lt_app_new();
    const lt_ini_const *c;
    LT_CHECK(lt_app_start_demo(app, lt_app_find_demo_ini(buf, sizeof(buf))) == 0);
    c = lt_lua_ecu_constant(app);
    LT_CHECK(c != NULL && c->type == LT_DT_STRING && c->size_bytes == 5000);
    LT_CHECK(lt_ext_dispatch_target(app, "lua-console") == 0);
    LT_CHECK(lt_app_tab_find(app, "lua_ecu", "lua-console") != NULL);
    LT_CHECK(lt_app_screenshot(app, "build/test_script_app_ecu.ppm", 1100, 760, 4) == 0);
    /* the console tab renders next to it */
    LT_CHECK(lt_ext_dispatch_target(app, "lua_console") == 0);
    LT_CHECK(lt_app_screenshot(app, "build/test_script_app_con2.ppm", 1100, 760, 4) == 0);
    lt_app_free(app);
}

LT_TEST(output_helpers)
{
    char out[40];
    unsigned char cls[64];
    const char *t = "> run\nhello\n=> 4\nERROR: x\n-- n\n";
    size_t i;
    out[0] = '\0';
    lt_lua_ui_out_add(out, sizeof(out), "one", 3);
    lt_lua_ui_out_add(out, sizeof(out), "two", 3);
    LT_CHECK_STR(out, "one\ntwo\n");
    for (i = 0; i < 20; i++) { lt_lua_ui_out_add(out, sizeof(out), "line number", 11); }
    LT_CHECK(strlen(out) < sizeof(out));
    LT_CHECK(strstr(out, "line number\n") != NULL);
    LT_CHECK(out[strlen(out) - 1] == '\n');
    /* a line longer than the buffer is cut, never overflows */
    lt_lua_ui_out_add(out, 8, "0123456789", 10);
    LT_CHECK(strlen(out) < 8);
    memset(cls, 9, sizeof(cls));
    lt_lua_ui_classify_out(t, strlen(t), cls, NULL);
    LT_CHECK(cls[0] == 1 && cls[6] == 0 && cls[12] == 5 && cls[18] == 6 && cls[28] == 3);
}

int main(void)
{
    LT_RUN(registration);
    LT_RUN(ecu_editor_with_demo_definition);
    LT_RUN(output_helpers);
    LT_TEST_MAIN_END
}
