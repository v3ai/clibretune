/* app_ttyshot.h - cell-mode snapshot harness (see app_ttyshot.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_APP_TTYSHOT_H
#define LT_APP_TTYSHOT_H

#include "../gui/gui.h"

LT_BEGIN_DECLS

/* LT_TRUE when LT_SHOT_TTY / LT_SHOT_TEXT / LT_SHOT_ANSI request a cell-mode snapshot */
lt_bool lt_app_shot_is_tty(void);
/* parse an LT_SHOT_KEYS script into events; returns the count */
int lt_app_shot_parse(const char *script, lt_event *out, int max);
/* write the outputs requested by the environment; 0 = ok */
int lt_app_shot_write(const lt_cells *g);

LT_END_DECLS

#endif
