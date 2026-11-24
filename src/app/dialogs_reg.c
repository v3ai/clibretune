/* dialogs_reg.c - registration and open hooks of the dialogs owner.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "dialogs.h"

lt_bool lt_dlg_contains_ci(const char *hay, const char *needle)
{
    size_t n, i, j;
    if (needle == NULL || needle[0] == '\0') { return LT_TRUE; }
    if (hay == NULL) { return LT_FALSE; }
    n = strlen(needle);
    for (i = 0; hay[i] != '\0'; i++) {
        for (j = 0; j < n; j++) {
            if (hay[i + j] == '\0') { return LT_FALSE; }
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j])) { break; }
        }
        if (j == n) { return LT_TRUE; }
    }
    return LT_FALSE;
}

void lt_app_register_dialogs(lt_app *app)
{
    lt_dlg_register_ini(app);
    lt_dlg_register_connect(app);
    lt_dlg_register_settings(app);
    lt_dlg_register_newproj(app);
    lt_dlg_register_misc(app);
    lt_dlg_register_log(app);
}

int lt_app_dialog_open(lt_app *app, const char *name)
{
    const lt_ini_dialog *dlg = lt_ini_find_dialog(app->ini, name);
    if (dlg == NULL) { dlg = lt_ini_std_panel(app->ini, name); }
    return lt_app_tab_open(app, "dialog", name, dlg != NULL && dlg->title != NULL && dlg->title[0] != '\0' ? dlg->title : name, 1)
           != NULL ? 0 : -1;
}

int lt_app_datalog_open(lt_app *app, int viewer)
{
    return lt_app_tab_open(app, viewer ? "datalog-viewer" : "datalog", "datalog",
                           viewer ? "Datalog Viewer" : "Data Logging", 1) != NULL ? 0 : -1;
}

int lt_app_special_open(lt_app *app, const char *id)
{
    const char *title = id, *kind = "special";
    if (strcmp(id, "och-status") == 0) { title = "Channel Status"; }
    else if (strcmp(id, "console") == 0) { title = "Console"; kind = "console"; }
    else if (strcmp(id, "tooth-logger") == 0) { title = "Tooth Logger"; kind = "toothlog"; }
    else if (strcmp(id, "composite-logger") == 0) { title = "Composite Logger"; kind = "complog"; }
    return lt_app_tab_open(app, kind, id, title, 1) != NULL ? 0 : -1;
}

int lt_app_connect_dialog(lt_app *app)
{
    return lt_app_dialog_push(app, "connect", "", "Connection Settings") != NULL ? 0 : -1;
}

int lt_app_settings_dialog(lt_app *app)
{
    return lt_app_dialog_push(app, "settings", "", "Settings") != NULL ? 0 : -1;
}

int lt_app_new_project_dialog(lt_app *app)
{
    return lt_app_dialog_push(app, "newproject", "", "New Project") != NULL ? 0 : -1;
}

int lt_app_basemap_wizard(lt_app *app)
{
    if (app->ini == NULL) { lt_app_toast(app, 2, "Open a project first"); return -1; }
    return lt_app_dialog_push(app, "basemap", "", "Base Map Wizard") != NULL ? 0 : -1;
}

int lt_app_restore_points_dialog(lt_app *app)
{
    return lt_app_dialog_push(app, "restore", "", "Restore Points") != NULL ? 0 : -1;
}
