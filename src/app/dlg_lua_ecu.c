/* dlg_lua_ecu.c - rusEFI/FOME ECU Lua script editor (G-121, Tools > ECU Lua Editor, INI target
 * "lua-console").  The script is the INI's "luaScript" string constant: the editor loads it from
 * the ECU (or the tune file offline), highlights Lua, checks the syntax with our own parser,
 * writes it back into the tune ("Apply to tune") and uploads it: write constant, burn, "luareset"
 * over the ECU console.  "Run Snippet" sends the selected code (or the first code line) as a
 * "lua <code>" console command.  Nothing is interpreted here: the script runs on the ECU.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lua_ui.h"
#include "../script/lua.h"

#define LUAE_OUT_CAP 32768

static const char default_script[] =
    "-- ECU Lua script (uploaded to the luaScript constant)\n"
    "print('Hello from ECU Lua')\n"
    "\n"
    "function onTick()\n"
    "  -- Called periodically by the ECU\n"
    "end\n";

typedef struct lua_ecu {
    char *script;
    size_t cap;
    char *out;
    lt_ui_multi ed, om;
    const lt_ini *seen;
    int loaded_online;
    char status[240];
    int status_err;
    lt_lua *chk;
} lua_ecu;

const lt_ini_const *lt_lua_ecu_constant(const lt_app *app)
{
    size_t i;
    if (app->ini == NULL) { return NULL; }
    for (i = 0; i < app->ini->n_constants; i++) {
        const lt_ini_const *c = app->ini->constants[i];
        if (c->type == LT_DT_STRING && lt_stricmp(c->name, "luaScript") == 0) { return c; }
    }
    return NULL;
}

static int is_online(const lt_app *app)
{
    return (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) && app->proto != NULL;
}

static void ecu_free(lt_app *app, void *p)
{
    lua_ecu *s = (lua_ecu *)p;
    LT_UNUSED(app);
    if (s->chk != NULL) { lt_lua_free(s->chk); }
    lt_ui_multi_free(&s->ed);
    lt_ui_multi_free(&s->om);
    lt_free(s->script);
    lt_free(s->out);
}

static lua_ecu *ecu_get(lt_app *app)
{
    lua_ecu *s = (lua_ecu *)lt_ext_state(app, "lua_ecu", sizeof(lua_ecu), ecu_free);
    if (s->out == NULL) {
        s->out = (char *)lt_calloc(LUAE_OUT_CAP, 1);
        lt_ui_multi_init(&s->ed);
        lt_ui_multi_init(&s->om);
    }
    return s;
}

static void note(lua_ecu *s, const char *prefix, const char *text)
{
    char b[600];
    lt_snprintf(b, sizeof(b), "%s%s", prefix, text);
    lt_lua_ui_out_add(s->out, LUAE_OUT_CAP, b, strlen(b));
    s->om.cursor = s->om.anchor = (int)strlen(s->out);
    s->om.follow = 1;
}

static void set_status(lua_ecu *s, int err, const char *fmt, const char *a)
{
    lt_snprintf(s->status, sizeof(s->status), fmt, a);
    s->status_err = err;
}

/* script text of the constant (ECU shadow when online, else the tune) */
static void load_script(lt_app *app, lua_ecu *s, const lt_ini_const *c, int announce)
{
    lt_pagesrc src = lt_app_pages(app);
    size_t cap = c->size_bytes + 1u;
    if (s->script == NULL || s->cap != cap) {
        lt_free(s->script);
        s->script = (char *)lt_calloc(cap, 1);
        s->cap = cap;
    }
    if (lt_ini_const_get_string(app->ini, c, &src, s->script, cap) != 0) { s->script[0] = '\0'; }
    if (s->script[0] == '\0') { lt_strlcpy(s->script, default_script, cap); }
    lt_ui_multi_reset(&s->ed, s->script);
    s->seen = app->ini;
    s->loaded_online = is_online(app);
    if (announce) {
        char b[160];
        lt_snprintf(b, sizeof(b), "Loaded %s from %s", c->name, is_online(app) ? "the ECU" : "the tune file");
        note(s, "<- ", b);
    }
}

static int write_script(lt_app *app, lua_ecu *s, const lt_ini_const *c)
{
    lt_pagesrc src = lt_app_pages(app);
    if (lt_ini_const_set_string(app->ini, c, &src, s->script) != 0) { return -1; }
    lt_app_edited(app);
    return 0;
}

static void check_syntax(lua_ecu *s)
{
    if (s->chk == NULL) { s->chk = lt_lua_new(); }
    if (lt_lua_check(s->chk, s->script, strlen(s->script), "script") == 0) {
        set_status(s, 0, "%s", "Syntax OK");
    } else {
        set_status(s, 1, "%s", lt_lua_error(s->chk));
    }
}

/* the code to try: the selection, else the first line that is not a comment, on one line */
static void snippet(const lua_ecu *s, char *out, size_t cap)
{
    const char *src = s->script;
    size_t lo = (size_t)(s->ed.cursor < s->ed.anchor ? s->ed.cursor : s->ed.anchor);
    size_t hi = (size_t)(s->ed.cursor < s->ed.anchor ? s->ed.anchor : s->ed.cursor);
    size_t i, o = 0;
    int space = 1;
    if (hi <= lo) {
        size_t ls = 0, n = strlen(src);
        lo = hi = 0;
        while (ls < n) {
            size_t le = ls, k = ls;
            while (src[le] != '\0' && src[le] != '\n') { le++; }
            while (k < le && (src[k] == ' ' || src[k] == '\t')) { k++; }
            if (k < le && !(src[k] == '-' && k + 1 < le && src[k + 1] == '-')) { lo = k; hi = le; break; }
            ls = le + 1;
        }
    }
    for (i = lo; i < hi && o + 2 < cap; i++) {
        char ch = src[i];
        if (ch == '\n' || ch == '\r' || ch == '\t') { ch = ' '; }
        if (ch == ' ') {
            if (space) { continue; }
            space = 1;
        } else {
            space = 0;
        }
        out[o++] = ch;
    }
    while (o > 0 && out[o - 1] == ' ') { o--; }
    out[o] = '\0';
}

static void send_console(lt_app *app, lua_ecu *s, const char *cmd)
{
    char *reply = NULL;
    note(s, "> ", cmd);
    if (!is_online(app)) { note(s, "ERROR: ", "not connected"); return; }
    if (lt_proto_console(app->proto, cmd, 1500, &reply) < 0) {
        note(s, "ERROR: ", lt_err_get());
    } else if (reply != NULL) {
        char *p = reply;
        while (*p != '\0') {
            char *e = p;
            char saved;
            while (*e != '\0' && *e != '\n' && *e != '\r') { e++; }
            saved = *e;
            *e = '\0';
            if (e > p) { note(s, "<- ", p); }
            if (saved == '\0') { break; }
            p = e + 1;
        }
    }
    lt_free(reply);
}

static void upload(lt_app *app, lua_ecu *s, const lt_ini_const *c)
{
    char b[160];
    if (!is_online(app)) {
        lt_app_toast(app, 2, "Connect to the ECU to upload Lua scripts.");
        return;
    }
    check_syntax(s);
    if (s->status_err) {
        note(s, "ERROR: ", s->status);
        lt_app_toast(app, 2, "Fix the syntax error before uploading.");
        return;
    }
    lt_snprintf(b, sizeof(b), "Uploading to %s (%lu bytes)", c->name, (unsigned long)strlen(s->script));
    note(s, "> ", b);
    if (write_script(app, s, c) != 0) {
        note(s, "ERROR: ", "the script does not fit the constant");
        return;
    }
    note(s, "<- ", "Script written to ECU RAM");
    if (lt_app_can_burn(app)) {
        note(s, "> ", "burn");
        if (lt_app_burn(app) == 0) { note(s, "<- ", "Burn complete"); }
        else { note(s, "ERROR: ", lt_err_get()); return; }
    } else {
        note(s, "<- ", "Script unchanged: nothing to burn");
    }
    send_console(app, s, "luareset");
    lt_app_toast(app, 1, "Lua script uploaded and VM reset");
}

static void ecu_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    const lt_ini_const *c = lt_lua_ecu_constant(app);
    lua_ecu *s = ecu_get(app);
    int w1 = -1, wb[8], online = is_online(app);
    char hdr[160];
    LT_UNUSED(tab);
    LT_UNUSED(r);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_heading(ui, "ECU Lua Editor");
    if (c == NULL) {
        lt_ui_label_wrap(ui, "This ECU definition does not include a luaScript string constant. Lua scripting "
                             "is available on rusEFI, FOME and epicEFI firmware with a Lua-enabled INI.",
                         th->muted);
        return;
    }
    if (s->seen != app->ini || s->script == NULL || (online && !s->loaded_online)) { load_script(app, s, c, 1); }

    lt_snprintf(hdr, sizeof(hdr), "%s: %s", c->name, online ? "connected, the script is sent to the ECU on upload"
                                                           : "DISCONNECTED, editing the tune file");
    lt_ui_label_ex(ui, hdr, online ? th->success : th->warning, LT_UI_ALIGN_LEFT);
    wb[0] = 0; wb[1] = 0; wb[2] = 0; wb[3] = 0; wb[4] = 0; wb[5] = 0; wb[6] = -1;
    lt_ui_layout_row(ui, 7, wb, 0);
    if (lt_ui_button(ui, online ? "Load from ECU" : "Load from Tune")) { load_script(app, s, c, 1); }
    if (lt_ui_button(ui, "Check Syntax")) { check_syntax(s); }
    if (lt_ui_button(ui, "Apply to Tune")) {
        check_syntax(s);
        if (write_script(app, s, c) == 0) {
            lt_app_toast(app, 1, "Script stored in the tune (Save Tune, or Burn to send it to the ECU)");
            note(s, "<- ", "Script written to the tune");
        } else {
            lt_app_toast(app, 3, "The script does not fit the constant");
        }
    }
    {
        int snip_ok = online;
        if (lt_ui_button_ex(ui, "Run Snippet", snip_ok ? 0 : LT_UI_BTN_DISABLED) && snip_ok) {
            char code[300], cmd[320];
            snippet(s, code, sizeof(code));
            if (code[0] == '\0') {
                lt_app_toast(app, 2, "Select code or add a non-comment line to run.");
            } else {
                lt_snprintf(cmd, sizeof(cmd), "lua %s", code);
                send_console(app, s, cmd);
            }
        }
        lt_ui_tooltip(ui, "Send the selection (or the first code line) as a 'lua' console command");
        if (lt_ui_button_ex(ui, "Upload + Burn + Reset", online ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED) && online) {
            upload(app, s, c);
        }
    }
    if (lt_ui_button(ui, "Clear Output")) {
        s->out[0] = '\0';
        lt_ui_multi_reset(&s->om, "");
    }
    lt_ui_label(ui, "");

    lt_ui_layout_row(ui, 1, &w1, -190);
    (void)lt_ui_text_multi(ui, "lua_ecu_src", &s->ed, s->script, s->cap, LT_UI_MULTI_LINENUMS,
                           lt_lua_ui_classify, NULL);
    {
        char cnt[48];
        int w2[2];
        lt_snprintf(cnt, sizeof(cnt), "%lu / %lu bytes", (unsigned long)strlen(s->script),
                    (unsigned long)(s->cap - 1u));
        w2[0] = -1; w2[1] = 190;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label_ex(ui, s->status, s->status_err ? th->error : th->success, LT_UI_ALIGN_LEFT);
        lt_ui_label_ex(ui, cnt, strlen(s->script) + 1u >= s->cap ? th->warning : th->muted, LT_UI_ALIGN_RIGHT);
    }
    wb[0] = 0; wb[1] = 0; wb[2] = 0; wb[3] = -1;
    lt_ui_layout_row(ui, 4, wb, 0);
    {
        static const char *const quick[3] = { "luareset", "luamemory", "help" };
        int i;
        for (i = 0; i < 3; i++) {
            if (lt_ui_button_ex(ui, quick[i], online ? LT_UI_BTN_FLAT : LT_UI_BTN_DISABLED) && online) {
                send_console(app, s, quick[i]);
            }
        }
    }
    lt_ui_label(ui, "");
    lt_ui_layout_row(ui, 1, &w1, 120);
    (void)lt_ui_text_multi(ui, "lua_ecu_out", &s->om, s->out, LUAE_OUT_CAP, LT_UI_MULTI_READONLY,
                           lt_lua_ui_classify_out, NULL);
}

static void ecu_close(lt_app *app, lt_app_tab *tab)
{
    LT_UNUSED(app);
    LT_UNUSED(tab);
}

static int open_ecu(lt_app *app, const char *target)
{
    LT_UNUSED(target);
    if (lt_lua_ecu_constant(app) == NULL) {
        lt_app_toast(app, 2, "This ECU definition has no luaScript constant.");
        return -1;
    }
    return lt_app_tab_open(app, "lua_ecu", "lua-console", "ECU Lua Editor", 1) != NULL ? 0 : -1;
}

static void act_ecu(lt_app *app) { (void)open_ecu(app, "lua-console"); }

static int pred_ecu(lt_app *app) { return lt_lua_ecu_constant(app) != NULL; }

void lt_lua_ecu_register(lt_app_ext *reg)
{
    lt_app_tab_kind tk;
    tk.kind = "lua_ecu";
    tk.draw = ecu_draw;
    tk.close = ecu_close;
    lt_ext_add_tab_kind(reg, &tk);
    lt_ext_add_target(reg, "lua-console", open_ecu);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "ECU Lua &Editor", NULL, act_ecu, pred_ecu);
}
