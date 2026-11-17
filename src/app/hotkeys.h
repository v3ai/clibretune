/* hotkeys.h - customizable keyboard shortcuts (G-91): key name parsing/formatting, the table of
 * built-in shortcut actions, the user overrides (hotkeys.conf in the config directory) and
 * conflict detection.  Pure data and helpers, no UI; the shell glue is in feat_hotkeys.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_HOTKEYS_H
#define LT_HOTKEYS_H

#include "../lt.h"

#define LT_HK_NONE 0          /* key value of an unbound action */
#define LT_HK_MAX_OV 96       /* user overrides kept */
#define LT_HK_ID_MAX 40

/* a shortcut action and its factory binding */
typedef struct lt_hk_def {
    const char *id;           /* stable name, e.g. "file.save" */
    const char *label;        /* English action name (translated for display) */
    int key;                  /* key as lt_ui_key_pressed takes it (letters lower case) */
    int mods;                 /* LT_MOD_* */
} lt_hk_def;

typedef struct lt_hk_ov { char id[LT_HK_ID_MAX]; int key, mods; } lt_hk_ov;
/* the user's changes on top of the factory bindings */
typedef struct lt_hk_set { lt_hk_ov ov[LT_HK_MAX_OV]; int n; } lt_hk_set;

/* "Ctrl+Shift+S", "F9", "Ctrl+,"; an unbound key gives "None".  Returns out. */
const char *lt_hk_format(int key, int mods, char *out, size_t cap);
/* parse the same notation (case-insensitive); "none" or "" gives key 0.  LT_FALSE when the text is
 * not a key combination */
lt_bool lt_hk_parse(const char *s, int *key, int *mods);

/* built-in shell actions (the factory table) */
int               lt_hk_core_count(void);
const lt_hk_def  *lt_hk_core_def(int i);
const lt_hk_def  *lt_hk_core_find(const char *id);

/* a binding is acceptable when it carries Ctrl/Alt (plain keys would clash with typing) or is a
 * function key; 0 = ok, otherwise a short English reason */
const char *lt_hk_check(int key, int mods);

void lt_hk_set_clear(lt_hk_set *s);
/* effective binding of def (override or factory); returns LT_TRUE when it was overridden */
lt_bool lt_hk_get(const lt_hk_set *s, const lt_hk_def *def, int *key, int *mods);
int  lt_hk_set_bind(lt_hk_set *s, const char *id, int key, int mods);   /* 0 ok, -1 full */
void lt_hk_set_reset(lt_hk_set *s, const char *id);                      /* back to factory */
/* index in defs (n entries) of the action other than skip_id that is bound to key+mods, or -1 */
int  lt_hk_conflict(const lt_hk_set *s, const lt_hk_def *defs, int n, int key, int mods,
                    const char *skip_id);

/* <dir>/hotkeys.conf: one "id=Ctrl+S" or "id=none" per line, '#' comments.  load keeps what
 * parses; 0 ok, -1 no file.  save writes only the overrides (an empty set removes nothing) */
int  lt_hk_load(lt_hk_set *s, const char *dir);
int  lt_hk_save(const lt_hk_set *s, const char *dir);

#endif
