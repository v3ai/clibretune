/* app_shell.c - menus, sidebar, tabs, status bar, welcome screen, main loop.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "app_ext.h"


static const char *const theme_names[] = { "dark", "light", "midnight", "carbon", "synthwave",
                                           "solarized", "nord", "dracula", "highcontrast" };
#define N_THEMES 9

static void clean_label(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (src == NULL) { src = ""; }
    while (*src != '\0' && n + 1 < cap) {
        if (*src == '&' && src[1] != '&' && src[1] != '\0') { src++; continue; }
        dst[n++] = *src++;
    }
    dst[n] = '\0';
}

static lt_bool icontains(const char *hay, const char *needle)
{
    size_t n = strlen(needle), h, i;
    if (hay == NULL) { return LT_FALSE; }
    h = strlen(hay);
    if (n == 0) { return LT_TRUE; }
    for (i = 0; i + n <= h; i++) {
        if (lt_strnicmp(hay + i, needle, n) == 0) { return LT_TRUE; }
    }
    return LT_FALSE;
}

/* ------------------------------------------------------------------ */
/* toolbar                                                             */
static void app_toolbar(lt_app *app)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int has = app->ini != NULL;
    int online = (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO);
    int rec = app->rec != NULL && lt_rec_is_recording(app->rec);
    int burn = lt_app_can_burn(app) ? 1 : 0;
    int cell = lt_ui_cell_mode(ui);
    lt_ui_toolbar_begin(ui);
    lt_ui_push_id(ui, "toolbar");
    if (cell) {
        /* tty: the same actions as a text row */
        if (lt_ui_button_ex(ui, "Open", LT_UI_BTN_FLAT)) {
            lt_app_file_dialog(app, LT_APP_FD_OPEN_PROJECT, LT_UI_FILE_OPEN, NULL, "project.json",
                               "Project|project.json|All files|*");
        }
        if (has && lt_ui_button_ex(ui, "Save", LT_UI_BTN_FLAT)) { lt_app_save_tune(app); }
        if (has && lt_ui_button_ex(ui, burn ? "Burn" : "Burn##x", burn ? LT_UI_BTN_FLAT : LT_UI_BTN_DISABLED) && burn) {
            app->burn_open = 1;
        }
        lt_ui_pop_id(ui);
        lt_ui_toolbar_end(ui);
        return;
    }
    if (lt_ui_icon_button(ui, LT_UI_LUCIDE(LT_ICON_FOLDER_OPEN), "Open project",  0)) {
        lt_app_file_dialog(app, LT_APP_FD_OPEN_PROJECT, LT_UI_FILE_OPEN, NULL, "project.json",
                           "Project|project.json|All files|*");
    }
    if (lt_ui_icon_button(ui, LT_UI_LUCIDE(LT_ICON_SAVE), "Save tune (Ctrl+S)", has ? 0 : LT_UI_BTN_DISABLED) && has) {
        lt_app_save_tune(app);
    }
    if (lt_ui_icon_button(ui, LT_UI_LUCIDE(LT_ICON_FLAME), burn ? "Burn to ECU (Ctrl+B)" : "Nothing to burn",
                          burn ? LT_UI_BTN_DANGER : LT_UI_BTN_DISABLED) && burn) {
        app->burn_open = 1;
    }
    lt_ui_toolbar_separator(ui);
    if (lt_ui_icon_button(ui, LT_UI_LUCIDE(online ? LT_ICON_UNPLUG : LT_ICON_PLUG_ZAP),
                          online ? "Disconnect (Ctrl+Shift+D)" : "Connect to ECU (Ctrl+Shift+C)",
                          has ? (online ? LT_UI_BTN_SELECTED : 0) : LT_UI_BTN_DISABLED) && has) {
        if (online) { lt_app_disconnect(app); } else { lt_app_connect_dialog(app); }
    }
    if (lt_ui_icon_button(ui, LT_UI_LUCIDE(LT_ICON_LAYOUT_DASHBOARD), "Dashboard (Ctrl+D)", has ? 0 : LT_UI_BTN_DISABLED) && has) {
        lt_app_dashboard_tab_open(app);
    }
    if (lt_ui_icon_button(ui, LT_UI_LUCIDE(LT_ICON_WAND_SPARKLES), "AutoTune (Ctrl+Shift+A)", has ? 0 : LT_UI_BTN_DISABLED) && has) {
        lt_app_autotune_open(app);
    }
    lt_ui_toolbar_separator(ui);
    if (lt_ui_icon_button(ui, LT_UI_LUCIDE(rec ? LT_ICON_CIRCLE_STOP : LT_ICON_CIRCLE),
                          rec ? "Stop data logging (Ctrl+L)" : "Start data logging (Ctrl+L)",
                          has ? (rec ? LT_UI_BTN_DANGER : 0) : LT_UI_BTN_DISABLED) && has) {
        lt_app_log_toggle(app);
    }
    lt_ui_toolbar_separator(ui);
    if (lt_ui_icon_button(ui, LT_UI_LUCIDE(LT_ICON_SETTINGS), "Settings (Ctrl+,)", 0)) { lt_app_settings_dialog(app); }
    lt_ui_pop_id(ui);
    lt_ui_toolbar_end(ui);
    LT_UNUSED(t);
}

/* ------------------------------------------------------------------ */
/* sidebar node icons: label keyword rules (same table as stock LibreTune,
 * first match wins). Pattern syntax: "|" separates alternatives, a leading
 * "=" means the whole label, a leading "~" means a whole word. */
typedef struct node_rule { const char *pat; int icon; } node_rule;
static const node_rule node_rules[] = {
    { "=setup", LT_ICON_WRENCH }, { "=fuel", LT_ICON_DROPLET }, { "=ignition", LT_ICON_FLAME },
    { "=cranking", LT_ICON_CIRCLE_PLAY }, { "=idle", LT_ICON_WIND },
    { "=advanced", LT_ICON_SETTINGS_2 }, { "=sensor|=sensors", LT_ICON_RADIO },
    { "=can bus|=can-bus|=canbus", LT_ICON_NETWORK }, { "=controller", LT_ICON_CPU },
    { "=expert/extra|=expert / extra|=expert extra", LT_ICON_PUZZLE },
    { "=help|=about", LT_ICON_CIRCLE_HELP }, { "=diagnostics|=diagnostic", LT_ICON_STETHOSCOPE },
    { "=tools|=tool", LT_ICON_WRENCH }, { "=tables|=table", LT_ICON_TABLE_2 },
    { "=curves|=curve|=graphs|=graph", LT_ICON_TRENDING_UP },
    { "vehicle info", LT_ICON_CAR },
    { "limits and protection|limits & protection|limit and protection", LT_ICON_SHIELD_ALERT },
    { "misfire", LT_ICON_TRIANGLE_ALERT }, { "vr sensor|reluctance", LT_ICON_ACTIVITY },
    { "odd fire", LT_ICON_GIT_BRANCH }, { "firing order", LT_ICON_LIST_ORDERED },
    { "~cam|camshaft", LT_ICON_DISC_3 }, { "~crank|crankshaft", LT_ICON_CIRCLE_DOT },
    { "~trigger", LT_ICON_ZAP }, { "battery|alternator", LT_ICON_BATTERY_CHARGING },
    { "key input|immobilizer|immobiliser", LT_ICON_KEY_ROUND },
    { "sd card|sd-card|sdcard", LT_ICON_MEMORY_STICK },
    { "data log|datalog|logging|~log", LT_ICON_SCROLL_TEXT },
    { "standard output", LT_ICON_PLUG }, { "programmable port|programmable output", LT_ICON_TOGGLE_LEFT },
    { "fans and water|fan & water|fans & water|fan and water|~fan|~fans", LT_ICON_FAN },
    { "water pump|~pump", LT_ICON_DROPLETS }, { "tach", LT_ICON_GAUGE }, { "speedo", LT_ICON_ROUTE },
    { "air condition|~ac|a/c", LT_ICON_SNOWFLAKE }, { "status led|~led|~leds", LT_ICON_LIGHTBULB },
    { "check engine|~mil", LT_ICON_CIRCLE_ALERT }, { "boost", LT_ICON_GAUGE },
    { "injector", LT_ICON_DROPLET }, { "afr|lambda|wideband|~o2|oxygen", LT_ICON_WAVES },
    { "knock", LT_ICON_TRIANGLE_ALERT }, { "spark|dwell|coil", LT_ICON_ZAP },
    { "throttle|~tps", LT_ICON_GAUGE }, { "~map|manifold pressure", LT_ICON_GAUGE },
    { "coolant|~clt", LT_ICON_THERMOMETER }, { "~iat|intake air temp", LT_ICON_THERMOMETER },
    { "~oil", LT_ICON_DROPLET },
    { "warm-up|warmup|warm up|~wue|~ase|cold start", LT_ICON_THERMOMETER },
    { "accel enrich|acceleration enrich|accel. enrich", LT_ICON_TRENDING_UP },
    { "launch control", LT_ICON_ROCKET },
    { "flat shift|shift cut|shift light", LT_ICON_ARROW_UP_DOWN }, { "nitrous", LT_ICON_FLAME },
    { "staging", LT_ICON_LAYERS }, { "~vvt", LT_ICON_DISC_3 }, { "wizard", LT_ICON_SPARKLES },
    { "communication|serial|usb", LT_ICON_USB }, { "board|pinout|wiring", LT_ICON_CIRCUIT_BOARD },
    { "security", LT_ICON_SHIELD_CHECK }, { "~custom", LT_ICON_SLIDERS_HORIZONTAL },
    { "warning|alarm", LT_ICON_TRIANGLE_ALERT }
};

static int word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* does one alternative (len bytes at alt) match label? */
static int alt_match(const char *label, const char *alt, size_t len)
{
    size_t ll = strlen(label), i;
    int exact = 0, word = 0;
    if (len > 0 && alt[0] == '=') { exact = 1; alt++; len--; }
    else if (len > 0 && alt[0] == '~') { word = 1; alt++; len--; }
    if (len == 0) { return 0; }
    if (exact) { return ll == len && lt_strnicmp(label, alt, len) == 0; }
    for (i = 0; i + len <= ll; i++) {
        if (lt_strnicmp(label + i, alt, len) != 0) { continue; }
        if (word && ((i > 0 && word_char(label[i - 1])) || (i + len < ll && word_char(label[i + len])))) { continue; }
        return 1;
    }
    return 0;
}

enum { NODE_FOLDER = 0, NODE_TABLE, NODE_DIALOG, NODE_DASH, NODE_LOG, NODE_HELP };

static int node_icon(const char *label, int type, lt_color *col, const lt_theme *t)
{
    size_t i;
    int icon = 0;
    for (i = 0; i < sizeof(node_rules) / sizeof(node_rules[0]) && icon == 0; i++) {
        const char *p = node_rules[i].pat;
        while (*p != '\0') {
            const char *e = strchr(p, '|');
            size_t len = e != NULL ? (size_t)(e - p) : strlen(p);
            if (alt_match(label, p, len)) { icon = node_rules[i].icon; break; }
            if (e == NULL) { break; }
            p = e + 1;
        }
    }
    switch (type) {
    case NODE_FOLDER: *col = t->warning; if (icon == 0) { icon = LT_ICON_FOLDER; } break;
    case NODE_TABLE: *col = t->info; if (icon == 0) { icon = LT_ICON_TABLE_2; } break;
    case NODE_DASH: *col = t->accent; if (icon == 0) { icon = LT_ICON_GAUGE; } break;
    case NODE_LOG: *col = t->text2; if (icon == 0) { icon = LT_ICON_SCROLL_TEXT; } break;
    case NODE_HELP: *col = t->primary; if (icon == 0) { icon = LT_ICON_CIRCLE_HELP; } break;
    default: *col = t->success; if (icon == 0) { icon = LT_ICON_SETTINGS_2; } break;
    }
    return icon;
}

static int tab_icon(const char *kind)
{
    if (strcmp(kind, "table") == 0) { return LT_ICON_TABLE_2; }
    if (strcmp(kind, "curve") == 0) { return LT_ICON_TRENDING_UP; }
    if (strcmp(kind, "dashboard") == 0) { return LT_ICON_GAUGE; }
    if (strncmp(kind, "datalog", 7) == 0) { return LT_ICON_SCROLL_TEXT; }
    if (strcmp(kind, "autotune") == 0) { return LT_ICON_WAND_SPARKLES; }
    if (strcmp(kind, "special") == 0) { return LT_ICON_ACTIVITY; }
    return LT_ICON_SETTINGS_2;
}

/* ------------------------------------------------------------------ */
/* file dialog                                                         */
const char *lt_app_file_result(lt_app *app, int purpose)
{
    if (app->fd_ready == purpose && purpose != LT_APP_FD_NONE) {
        app->fd_ready = LT_APP_FD_NONE;
        return app->fd.path;
    }
    return NULL;
}

static char *dir_of(const char *path)
{
    char *d = lt_strdup(path);
    char *sl = strrchr(d, '/');
    if (sl == NULL) { sl = strrchr(d, '\\'); }
    if (sl != NULL) { *sl = '\0'; } else { lt_strlcpy(d, ".", 2); }
    return d;
}

static void file_dialog_frame(lt_app *app)
{
    int res;
    const char *title = "Choose file";
    if (!app->fd.open) { return; }
    switch (app->fd_purpose) {
    case LT_APP_FD_LOAD_TUNE: title = "Load tune"; break;
    case LT_APP_FD_SAVE_TUNE: title = "Save tune as"; break;
    case LT_APP_FD_OPEN_INI_DEMO: title = "Choose ECU definition (INI) for demo mode"; break;
    case LT_APP_FD_NEW_PROJECT_INI: title = "Choose ECU definition (INI)"; break;
    case LT_APP_FD_IMPORT_TS: title = "Pick any file inside the TunerStudio project"; break;
    case LT_APP_FD_OPEN_PROJECT: title = "Pick project.json of the project"; break;
    default: break;
    }
    res = lt_ui_file_dialog(app->ui, &app->fd, title);
    if (res != 1) { return; }
    switch (app->fd_purpose) {
    case LT_APP_FD_LOAD_TUNE: lt_app_load_tune(app, app->fd.path); break;
    case LT_APP_FD_SAVE_TUNE: lt_app_save_tune_as(app, app->fd.path); break;
    case LT_APP_FD_OPEN_INI_DEMO: lt_app_start_demo(app, app->fd.path); break;
    case LT_APP_FD_IMPORT_TS:
    case LT_APP_FD_OPEN_PROJECT: {
        char *d = dir_of(app->fd.path);
        if (app->fd_purpose == LT_APP_FD_IMPORT_TS) { lt_app_import_ts(app, d); }
        else { lt_app_open_project(app, d); }
        lt_free(d);
        break;
    }
    default: app->fd_ready = app->fd_purpose; break;
    }
}

/* ------------------------------------------------------------------ */
/* menus                                                               */
static lt_bool env_test(lt_app *app, const char *cond)
{
    if (cond == NULL || cond[0] == '\0') { return LT_TRUE; }
    return lt_expr_cache_test(&app->expr, cond, &app->varenv.env);
}

static void ini_items(lt_app *app, lt_ini_menu_item *const *items, size_t n)
{
    size_t i;
    char lab[160];
    for (i = 0; i < n; i++) {
        const lt_ini_menu_item *it = items[i];
        int flags;
        if (it->kind == LT_MENU_SEPARATOR ||
            (it->target != NULL && strcmp(it->target, "std_separator") == 0)) {
            lt_ui_menu_separator(app->ui);
            continue;
        }
        if (!env_test(app, it->vis_cond)) { continue; }
        flags = env_test(app, it->en_cond) ? 0 : LT_UI_MENU_DISABLED;
        clean_label(lab, sizeof(lab), it->label != NULL ? it->label : it->target);
        if (it->kind == LT_MENU_SUBMENU) {
            if (lt_ui_submenu_begin(app->ui, lab, flags)) {
                ini_items(app, items[i]->items, items[i]->n_items);
                lt_ui_submenu_end(app->ui);
            }
        } else if (lt_ui_menu_item(app->ui, lab, NULL, flags)) {
            if (it->kind == LT_MENU_HELP) { lt_app_toast(app, 0, "Help topic: %s", it->target); }
            else { lt_app_open_target(app, it->target, it->label); }
        }
    }
}

static void menu_file(lt_app *app)
{
    lt_ui *ui = app->ui;
    int i, online = (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO);
    int has = app->ini != NULL;
    char lab[600];
    if (lt_ui_menu_item(ui, "&New Project...", "Ctrl+N", 0)) { lt_app_new_project_dialog(app); }
    if (lt_ui_menu_item(ui, "&Open Project...", NULL, 0)) {
        lt_app_file_dialog(app, LT_APP_FD_OPEN_PROJECT, LT_UI_FILE_OPEN,
                           app->settings.last_project[0] ? NULL : NULL, "project.json",
                           "Project|project.json|All files|*");
    }
    if (lt_ui_submenu_begin(ui, "Open &Recent", app->settings.n_recent ? 0 : LT_UI_MENU_DISABLED)) {
        for (i = 0; i < app->settings.n_recent; i++) {
            lt_snprintf(lab, sizeof(lab), "%s", lt_path_base(app->settings.recent[i]));
            lt_ui_push_id_int(ui, i);
            if (lt_ui_menu_item(ui, lab, NULL, 0)) { lt_app_open_project(app, app->settings.recent[i]); }
            lt_ui_pop_id(ui);
        }
        lt_ui_submenu_end(ui);
    }
    if (lt_ui_menu_item(ui, "&Import TunerStudio Project...", NULL, 0)) {
        lt_app_file_dialog(app, LT_APP_FD_IMPORT_TS, LT_UI_FILE_OPEN, NULL, "", "All files|*");
    }
    if (lt_ui_menu_item(ui, "Demo mode (simulated ECU)", NULL, 0)) { lt_app_start_demo(app, NULL); }
    if (has) {
        if (lt_ui_menu_item(ui, "&Close Project", NULL, 0)) {
            if (app->modified) { app->close_proj_open = 1; } else { lt_app_close_project(app); }
        }
    }
    lt_ui_menu_separator(ui);
    if (has) {
        if (lt_ui_menu_item(ui, "Connect ECU...", "Ctrl+Shift+C", 0)) { lt_app_connect_dialog(app); }
        if (lt_ui_menu_item(ui, "Disconnect", "Ctrl+Shift+D", online ? 0 : LT_UI_MENU_DISABLED)) {
            lt_app_disconnect(app);
        }
        lt_ui_menu_separator(ui);
        if (lt_ui_menu_item(ui, "&Load Tune...", "Ctrl+O", 0)) {
            lt_app_file_dialog(app, LT_APP_FD_LOAD_TUNE, LT_UI_FILE_OPEN, NULL, "",
                               "Tune files|*.msq;*.xml|All files|*");
        }
        if (lt_ui_menu_item(ui, "&Save Tune", "Ctrl+S", 0)) { lt_app_save_tune(app); }
        if (lt_ui_menu_item(ui, "Save Tune &As...", "Ctrl+Shift+S", 0)) {
            lt_app_file_dialog(app, LT_APP_FD_SAVE_TUNE, LT_UI_FILE_SAVE, NULL, "tune.msq",
                               "Tune files|*.msq;*.xml|All files|*");
        }
        lt_ui_menu_separator(ui);
        if (lt_ui_menu_item(ui, "Create Restore Point", NULL, app->has_project ? 0 : LT_UI_MENU_DISABLED)) {
            char *p = NULL;
            if (app->mem != NULL) { lt_tune_from_mem(app->tune, app->mem); }
            if (lt_restore_create(&app->project, &p) == 0) {
                lt_app_toast(app, 1, "Restore point created");
                lt_free(p);
            } else { lt_app_toast(app, 3, "Restore point failed: %s", lt_err_get()); }
        }
        if (lt_ui_menu_item(ui, "Restore Points...", NULL, app->has_project ? 0 : LT_UI_MENU_DISABLED)) {
            lt_app_restore_points_dialog(app);
        }
        lt_ui_menu_separator(ui);
        if (lt_ui_menu_item(ui, "&Burn to ECU", "Ctrl+B", lt_app_can_burn(app) ? 0 : LT_UI_MENU_DISABLED)) {
            app->burn_open = 1;
        }
        lt_ui_menu_separator(ui);
    }
    if (lt_ext_menu_draw(app, LT_EXT_MENU_FILE, 0) > 0) { lt_ui_menu_separator(ui); }
    if (lt_ui_menu_item(ui, "E&xit", "Ctrl+Q", 0)) { lt_app_request_quit(app); }
}

static void menu_view(lt_app *app)
{
    lt_ui *ui = app->ui;
    int i;
    if (app->ini != NULL) {
        if (lt_ui_menu_item(ui, "&Dashboard", "Ctrl+D", 0)) { lt_app_dashboard_tab_open(app); }
    }
    if (lt_ui_menu_item(ui, "Toggle &Sidebar", "F9", app->settings.sidebar_on ? LT_UI_MENU_CHECKED : 0)) {
        app->settings.sidebar_on = !app->settings.sidebar_on;
        lt_app_settings_save(app);
    }
    if (lt_ui_menu_item(ui, "Show ECU menus in menu bar", NULL,
                        app->settings.show_ecu_menus ? LT_UI_MENU_CHECKED : 0)) {
        app->settings.show_ecu_menus = !app->settings.show_ecu_menus;
        lt_app_settings_save(app);
    }
    lt_ui_menu_separator(ui);
    if (lt_ui_submenu_begin(ui, "&Theme", 0)) {
        for (i = 0; i < N_THEMES; i++) {
            int on = strcmp(app->settings.theme, theme_names[i]) == 0;
            if (lt_ui_menu_item(ui, theme_names[i], NULL, on ? LT_UI_MENU_CHECKED : 0)) {
                lt_app_apply_theme(app, theme_names[i]);
                lt_app_settings_save(app);
            }
        }
        lt_ui_submenu_end(ui);
    }
    if (lt_ui_submenu_begin(ui, "&Units", 0)) {
        if (lt_ui_menu_item(ui, "Metric", NULL, app->settings.metric ? LT_UI_MENU_CHECKED : 0)) {
            app->settings.metric = 1; lt_app_settings_save(app);
        }
        if (lt_ui_menu_item(ui, "Imperial", NULL, app->settings.metric ? 0 : LT_UI_MENU_CHECKED)) {
            app->settings.metric = 0; lt_app_settings_save(app);
        }
        lt_ui_submenu_end(ui);
    }
    lt_ext_menu_draw(app, LT_EXT_MENU_VIEW, 1);
}

static void menu_tools(lt_app *app)
{
    lt_ui *ui = app->ui;
    const lt_ini *ini = app->ini;
    int rec = app->rec != NULL && lt_rec_is_recording(app->rec);
    if (lt_ui_menu_item(ui, "&AutoTune", "Ctrl+Shift+A", 0)) { lt_app_autotune_open(app); }
    if (ini->n_dl_entries > 0 || ini->n_channels > 0) {
        if (lt_ui_menu_item(ui, rec ? "Stop Data &Logging" : "Start Data &Logging", "Ctrl+L", 0)) {
            lt_app_log_toggle(app);
        }
        if (lt_ui_menu_item(ui, "Datalog &Viewer...", NULL, 0)) { lt_app_datalog_open(app, 1); }
        if (lt_ui_menu_item(ui, "&Output Channel Status", NULL, 0)) { lt_app_special_open(app, "och-status"); }
    }
    if (ini->n_loggers > 0) {
        if (lt_ui_menu_item(ui, "Tooth Logger", NULL, 0)) { lt_app_special_open(app, "tooth-logger"); }
        if (lt_ui_menu_item(ui, "Composite Logger", NULL, 0)) { lt_app_special_open(app, "composite-logger"); }
    }
    lt_ui_menu_separator(ui);
    if (lt_ui_menu_item(ui, "ECU &Connection...", "Ctrl+Shift+C", 0)) { lt_app_connect_dialog(app); }
    if (lt_ui_menu_item(ui, "&Settings...", "Ctrl+,", 0)) { lt_app_settings_dialog(app); }
    lt_ext_menu_draw(app, LT_EXT_MENU_TOOLS, 1);
}

static void menu_ini(lt_app *app)
{
    lt_ui *ui = app->ui;
    size_t i, total = 0;
    int group;
    char lab[160];
    const lt_ini *ini = app->ini;
    if (ini == NULL || !app->settings.show_ecu_menus || ini->n_menus == 0) { return; }
    for (i = 0; i < ini->n_menus; i++) {
        total += (size_t)lt_ui_text_width(ui, ini->menus[i].title) + (size_t)lt_ui_px(ui, 24);
    }
    group = (int)total > lt_ui_surface(ui).w - lt_ui_px(ui, 520);
    if (group) {
        if (lt_ui_menu_begin(ui, "&Tuning")) {
            for (i = 0; i < ini->n_menus; i++) {
                clean_label(lab, sizeof(lab), ini->menus[i].title);
                if (lt_ui_submenu_begin(ui, lab, 0)) {
                    ini_items(app, ini->menus[i].items, ini->menus[i].n_items);
                    lt_ui_submenu_end(ui);
                }
            }
            lt_ui_menu_end(ui);
        }
        return;
    }
    for (i = 0; i < ini->n_menus; i++) {
        if (lt_ui_menu_begin(ui, ini->menus[i].title)) {
            ini_items(app, ini->menus[i].items, ini->menus[i].n_items);
            lt_ui_menu_end(ui);
        }
    }
}

static void menubar(lt_app *app)
{
    lt_ui *ui = app->ui;
    if (!lt_ui_menubar_begin(ui)) { return; }
    if (lt_ui_menu_begin(ui, "&File")) { menu_file(app); lt_ui_menu_end(ui); }
    if (app->ini != NULL && lt_ui_menu_begin(ui, "&Edit")) {
        if (lt_ui_menu_item(ui, "Undo", "Ctrl+Z", 0) || lt_ui_menu_item(ui, "Redo", "Ctrl+Y", 0) ||
            lt_ui_menu_item(ui, "Cut", "Ctrl+X", 0) || lt_ui_menu_item(ui, "Copy", "Ctrl+C", 0) ||
            lt_ui_menu_item(ui, "Paste", "Ctrl+V", 0)) {
            lt_app_toast(app, 0, "Use the table-specific controls for undo/redo/copy/paste");
        }
        lt_ui_menu_separator(ui);
        if (lt_ui_menu_item(ui, "Reset Tune to &Defaults", NULL, 0)) { lt_app_reset_defaults(app); }
        lt_ui_menu_end(ui);
    }
    if (lt_ui_menu_begin(ui, "&View")) { menu_view(app); lt_ui_menu_end(ui); }
    if (app->ini != NULL && lt_ui_menu_begin(ui, "&Tools")) { menu_tools(app); lt_ui_menu_end(ui); }
    if (app->ini != NULL && app->settings.show_ecu_menus && app->ini->n_menus > 0) {
        lt_ui_menubar_separator(ui);
        menu_ini(app);
    }
    lt_ui_menubar_separator(ui);
    if (lt_ui_menu_begin(ui, "&Help")) {
        lt_ext_menu_draw(app, LT_EXT_MENU_HELP, 0);
        lt_ui_menu_end(ui);
    }
    lt_ui_menubar_end(ui);
}

/* ------------------------------------------------------------------ */
/* sidebar                                                             */
static lt_bool item_matches(const lt_ini_menu_item *it, const char *q)
{
    return icontains(it->label, q) || icontains(it->target, q);
}

static lt_bool subtree_matches(const lt_ini_menu_item *it, const char *q)
{
    size_t i;
    if (item_matches(it, q)) { return LT_TRUE; }
    for (i = 0; i < it->n_items; i++) {
        if (subtree_matches(it->items[i], q)) { return LT_TRUE; }
    }
    return LT_FALSE;
}

static void sidebar_items(lt_app *app, lt_ini_menu_item *const *items, size_t n, const char *q,
                          lt_bool parent_match, const char *active, int *counter)
{
    size_t i;
    char lab[160];
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    for (i = 0; i < n; i++) {
        const lt_ini_menu_item *it = items[i];
        int r, ico;
        lt_color ic;
        if (it->kind == LT_MENU_SEPARATOR ||
            (it->target != NULL && strcmp(it->target, "std_separator") == 0)) { continue; }
        if (!env_test(app, it->vis_cond)) { continue; }
        clean_label(lab, sizeof(lab), it->label != NULL ? it->label : it->target);
        lt_ui_push_id_int(ui, (*counter)++);
        if (it->kind == LT_MENU_SUBMENU) {
            lt_bool self = item_matches(it, q);
            if (q[0] != '\0' && !parent_match && !subtree_matches(it, q)) { lt_ui_pop_id(ui); continue; }
            ico = node_icon(lab, NODE_FOLDER, &ic, th);
            r = lt_ui_tree_node_ex(ui, lab, LT_UI_LUCIDE(ico), ic, q[0] ? LT_UI_TREE_FORCE_OPEN : 0);
            if (r & LT_UI_TREE_OPEN) {
                sidebar_items(app, items[i]->items, items[i]->n_items, q, parent_match || self,
                              active, counter);
                lt_ui_tree_end(ui);
            }
        } else {
            int fl = LT_UI_TREE_LEAF, type = NODE_DIALOG;
            int en = env_test(app, it->en_cond);
            if (q[0] != '\0' && !parent_match && !item_matches(it, q)) { lt_ui_pop_id(ui); continue; }
            if (it->target != NULL && active != NULL && strcmp(it->target, active) == 0) {
                fl |= LT_UI_TREE_SELECTED;
            }
            if (!en) { fl |= LT_UI_TREE_DISABLED; }
            if (it->kind == LT_MENU_HELP) { type = NODE_HELP; }
            else if (lt_ini_find_table(app->ini, it->target ? it->target : "")) { type = NODE_TABLE; }
            else if (it->target != NULL && (strstr(it->target, "realtime") != NULL || strstr(it->target, "dash") != NULL)) {
                type = NODE_DASH;
            }
            ico = node_icon(lab, type, &ic, th);
            r = lt_ui_tree_node_ex(ui, lab, LT_UI_LUCIDE(ico), ic, fl);
            if (r & LT_UI_TREE_CLICKED) { lt_app_open_target(app, it->target, it->label); }
        }
        lt_ui_pop_id(ui);
    }
}

static void sidebar(lt_app *app, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    const lt_ini *ini = app->ini;
    int w1 = -1, counter = 0;
    int cell = lt_ui_cell_mode(ui);
    int hh = t->tab_h, sh = cell ? 16 : lt_ui_px(ui, 30);
    size_t i;
    char lab[160], title[160];
    const char *active = NULL;
    lt_rect hdr, srow, sin, tree;
    lt_app_tab *at = lt_app_tab_active(app);
    if (at != NULL) { active = at->target; }
    lt_ui_region_begin(ui, r, t->sidebar);
    /* header: project name in small caps */
    hdr = lt_rect_make(r.x, r.y, r.w, hh);
    lt_ui_draw_fill(ui, hdr, t->bg_toolbar);
    if (!cell) { lt_ui_draw_hline(ui, r.x, r.x + r.w - 1, hdr.y + hdr.h - 1, t->border_subtle); }
    {
        const char *nm = ini == NULL ? "Project"
                       : (app->has_project ? app->project.cfg.name
                          : (ini->signature != NULL ? ini->signature : "Definition"));
        size_t k;
        lt_strlcpy(title, nm, sizeof(title));
        if (!cell) { for (k = 0; title[k] != '\0'; k++) { if (title[k] >= 'a' && title[k] <= 'z') { title[k] = (char)(title[k] - 32); } } }
        lt_ui_draw_text_in_f(ui, lt_rect_make(hdr.x + t->padding, hdr.y, hdr.w - 2 * t->padding, hdr.h), title,
                             t->text2, 0, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_SM);
    }
    /* search row */
    srow = lt_rect_make(r.x, r.y + hh, r.w, sh);
    lt_ui_draw_fill(ui, srow, t->bg_toolbar);
    if (!cell) { lt_ui_draw_hline(ui, r.x, r.x + r.w - 1, srow.y + srow.h - 1, t->border_subtle); }
    {
        int ix = srow.x + (cell ? 0 : lt_ui_px(ui, 6)), bw = cell ? 16 : lt_ui_px(ui, 22);
        int gap = cell ? 0 : lt_ui_px(ui, 4);
        lt_rect ib;
        lt_ui_draw_icon(ui, lt_rect_make(ix, srow.y, cell ? 16 : lt_ui_px(ui, 14), srow.h),
                        cell ? LT_UI_ICON_SEARCH : LT_UI_LUCIDE(LT_ICON_SEARCH), t->muted);
        ix += cell ? 16 : lt_ui_px(ui, 14) + gap;
        sin = lt_rect_make(ix, srow.y + (srow.h - (cell ? lt_ui_px(ui, 16) : bw)) / 2, srow.x + srow.w - ix - bw - 2 * gap - (cell ? 0 : lt_ui_px(ui, 2)),
                           cell ? 16 : bw);
        lt_ui_region_begin(ui, sin, 0);
        lt_ui_layout_row_px(ui, 1, &w1, sin.h);
        lt_ui_layout_gap(ui, 0, 0);
        lt_ui_text_style(ui, (int)LT_FONT_SANS, LT_UI_FS_SM);
        lt_ui_text(ui, "search", app->search, sizeof(app->search), 0);
        lt_ui_text_style_reset(ui);
        if (ini != NULL && lt_ui_key_pressed(ui, 'k', LT_MOD_CTRL)) { lt_ui_set_focus(ui, lt_ui_last_id(ui)); }
        if (app->search[0] == '\0' && !lt_ui_text_focused(ui)) {
            lt_rect sr = lt_ui_last_rect(ui);
            lt_ui_draw_text_in_f(ui, lt_rect_make(sr.x + t->pad_x, sr.y, sr.w - 2 * t->pad_x, sr.h),
                                 "Search... (Ctrl+K)", t->muted, 0, (int)LT_FONT_SANS, LT_UI_FS_SM);
        } else if (app->search[0] != '\0') {
            lt_rect cr = lt_rect_make(sin.x + sin.w - lt_ui_px(ui, 20), sin.y + 2, lt_ui_px(ui, 18), sin.h - 4);
            lt_ui_region_begin(ui, cr, 0);
            lt_ui_layout_row_px(ui, 1, &w1, cr.h);
            lt_ui_layout_gap(ui, 0, 0);
            if (lt_ui_icon_button(ui, cell ? LT_UI_ICON_CLOSE : LT_UI_LUCIDE(LT_ICON_X), "Clear search", 0)) { app->search[0] = '\0'; }
            lt_ui_region_end(ui);
        }
        lt_ui_region_end(ui);
        ib = lt_rect_make(sin.x + sin.w + gap, sin.y, bw, sin.h);
        lt_ui_region_begin(ui, ib, 0);
        lt_ui_layout_row_px(ui, 1, &w1, ib.h);
        lt_ui_layout_gap(ui, 0, 0);
        lt_ui_push_id(ui, "expand");
        if (lt_ui_icon_button(ui, cell ? LT_UI_ICON_PLUS : LT_UI_LUCIDE(LT_ICON_CHEVRONS_UP_DOWN),
                              app->tree_expanded ? "Collapse all" : "Expand all", 0)) {
            app->tree_expanded = !app->tree_expanded;
            lt_ui_tree_set_all(ui, app->tree_expanded);
        }
        lt_ui_pop_id(ui);
        lt_ui_region_end(ui);
    }
    /* tree */
    tree = lt_rect_make(r.x, srow.y + srow.h, r.w, r.h - hh - sh);
    if (ini == NULL) { lt_ui_region_end(ui); return; }
    lt_ui_region_begin(ui, tree, 0);
    lt_ui_layout_row(ui, 1, &w1, -1);
    if (lt_ui_scroll_begin(ui, "tree")) {
        lt_ui_layout_gap(ui, 0, 0);
        lt_ui_layout_row_px(ui, 1, &w1, t->tree_row_h);
        for (i = 0; i < ini->n_menus; i++) {
            const lt_ini_menu *m = &ini->menus[i];
            size_t k;
            lt_bool any = LT_FALSE;
            lt_color ic;
            int ico;
            if (app->search[0] != '\0') {
                if (!icontains(m->title, app->search)) {
                    for (k = 0; k < m->n_items && !any; k++) { any = subtree_matches(m->items[k], app->search); }
                    if (!any) { continue; }
                }
            }
            clean_label(lab, sizeof(lab), m->title);
            lt_ui_push_id_int(ui, 100000 + (int)i);
            ico = node_icon(lab, NODE_FOLDER, &ic, t);
            if (lt_ui_tree_node_ex(ui, lab, LT_UI_LUCIDE(ico), ic,
                                   app->search[0] ? LT_UI_TREE_FORCE_OPEN : (i < 2 ? LT_UI_TREE_DEFAULT_OPEN : 0))
                & LT_UI_TREE_OPEN) {
                sidebar_items(app, m->items, m->n_items, app->search,
                              app->search[0] != '\0' && icontains(m->title, app->search), active, &counter);
                lt_ui_tree_end(ui);
            }
            lt_ui_pop_id(ui);
        }
        lt_ui_scroll_end(ui);
    }
    lt_ui_region_end(ui);
    lt_ui_region_end(ui);
}

/* ------------------------------------------------------------------ */
/* status bar                                                          */
static void statusbar_cell(lt_app *app)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w[7];
    char buf[200];
    lt_color cc;
    lt_rect dr;
    lt_bool burn = lt_app_can_burn(app);
    lt_ui_statusbar_begin(ui);
    w[0] = 8 * 36; w[1] = -1; w[2] = 8 * 18; w[3] = 8 * 11; w[4] = 8 * 9; w[5] = 8 * 10; w[6] = 8 * 7;
    lt_ui_layout_row(ui, 7, w, 0);
    lt_ui_layout_indent(ui, 0);
    switch (app->conn) {
    case LT_APP_ONLINE: cc = t->success; break;
    case LT_APP_DEMO: cc = t->accent; break;
    case LT_APP_CONNECTING: cc = t->warning; break;
    default: cc = t->error; break;
    }
    if (app->conn == LT_APP_OFFLINE) { lt_snprintf(buf, sizeof(buf), "  Disconnected"); }
    else { lt_snprintf(buf, sizeof(buf), "  %s: %s", lt_app_conn_label(app), app->signature); }
    lt_ui_label_ex(ui, buf, cc, LT_UI_ALIGN_LEFT);
    dr = lt_ui_last_rect(ui);
    lt_ui_draw_icon(ui, lt_rect_make(dr.x - lt_ui_px(ui, 2), dr.y, lt_ui_px(ui, 16), dr.h), LT_UI_ICON_DOT, cc);
    if (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) {
        double rpm = 0.0;
        if (lt_app_channel(app, "RPMValue", &rpm) || lt_app_channel(app, "rpm", &rpm)) {
            lt_snprintf(buf, sizeof(buf), "RPM %.0f  %s", rpm, app->status);
        } else { lt_snprintf(buf, sizeof(buf), "%s", app->status); }
    } else { lt_snprintf(buf, sizeof(buf), "%s", app->status); }
    lt_ui_label_ex(ui, buf, t->text2, LT_UI_ALIGN_LEFT);
    lt_snprintf(buf, sizeof(buf), "%s%s", app->has_project ? app->project.cfg.name : "(no project)",
                app->modified ? " *" : "");
    lt_ui_label_ex(ui, buf, app->modified ? t->warning : t->text2, LT_UI_ALIGN_RIGHT);
    if (lt_ui_button_ex(ui, "Burn", burn ? LT_UI_BTN_ACCENT : LT_UI_BTN_DISABLED) && burn) {
        app->burn_open = 1;
    }
    if (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) {
        lt_snprintf(buf, sizeof(buf), "%.0f Hz", app->data_rate);
    } else { buf[0] = '\0'; }
    lt_ui_label_ex(ui, buf, t->muted, LT_UI_ALIGN_RIGHT);
    lt_ui_label_ex(ui, app->settings.metric ? "Metric" : "Imperial", t->muted, LT_UI_ALIGN_RIGHT);
    lt_ui_label_ex(ui, "v" LT_APP_VERSION, t->muted, LT_UI_ALIGN_RIGHT);
    lt_ui_statusbar_end(ui);
}

static void statusbar(lt_app *app)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w[7];
    char conn[200], mid[200], proj[200], rate[48];
    const char *units = app->settings.metric ? "Metric" : "Imperial";
    const char *logt;
    lt_color cc;
    lt_rect r;
    int rec = app->rec != NULL && lt_rec_is_recording(app->rec);
    if (lt_ui_cell_mode(ui)) { statusbar_cell(app); return; }
    lt_ui_text_style(ui, (int)LT_FONT_SANS, LT_UI_FS_SM);
    switch (app->conn) {
    case LT_APP_ONLINE: cc = t->success; break;
    case LT_APP_DEMO: cc = t->accent; break;
    case LT_APP_CONNECTING: cc = t->warning; break;
    default: cc = t->error; break;
    }
    if (app->conn == LT_APP_OFFLINE) { lt_snprintf(conn, sizeof(conn), "Disconnected"); }
    else { lt_snprintf(conn, sizeof(conn), "%s: %s", lt_app_conn_label(app), app->signature); }
    mid[0] = '\0';
    if (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) {
        double rpm = 0.0;
        if (lt_app_channel(app, "RPMValue", &rpm) || lt_app_channel(app, "rpm", &rpm)) {
            lt_snprintf(mid, sizeof(mid), "RPM %.0f   %s", rpm, app->status);
        }
    }
    if (mid[0] == '\0') { lt_snprintf(mid, sizeof(mid), "%s", app->status); }
    lt_snprintf(proj, sizeof(proj), "%s%s", app->has_project ? app->project.cfg.name : "",
                app->modified ? " (modified)" : "");
    if (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) { lt_snprintf(rate, sizeof(rate), "%.0f Hz", app->data_rate); }
    else { rate[0] = '\0'; }
    logt = rec ? "Logging" : "Not Logging";
    lt_ui_statusbar_begin(ui);
    w[0] = LT_MIN(lt_ui_text_width(ui, conn) + lt_ui_px(ui, 24), lt_ui_surface(ui).w / 2);
    w[1] = -1;
    w[2] = proj[0] ? lt_ui_text_width(ui, proj) + lt_ui_px(ui, 16) : 1;
    w[3] = rate[0] ? lt_ui_text_width(ui, rate) + lt_ui_px(ui, 16) : 1;
    w[4] = lt_ui_text_width(ui, units) + lt_ui_px(ui, 16);
    w[5] = lt_ui_px(ui, 20) + lt_ui_text_width(ui, logt) + lt_ui_px(ui, 8);
    w[6] = lt_ui_text_width(ui, "v" LT_APP_VERSION) + lt_ui_px(ui, 8);
    lt_ui_layout_row_px(ui, 7, w, 0);
    lt_ui_layout_indent(ui, 0);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_circle_fill(ui, r.x + (double)lt_ui_px(ui, 6), r.y + r.h / 2.0, 3.5 * (double)lt_ui_scale_pct(ui) / 100.0, cc);
    lt_ui_draw_text_in(ui, lt_rect_make(r.x + lt_ui_px(ui, 16), r.y, r.w - lt_ui_px(ui, 16), r.h), conn, t->text2, 0);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_text_in(ui, r, mid, t->muted, 1);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_text_in(ui, r, proj, app->modified ? t->warning : t->text2, 2);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_text_in(ui, r, rate, t->muted, 2);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_text_in(ui, r, units, t->muted, 2);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_lucide(ui, lt_rect_make(r.x, r.y, lt_ui_px(ui, 16), r.h), rec ? LT_ICON_CIRCLE_DOT : LT_ICON_CIRCLE, 12, 2.0,
                      rec ? t->error : t->muted);
    lt_ui_draw_text_in(ui, lt_rect_make(r.x + lt_ui_px(ui, 20), r.y, r.w - lt_ui_px(ui, 20), r.h), logt, rec ? t->error : t->muted, 0);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_text_in_f(ui, r, "v" LT_APP_VERSION, t->muted, 2, (int)LT_FONT_MONO, LT_UI_FS_XS);
    lt_ui_statusbar_end(ui);
    lt_ui_text_style_reset(ui);
}

/* ------------------------------------------------------------------ */
/* welcome                                                             */
static void welcome_cell(lt_app *app, lt_rect area)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int cell = lt_ui_cell_mode(ui);
    int cw = LT_MIN(area.w - 32, 640), i, w1 = -1, w2[2], rh = lt_ui_row_height(ui);
    lt_rect r;
    char buf[640];
    r = lt_rect_make(area.x + (area.w - cw) / 2, area.y + (cell ? lt_ui_px(ui, 16) : lt_ui_px(ui, 40)), cw, area.h - lt_ui_px(ui, 40));
    lt_ui_draw_fill(ui, area, t->bg_app);
    if (cell) {
        lt_ui_draw_text_in(ui, lt_rect_make(r.x, r.y, r.w, lt_ui_px(ui, 16)), "LibreTune", t->primary, 1);
        lt_ui_draw_text_in(ui, lt_rect_make(r.x, r.y + lt_ui_px(ui, 16), r.w, lt_ui_px(ui, 16)),
                           "Open-source ECU tuning software", t->text2, 1);
        r.y += 48;
    } else {
        lt_ui_draw_vtext(ui, r.x + r.w / 2.0, r.y + 28, "LibreTune", 44.0, 0.0, 1, 1, 3.0, t->primary);
        lt_ui_draw_text_in(ui, lt_rect_make(r.x, r.y + lt_ui_px(ui, 64), r.w, lt_ui_px(ui, 24)),
                           "Open-source ECU tuning software", t->text2, 1);
        r.y += 110;
    }
    r.h = area.y + area.h - r.y;
    lt_ui_region_begin(ui, r, 0);
    w2[0] = -1; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, rh * 2);
    if (lt_ui_button_ex(ui, "Demo mode (simulated ECU)", LT_UI_BTN_PRIMARY)) { lt_app_start_demo(app, NULL); }
    if (lt_ui_button(ui, "New Project...")) { lt_app_new_project_dialog(app); }
    lt_ui_layout_row(ui, 2, w2, 0);
    if (lt_ui_button(ui, "Open Project...")) {
        lt_app_file_dialog(app, LT_APP_FD_OPEN_PROJECT, LT_UI_FILE_OPEN, NULL, "project.json",
                           "Project|project.json|All files|*");
    }
    if (lt_ui_button(ui, "Import TunerStudio Project...")) {
        lt_app_file_dialog(app, LT_APP_FD_IMPORT_TS, LT_UI_FILE_OPEN, NULL, "", "All files|*");
    }
    lt_ui_layout_space(ui, 12);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_heading(ui, "Recent Projects");
    if (app->settings.n_recent == 0) {
        lt_ui_label_ex(ui, "No recent projects yet.", t->muted, LT_UI_ALIGN_LEFT);
    }
    for (i = 0; i < app->settings.n_recent; i++) {
        int w3[2];
        w3[0] = -1; w3[1] = 8 * 11;
        lt_ui_layout_row(ui, 2, w3, 0);
        lt_ui_push_id_int(ui, i);
        lt_snprintf(buf, sizeof(buf), "%s   %s", lt_path_base(app->settings.recent[i]),
                    app->settings.recent[i]);
        if (lt_ui_button_ex(ui, buf, LT_UI_BTN_FLAT)) { lt_app_open_project(app, app->settings.recent[i]); }
        if (lt_ui_button_ex(ui, app->confirm_delete == i ? "Confirm" : "Remove",
                            app->confirm_delete == i ? LT_UI_BTN_DANGER : LT_UI_BTN_FLAT)) {
            if (app->confirm_delete == i) {
                int k;
                for (k = i; k < app->settings.n_recent - 1; k++) {
                    lt_strlcpy(app->settings.recent[k], app->settings.recent[k + 1],
                               sizeof(app->settings.recent[0]));
                }
                app->settings.n_recent--;
                app->confirm_delete = -1;
                lt_app_settings_save(app);
                lt_ui_pop_id(ui);
                break;
            }
            app->confirm_delete = i;
        }
        lt_ui_pop_id(ui);
    }
    lt_ui_region_end(ui);
}

static void welcome(lt_app *app, lt_rect area)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int i, cw, y, bw1, bw2, bh, gap, cardw, cardh;
    lt_rect r;
    int w1 = -1, w2[2];
    char buf[640];
    if (lt_ui_cell_mode(ui)) { welcome_cell(app, area); return; }
    lt_ui_draw_fill(ui, area, t->bg_app);
    /* the stock welcome view is centred about 40% down from the top */
    y = area.y + LT_MAX(lt_ui_px(ui, 24), (area.h - lt_ui_px(ui, 560)) / 3);
    lt_ui_draw_text_in_f(ui, lt_rect_make(area.x, y, area.w, lt_ui_px(ui, 48)), "LibreTune", t->text, 1,
                         (int)LT_FONT_SANS_SEMIBOLD, 36);
    lt_ui_draw_text_in_f(ui, lt_rect_make(area.x, y + lt_ui_px(ui, 46), area.w, lt_ui_px(ui, 24)),
                         "Open-source ECU tuning software", t->muted, 1, (int)LT_FONT_SANS, LT_UI_FS_LG);
    y += lt_ui_px(ui, 100);
    bw1 = lt_ui_px(ui, 154); bw2 = lt_ui_px(ui, 164); bh = lt_ui_px(ui, 56); gap = lt_ui_px(ui, 16);
    cw = bw1 + gap + bw2;
    r = lt_rect_make(area.x + (area.w - cw) / 2, y, cw, bh);
    lt_ui_region_begin(ui, r, 0);
    w2[0] = bw1; w2[1] = bw2;
    lt_ui_layout_row_px(ui, 2, w2, bh);
    lt_ui_layout_gap_px(ui, gap, 0);
    lt_ui_text_style(ui, (int)LT_FONT_SANS_MEDIUM, LT_UI_FS_XL);
    if (lt_ui_button_ex(ui, "New Project", LT_UI_BTN_PRIMARY)) { lt_app_new_project_dialog(app); }
    if (lt_ui_button(ui, "Open Project")) {
        lt_app_file_dialog(app, LT_APP_FD_OPEN_PROJECT, LT_UI_FILE_OPEN, NULL, "project.json",
                           "Project|project.json|All files|*");
    }
    lt_ui_text_style_reset(ui);
    lt_ui_region_end(ui);
    y += bh + lt_ui_px(ui, 12);
    /* secondary actions */
    r = lt_rect_make(area.x + (area.w - cw) / 2, y, cw, lt_ui_px(ui, 26));
    lt_ui_region_begin(ui, r, 0);
    lt_ui_layout_row_px(ui, 2, w2, lt_ui_px(ui, 26));
    lt_ui_layout_gap_px(ui, gap, 0);
    if (lt_ui_button_ex(ui, "Demo mode", LT_UI_BTN_FLAT)) { lt_app_start_demo(app, NULL); }
    if (lt_ui_button_ex(ui, "Import TunerStudio", LT_UI_BTN_FLAT)) {
        lt_app_file_dialog(app, LT_APP_FD_IMPORT_TS, LT_UI_FILE_OPEN, NULL, "", "All files|*");
    }
    lt_ui_region_end(ui);
    y += lt_ui_px(ui, 26) + lt_ui_px(ui, 36);
    /* recent projects */
    cardw = LT_MIN(lt_ui_px(ui, 500), area.w - lt_ui_px(ui, 32));
    cardh = lt_ui_px(ui, 72);
    if (app->settings.n_recent > 0) {
        lt_ui_draw_text_in_f(ui, lt_rect_make(area.x, y, area.w, lt_ui_px(ui, 20)), "Recent Projects", t->text2, 1,
                             (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_MD);
        y += lt_ui_px(ui, 32);
    }
    for (i = 0; i < app->settings.n_recent; i++) {
        lt_rect card = lt_rect_make(area.x + (area.w - cardw) / 2, y, cardw, cardh);
        lt_rect del = lt_rect_make(card.x + card.w - lt_ui_px(ui, 28) - lt_ui_px(ui, 8), card.y + (card.h - lt_ui_px(ui, 24)) / 2, lt_ui_px(ui, 28), lt_ui_px(ui, 24));
        lt_rect body = lt_rect_make(card.x, card.y, card.w - lt_ui_px(ui, 40), card.h);
        int hov;
        if (card.y + card.h > area.y + area.h) { break; }
        lt_ui_push_id_int(ui, i);
        {
            lt_point mp = lt_ui_mouse(ui);
            lt_bool hv = lt_rect_contains(card, mp.x, mp.y) && !lt_ui_popup_open(ui);
            lt_ui_draw_shadow(ui, card, t->radius_lg, lt_ui_px(ui, 4), 40);
            lt_ui_draw_box(ui, card, hv ? t->hover : t->elevated, hv ? t->border_strong : t->border, t->radius_lg);
        }
        lt_ui_region_begin(ui, body, 0);
        lt_ui_layout_row_px(ui, 1, &w1, body.h);
        lt_ui_layout_gap(ui, 0, 0);
        hov = lt_ui_button_ex(ui, "##card", LT_UI_BTN_FLAT);
        {
            lt_rect cr = lt_ui_last_rect(ui);
            lt_ui_draw_text_in_f(ui, lt_rect_make(cr.x + lt_ui_px(ui, 16), cr.y + lt_ui_px(ui, 12), cr.w - lt_ui_px(ui, 24), lt_ui_px(ui, 20)),
                                 lt_path_base(app->settings.recent[i]), t->text, 0, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_LG);
            lt_snprintf(buf, sizeof(buf), "%s", app->settings.recent[i]);
            lt_ui_draw_text_in_f(ui, lt_rect_make(cr.x + lt_ui_px(ui, 16), cr.y + lt_ui_px(ui, 36), cr.w - lt_ui_px(ui, 24), lt_ui_px(ui, 18)), buf, t->muted, 0,
                                 (int)LT_FONT_SANS, LT_UI_FS_BASE);
        }
        lt_ui_region_end(ui);
        if (hov) { lt_app_open_project(app, app->settings.recent[i]); lt_ui_pop_id(ui); break; }
        lt_ui_region_begin(ui, del, 0);
        lt_ui_layout_row_px(ui, 1, &w1, del.h);
        lt_ui_layout_gap(ui, 0, 0);
        if (lt_ui_icon_button(ui, LT_UI_LUCIDE(LT_ICON_TRASH_2), app->confirm_delete == i ? "Click again to remove" : "Remove from list",
                              app->confirm_delete == i ? LT_UI_BTN_DANGER : 0)) {
            if (app->confirm_delete == i) {
                int k;
                for (k = i; k < app->settings.n_recent - 1; k++) {
                    lt_strlcpy(app->settings.recent[k], app->settings.recent[k + 1],
                               sizeof(app->settings.recent[0]));
                }
                app->settings.n_recent--;
                app->confirm_delete = -1;
                lt_app_settings_save(app);
                lt_ui_region_end(ui);
                lt_ui_pop_id(ui);
                break;
            }
            app->confirm_delete = i;
        }
        lt_ui_region_end(ui);
        lt_ui_pop_id(ui);
        y += cardh + lt_ui_px(ui, 8);
    }
}

/* ------------------------------------------------------------------ */
/* tab area                                                            */
static void tab_area(lt_app *app, lt_rect main_r)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int cell = lt_ui_cell_mode(ui);
    int w1 = -1, i, closed = -1, k;
    lt_rect inner, body;
    lt_app_tab *at;
    lt_ui_draw_fill(ui, main_r, t->bg_app);
    inner = cell ? lt_rect_inset(main_r, t->padding, 0) : main_r;
    lt_ui_region_begin(ui, inner, 0);
    lt_ui_layout_gap(ui, 0, 0);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_tabbar_begin(ui, "tabs", &app->active_tab);
    for (i = 0; i < app->n_tabs; i++) {
        int fl = (app->tabs[i]->closable ? LT_UI_TAB_CLOSABLE : 0) | (app->tabs[i]->dirty ? LT_UI_TAB_DIRTY : 0);
        lt_ui_push_id_int(ui, i);
        if (lt_ui_tab_icon(ui, app->tabs[i]->title, cell ? 0 : LT_UI_LUCIDE(tab_icon(app->tabs[i]->kind)), fl) & LT_UI_TAB_CLOSED) { closed = i; }
        lt_ui_pop_id(ui);
    }
    lt_ui_tabbar_end(ui);
    lt_ui_layout_row(ui, 1, &w1, -1);
    body = lt_ui_layout_next(ui);
    at = lt_app_tab_active(app);
    if (at != NULL) {
        lt_ui_region_begin(ui, body, 0);
        for (k = 0; k < app->n_tab_kinds; k++) {
            if (strcmp(app->tab_kinds[k].kind, at->kind) == 0) { app->tab_kinds[k].draw(app, at, body); break; }
        }
        lt_ui_region_end(ui);
    }
    lt_ui_region_end(ui);
    if (closed >= 0 && closed < app->n_tabs) { lt_app_tab_close(app, app->tabs[closed]); }
}

/* ------------------------------------------------------------------ */
/* quit, shortcuts, dialogs                                            */
void lt_app_request_quit(lt_app *app)
{
    if (app->ini != NULL && (app->modified || (app->mem != NULL && lt_ecu_mem_has_pending_burn(app->mem)))) {
        app->quit_open = 1;
    } else {
        app->quit = 1;
    }
}

static void shortcuts(lt_app *app)
{
    lt_ui *ui = app->ui;
    int i;
    if (lt_ui_key_pressed(ui, 'q', LT_MOD_CTRL)) { lt_app_request_quit(app); }
    if (lt_ui_key_pressed(ui, 'n', LT_MOD_CTRL)) { lt_app_new_project_dialog(app); }
    if (lt_ui_key_pressed(ui, LT_KEY_F9, 0)) { app->settings.sidebar_on = !app->settings.sidebar_on; }
    lt_ext_shortcuts(app);
    if (app->ini == NULL) { return; }
    if (lt_ui_key_pressed(ui, 'o', LT_MOD_CTRL)) {
        lt_app_file_dialog(app, LT_APP_FD_LOAD_TUNE, LT_UI_FILE_OPEN, NULL, "", "Tune files|*.msq;*.xml|All files|*");
    }
    if (lt_ui_key_pressed(ui, 's', LT_MOD_CTRL)) { lt_app_save_tune(app); }
    if (lt_ui_key_pressed(ui, 's', LT_MOD_CTRL | LT_MOD_SHIFT)) {
        lt_app_file_dialog(app, LT_APP_FD_SAVE_TUNE, LT_UI_FILE_SAVE, NULL, "tune.msq", "Tune files|*.msq;*.xml|All files|*");
    }
    if (lt_ui_key_pressed(ui, 'b', LT_MOD_CTRL) && lt_app_can_burn(app)) { app->burn_open = 1; }
    if (lt_ui_key_pressed(ui, 'c', LT_MOD_CTRL | LT_MOD_SHIFT)) { lt_app_connect_dialog(app); }
    if (lt_ui_key_pressed(ui, 'd', LT_MOD_CTRL | LT_MOD_SHIFT)) { lt_app_disconnect(app); }
    if (lt_ui_key_pressed(ui, 'd', LT_MOD_CTRL)) { lt_app_dashboard_tab_open(app); }
    if (lt_ui_key_pressed(ui, 'l', LT_MOD_CTRL)) { lt_app_log_toggle(app); }
    if (lt_ui_key_pressed(ui, ',', LT_MOD_CTRL)) { lt_app_settings_dialog(app); }
    if (lt_ui_key_pressed(ui, 'w', LT_MOD_CTRL)) {
        lt_app_tab *a = lt_app_tab_active(app);
        if (a != NULL && a->closable) { lt_app_tab_close(app, a); }
    }
    if (lt_ui_key_pressed(ui, LT_KEY_TAB, LT_MOD_CTRL) && app->n_tabs > 0) {
        app->active_tab = (app->active_tab + 1) % app->n_tabs;
    }
    if (lt_ui_key_pressed(ui, LT_KEY_TAB, LT_MOD_CTRL | LT_MOD_SHIFT) && app->n_tabs > 0) {
        app->active_tab = (app->active_tab + app->n_tabs - 1) % app->n_tabs;
    }
    for (i = 1; i <= 9; i++) {
        if (lt_ui_key_pressed(ui, '0' + i, LT_MOD_CTRL) && i <= app->n_tabs) { app->active_tab = i - 1; }
    }
}

static void app_dialogs(lt_app *app)
{
    lt_ui *ui = app->ui;
    int res, i, k;
    if (app->burn_open) {
        res = lt_ui_msgbox(ui, "burn", "Burn to ECU",
                           "Burn writes the current tune to the ECU's permanent flash memory.\n"
                           "Make sure the engine is not running. Continue?", "Burn|Cancel", &app->burn_open);
        if (res == 1) { lt_app_burn(app); }
    }
    if (app->quit_open) {
        res = lt_ui_msgbox(ui, "quit", "Unsaved changes", "The tune has unsaved changes.",
                           "Save|Discard|Cancel", &app->quit_open);
        if (res == 1) { if (app->has_project) { lt_app_save_tune(app); app->quit = 1; } else { app->quit = 1; } }
        else if (res == 2) { app->modified = 0; app->quit = 1; }
    }
    if (app->close_proj_open) {
        res = lt_ui_msgbox(ui, "closeproj", "Unsaved changes", "The tune has unsaved changes.",
                           "Save|Discard|Cancel", &app->close_proj_open);
        if (res == 1) { lt_app_save_tune(app); lt_app_close_project(app); }
        else if (res == 2) { lt_app_close_project(app); }
    }
    file_dialog_frame(app);
    lt_ext_overlays(app);
    for (i = 0; i < app->n_dialogs; i++) {
        lt_app_dialog *d = app->dialogs[i];
        for (k = 0; k < app->n_dlg_kinds; k++) {
            if (strcmp(app->dlg_kinds[k].kind, d->kind) == 0) { app->dlg_kinds[k].draw(app, d); break; }
        }
        if (!d->open) {
            int j;
            if (k < app->n_dlg_kinds && app->dlg_kinds[k].close != NULL) { app->dlg_kinds[k].close(app, d); }
            for (j = i; j < app->n_dialogs - 1; j++) { app->dialogs[j] = app->dialogs[j + 1]; }
            app->n_dialogs--;
            lt_free(d);
            i--;
        }
    }
}

/* sidebar width is stored in logical px; the splitter works in device px */
static void sidebar_split(lt_app *app, lt_rect cr, lt_rect *side, lt_rect *main_r)
{
    lt_ui *ui = app->ui;
    int dev = lt_ui_px(ui, app->settings.sidebar_w), old = dev;
    lt_ui_splitter(ui, "sidebar", cr, 1, &dev, lt_ui_px(ui, 160), cr.w / 2, side, main_r);
    if (dev != old) { app->settings.sidebar_w = (int)((lt_s64)dev * 100 / lt_ui_scale_pct(ui)); }
}

/* ------------------------------------------------------------------ */
void lt_app_frame(lt_app *app)
{
    lt_ui *ui = app->ui;
    lt_rect cr, side, main_r;
    char title[200];
    if (lt_dashx_kiosk_frame(app)) { return; }
    menubar(app);
    if (!lt_ui_cell_mode(ui)) { app_toolbar(app); }
    statusbar(app);
    cr = lt_ui_client_rect(ui);
    if (app->ini == NULL) {
        if (!lt_ui_cell_mode(ui) && app->settings.sidebar_on) {
            sidebar_split(app, cr, &side, &main_r);
            sidebar(app, side);
        } else {
            main_r = cr;
        }
        welcome(app, main_r);
    } else {
        if (app->settings.sidebar_on) {
            sidebar_split(app, cr, &side, &main_r);
            sidebar(app, side);
        } else {
            main_r = cr;
        }
        tab_area(app, main_r);
    }
    shortcuts(app);
    app_dialogs(app);
    lt_snprintf(title, sizeof(title), "LibreTune%s%s",
                app->has_project ? " - " : "", app->has_project ? app->project.cfg.name : "");
    if (strcmp(title, app->title) != 0) {
        lt_strlcpy(app->title, title, sizeof(app->title));
        if (app->backend != NULL && app->backend->set_title != NULL) { app->backend->set_title(app->backend, title); }
    }
}

/* ------------------------------------------------------------------ */
/* window + main loop                                                  */
static void init_surface(lt_app *app, int w, int h)
{
    if (app->cell_mode) {
        lt_cells_init(&app->cells, w, h);
        lt_gfx_init_cells(&app->gfx, &app->cells);
    } else {
        lt_canvas_init(&app->canvas, w, h);
        lt_gfx_init_canvas(&app->gfx, &app->canvas);
    }
    app->ui = lt_ui_create();
    lt_ui_set_backend(app->ui, app->backend);
    lt_ui_set_scale_pct(app->ui, app->scale_pct);
    lt_app_apply_theme(app, app->settings.theme);
}

#define WIN_DEF_W 1280          /* stock LibreTune window, logical px */
#define WIN_DEF_H 800
#define WIN_MIN_W 1024
#define WIN_MIN_H 700

static int round25(double pct)
{
    return (int)(pct / 25.0 + 0.5) * 25;
}

int lt_app_pick_scale(const char *env, int setting_pct, double hint)
{
    int pct = 0;
    if (env != NULL && env[0] != '\0' && lt_stricmp(env, "auto") != 0) {
        pct = atoi(env);
        if (pct >= 1 && pct <= 3) { pct *= 100; }
    }
    if (pct <= 0) { pct = setting_pct; }
    if (pct <= 0) { pct = round25(hint * 100.0); }
    return LT_CLAMP(pct, 100, 300);
}

void lt_app_pick_window(const lt_app_settings *s, int pct, int scr_w, int scr_h,
                        int *w, int *h, int *x, int *y, int *flags)
{
    int lw = WIN_DEF_W, lh = WIN_DEF_H, pw, ph;
    *x = 0; *y = 0; *flags = 0;
    if (s->win_w >= WIN_MIN_W && s->win_h >= WIN_MIN_H && s->win_w <= 16384 && s->win_h <= 16384) {
        int sw = (int)((lt_s64)s->win_w * pct / 100), sh = (int)((lt_s64)s->win_h * pct / 100);
        if (scr_w <= 0 || scr_h <= 0 || (sw <= scr_w && sh <= scr_h)) { lw = s->win_w; lh = s->win_h; }
    }
    pw = (int)((lt_s64)lw * pct / 100);
    ph = (int)((lt_s64)lh * pct / 100);
    if (scr_w > 0 && scr_h > 0) {            /* never larger than the desktop */
        if (pw > scr_w) { pw = scr_w; }
        if (ph > scr_h) { ph = scr_h; }
    }
    *w = pw; *h = ph;
    if (s->win_pos) {
        int ok = 1;
        if (scr_w > 0 && scr_h > 0) {
            ok = s->win_x > -(pw - 100) && s->win_x < scr_w - 100 && s->win_y >= 0 && s->win_y < scr_h - 100;
        } else {
            ok = s->win_x >= 0 && s->win_y >= 0;
        }
        if (ok) { *x = s->win_x; *y = s->win_y; *flags |= LT_GEOM_POS; }
    }
    if (s->win_max) { *flags |= LT_GEOM_MAX; }
}

static void apply_min_size(lt_app *app)
{
    int sw = 0, sh = 0, mw = (int)((lt_s64)WIN_MIN_W * app->scale_pct / 100);
    int mh = (int)((lt_s64)WIN_MIN_H * app->scale_pct / 100);
    if (lt_backend_screen_size(app->backend, &sw, &sh)) {
        if (mw > sw) { mw = sw; }
        if (mh > sh) { mh = sh; }
    }
    lt_backend_set_min_size(app->backend, mw, mh);
}

void lt_app_set_scale_setting(lt_app *app, int pct)
{
    char *e = lt_plat_getenv("LT_SCALE");
    app->settings.scale_pct = pct == 0 ? 0 : LT_CLAMP(pct, 100, 300);
    app->scale_pct = lt_app_pick_scale(e, app->settings.scale_pct, lt_backend_scale_hint(app->backend));
    lt_free(e);
    if (app->ui != NULL) { lt_ui_set_scale_pct(app->ui, app->scale_pct); lt_ui_request_redraw(app->ui); }
    if (app->backend != NULL && !app->cell_mode) { apply_min_size(app); }
}

int lt_app_open_window(lt_app *app, const char *backend, int w, int h)
{
    lt_backend *be = lt_backend_select(backend != NULL ? backend : app->settings.backend);
    char *e;
    int sw = 0, sh = 0, x, y, flags;
    if (be == NULL) { lt_err_set("no GUI backend available"); return -1; }
    e = lt_plat_getenv("LT_SCALE");
    app->scale_pct = lt_app_pick_scale(e, app->settings.scale_pct, lt_backend_scale_hint(be));
    lt_free(e);
    app->backend = be;
    if (!be->cell_mode) {
        /* w x h is the default LOGICAL size; the saved size (if any) wins */
        (void)lt_backend_screen_size(be, &sw, &sh);
        lt_app_pick_window(&app->settings, app->scale_pct, sw, sh, &w, &h, &x, &y, &flags);
        apply_min_size(app);
        lt_backend_set_initial_geometry(be, x, y, flags);
    }
    if (be->open(be, "LibreTune", w, h) != 0) {
        lt_err_set("cannot open backend '%s': %s", be->name, lt_err_get());
        lt_backend_destroy(be);
        app->backend = NULL;
        return -1;
    }
    app->cell_mode = be->cell_mode ? 1 : 0;
    be->size(be, &w, &h);
    init_surface(app, w, h);
    return 0;
}

/* remember window geometry (logical size) for the next start */
static void save_geometry(lt_app *app)
{
    lt_app_settings *s = &app->settings;
    int gx = 0, gy = 0, gm = 0, w = 0, h = 0, pct = app->scale_pct > 0 ? app->scale_pct : 100;
    if (app->backend == NULL || app->cell_mode || app->headless) { return; }
    if (lt_backend_get_geometry(app->backend, &gx, &gy, &gm)) {
        s->win_max = gm ? 1 : 0;
        s->win_x = gx; s->win_y = gy; s->win_pos = 1;
    }
    if (!s->win_max) {                       /* keep the last normal size while maximized */
        app->backend->size(app->backend, &w, &h);
        if (w > 0 && h > 0) {
            s->win_w = (int)((lt_s64)w * 100 / pct);
            s->win_h = (int)((lt_s64)h * 100 / pct);
        }
    }
}

static void run_frame(lt_app *app)
{
    lt_ui_begin(app->ui, &app->gfx, lt_plat_now_ms());
    lt_app_frame(app);
    lt_ui_end(app->ui);
    if (app->backend != NULL) {
        app->backend->present(app->backend, app->cell_mode ? NULL : &app->canvas,
                              app->cell_mode ? &app->cells : NULL, app->gfx.dirty);
    }
    lt_gfx_reset_dirty(&app->gfx);
}

int lt_app_run(lt_app *app)
{
    lt_event ev;
    lt_backend *be = app->backend;
    int got, timeout, hint, frames = 0, max_frames = 0;
    double last_frame_ms = 0.0;
    char *fenv = lt_plat_getenv("LT_MAX_FRAMES");
    if (fenv != NULL) { max_frames = atoi(fenv); lt_free(fenv); }
    while (!app->quit) {
        hint = lt_ui_wait_hint_ms(app->ui);
        timeout = lt_ui_needs_redraw(app->ui) ? 0 : hint;
        if (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) {
            if (timeout < 0 || timeout > 25) { timeout = 25; }
        }
        got = be->poll(be, &ev, timeout);
        while (got) {
            if (ev.type == LT_EV_QUIT) {
                lt_app_request_quit(app);
            } else if (ev.type == LT_EV_SCALE) {
                if (app->settings.scale_pct == 0) {
                    char *se = lt_plat_getenv("LT_SCALE");
                    app->scale_pct = lt_app_pick_scale(se, 0, (double)ev.x / 100.0);
                    lt_free(se);
                    lt_ui_set_scale_pct(app->ui, app->scale_pct);
                    apply_min_size(app);
                    lt_ui_request_redraw(app->ui);
                }
            } else if (ev.type == LT_EV_RESIZE) {
                if (app->cell_mode) {
                    lt_cells_resize(&app->cells, ev.x, ev.y);
                    lt_gfx_init_cells(&app->gfx, &app->cells);
                } else if (ev.x > 0 && ev.y > 0) {
                    lt_canvas_resize(&app->canvas, ev.x, ev.y);
                    lt_gfx_init_canvas(&app->gfx, &app->canvas);
                }
                lt_ui_request_redraw(app->ui);
            } else {
                lt_ui_input(app->ui, &ev);
            }
            got = be->poll(be, &ev, 0);
        }
        if (lt_app_service(app)) { lt_ui_request_redraw(app->ui); }
        /* Redraw only when something changed (input, new ECU data, explicit
         * request) or when the toolkit's own timer (tooltips, toasts, caret
         * blink) has come due. Redrawing unconditionally every poll tick made
         * the app CPU-bound and input laggy. */
        if (lt_ui_needs_redraw(app->ui) ||
            (hint >= 0 && lt_plat_now_ms() - last_frame_ms >= (double)hint)) {
            last_frame_ms = lt_plat_now_ms();
            run_frame(app);
            frames++;
            if (max_frames > 0 && frames >= max_frames) { app->quit = 1; }
        }
    }
    save_geometry(app);
    lt_app_settings_save(app);
    return 0;
}

static void write_ppm(const lt_canvas *c, const char *path)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (f == NULL) { return; }
    fprintf(f, "P6\n%d %d\n255\n", c->w, c->h);
    for (y = 0; y < c->h; y++) {
        for (x = 0; x < c->w; x++) {
            lt_u32 v = c->px[y * c->pitch + x];
            fputc((int)((v >> 16) & 255), f);
            fputc((int)((v >> 8) & 255), f);
            fputc((int)(v & 255), f);
        }
    }
    fclose(f);
}

int lt_app_screenshot(lt_app *app, const char *ppm_path, int w, int h, int frames)
{
    int i;
    char *click = lt_plat_getenv("LT_SHOT_CLICK");
    int cx = -1, cy = -1, button = LT_MB_LEFT;
    lt_event ev;
    app->cell_mode = 0;
    app->headless = 1;
    {
        char *se = lt_plat_getenv("LT_SCALE");
        if (se == NULL) { se = lt_plat_getenv("LT_SHOT_SCALE"); }
        app->scale_pct = lt_app_pick_scale(se, app->settings.scale_pct, 1.0);
        lt_free(se);
    }
    init_surface(app, w, h);
    if (click == NULL) {
        click = lt_plat_getenv("LT_SHOT_RCLICK");   /* same, with the right mouse button */
        if (click != NULL) { button = LT_MB_RIGHT; }
    }
    if (click != NULL) { sscanf(click, "%d,%d", &cx, &cy); lt_free(click); }
    for (i = 0; i < frames; i++) {
        lt_app_service(app);
        if (cx >= 0 && i == frames - 3) {
            memset(&ev, 0, sizeof(ev));
            ev.type = LT_EV_MOUSE_MOVE; ev.x = cx; ev.y = cy; lt_ui_input(app->ui, &ev);
            run_frame(app);
            ev.type = LT_EV_MOUSE_DOWN; ev.button = button; lt_ui_input(app->ui, &ev);
            run_frame(app);
            ev.type = LT_EV_MOUSE_UP; lt_ui_input(app->ui, &ev);
        }
        run_frame(app);
        lt_plat_sleep_ms(60);
    }
    write_ppm(&app->canvas, ppm_path);
    return 0;
}
