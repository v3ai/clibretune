/* feat_lua.c - Lua console (G-120): a sandboxed script runner (Tools > Lua Console) on our own
 * interpreter (src/script).  The editor runs the script, prints show in the output pane and the
 * returned value follows as "=> value".  Like the original console it has no file, network or OS
 * access and no way to change the tune; unlike it, two read-only helpers are provided:
 * channel("name") (live value or nil) and constant("name") (tune value, number or string).
 * Limits: instruction budget, memory budget, call depth, output size (src/script/lua.h).
 * The rusEFI ECU script editor (G-121) lives in dlg_lua_ecu.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lua_ui.h"
#include "../script/lua.h"

#define LUA_SCRIPT_CAP 32768
#define LUA_OUT_CAP 65536
#define LUA_STEPS 3000000ul
#define LUA_MEM (32ul * 1024ul * 1024ul)
#define LUA_FILE "lua_console.lua"

/* ------------------------------------------------------------------ shared helpers */
void lt_lua_ui_classify(const char *text, size_t len, unsigned char *cls, void *user)
{
    LT_UNUSED(user);
    lt_lua_highlight(text, len, cls);
}

void lt_lua_ui_classify_out(const char *text, size_t len, unsigned char *cls, void *user)
{
    size_t ls = 0, i;
    LT_UNUSED(user);
    memset(cls, 0, len);
    for (i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            if (i > ls) {
                int c = 0;
                if (i - ls >= 5 && memcmp(text + ls, "ERROR", 5) == 0) { c = 6; }
                else if (i - ls >= 2 && memcmp(text + ls, "=>", 2) == 0) { c = 5; }
                else if (i - ls >= 2 && memcmp(text + ls, "--", 2) == 0) { c = 3; }
                else if (i - ls >= 1 && text[ls] == '>') { c = 1; }
                if (c != 0) { memset(cls + ls, c, i - ls); }
            }
            ls = i + 1;
        }
    }
}

void lt_lua_ui_out_add(char *buf, size_t cap, const char *line, size_t len)
{
    size_t n = strlen(buf);
    if (len + 2 > cap) { len = cap > 2 ? cap - 2 : 0; }
    if (n + len + 2 > cap) {
        size_t cut = cap / 3;
        while (cut < n && buf[cut] != '\n') { cut++; }
        if (cut < n) { cut++; }
        if (n - cut + len + 2 > cap) { cut = n; }
        memmove(buf, buf + cut, n - cut + 1);
        n -= cut;
    }
    memcpy(buf + n, line, len);
    buf[n + len] = '\n';
    buf[n + len + 1] = '\0';
}

/* ------------------------------------------------------------------ console state */
typedef struct lua_con {
    char *script;                 /* LUA_SCRIPT_CAP */
    char *out;                    /* LUA_OUT_CAP */
    lt_ui_multi ed, om;
    lt_lua *L;
    int started, ex_sel;
    char status[240];
    int status_err;
} lua_con;

static const char *const examples[] = {
    "Examples...",
    "Hello",
    "Loops and tables",
    "Strings and format",
    "Closures",
    "Live channels"
};

static const char *const example_src[] = {
    "",
    "-- print() writes to the output, the returned value is shown after =>\n"
    "print('Hello from Lua')\n"
    "return 2 + 2\n",
    "-- tables, loops and the table library\n"
    "local t = {}\n"
    "for i = 1, 10 do t[#t + 1] = i * i end\n"
    "print(table.concat(t, ', '))\n"
    "local sum = 0\n"
    "for _, v in ipairs(t) do sum = sum + v end\n"
    "return sum, #t\n",
    "-- string library and string.format\n"
    "local s = 'Air fuel ratio 14.7'\n"
    "print(s:upper(), #s, s:sub(1, 3))\n"
    "print(string.format('%5.2f|%-6s|%04d', 3.14159, 'ab', 42))\n"
    "for w in s:gmatch('%w+') do io_ = (io_ or '') .. w:sub(1, 1) end\n"
    "return io_, (s:gsub('%d+', '#'))\n",
    "-- closures and varargs\n"
    "local function counter()\n"
    "  local n = 0\n"
    "  return function() n = n + 1; return n end\n"
    "end\n"
    "local a, b = counter(), counter()\n"
    "a(); a()\n"
    "local function sum(...)\n"
    "  local s = 0\n"
    "  for _, v in ipairs({...}) do s = s + v end\n"
    "  return s\n"
    "end\n"
    "return a(), b(), sum(1, 2, 3, 4)\n",
    "-- read-only access to the connected ECU / open tune\n"
    "local rpm = channel('rpm')\n"
    "print('rpm', rpm)\n"
    "print('reqFuel', constant('reqFuel'))\n"
    "return rpm ~= nil\n"
};

static char *script_path(lt_app *app)
{
    return lt_path_join(app->config_dir != NULL ? app->config_dir : ".", LUA_FILE);
}

static void con_free(lt_app *app, void *p)
{
    lua_con *s = (lua_con *)p;
    LT_UNUSED(app);
    if (s->L != NULL) { lt_lua_free(s->L); }
    lt_ui_multi_free(&s->ed);
    lt_ui_multi_free(&s->om);
    lt_free(s->script);
    lt_free(s->out);
}

static lua_con *con_get(lt_app *app)
{
    lua_con *s = (lua_con *)lt_ext_state(app, "lua_console", sizeof(lua_con), con_free);
    if (!s->started) {
        char *path = script_path(app);
        size_t len = 0;
        char *txt;
        s->started = 1;
        s->script = (char *)lt_calloc(LUA_SCRIPT_CAP, 1);
        s->out = (char *)lt_calloc(LUA_OUT_CAP, 1);
        lt_ui_multi_init(&s->ed);
        lt_ui_multi_init(&s->om);
        txt = lt_file_read(path, &len);
        if (txt != NULL && len > 0 && len < LUA_SCRIPT_CAP) {
            memcpy(s->script, txt, len);
        } else {
            lt_strlcpy(s->script, example_src[1], LUA_SCRIPT_CAP);
        }
        lt_free(txt);
        lt_free(path);
    }
    return s;
}

/* ------------------------------------------------------------------ host functions */
static int h_channel(lt_lua_call *c, void *ud)
{
    lt_app *app = (lt_app *)ud;
    const char *name = lt_lua_argstr(c, 0);
    double v = 0.0;
    if (name == NULL) { lt_lua_fail(c, "bad argument #1 to 'channel' (string expected)"); }
    if (lt_app_channel(app, name, &v)) { lt_lua_pushnum(c, v); } else { lt_lua_pushnil(c); }
    return 1;
}

static int h_constant(lt_lua_call *c, void *ud)
{
    lt_app *app = (lt_app *)ud;
    const char *name = lt_lua_argstr(c, 0);
    const lt_ini_const *k;
    lt_pagesrc src;
    double v = 0.0;
    if (name == NULL) { lt_lua_fail(c, "bad argument #1 to 'constant' (string expected)"); }
    if (app->ini == NULL) { lt_lua_pushnil(c); return 1; }
    k = lt_ini_find_const(app->ini, name);
    if (k == NULL) { lt_lua_pushnil(c); return 1; }
    src = lt_app_pages(app);
    if (k->type == LT_DT_STRING) {
        char buf[512];
        if (lt_ini_const_get_string(app->ini, k, &src, buf, sizeof(buf)) == 0) { lt_lua_pushstr(c, buf); }
        else { lt_lua_pushnil(c); }
    } else if (lt_ini_get(app->ini, &src, name, &v) == 0) {
        lt_lua_pushnum(c, v);
    } else {
        lt_lua_pushnil(c);
    }
    return 1;
}

static void on_print(void *ud, const char *line, size_t len)
{
    lua_con *s = (lua_con *)ud;
    lt_lua_ui_out_add(s->out, LUA_OUT_CAP, line, len);
}

static void out_note(lua_con *s, const char *fmt, const char *a)
{
    char b[400];
    lt_snprintf(b, sizeof(b), fmt, a);
    lt_lua_ui_out_add(s->out, LUA_OUT_CAP, b, strlen(b));
}

static void follow_end(lua_con *s)
{
    s->om.cursor = s->om.anchor = (int)strlen(s->out);
    s->om.follow = 1;
}

static void con_run(lt_app *app, lua_con *s)
{
    double t0, ms;
    int rc;
    char *path;
    if (s->L == NULL) {
        s->L = lt_lua_new();
        lt_lua_register(s->L, "channel", h_channel, app);
        lt_lua_register(s->L, "constant", h_constant, app);
        lt_lua_set_print(s->L, on_print, s);
    }
    lt_lua_set_limits(s->L, LUA_STEPS, LUA_MEM);
    lt_lua_ui_out_add(s->out, LUA_OUT_CAP, "> run", 5);
    t0 = lt_plat_now_ms();
    rc = lt_lua_run(s->L, s->script, strlen(s->script), "console");
    ms = lt_plat_now_ms() - t0;
    if (rc == 0) {
        if (lt_lua_result(s->L)[0] != '\0') { out_note(s, "=> %s", lt_lua_result(s->L)); }
        lt_snprintf(s->status, sizeof(s->status), "OK: %lu steps, %lu KB, %.0f ms",
                    lt_lua_steps(s->L), (unsigned long)(lt_lua_mem_used(s->L) / 1024u), ms);
        s->status_err = 0;
    } else {
        out_note(s, "ERROR: %s", lt_lua_error(s->L));
        lt_snprintf(s->status, sizeof(s->status), "%s", lt_lua_error(s->L));
        s->status_err = 1;
    }
    follow_end(s);
    path = script_path(app);
    (void)lt_file_write(path, s->script, strlen(s->script));
    lt_free(path);
}

/* ------------------------------------------------------------------ the tab */
static void con_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    lua_con *s = con_get(app);
    int w1 = -1, wb[4], run = 0;
    LT_UNUSED(tab);
    LT_UNUSED(r);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_heading(ui, "Lua Console");
    lt_ui_label_wrap(ui, "Sandboxed Lua 5.x subset. print() writes to the output, a returned value is "
                         "shown after =>. No file, network or OS access. Read-only helpers: "
                         "channel('name'), constant('name').", th->muted);
    wb[0] = 150; wb[1] = 130; wb[2] = 170; wb[3] = -1;
    lt_ui_layout_row(ui, 4, wb, 0);
    if (lt_ui_button_ex(ui, "Run  (Ctrl+Enter)", LT_UI_BTN_PRIMARY)) { run = 1; }
    if (lt_ui_button(ui, "Clear Output")) {
        s->out[0] = '\0';
        s->status[0] = '\0';
        s->status_err = 0;
        lt_ui_multi_reset(&s->om, "");
    }
    s->ex_sel = 0;
    if (lt_ui_combo(ui, "lua_examples", &s->ex_sel, examples, (int)LT_COUNTOF(examples)) && s->ex_sel > 0) {
        lt_strlcpy(s->script, example_src[s->ex_sel], LUA_SCRIPT_CAP);
        lt_ui_multi_reset(&s->ed, s->script);
    }
    lt_ui_label_ex(ui, "", th->muted, LT_UI_ALIGN_LEFT);

    lt_ui_layout_row(ui, 1, &w1, -200);
    (void)lt_ui_text_multi(ui, "lua_src", &s->ed, s->script, LUA_SCRIPT_CAP, LT_UI_MULTI_LINENUMS,
                           lt_lua_ui_classify, NULL);
    if (lt_ui_key_pressed(ui, LT_KEY_ENTER, LT_MOD_CTRL) || lt_ui_key_pressed(ui, LT_KEY_F5, 0)) { run = 1; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (s->status[0] != '\0') {
        lt_ui_label_ex(ui, s->status, s->status_err ? th->error : th->success, LT_UI_ALIGN_LEFT);
    } else {
        lt_ui_label_ex(ui, "Limits per run: 3M steps, 32 MB, 150 call levels.", th->muted, LT_UI_ALIGN_LEFT);
    }
    lt_ui_layout_row(ui, 1, &w1, 140);
    (void)lt_ui_text_multi(ui, "lua_out", &s->om, s->out, LUA_OUT_CAP, LT_UI_MULTI_READONLY,
                           lt_lua_ui_classify_out, NULL);
    if (run) { con_run(app, s); }
}

static void con_close(lt_app *app, lt_app_tab *tab)
{
    LT_UNUSED(app);
    LT_UNUSED(tab);
}

static int open_con(lt_app *app, const char *target)
{
    LT_UNUSED(target);
    return lt_app_tab_open(app, "lua_console", "lua_console", "Lua Console", 1) != NULL ? 0 : -1;
}

static void act_con(lt_app *app) { (void)open_con(app, "lua_console"); }

/* ECU script editor entry shared with dlg_lua_ecu.c through the tab kind it registers */
void lt_feat_lua_register(lt_app_ext *reg)
{
    lt_app_tab_kind tk;
    tk.kind = "lua_console";
    tk.draw = con_draw;
    tk.close = con_close;
    lt_ext_add_tab_kind(reg, &tk);
    lt_ext_add_target(reg, "lua_console", open_con);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "&Lua Console", NULL, act_con, NULL);
    lt_lua_ecu_register(reg);
}
