/* dialogs.h - internal glue between the dialogs_*.c files (not for other owners).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_DIALOGS_H
#define LT_DIALOGS_H

#include "app.h"

void lt_dlg_register_ini(lt_app *app);      /* tab kind "dialog" (INI [UserDefined]) */
void lt_dlg_register_connect(lt_app *app);   /* connect dialog */
void lt_dlg_register_settings(lt_app *app);  /* settings dialog */
void lt_dlg_register_newproj(lt_app *app);   /* new project wizard */
void lt_dlg_register_misc(lt_app *app);     /* basemap, restore, special tabs */
void lt_dlg_register_log(lt_app *app);      /* tab kinds datalog, datalog-viewer */

/* shared helpers */
/* case-insensitive substring test; empty needle matches */
lt_bool lt_dlg_contains_ci(const char *hay, const char *needle);
#define LT_DLG_FD_LOG_OPEN   (LT_APP_FD_USER0 + 1)
#define LT_DLG_FD_LOG_EXPORT (LT_APP_FD_USER0 + 2)
#define LT_DLG_FD_PROJ_INI   LT_APP_FD_NEW_PROJECT_INI

#endif
