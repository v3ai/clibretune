/* actions_logic.h - action recorder / manager helpers without UI (G-82): recording table edits as
 * actions, readable action text, file names, ECU compatibility, tag lists, playback.
 * Unit-tested in tests/test_actions.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_ACTIONS_LOGIC_H
#define LT_ACTIONS_LOGIC_H

#include "../lt.h"
#include "../project/action.h"

LT_BEGIN_DECLS

/* One TableEdit action (x = column, y = row; old value included) per cell where z differs from
 * z0 by more than 1e-9, into the recording; returns the number recorded (0 when idle). */
size_t lt_actx_record_table(lt_arec *r, const char *table, const double *z, const double *z0,
                            size_t rows, size_t cols);
/* ConstantChange action unless old and new are equal; 1 when recorded */
int    lt_actx_record_constant(lt_arec *r, const char *name, double old_v, double new_v);

/* "Table edit veTable [5, 3]: 74.2 -> 75.5", "Constant reqFuel: 8 -> 9", "Scale veTable (3 cells)
 * factor 1.05", "Pause 500 ms", "Send command xyz", "Lua: description" */
void   lt_actx_describe(const lt_action *a, char *out, size_t cap);
/* "<sanitised name>.ltaction" that does not exist in dir (name 2.ltaction ...) */
void   lt_actx_new_file(const char *dir, const char *name, char *out, size_t cap);
/* set has no ECU restriction, or one of its compatible ECUs matches the signature (case-insensitive,
 * either contains the other) */
lt_bool lt_actx_compatible(const lt_action_set *s, const char *signature);
/* "a, b ,c" -> trimmed non-empty entries (malloc'd vector, lt_strv_free) */
size_t lt_actx_split_list(const char *text, char ***out);
/* tags / ECUs joined with ", " */
void   lt_actx_join_list(char *const *v, size_t n, char *out, size_t cap);
/* replace tags (or compatible ECUs) of the set with the entries of the comma separated text */
void   lt_actx_set_tags(lt_action_set *s, const char *text);
void   lt_actx_set_ecus(lt_action_set *s, const char *text);
/* remove action i; 0 or LT_ERANGE */
int    lt_actx_remove(lt_action_set *s, size_t i);
/* append a copy of a to the set */
void   lt_actx_append(lt_action_set *s, const lt_action *a);
/* set modified_at = now (RFC3339 UTC) */
void   lt_actx_touch(lt_action_set *s);

LT_END_DECLS

#endif
