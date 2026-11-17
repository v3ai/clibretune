/* lua_ui.h - shared pieces of the two Lua screens (feat_lua.c: sandbox console, dlg_lua_ecu.c:
 * rusEFI ECU script editor).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_LUA_UI_H
#define LT_LUA_UI_H

#include "feat.h"
#include "../gui/ui.h"

/* editor colours for Lua text (lt_ui_classify_fn) */
void lt_lua_ui_classify(const char *text, size_t len, unsigned char *cls, void *user);
/* output pane colours: "ERROR..." red, "=> value" blue, "-- note" muted */
void lt_lua_ui_classify_out(const char *text, size_t len, unsigned char *cls, void *user);
/* append a line (and a newline) to a bounded output buffer, dropping the oldest lines when full */
void lt_lua_ui_out_add(char *buf, size_t cap, const char *line, size_t len);

/* ECU script editor (dlg_lua_ecu.c) */
void lt_lua_ecu_register(lt_app_ext *reg);
/* the string constant that holds the rusEFI script ("luaScript"), NULL when the INI has none */
const lt_ini_const *lt_lua_ecu_constant(const lt_app *app);

#endif
